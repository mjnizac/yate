// The single Lua <-> C++ translation unit (spec section 8).
//
// Everything Sol2 touches is here. The headers expose only `RunScript`, so Sol2's compile cost is
// paid once and no Sol2 type reaches a consumer of the engine.
//
// Three rules shape the code below.
//
//  1. **Script functions do no math.** Every binding reads its named arguments, validates them,
//     appends one node to the graph and hands back a handle. The handle is a 12-byte `Value`.
//  2. **Every error names the script line that caused it.** A binding reads the caller's line out of
//     the Lua stack *before* it touches the graph, so a mapping mismatch is reported where it was
//     written rather than where the compiler eventually tripped over it.
//  3. **Nothing throws across the C boundary.** A failing binding throws a C++ exception that Sol2's
//     own wrapper catches one frame later, inside the same C++ frame that called into Lua. The
//     structured `Error` travels in a file-scope slot so the message survives the trip through
//     Lua's string-only error channel.

#include <engine/lua.hpp>

#include <engine/log.hpp>
#include <engine/memory/allocator.hpp>
#include <engine/memory/general.hpp>
#include <engine/terrain/graph.hpp>

#include <sol/sol.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <optional>
#include <string>

namespace engine::lua {
namespace {

using terrain::ArithOp;
using terrain::CurveOp;
using terrain::Domain;
using terrain::Graph;
using terrain::Mapping;
using terrain::NoiseKind;
using terrain::SourceLocation;
using terrain::Value;

// --- Error plumbing -----------------------------------------------------------------------------

/// The last error a binding raised. Lua's error channel carries a string, so the structured `Error`
/// with its code, stage and script line is parked here and picked up after the protected call fails.
///
/// One per thread because a script runs on whichever thread asked for it, and file-scope rather than
/// a member because the bindings are plain lambdas with no runtime object to reach for.
thread_local std::optional<Error> t_scriptError;

/// Thrown by a failing binding. Sol2 catches it in its own wrapper and turns it into a Lua error, so
/// the throw never crosses into C code.
class ScriptException final : public std::exception {
public:
    explicit ScriptException(const char* message) : m_message(message) {}
    [[nodiscard]] const char* what() const noexcept override { return m_message.c_str(); }

private:
    std::string m_message;
};

[[noreturn]] void Raise(Error error) {
    const std::array<char, Error::kMaxFormat> formatted = error.Format();
    t_scriptError                                        = error;
    throw ScriptException(formatted.data());
}

template <typename... Args>
[[noreturn]] void Fail(SourceLocation location, std::format_string<Args...> fmt, Args&&... args) {
    Raise(MakeScriptError(ErrorCode::ScriptError, ErrorStage::Script, location.file, location.line,
                          fmt, std::forward<Args>(args)...));
}

/// Unwraps a builder result, turning a failure into a script error at the caller's line.
///
/// The graph builders already produce the exact message and carry the location they were given, so
/// there is nothing to translate: the error only has to be routed out through Lua.
Value Unwrap(Result<Value> result) {
    if (!result) {
        Raise(result.error());
    }
    return *result;
}

// --- Source locations ---------------------------------------------------------------------------

/// Interns a chunk name so a `SourceLocation`'s `string_view` stays valid for the life of the graph.
///
/// Lua owns its `short_src` buffer and recycles it, and a node outlives the call that created it, so
/// the name has to be copied somewhere stable. There are only ever a handful of distinct chunk names
/// in one run, and these are never freed, exactly like the interned Tracy pool names.
///
/// The table is shared by every state, because the point of interning is that two graphs built from the
/// same file share one copy of its name. That makes it the one piece of mutable state here that is not
/// per-script, so it takes a lock: the viewer reloads on a worker thread while a previous graph may
/// still be alive, and an entry half-copied under a racing reader would hand out a dangling view. The
/// lock is taken a handful of times per script and never on a hot path.
[[nodiscard]] std::string_view InternChunkName(const char* name) {
    static constexpr usize_t kMaxNames  = 16;
    static constexpr usize_t kMaxLength = 128;
    static std::array<std::array<char, kMaxLength>, kMaxNames> names{};
    static usize_t                                            count = 0;
    static std::mutex                                         mutex;

    const std::string_view      incoming = name != nullptr ? std::string_view{name} : "?";
    std::lock_guard<std::mutex> lock(mutex);
    for (usize_t i = 0; i < count; ++i) {
        if (std::string_view{names[i].data()} == incoming) {
            return std::string_view{names[i].data()};
        }
    }
    if (count == kMaxNames) {
        return "?";
    }
    detail::CopyBounded(names[count], incoming);
    return std::string_view{names[count++].data()};
}

/// Where in the script the Lua code that called this binding sits.
///
/// Level 1 is the caller of the running C function. Nothing here uses the `debug` library, which is
/// deliberately not opened for scripts.
[[nodiscard]] SourceLocation CallerLocation(lua_State* state) {
    lua_Debug frame{};
    if (lua_getstack(state, 1, &frame) == 0 || lua_getinfo(state, "Sl", &frame) == 0) {
        return SourceLocation{"?", 0};
    }
    return SourceLocation{InternChunkName(frame.short_src),
                          frame.currentline > 0 ? static_cast<u32_t>(frame.currentline) : 0u};
}

// --- Handles ------------------------------------------------------------------------------------

/// What a script holds on to: one channel of one node.
///
/// A plain value with no back pointer. The graph is reachable from the bindings by capture, so a
/// handle stays 12 bytes and copying one is free, which is what makes the lazy API cheap.
struct Handle {
    Value value;
};

[[nodiscard]] const char* DomainName(Domain domain) { return domain == Domain::R3 ? "R3" : "R2"; }

// --- Named arguments ----------------------------------------------------------------------------

/// Reader for the named-table call convention, which rejects keys it was not asked for.
///
/// Silently ignoring an unknown key is the worst failure mode a named-parameter API has: a script
/// that says `persistance = 0.3` would run, look plausible and be wrong. Every accepted key is
/// recorded as it is read, and `Finish` reports anything left over together with the full list of
/// what the op does accept.
class Args {
public:
    Args(sol::table table, const char* op, SourceLocation location)
        : m_table(std::move(table)), m_op(op), m_location(location) {}

