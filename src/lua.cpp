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
#include <engine/platform.hpp>
#include <engine/terrain/graph.hpp>
#include <engine/terrain/kernels.hpp>
#include <engine/terrain/sha256.hpp>

#include <sol/sol.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
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
using terrain::OpKind;
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
    // Sixty-four, not sixteen. The table is never emptied, because a `SourceLocation` taken from it can
    // outlive the state that produced it: the viewer keeps a compiled graph rendering while it compiles
    // a new one, so the old graph's nodes still point here. That means the table fills with every
    // *distinct* script a process loads, and at sixteen the Lua error tests ran it dry — scripts after
    // the sixteenth reported their errors against `?` instead of their own file name, silently.
    //
    // Sixty-four distinct scripts in one process is past any real use, and if it is ever reached the
    // warning below says so once rather than degrading in silence.
    static constexpr usize_t kMaxNames  = 64;
    static constexpr usize_t kMaxLength = 128;
    static std::array<std::array<char, kMaxLength>, kMaxNames> names{};
    static usize_t                                            count = 0;
    static b8_t                                               warned = false;
    static std::mutex                                         mutex;

    const std::string_view      incoming = name != nullptr ? std::string_view{name} : "?";
    std::lock_guard<std::mutex> lock(mutex);
    for (usize_t i = 0; i < count; ++i) {
        if (std::string_view{names[i].data()} == incoming) {
            return std::string_view{names[i].data()};
        }
    }
    if (count == kMaxNames) {
        if (!warned) {
            warned = true;
            LOG_WARN("more than {} distinct scripts have been loaded; further errors will name their "
                     "file as '?'",
                     kMaxNames);
        }
        return "?";
    }
    detail::CopyBounded(names[count], incoming);
    return std::string_view{names[count++].data()};
}