    [[nodiscard]] b8_t Has(const char* key) {
        Accept(key);
        return m_table[key].valid();
    }

    [[nodiscard]] f64_t Number(const char* key, f64_t fallback) {
        Accept(key);
        if (!m_table[key].valid()) {
            return fallback;
        }
        const sol::optional<f64_t> value = m_table[key];
        if (!value.has_value()) {
            WrongType(key, "a number");
        }
        return *value;
    }

    /// Reads a number that must lie in `[minimum, maximum]`, reporting the bound it broke.
    [[nodiscard]] f64_t Number(const char* key, f64_t fallback, f64_t minimum, f64_t maximum) {
        const f64_t value = Number(key, fallback);
        if (!(value >= minimum) || !(value <= maximum)) {
            Fail(m_location, "{} expects {} in [{}, {}], got {}", m_op, key, minimum, maximum,
                 value);
        }
        return value;
    }

    [[nodiscard]] f32_t Float(const char* key, f64_t fallback) {
        return static_cast<f32_t>(Number(key, fallback));
    }

    [[nodiscard]] u32_t Count(const char* key, u32_t fallback, u32_t minimum, u32_t maximum) {
        const f64_t value = Number(key, static_cast<f64_t>(fallback));
        const f64_t whole = std::floor(value);
        if (whole != value) {
            Fail(m_location, "{} expects {} to be a whole number, got {}", m_op, key, value);
        }
        if (whole < static_cast<f64_t>(minimum) || whole > static_cast<f64_t>(maximum)) {
            Fail(m_location, "{} expects {} in [{}, {}], got {}", m_op, key, minimum, maximum,
                 value);
        }
        return static_cast<u32_t>(whole);
    }

    [[nodiscard]] b8_t Boolean(const char* key, b8_t fallback) {
        Accept(key);
        if (!m_table[key].valid()) {
            return fallback;
        }
        const sol::optional<b8_t> value = m_table[key];
        if (!value.has_value()) {
            WrongType(key, "a boolean");
        }
        return *value;
    }

    [[nodiscard]] std::string Text(const char* key, const char* fallback) {
        Accept(key);
        if (!m_table[key].valid()) {
            return std::string{fallback};
        }
        const sol::optional<std::string> value = m_table[key];
        if (!value.has_value()) {
            WrongType(key, "a string");
        }
        return *value;
    }

    /// Reads a handle. Numbers are not accepted here: an op that takes a field needs a field, and
    /// saying so beats building a constant node the script never asked for.
    [[nodiscard]] Value Input(const char* key) {
        Accept(key);
        if (!m_table[key].valid()) {
            Fail(m_location, "{} needs a graph value as '{}'", m_op, key);
        }
        const sol::optional<Handle> handle = m_table[key];
        if (!handle.has_value()) {
            WrongType(key, "a graph value");
        }
        return handle->value;
    }

    [[nodiscard]] SourceLocation Location() const noexcept { return m_location; }

    /// Reports any key the op does not accept, listing the ones it does.
    void Finish() {
        for (const auto& entry : m_table) {
            const sol::optional<std::string> key = entry.first.as<sol::optional<std::string>>();
            if (!key.has_value() || IsAccepted(*key)) {
                continue;
            }
            Fail(m_location, "{} does not take '{}'; it accepts {}", m_op, *key, AcceptedList());
        }
    }

private:
    static constexpr usize_t kMaxKeys = 16;

    void Accept(const char* key) {
        if (m_acceptedCount < kMaxKeys) {
            m_accepted[m_acceptedCount++] = key;
        }
    }

    [[nodiscard]] b8_t IsAccepted(std::string_view key) const {
        for (usize_t i = 0; i < m_acceptedCount; ++i) {
            if (key == m_accepted[i]) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::string AcceptedList() const {
        std::string list;
        for (usize_t i = 0; i < m_acceptedCount; ++i) {
            if (i != 0) {
                list += ", ";
            }
            list += m_accepted[i];
        }
        return list;
    }

    /// A key that is present but of the wrong type. Worth its own message: the script did mean to
    /// set it, so naming the type it needs is more useful than saying it is missing.
    [[noreturn]] void WrongType(const char* key, const char* what) {
        const sol::object value = m_table[key];
        Fail(m_location, "{} expects '{}' to be {}, got {}", m_op, key, what,
             sol::type_name(value.lua_state(), value.get_type()));
    }

    sol::table                            m_table;
    const char*                           m_op;
    SourceLocation                        m_location;
    std::array<const char*, kMaxKeys>     m_accepted{};
    usize_t                               m_acceptedCount = 0;
};

// --- Shared coercions ---------------------------------------------------------------------------

/// The `R2 -> R2` gradient an op like `Terrain.Normals` needs, from whatever the script handed over.
///
/// A script that writes `Terrain.Normals{ input = base }` means the gradient of `base`, and an fBm
/// node already carries one on its second channel, so that case is resolved rather than refused.
/// Anything else is an error naming `.gradient`, because there is no gradient op for an arbitrary
/// field: computing one would mean finite differences, which this engine takes analytically or not
/// at all.
[[nodiscard]] Value GradientOf(const Graph& graph, Value value, const char* op,
                               SourceLocation location) {
    if (value.mapping.domain == Domain::R2 && value.mapping.components == 2) {
        return value;
    }
    if (value.mapping.domain == Domain::R2 && value.mapping.components == 1) {
        if (Result<Value> gradient = graph.Channel(value, 1);
            gradient && gradient->mapping.domain == Domain::R2 && gradient->mapping.components == 2) {
            return *gradient;
        }
    }
    Fail(location, "{} expects an R2->R2 gradient, got {}; a noise handle exposes one as .gradient",
         op, ToString(value.mapping));
}

/// Wraps a Lua number as a constant field matching `like`'s domain, so `h * 900.0` works.
[[nodiscard]] Value ConstantLike(Graph& graph, f64_t scalar, Value like, SourceLocation location) {
    return Unwrap(graph.AddConst(Mapping{like.mapping.domain, 1},
                                 {static_cast<f32_t>(scalar), 0.0f, 0.0f, 0.0f}, location));
}

/// One side of a binary operator: a handle stays as it is, a number becomes a constant field.
[[nodiscard]] Value Operand(Graph& graph, const sol::object& object, Value other, const char* op,
                            SourceLocation location) {
    if (object.is<Handle>()) {
        return object.as<Handle>().value;
    }
    if (object.is<f64_t>()) {
        return ConstantLike(graph, object.as<f64_t>(), other, location);
    }
    Fail(location, "{} expects a graph value or a number", op);
}

[[nodiscard]] Result<NoiseKind> ParseNoiseKind(std::string_view name) {
    if (name == "simplex") {
        return NoiseKind::Simplex;
    }
    if (name == "ridged") {
        return NoiseKind::Ridged;
    }
    if (name == "billow") {
        return NoiseKind::Billow;
    }
    ENGINE_FAIL(ErrorCode::ScriptError, ErrorStage::Script,
                "unknown noise kind '{}'; expected simplex, ridged or billow", name);
}

[[nodiscard]] Domain ParseDomain(Args& args) {
    const u32_t dimensions = args.Count("domain", 2, 2, 3);
    return dimensions == 3 ? Domain::R3 : Domain::R2;
}

} // namespace

// --- Bindings -----------------------------------------------------------------------------------

namespace {

/// Registers the handle type and every op namespace into `state`, building into `graph`.
///
/// The bindings capture the graph, which is what keeps `Handle` free of a back pointer and keeps the
/// whole API in one place: adding an op is one lambda here plus one builder on `Graph`.
void Bind(sol::state& state, Graph& graph) {
    // --- Handles --------------------------------------------------------------------------------
    //
    // `.value` and `.gradient` select a channel; `.x`, `.y`, `.z` and `.w` extract a component.
    // Both go through one index function because Lua gives field access a single hook, and because a
    // typo in either should produce the same kind of message.
    state.new_usertype<Handle>(
        "Value", sol::no_constructor, sol::meta_function::index,
        [&graph](sol::this_state lua, Handle& self, const std::string& key) -> sol::object {
            const SourceLocation location = CallerLocation(lua);
            const auto           wrap     = [&lua](Value value) {
                return sol::make_object(lua.lua_state(), Handle{value});
            };
            if (key == "value") {
                return wrap(Unwrap(graph.Channel(self.value, 0)));
            }
            if (key == "gradient") {
                Result<Value> gradient = graph.Channel(self.value, 1);
                if (!gradient) {
                    Fail(location, "this value has no .gradient channel");
                }
                return wrap(*gradient);
            }
            for (u8_t component = 0; component < 4; ++component) {
                if (key == std::string_view{"xyzw"}.substr(component, 1)) {
                    return wrap(Unwrap(graph.AddExtract(self.value, component, location)));
                }
            }
            Fail(location, "a graph value has no '{}'; it has .value, .gradient, .x, .y, .z and .w",
                 key);
        },
        sol::meta_function::to_string,
        [](const Handle& self) { return std::string{ToString(self.value.mapping)}; },
        sol::meta_function::addition,
        [&graph](sol::this_state lua, const sol::object& a, const sol::object& b) {
            const SourceLocation location = CallerLocation(lua);
            const Value          left     = Operand(graph, a, Value{}, "+", location);
            return Handle{Unwrap(graph.AddArith(ArithOp::Add, left,
                                                Operand(graph, b, left, "+", location), location))};
        },
        sol::meta_function::subtraction,
        [&graph](sol::this_state lua, const sol::object& a, const sol::object& b) {
            const SourceLocation location = CallerLocation(lua);
            const Value          left     = Operand(graph, a, Value{}, "-", location);
            return Handle{Unwrap(graph.AddArith(
                ArithOp::Subtract, left, Operand(graph, b, left, "-", location), location))};
        },
        sol::meta_function::multiplication,
        [&graph](sol::this_state lua, const sol::object& a, const sol::object& b) {
            const SourceLocation location = CallerLocation(lua);
            const Value          left     = Operand(graph, a, Value{}, "*", location);
            return Handle{Unwrap(graph.AddArith(
                ArithOp::Multiply, left, Operand(graph, b, left, "*", location), location))};
        },
        sol::meta_function::division,
        [&graph](sol::this_state lua, const sol::object& a, const sol::object& b) {
            const SourceLocation location = CallerLocation(lua);
            const Value          left     = Operand(graph, a, Value{}, "/", location);
            return Handle{Unwrap(graph.AddArith(
                ArithOp::Divide, left, Operand(graph, b, left, "/", location), location))};
        },
        sol::meta_function::unary_minus, [&graph](sol::this_state lua, const Handle& self) {
            const SourceLocation location = CallerLocation(lua);
            const Value          zero = ConstantLike(graph, 0.0, self.value, location);
            return Handle{
                Unwrap(graph.AddArith(ArithOp::Subtract, zero, self.value, location))};
        });

    // --- Noises ---------------------------------------------------------------------------------

    const auto noise = [&graph](sol::this_state lua, sol::table table, const char* op,
                                const char* fixedKind) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), op, location);