/// Where in the script the Lua code that called this binding sits.
///
/// Level 1 is the caller of the running C function. Frames of the prelude are skipped: when
/// `Landforms.Cone` calls `Geometry.Circle` and the radius is wrong, the line that is wrong is the one
/// in the script that called `Landforms.Cone`, not a line of the engine's own Lua. Nothing here uses
/// the `debug` library, which is deliberately not opened for scripts.
[[nodiscard]] SourceLocation CallerLocation(lua_State* state) {
    SourceLocation found{"?", 0};
    lua_Debug      frame{};
    for (int level = 1; lua_getstack(state, level, &frame) != 0; ++level) {
        if (lua_getinfo(state, "Sl", &frame) == 0) {
            break;
        }
        found = SourceLocation{InternChunkName(frame.short_src),
                               frame.currentline > 0 ? static_cast<u32_t>(frame.currentline) : 0u};
        if (std::strncmp(frame.short_src, "prelude/", 8) != 0) {
            return found;
        }
    }
    return found;
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

    /// A pair `{ x, z }`. With `broadcast` a bare number stands for both, so `scale = 2` works.
    [[nodiscard]] std::array<f64_t, 2> Pair(const char* key, std::array<f64_t, 2> fallback,
                                            b8_t broadcast = false) {
        Accept(key);
        if (!m_table[key].valid()) {
            return fallback;
        }
        if (broadcast) {
            if (const sol::optional<f64_t> number = m_table[key]; number.has_value()) {
                return {*number, *number};
            }
        }
        const sol::optional<sol::table> table = m_table[key];
        if (!table.has_value()) {
            WrongType(key, broadcast ? "a number or a pair { x, z }" : "a pair { x, z }");
        }
        return ReadPair(*table, key);
    }

    /// Most points a list may hold: a closed polygon of `kMaxShapeSegments` segments.
    static constexpr usize_t kMaxPoints = terrain::kMaxShapeSegments + 1;

    /// A list of `{ x, z }` points, written into `out`. Returns how many there were.
    [[nodiscard]] usize_t Points(const char* key, std::array<std::array<f64_t, 2>, kMaxPoints>& out) {
        Accept(key);
        if (!m_table[key].valid()) {
            Fail(m_location, "{} needs a list of points as '{}'", m_op, key);
        }
        const sol::optional<sol::table> list = m_table[key];
        if (!list.has_value()) {
            WrongType(key, "a list of { x, z } points");
        }
        usize_t count = 0;
        for (usize_t i = 1;; ++i) {
            const sol::object entry = (*list)[i];
            if (!entry.valid() || entry.get_type() == sol::type::lua_nil) {
                break;
            }
            if (count == out.size()) {
                Fail(m_location, "{} takes at most {} points in '{}'", m_op, out.size(), key);
            }
            if (entry.get_type() != sol::type::table) {
                Fail(m_location, "{} expects point {} of '{}' to be a pair {{ x, z }}", m_op, i, key);
            }
            out[count++] = ReadPair(entry.as<sol::table>(), key);
        }
        return count;
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

    [[nodiscard]] std::array<f64_t, 2> ReadPair(const sol::table& table, const char* key) {
        const sol::optional<f64_t> first  = table[1];
        const sol::optional<f64_t> second = table[2];
        if (!first.has_value() || !second.has_value() || table[3].valid()) {
            Fail(m_location, "{} expects '{}' to be a pair of numbers {{ x, z }}", m_op, key);
        }
        if (!std::isfinite(*first) || !std::isfinite(*second)) {
            Fail(m_location, "{} expects '{}' to hold finite numbers", m_op, key);
        }
        return {*first, *second};
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
    Fail(location,
         "{} expects an R2->R2 gradient, got {}; a noise handle exposes an exact one as .gradient, and "
         "Terrain.Gradient measures one from any height field",
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

// --- Erosion and the sample spacing ---------------------------------------------------------------
//
// One iteration of either erosion moves material between neighbouring cells, so a count of iterations
// is a distance in cells and, because the scheme is explicit, an amount of diffusion that grows with
// the *cells* it is spread over. Run unchanged at 4 m and at 8 m the same script erodes by different
// amounts: measured on a 2 km region, 8 m against 16 m differed by 1.5 m on average for thermal erosion
// and 10.8 m for hydraulic erosion alone, and a viewer that changes level as the camera moves showed it
// as terrain that re-eroded on every zoom.
//
// The counts a script writes are therefore *at a reference spacing*, and the engine runs the count that
// gives the same amount of erosion at the spacing in use:
//
//   * iterations scale as (reference / spacing)^2. An explicit diffusion step is a fixed fraction of the
//     height difference to the neighbours, and diffusing the same physical length takes four times the
//     iterations at half the cell size. Thermal erosion: 4 m against 8 m went from 0.79 m to 0.05 m of
//     difference.
//   * hydraulic erosion's per-iteration rates (rain, evaporation, erosion, deposition) scale as
//     (spacing / reference), so the totals over the run are those of the reference. Counting iterations
//     alone made hydraulic erosion worse; with both, 4 m, 8 m and 16 m on one terrain came within
//     3.8, 4.1 and 5.1 m of each other, against 6.7, 10.8 and 16.0 m.
//
// It is an approximation, and says so in the log. A finer spacing than the reference needs more
// iterations than the halo can carry beyond `kScaledIterationCap`, and a coarser one runs out of
// iterations at one.
constexpr f64_t kErosionReferenceSpacing = 2.0;
constexpr u32_t kScaledIterationCap      = 96;

struct ErosionScale {
    u32_t iterations = 1;
    f64_t rate       = 1.0;
};

[[nodiscard]] ErosionScale ScaleErosion(u32_t written, f64_t spacing, const char* op) {
    const f64_t k = spacing > 0.0 ? spacing / kErosionReferenceSpacing : 1.0;
    if (k == 1.0) {
        return {written, 1.0};
    }
    f64_t wanted = static_cast<f64_t>(written) / (k * k);
    // Never cut a script's own count below what it wrote on the way up, and never ask the halo for more
    // than the cap unless the script already did.
    const f64_t cap = static_cast<f64_t>(std::max(written, kScaledIterationCap));
    const b8_t  limited = wanted > cap;
    wanted = std::min(wanted, cap);
    const u32_t run = static_cast<u32_t>(std::max(1.0, std::floor(wanted + 0.5)));
    LOG_INFO("{} at {:.4g} m: {} iteration(s) written for {:.4g} m run as {}{}", op, spacing, written,
             kErosionReferenceSpacing, run,
             limited ? ", the most the halo is allowed to carry; the result is less eroded than the "
                       "reference"
                     : "");
    return {run, k};
}

/// The position field a shape is evaluated at: the one the script passed as `position`, or the
/// world position in metres. Passing `Coords + noise` is how a shape is warped.
[[nodiscard]] Value PositionOrCoords(Args& args, Graph& graph, SourceLocation location) {
    if (args.Has("position")) {
        return args.Input("position");
    }
    return Unwrap(graph.AddCoords(Domain::R2, location));
}

/// The matrix of a `Transform` node, from where the script wants the shape.
///
/// The shape sits at `at` after being scaled and rotated about `pivot` (a point of its own local
/// space), and rotating by a positive angle turns +x towards +z. The kernel wants the inverse of
/// that, world to local: `local = S^-1 R^-1 (world - at) + pivot`. Worked out in double and rounded
/// once, so a rotation and its inverse do not accumulate float error.
[[nodiscard]] std::array<f32_t, 6> PlacementToLocal(std::array<f64_t, 2> at, f64_t degrees,
                                                    std::array<f64_t, 2> scale,
                                                    std::array<f64_t, 2> pivot,
                                                    SourceLocation       location) {
    if (!std::isfinite(degrees)) {
        Fail(location, "Terrain.Transform expects a finite rotation, got {}", degrees);
    }
    if (scale[0] == 0.0 || scale[1] == 0.0) {
        Fail(location,
             "Terrain.Transform expects a scale other than zero on both axes, got {} and {}; a "
             "zero scale has no inverse",
             scale[0], scale[1]);
    }
    constexpr f64_t kPi     = 3.14159265358979323846;
    const f64_t     radians = degrees * kPi / 180.0;
    const f64_t     c       = std::cos(radians);
    const f64_t     s       = std::sin(radians);
    const f64_t     m00     = c / scale[0];
    const f64_t     m01     = s / scale[0];
    const f64_t     m10     = -s / scale[1];
    const f64_t     m11     = c / scale[1];
    return {static_cast<f32_t>(m00),
            static_cast<f32_t>(m01),
            static_cast<f32_t>(m10),
            static_cast<f32_t>(m11),
            static_cast<f32_t>(pivot[0] - (m00 * at[0] + m01 * at[1])),
            static_cast<f32_t>(pivot[1] - (m10 * at[0] + m11 * at[1]))};
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
void Bind(sol::state& state, Graph& graph, f64_t spacing) {
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

            // Channel names come from the op registry rather than from a list here. The bindings used to
            // know `value` and `gradient` and nothing else, which meant an op whose second channel is
            // not a gradient could not be named at all, and `.gradient` would have been accepted on
            // something that produces water and sediment.
            const OpKind kind = graph.NodeAt(self.value.node).kind;
            if (const u8_t channel = terrain::ChannelIndexOf(kind, key);
                channel < terrain::kMaxNodeChannels) {
                Result<Value> selected = graph.Channel(self.value, channel);
                if (!selected) {
                    Fail(location, "{} has no '{}' channel here; it offers {}", ToString(kind), key,
                         terrain::ChannelNamesOf(kind).data());
                }
                return wrap(*selected);
            }

            // A two-component value over `R2` is `(x, z)`: the plane's own axes, since the height lives
            // in a third dimension that is not there. So `.z` on one means its second component, which
            // is what every script that wrote `Terrain.Coords{ domain = 2 }.z` meant. Accepted rather
            // than refused with "component 2 was requested", an error about indices for what was a
            // question about an axis. `.y` keeps meaning component 1, so both spellings work.
            //
            // Not extended to `R2 -> R3`: there `.z` is a third component, and a normal's `z` is not
            // the plane's z axis.
            const terrain::Mapping mapping = self.value.mapping;
            if (key == "z" && mapping.domain == terrain::Domain::R2 && mapping.components == 2) {
                return wrap(Unwrap(graph.AddExtract(self.value, 1, location)));
            }
            for (u8_t component = 0; component < 4; ++component) {
                if (key == std::string_view{"xyzw"}.substr(component, 1)) {
                    return wrap(Unwrap(graph.AddExtract(self.value, component, location)));
                }
            }

            Fail(location, "a {} value has no '{}'; it offers {} and the components .x, .y, .z, .w",
                 ToString(kind), key, terrain::ChannelNamesOf(kind).data());
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
    terrain.set_function("Gradient", [&graph](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Terrain.Gradient", location);
        const Value          input = args.Input("input");
        args.Finish();
        return Handle{Unwrap(graph.AddGradient(input, location))};
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

    // Places a position field. `at`, `rotation`, `scale` and `pivot` describe where the *shape* ends up,
    // and the node is built from the inverse, because the kernel maps world positions to the shape's
    // local space: a shape moved right is the same shape evaluated at positions moved left.
    terrain.set_function("Transform", [&graph](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Terrain.Transform", location);
        const Value          position = PositionOrCoords(args, graph, location);
        const auto           at       = args.Pair("at", {0.0, 0.0});
        const f64_t          degrees  = args.Number("rotation", 0.0);
        const auto           scale    = args.Pair("scale", {1.0, 1.0}, true);
        const auto           pivot    = args.Pair("pivot", {0.0, 0.0});
        args.Finish();
        return Handle{Unwrap(graph.AddTransform(
            position, PlacementToLocal(at, degrees, scale, pivot, location), location))};
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

    // The rounded versions. `k` is the width of the blend in the units of the operands: where the two
    // differ by more than `k` the result is exactly Min or Max, and inside it the corner is rounded,
    // by up to a quarter of `k`. Both are symmetric in `a` and `b`.
    const auto smoothPair = [&graph](sol::this_state lua, sol::table table, const char* op,
                                     ArithOp kind) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), op, location);
        const Value          a = args.Input("a");
        const Value          b = args.Input("b");
        const f32_t          k = args.Float("k", 1.0);
        args.Finish();
        if (!(k > 0.0f)) {
            Fail(location, "{} expects k greater than zero, got {}", op, static_cast<f64_t>(k));
        }
        return Handle{Unwrap(graph.AddArith(kind, a, b, location, k))};
    };
    combine.set_function("SmoothMin", [smoothPair](sol::this_state lua, sol::table table) {
        return smoothPair(lua, std::move(table), "Combine.SmoothMin", ArithOp::SmoothMinimum);
    });
    combine.set_function("SmoothMax", [smoothPair](sol::this_state lua, sol::table table) {
        return smoothPair(lua, std::move(table), "Combine.SmoothMax", ArithOp::SmoothMaximum);
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

    // A profile turns a signed distance into a factor in [0, 1], 0 outside and 1 inside; multiplied by a
    // height it is a landform. The shape sets where the edge is and the profile sets how the ground
    // leaves it, which is why the two are separate.
    curves.set_function("Profile", [&graph](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Curves.Profile", location);
        const Value          input = args.Input("input");
        const std::string    kind  = args.Text("kind", "smooth");
        const b8_t           hasWidth   = args.Has("width");
        const b8_t           hasSigma   = args.Has("sigma");
        const b8_t           hasEdge    = args.Has("edge");
        const b8_t           hasFalloff = args.Has("falloff");
        const f32_t          width      = args.Float("width", 1.0);
        const f32_t          sigma      = args.Float("sigma", 1.0);
        const f32_t          edge       = args.Float("edge", 0.0);
        const f32_t          falloff    = args.Float("falloff", 1.0);
        args.Finish();

        CurveOp op = CurveOp::ProfileSmooth;
        if (kind == "linear") {
            op = CurveOp::ProfileLinear;
        } else if (kind == "smooth") {
            op = CurveOp::ProfileSmooth;
        } else if (kind == "dome") {
            op = CurveOp::ProfileDome;
        } else if (kind == "gaussian") {
            op = CurveOp::ProfileGaussian;
        } else if (kind == "plateau") {
            op = CurveOp::ProfilePlateau;
        } else {
            Fail(location,
                 "Curves.Profile does not know the kind '{}'; expected linear, smooth, dome, "
                 "gaussian or plateau",
                 kind);
        }

        // The keys that mean something depend on the kind, and one that does not would be read by
        // nothing: say so rather than let `sigma` on a linear profile be silently ignored.
        const b8_t needsWidth = op == CurveOp::ProfileLinear || op == CurveOp::ProfileSmooth
                                || op == CurveOp::ProfileDome;
        const b8_t needsSigma   = op == CurveOp::ProfileGaussian;
        const b8_t needsPlateau = op == CurveOp::ProfilePlateau;
        if (hasWidth && !needsWidth) {
            Fail(location, "Curves.Profile kind '{}' does not take 'width'", kind);
        }
        if (hasSigma && !needsSigma) {
            Fail(location, "Curves.Profile kind '{}' does not take 'sigma'; it is for gaussian", kind);
        }
        if ((hasEdge || hasFalloff) && !needsPlateau) {
            Fail(location, "Curves.Profile kind '{}' does not take 'edge' or 'falloff'; they are for "
                           "plateau",
                 kind);
        }
        if (needsWidth && !hasWidth) {
            Fail(location, "Curves.Profile kind '{}' needs 'width', in metres", kind);
        }
        if (needsSigma && !hasSigma) {
            Fail(location, "Curves.Profile kind 'gaussian' needs 'sigma', in metres");
        }
        if (needsPlateau && !hasFalloff) {
            Fail(location, "Curves.Profile kind 'plateau' needs 'falloff', in metres");
        }

        const f32_t a = needsWidth ? width : (needsSigma ? sigma : edge);
        const f32_t b = needsPlateau ? falloff : 0.0f;
        return Handle{Unwrap(graph.AddCurve(op, input, a, b, location))};
    });

    // Terraces take a height in metres, not a distance.
    curves.set_function("Terrace", [&graph](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Curves.Terrace", location);
        const Value          input     = args.Input("input");
        const f32_t          step      = args.Float("step", 10.0);
        const f32_t          sharpness = args.Float("sharpness", 0.5);
        args.Finish();
        if (!(step > 0.0f)) {
            Fail(location, "Curves.Terrace expects step greater than zero, got {}",
                 static_cast<f64_t>(step));
        }
        if (!(sharpness >= 0.0f && sharpness <= 1.0f)) {
            Fail(location, "Curves.Terrace expects sharpness in [0, 1], got {}",
                 static_cast<f64_t>(sharpness));
        }
        return Handle{Unwrap(graph.AddCurve(CurveOp::Terrace, input, step, sharpness, location))};
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

    // --- Erosion --------------------------------------------------------------------------------
    //
    // The first iterative op. `iterations` is not just a quality knob: it is the influence radius, so
    // it is also the halo every section has to compute beyond its own interior, and the binding says so
    // when a script asks for more than the interface can carry.

    sol::table erosion = state.create_named_table("Erosion");
    erosion.set_function("Thermal", [&graph, spacing](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Erosion.Thermal", location);
        const Value          input = args.Input("input");

        Graph::ThermalParams params;
        params.iterations = args.Count("iterations", 16, 1, terrain::kMaxHalo);
        params.talus      = args.Float("talus", 0.7);
        params.strength   = args.Float("strength", 0.25);
        args.Finish();
        params.iterations = ScaleErosion(params.iterations, spacing, "Erosion.Thermal").iterations;

        return Handle{Unwrap(graph.AddThermalErosion(input, params, location))};
    });

    erosion.set_function("Hydraulic", [&graph, spacing](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Erosion.Hydraulic", location);
        const Value          input = args.Input("input");

        Graph::HydraulicParams params;
        params.iterations  = args.Count("iterations", 32, 1, terrain::kMaxHalo);
        params.rain        = args.Float("rain", 0.02);
        params.evaporation = args.Float("evaporation", 0.05);
        params.capacity    = args.Float("capacity", 4.0);
        params.erosionRate = args.Float("erosion_rate", 0.3);
        params.deposition  = args.Float("deposition", 0.3);
        params.flowRate    = args.Float("flow_rate", 0.15);
        args.Finish();

        // Validated as written, so a bad value is reported as the script wrote it and not as whatever
        // scaling turned it into. The same bounds `Graph::AddHydraulicErosion` enforces.
        const auto within = [&](const char* key, f32_t value, f32_t low, f32_t high) {
            if (!(value >= low) || !(value <= high)) {
                Fail(location, "Erosion.Hydraulic expects {} in [{}, {}], got {}", key,
                     static_cast<f64_t>(low), static_cast<f64_t>(high), static_cast<f64_t>(value));
            }
        };
        within("rain", params.rain, 0.0f, 1e6f);
        within("evaporation", params.evaporation, 0.0f, 0.999f);
        within("erosion_rate", params.erosionRate, 0.0f, 1.0f);
        within("deposition", params.deposition, 0.0f, 1.0f);
        const ErosionScale scale = ScaleErosion(params.iterations, spacing, "Erosion.Hydraulic");
        params.iterations        = scale.iterations;
        params.rain              = static_cast<f32_t>(params.rain * scale.rate);
        params.evaporation       = static_cast<f32_t>(std::min(params.evaporation * scale.rate, 0.999));
        params.erosionRate       = static_cast<f32_t>(std::min(params.erosionRate * scale.rate, 1.0));
        params.deposition        = static_cast<f32_t>(std::min(params.deposition * scale.rate, 1.0));

        return Handle{Unwrap(graph.AddHydraulicErosion(input, params, location))};
    });

    // --- Geometry -------------------------------------------------------------------------------
    //
    // Signed distances, in metres and negative inside, in the shape's own local space: centred on the
    // origin and unrotated. `position` is the field they are evaluated at and defaults to the world
    // position; `Terrain.Transform` is what places one, and `Coords + noise` is what warps one. The
    // prelude's `Shapes.*` does the placing, so these are the building blocks rather than the API most
    // scripts reach for.

    sol::table geometry = state.create_named_table("Geometry");

    const auto primitive = [&graph](const char* name, terrain::ShapeKind kind) {
        return [&graph, name, kind](sol::this_state lua, sol::table table) {
            const SourceLocation location = CallerLocation(lua);
            Args                 args(std::move(table), name, location);
            const Value          position = PositionOrCoords(args, graph, location);
            Graph::ShapeParams   params;
            switch (kind) {
                case terrain::ShapeKind::Circle:
                    params.values[0] = args.Float("radius", 1.0);
                    break;
                case terrain::ShapeKind::Ellipse: {
                    const auto radii = args.Pair("radii", {1.0, 1.0});
                    params.values[0] = static_cast<f32_t>(radii[0]);
                    params.values[1] = static_cast<f32_t>(radii[1]);
                    break;
                }
                case terrain::ShapeKind::Box:
                case terrain::ShapeKind::BoxChebyshev: {
                    // The full size on each axis, which is what a person means by "a 400 by 200 box".
                    const auto size  = args.Pair("size", {1.0, 1.0});
                    params.values[0] = static_cast<f32_t>(size[0] * 0.5);
                    params.values[1] = static_cast<f32_t>(size[1] * 0.5);
                    if (kind == terrain::ShapeKind::Box) {
                        params.values[2] = args.Float("rounding", 0.0);
                    }
                    break;
                }
                default:
                    break;
            }
            args.Finish();
            return Handle{
                Unwrap(graph.AddShape(kind, position, Value{}, params, location))};
        };
    };
    geometry.set_function("Circle", primitive("Geometry.Circle", terrain::ShapeKind::Circle));
    geometry.set_function("Ellipse", primitive("Geometry.Ellipse", terrain::ShapeKind::Ellipse));
    geometry.set_function("Box", primitive("Geometry.Box", terrain::ShapeKind::Box));
    geometry.set_function("BoxChebyshev",
                          primitive("Geometry.BoxChebyshev", terrain::ShapeKind::BoxChebyshev));

    // A polygon or a polyline from a list of points. The chain is built here, in C++, so the parity
    // rule and the segment limit live in one place and a script cannot build a chain that is half of
    // one.
    geometry.set_function("Segments", [&graph](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Geometry.Segments", location);
        const Value          position = PositionOrCoords(args, graph, location);
        std::array<std::array<f64_t, 2>, Args::kMaxPoints> points{};
        const usize_t        count  = args.Points("points", points);
        const b8_t           closed = args.Boolean("closed", true);
        args.Finish();

        if (closed && count < 3) {
            Fail(location, "Geometry.Segments needs at least 3 points for a closed polygon, got {}",
                 count);
        }
        if (!closed && count < 2) {
            Fail(location, "Geometry.Segments needs at least 2 points for a polyline, got {}", count);
        }
        const usize_t segments = closed ? count : count - 1;
        if (segments > terrain::kMaxShapeSegments) {
            Fail(location,
                 "Geometry.Segments takes at most {} segments, got {}; each is a pass over every "
                 "sample, so a long outline is better described with fewer points",
                 terrain::kMaxShapeSegments, segments);
        }

        const auto segmentAt = [&](usize_t i, Graph::ShapeParams& params, usize_t slot) {
            const auto& a = points[i];
            const auto& b = points[(i + 1) % count];
            params.values[slot * 4 + 0] = static_cast<f32_t>(a[0]);
            params.values[slot * 4 + 1] = static_cast<f32_t>(a[1]);
            params.values[slot * 4 + 2] = static_cast<f32_t>(b[0]);
            params.values[slot * 4 + 3] = static_cast<f32_t>(b[1]);
        };

        Value chain{};
        for (usize_t i = 0; i < segments; i += 2) {
            Graph::ShapeParams params;
            params.segments = static_cast<u32_t>(std::min<usize_t>(2, segments - i));
            segmentAt(i, params, 0);
            if (params.segments == 2) {
                segmentAt(i + 1, params, 1);
            }
            chain = Unwrap(graph.AddShape(i == 0 ? terrain::ShapeKind::SegmentsFirst
                                                 : terrain::ShapeKind::SegmentsNext,
                                          position, chain, params, location));
        }
        return Handle{Unwrap(graph.AddShape(closed ? terrain::ShapeKind::ResolvePolygon
                                                   : terrain::ShapeKind::ResolvePolyline,
                                            Value{}, chain, Graph::ShapeParams{}, location))};
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

// --- The prelude --------------------------------------------------------------------------------
//
// Lua the engine runs before the script: `Shapes.*` and `Landforms.*`, which are compositions of the
// ops rather than ops themselves. They are Lua because that is the language a script author reads and
// edits, and because a landform is five lines of calls, not a kernel.
//
// A closed list of files next to the executable, in load order, rather than a directory scan: what the
// engine runs before a script is part of what a terrain *is*, so it is named, hashed into the sidecar
// and never picked up from wherever a path happens to lead.

constexpr std::array<const char*, 2> kPreludeFiles{"10_shapes.lua", "20_landforms.lua"};

[[nodiscard]] std::string PreludePath(const char* name) {
    const std::string_view root = platform::ExecutableDirectory();
    return std::string(root) + "lua/prelude/" + name;
}

[[nodiscard]] Result<std::string> ReadPrelude(const char* name) {
    const std::string path = PreludePath(name);
    std::ifstream     stream(path, std::ios::binary);
    if (!stream) {
        ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Script,
                    "could not open the engine prelude {}; it is copied next to the executable by the "
                    "build",
                    path);
    }
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

/// Runs the prelude in `state`. Each file is called with one argument, a table holding what only the
/// engine may reach: `fail(message)`, which raises a script error at the line of the script that called
/// into the prelude. It is passed rather than made global so a script cannot call it.
[[nodiscard]] Status LoadPrelude(sol::state& state) {
    sol::table engineApi = state.create_table();
    engineApi.set_function("fail", [](sol::this_state lua, const std::string& message) {
        Fail(CallerLocation(lua), "{}", message);
    });

    for (const char* name : kPreludeFiles) {
        Result<std::string> text = ReadPrelude(name);
        if (!text) {
            return std::unexpected(text.error());
        }
        const std::string chunkName = std::string("=prelude/") + name;
        sol::load_result  chunk     = state.load(*text, chunkName);
        if (!chunk.valid()) {
            const sol::error error = chunk;
            ENGINE_FAIL(ErrorCode::ScriptError, ErrorStage::Script, "the prelude {} does not load: {}",
                        name, error.what());
        }
        sol::protected_function run    = chunk;
        const auto              result = run(engineApi);
        if (!result.valid()) {
            const sol::error error = result;
            ENGINE_FAIL(ErrorCode::ScriptError, ErrorStage::Script, "the prelude {} failed: {}", name,
                        error.what());
        }
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

    // An error raised by the Lua VM itself -- calling a nil, indexing a nil, arithmetic on a string --
    // has no binding behind it, so nothing recorded the line. It does carry it, as a `file:LINE:`
    // prefix inside the text, and a traceback the script's author does not need appended after it.
    // Without this every such error reported line 0, which is the one thing a message from this
    // engine is supposed to never do.
    if (const usize_t trace = message.find("\nstack traceback"); trace != std::string::npos) {
        message.erase(trace);
    }
    u32_t line = 0;
    for (usize_t i = 0; i + 2 < message.size() && line == 0; ++i) {
        if (message[i] != ':' || message[i + 1] < '0' || message[i + 1] > '9') {
            continue;
        }
        usize_t end = i + 1;
        u32_t   value = 0;
        while (end < message.size() && message[end] >= '0' && message[end] <= '9' && value < 1000000) {
            value = value * 10 + static_cast<u32_t>(message[end] - '0');
            ++end;
        }
        if (end < message.size() && message[end] == ':') {
            line = value;
        }
    }
    return MakeScriptError(ErrorCode::ScriptError, ErrorStage::Script, chunkName, line, "{}: {}",
                           what, message);
}

// The wall-clock budget, checked from a count hook. Thread-local because RunScript runs on the
// viewer's reload worker as well as on the main thread, and two scripts never share a thread.
thread_local std::chrono::steady_clock::time_point t_deadline;
thread_local double                                 t_budgetSeconds = 0.0;

/// Runs every `kHookStride` VM instructions. Nothing here owns a destructor, because `luaL_error`
/// unwinds with `longjmp` or an exception depending on how Lua was built and must work for both.
void BudgetHook(lua_State* L, lua_Debug*) {
    if (std::chrono::steady_clock::now() > t_deadline) {
        // `lua_pushfstring` has no precision modifiers, so whole seconds through `%d`.
        luaL_error(L, "the script exceeded its time budget of %d s; is there an infinite loop?",
                   static_cast<int>(t_budgetSeconds));
    }
}

/// How often the budget is looked at. A clock read is cheap, but not free per instruction.
constexpr int kHookStride = 10000;

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
    // `package` are not opened, so a script cannot start processes or pull in C modules.
    state.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table);

    // `base` still carries the loaders, and a loader is how a script reads a file or runs code the
    // sidecar's script hash does not describe. `collectgarbage` goes too: the allocator is the
    // engine's, and a script has no business steering a collector it does not own.
    //
    // `pcall` and `xpcall` go because they defeat the time budget: the hook can only *raise* an error,
    // and a script that wraps its loop in `pcall` catches it and carries on. Measured, not assumed:
    // `while true do pcall(function() while true do end end) end` ran until it was killed from
    // outside. A graph builder has no use for catching errors, since every failure it can hit is a
    // validation error that should reach the user with its line.
    for (const char* name :
         {"dofile", "loadfile", "load", "loadstring", "collectgarbage", "pcall", "xpcall"}) {
        state[name] = sol::lua_nil;
    }

    if (environment.timeoutSeconds > 0.0) {
        t_budgetSeconds = environment.timeoutSeconds;
        t_deadline      = std::chrono::steady_clock::now()
                          + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(environment.timeoutSeconds));
        lua_sethook(state.lua_state(), BudgetHook, LUA_MASKCOUNT, kHookStride);
    }

    Bind(state, graph, environment.resolution);

    // Before the script, after the bindings it composes and the sandbox it runs inside.
    if (Status prelude = LoadPrelude(state); !prelude) {
        return std::unexpected(prelude.error());
    }

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

Result<std::array<char, 65>> PreludeHash() {
    terrain::Sha256 hash;
    for (const char* name : kPreludeFiles) {
        Result<std::string> text = ReadPrelude(name);
        if (!text) {
            return std::unexpected(text.error());
        }
        hash.Update(std::string_view{name});
        hash.Update("\n", 1);
        hash.Update(text->data(), text->size());
    }
    return hash.Final();
}

u64_t AllocatedBytes() { return memory::Lua().Stats().used; }

} // namespace engine::lua