        Graph::NoiseParams params;
        params.domain = ParseDomain(args);
        if (fixedKind != nullptr) {
            Result<NoiseKind> kind = ParseNoiseKind(fixedKind);
            params.kind            = *kind;
        } else {
            const std::string  name = args.Text("kind", "simplex");
            Result<NoiseKind>  kind = ParseNoiseKind(name);
            if (!kind) {
                Fail(location, "{}", kind.error().Message());
            }
            params.kind = *kind;
        }
        // Strictly positive, not merely non-negative: a frequency, a persistence or a lacunarity of
        // zero collapses the octave sum instead of producing a degenerate but meaningful field, so
        // it is a mistake worth naming rather than a value worth honouring.
        params.frequency   = args.Float("frequency", 0.002);
        params.octaves     = args.Count("octaves", 6, 1, 32);
        params.lacunarity  = args.Float("lacunarity", 2.0);
        params.persistence = args.Float("persistence", 0.5);
        params.amplitude   = args.Float("amplitude", 1.0);
        params.offset      = args.Float("offset", 0.0);
        params.seedSalt    = args.Count("seed", 0, 0, ~0u);
        params.normalize   = args.Boolean("normalize", true);
        args.Finish();

        return Handle{Unwrap(graph.AddNoise(params, location))};
    };

    sol::table noises = state.create_named_table("Noises");
    noises.set_function("fBm", [noise](sol::this_state lua, sol::table table) {
        return noise(lua, std::move(table), "Noises.fBm", nullptr);
    });
    noises.set_function("Ridged", [noise](sol::this_state lua, sol::table table) {
        return noise(lua, std::move(table), "Noises.Ridged", "ridged");
    });
    noises.set_function("Billow", [noise](sol::this_state lua, sol::table table) {
        return noise(lua, std::move(table), "Noises.Billow", "billow");
    });

    // --- Terrain --------------------------------------------------------------------------------

    sol::table terrain = state.create_named_table("Terrain");
    terrain.set_function("Normals", [&graph](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Terrain.Normals", location);
        const Value          input    = args.Input("input");
        const f32_t          vertical = args.Float("vertical_scale", 1.0);
        args.Finish();
        const Value gradient = GradientOf(graph, input, "Terrain.Normals", location);
        return Handle{Unwrap(graph.AddNormals(gradient, vertical, location))};
    });
    terrain.set_function("Coords", [&graph](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Terrain.Coords", location);
        const Domain         domain = ParseDomain(args);
        args.Finish();
        return Handle{Unwrap(graph.AddCoords(domain, location))};
    });
    terrain.set_function("Const", [&graph](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Terrain.Const", location);
        const Domain         domain = ParseDomain(args);
        const f32_t          value  = args.Float("value", 0.0);
        args.Finish();
        return Handle{Unwrap(graph.AddConst(Mapping{domain, 1}, {value, 0.0f, 0.0f, 0.0f},
                                            location))};
    });

    // --- Masks ----------------------------------------------------------------------------------

    sol::table masks = state.create_named_table("Masks");
    masks.set_function("Slope", [&graph](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Masks.Slope", location);
        const Value          input   = args.Input("input");
        const f32_t          minimum = args.Float("min", 0.0);
        const f32_t          maximum = args.Float("max", 1.0);
        args.Finish();
        if (!(minimum < maximum)) {
            Fail(location, "Masks.Slope expects min < max, got min = {} and max = {}",
                 static_cast<f64_t>(minimum), static_cast<f64_t>(maximum));
        }
        const Value gradient = GradientOf(graph, input, "Masks.Slope", location);
        return Handle{Unwrap(graph.AddSlopeMask(gradient, minimum, maximum, location))};
    });

    // --- Combine --------------------------------------------------------------------------------

    sol::table combine = state.create_named_table("Combine");
    combine.set_function("Blend", [&graph](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Combine.Blend", location);
        const Value          a    = args.Input("a");
        const Value          b    = args.Input("b");
        const Value          mask = args.Input("mask");
        args.Finish();
        return Handle{Unwrap(graph.AddBlend(a, b, mask, location))};
    });
    const auto pair = [&graph](sol::this_state lua, sol::table table, const char* op, ArithOp op2) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), op, location);
        const Value          a = args.Input("a");
        const Value          b = args.Input("b");
        args.Finish();
        return Handle{Unwrap(graph.AddArith(op2, a, b, location))};
    };
    combine.set_function("Min", [pair](sol::this_state lua, sol::table table) {
        return pair(lua, std::move(table), "Combine.Min", ArithOp::Minimum);
    });
    combine.set_function("Max", [pair](sol::this_state lua, sol::table table) {
        return pair(lua, std::move(table), "Combine.Max", ArithOp::Maximum);
    });

    // --- Curves ---------------------------------------------------------------------------------
    //
    // All four take the same shape: one input and two parameters whose meaning depends on the curve,
    // which is why each one names its own keys instead of sharing a generic `a` and `b`.

    sol::table curves = state.create_named_table("Curves");
    curves.set_function("Clamp", [&graph](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Curves.Clamp", location);
        const Value          input   = args.Input("input");
        const f32_t          minimum = args.Float("min", 0.0);
        const f32_t          maximum = args.Float("max", 1.0);
        args.Finish();
        return Handle{Unwrap(graph.AddCurve(CurveOp::Clamp, input, minimum, maximum, location))};
    });
    curves.set_function("Remap", [&graph](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Curves.Remap", location);
        const Value          input   = args.Input("input");
        const f32_t          minimum = args.Float("min", 0.0);
        const f32_t          maximum = args.Float("max", 1.0);
        args.Finish();
        return Handle{Unwrap(graph.AddCurve(CurveOp::Remap, input, minimum, maximum, location))};
    });
    curves.set_function("Power", [&graph](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Curves.Power", location);
        const Value          input    = args.Input("input");
        const f32_t          exponent = static_cast<f32_t>(args.Number("exponent", 1.0, 0.0, 64.0));
        args.Finish();
        return Handle{Unwrap(graph.AddCurve(CurveOp::Power, input, exponent, 0.0f, location))};
    });
    curves.set_function("Smoothstep", [&graph](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Curves.Smoothstep", location);
        const Value          input   = args.Input("input");
        const f32_t          minimum = args.Float("min", 0.0);
        const f32_t          maximum = args.Float("max", 1.0);
        args.Finish();
        if (!(minimum < maximum)) {
            Fail(location, "Curves.Smoothstep expects min < max, got min = {} and max = {}",
                 static_cast<f64_t>(minimum), static_cast<f64_t>(maximum));
        }
        return Handle{
            Unwrap(graph.AddCurve(CurveOp::Smoothstep, input, minimum, maximum, location))};
    });

    // --- Filters --------------------------------------------------------------------------------

    sol::table filters = state.create_named_table("Filters");
    filters.set_function("Blur", [&graph](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Filters.Blur", location);
        const Value          input  = args.Input("input");
        const u32_t          radius = args.Count("radius", 1, 1, terrain::kMaxBlurRadius);
        args.Finish();
        return Handle{Unwrap(graph.AddBlur(input, radius, location))};
    });

    // --- Vec ------------------------------------------------------------------------------------

    sol::table vec = state.create_named_table("Vec");
    vec.set_function("Combine", [&graph](sol::this_state lua, const sol::table& table) {
        const SourceLocation                         location = CallerLocation(lua);
        std::array<Value, terrain::kMaxNodeChannels * 2> components{};
        u8_t                                         count = 0;
        for (u8_t i = 0; i < 4; ++i) {
            const sol::optional<Handle> handle = table[i + 1];
            if (!handle.has_value()) {
                break;
            }
            components[count++] = handle->value;
        }
        if (count < 2) {
            Fail(location, "Vec.Combine expects 2 to 4 scalar values in a list, got {}", count);
        }
        return Handle{Unwrap(graph.AddCombine(components.data(), count, location))};
    });
}

// --- The params table ---------------------------------------------------------------------------

/// Builds the `params` table `main` is called with.
///
/// User parameters arrive from the CLI as text. One that reads as a whole number is pushed as a
/// number, because `--param amplitude=900` means the number 900 to whoever typed it; anything else
/// stays a string, so `kind = "simplex"` survives.
[[nodiscard]] sol::table MakeParams(sol::state& state, const ScriptEnvironment& environment) {
    sol::table params = state.create_table();
    params["seed"]       = static_cast<f64_t>(environment.seed);
    params["resolution"] = environment.resolution;

    sol::table bounds = state.create_table();
    bounds["min"]     = state.create_table_with(1, environment.minX, 2, environment.minY, 3,
                                                environment.minZ);
    bounds["max"]     = state.create_table_with(1, environment.maxX, 2, environment.maxY, 3,
                                                environment.maxZ);
    bounds["min_x"] = environment.minX;
    bounds["min_y"] = environment.minY;
    bounds["min_z"] = environment.minZ;
    bounds["max_x"] = environment.maxX;
    bounds["max_y"] = environment.maxY;
    bounds["max_z"] = environment.maxZ;
    params["bounds"] = bounds;

    if (environment.params != nullptr) {
        for (usize_t i = 0; i < environment.params->Count(); ++i) {
            const std::string_view key  = environment.params->KeyAt(i);
            const std::string_view text = environment.params->ValueAt(i);
            char*                  end  = nullptr;
            const std::string      copy(text);
            const f64_t            number = std::strtod(copy.c_str(), &end);
            if (end != nullptr && *end == '\0' && !copy.empty()) {
                params[std::string(key)] = number;
            } else {
                params[std::string(key)] = copy;
            }
        }
    }
    return params;
}

// --- Output requests ----------------------------------------------------------------------------

/// Reads the table `main` returned into the graph's output requests.
///
/// The long form is `{ value = handle, range = { min, max } }`. A bare handle is accepted as a
/// shorthand, which is what a vector output wants anyway: its range is always [-1, 1].
[[nodiscard]] Status ReadOutputs(Graph& graph, const sol::object& returned,
                                 std::string_view chunkName) {
    const SourceLocation location{chunkName, 0};
    if (!returned.is<sol::table>()) {
        return std::unexpected(MakeScriptError(
            ErrorCode::ScriptError, ErrorStage::Script, location.file, location.line,
            "main must return a table of outputs, got {}",
            sol::type_name(returned.lua_state(), returned.get_type())));
    }

    // Collected, then requested in name order. A Lua table has no iteration order, and the request
    // order reaches the buffer planner and the sidecar, so leaving it to the interpreter would make
    // two runs of the same script differ in ways that are tedious to diff.
    struct Collected {
        std::string name;
        Value       value;
        f32_t       rangeMin;
        f32_t       rangeMax;
    };
    std::array<Collected, Graph::kMaxOutputs> collected{};
    usize_t                                   count = 0;

    const sol::table table = returned.as<sol::table>();
    for (const auto& entry : table) {
        const sol::optional<std::string> name = entry.first.as<sol::optional<std::string>>();
        if (!name.has_value()) {
            return std::unexpected(MakeScriptError(ErrorCode::ScriptError, ErrorStage::Script,
                                                   location.file, location.line,
                                                   "every output must be named"));
        }

        Value value{};
        f32_t rangeMin = -1.0f;
        f32_t rangeMax = 1.0f;
        if (entry.second.is<Handle>()) {
            value = entry.second.as<Handle>().value;
        } else if (entry.second.is<sol::table>()) {
            const sol::table           description = entry.second.as<sol::table>();
            const sol::optional<Handle> handle     = description["value"];
            if (!handle.has_value()) {
                return std::unexpected(MakeScriptError(
                    ErrorCode::ScriptError, ErrorStage::Script, location.file, location.line,
                    "output '{}' has no value; expected {{ value = <graph value>, range = {{ min, "
                    "max }} }}",
                    *name));
            }
            value = handle->value;
            if (const sol::optional<sol::table> range = description["range"]; range.has_value()) {
                const sol::optional<f64_t> low  = (*range)[1];
                const sol::optional<f64_t> high = (*range)[2];
                if (!low.has_value() || !high.has_value()) {
                    return std::unexpected(MakeScriptError(
                        ErrorCode::ScriptError, ErrorStage::Script, location.file, location.line,
                        "output '{}' has a range that is not a pair of numbers", *name));
                }
                rangeMin = static_cast<f32_t>(*low);
                rangeMax = static_cast<f32_t>(*high);
            } else if (value.mapping.components == 1) {
                return std::unexpected(MakeScriptError(
                    ErrorCode::ScriptError, ErrorStage::Script, location.file, location.line,
                    "output '{}' is {} and needs a range to normalize to 16 bits", *name,
                    ToString(value.mapping)));
            }
        } else {
            return std::unexpected(MakeScriptError(
                ErrorCode::ScriptError, ErrorStage::Script, location.file, location.line,
                "output '{}' must be a graph value or a table describing one", *name));
        }

        if (count == Graph::kMaxOutputs) {
            return std::unexpected(MakeScriptError(
                ErrorCode::ScriptError, ErrorStage::Script, location.file, location.line,
                "main returned more than {} outputs", Graph::kMaxOutputs));
        }
        collected[count++] = Collected{*name, value, rangeMin, rangeMax};
    }

    std::sort(collected.begin(), collected.begin() + static_cast<std::ptrdiff_t>(count),
              [](const Collected& a, const Collected& b) { return a.name < b.name; });
    for (usize_t i = 0; i < count; ++i) {
        if (Status requested = graph.RequestOutput(collected[i].name, collected[i].value,
                                                  collected[i].rangeMin, collected[i].rangeMax);
            !requested) {
            return requested;
        }
    }

    if (count == 0) {
        return std::unexpected(MakeScriptError(ErrorCode::ScriptError, ErrorStage::Script,
                                               location.file, location.line,
                                               "main returned no outputs"));
    }
    return {};
}

/// Lua calls this when an error escapes an unprotected call, which cannot be recovered from.
///
/// Every call this file makes into Lua is protected, so reaching here means a bug rather than a bad
/// script. It logs and aborts instead of throwing, because a panic handler returns into C.
int AtPanic(lua_State* state) noexcept {
    const char* message = lua_tostring(state, -1);
    LOG_FATAL("the Lua runtime panicked: {}", message != nullptr ? message : "no message");
    std::abort();
}

/// Turns whatever came out of a failed protected call into an engine error.
///
/// A binding that failed on purpose left the structured error behind, complete with the script line.
/// Lua's own failures (a syntax error, arithmetic on a nil) only have the string, which already
/// starts with `chunk:line:`, so it is passed through rather than reformatted.
[[nodiscard]] Error ErrorFrom(const sol::protected_function_result& result,
                              std::string_view chunkName, const char* what) {
    if (t_scriptError.has_value()) {
        const Error error = *t_scriptError;
        t_scriptError.reset();
        return error;
    }
    std::string message = "no message";
    if (!result.valid()) {
        const sol::error error = result;
        message               = error.what();
    }
    return MakeScriptError(ErrorCode::ScriptError, ErrorStage::Script, chunkName, 0, "{}: {}", what,
                           message);
}

} // namespace

Status RunScript(Graph& graph, std::string_view path, const ScriptEnvironment& environment) {
#ifdef TRACY_ENABLE
    ZoneScopedN("lua::RunScript");
#endif
    graph.Clear();
    t_scriptError.reset();

    // Every byte the interpreter touches comes from the `CPU/Lua` allocator, so the Tracy memory
    // view shows the script's cost as its own pool (spec section 7.4).
    sol::state state(AtPanic, memory::LuaAlloc, &memory::Lua());
    // Deliberately narrow: a terrain script needs arithmetic, strings and tables. `io`, `os` and
    // `package` are not opened, so a script cannot read files, start processes or pull in C modules.
    state.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table);

    Bind(state, graph);

    const std::string          file  = std::string(path);
    sol::protected_function_result loaded = state.safe_script_file(
        file, sol::script_pass_on_error);
    if (!loaded.valid()) {
        return std::unexpected(ErrorFrom(loaded, path, "could not load the script"));
    }

    // A script either returns `main` or leaves it as a global. Both forms appear in the spec's own
    // examples, so both are accepted.
    sol::protected_function main = loaded.get_type() == sol::type::function
                                       ? loaded.get<sol::protected_function>()
                                       : state["main"];
    if (!main.valid()) {
        return std::unexpected(MakeScriptError(ErrorCode::ScriptError, ErrorStage::Script, path, 0,
                                               "the script defines no main(params)"));
    }

    const sol::protected_function_result returned = main(MakeParams(state, environment));
    if (!returned.valid()) {
        return std::unexpected(ErrorFrom(returned, path, "main failed"));
    }

    if (Status outputs = ReadOutputs(graph, returned, path); !outputs) {
        return outputs;
    }

    LOG_INFO("script {} built {} node(s) and {} output(s), {} KiB of Lua state", path,
             graph.NodeCount(), graph.OutputCount(), AllocatedBytes() / 1024);
    return {};
}

u64_t AllocatedBytes() { return memory::Lua().Stats().used; }

} // namespace engine::lua
