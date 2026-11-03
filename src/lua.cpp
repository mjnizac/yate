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
#include <engine/prelude_files.hpp>
#include <engine/terrain/graph.hpp>
#include <engine/terrain/kernels.hpp>
#include <engine/terrain/sampler.hpp>
#include <engine/terrain/sha256.hpp>
#include <engine/terrain/hydrology.hpp>
#include <engine/terrain/raster.hpp>
#include <engine/terrain/stream_power.hpp>
#include <engine/terrain/wind.hpp>

#include <sol/sol.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
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

    /// A flat list of exactly `count` numbers, appended to `out` as `T`.
    template <typename T>
    void Numbers(const char* key, usize_t count, std::pmr::vector<T>& out) {
        Accept(key);
        const sol::optional<sol::table> list = m_table[key];
        if (!list.has_value()) {
            Fail(m_location, "{} needs a list of {} numbers as '{}'", m_op, count, key);
        }
        if (list->size() != count) {
            Fail(m_location, "{} expects '{}' to hold {} numbers, got {}", m_op, key, count,
                 list->size());
        }
        out.reserve(out.size() + count);
        for (usize_t i = 1; i <= count; ++i) {
            const sol::optional<f64_t> value = (*list)[i];
            if (!value.has_value()) {
                Fail(m_location, "{} expects entry {} of '{}' to be a number", m_op, i, key);
            }
            out.push_back(static_cast<T>(*value));
        }
    }

    /// How many entries the list `key` holds, for a list whose length is the caller's to read.
    [[nodiscard]] usize_t Length(const char* key) {
        Accept(key);
        const sol::optional<sol::table> list = m_table[key];
        if (!list.has_value()) {
            Fail(m_location, "{} needs a list as '{}'", m_op, key);
        }
        return list->size();
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
    if (name == "cellular") {
        return NoiseKind::Cellular;
    }
    ENGINE_FAIL(ErrorCode::ScriptError, ErrorStage::Script,
                "unknown noise kind '{}'; expected simplex, ridged, billow or cellular", name);
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
        // Which neighbour the cellular distance is taken to: 2 is F2 - F1, 3 is F3 - F1.
        if (params.kind == NoiseKind::Cellular && args.Count("order", 2, 2, 3) == 3) {
            params.kind = NoiseKind::Cellular3;
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
        // A position field in metres, `R2 -> R2` (x, z) or `R3 -> R3`, such as `Shapes.Warp{}`: the
        // noise is sampled where it points instead of where the sample is.
        if (args.Has("position")) {
            params.position = args.Input("position");
        }
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
    noises.set_function("Cellular", [noise](sol::this_state lua, sol::table table) {
        return noise(lua, std::move(table), "Noises.Cellular", "cellular");
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
    // The same window as Blur, taking its largest (`Expand`) or smallest (`Shrink`) value: a mask grows
    // or shrinks by `radius` samples, a height is dilated or eroded. `shape` cuts the window (World
    // Machine's Expander has only the square); `radius_map` scales the radius per sample.
    const auto morphology = [&graph](const char* name, terrain::BlurMode mode) {
        return [&graph, name, mode](sol::this_state lua, sol::table table) {
            const SourceLocation location = CallerLocation(lua);
            Args                 args(std::move(table), name, location);
            const Value          input  = args.Input("input");
            const u32_t          radius = args.Count("radius", 1, 1, terrain::kMaxBlurRadius);
            const std::string    shapeName = args.Text("shape", "square");
            const std::string    falloffName = args.Text("falloff", "flat");
            const f32_t          slope       = args.Float("slope", 1.0);
            std::optional<Value> radiusMap;
            if (args.Has("radius_map")) {
                radiusMap = args.Input("radius_map");
                const Mapping wanted{input.mapping.domain, 1};
                if (!(radiusMap->mapping == wanted)) {
                    Fail(location, "{} expects radius_map to be {}, got {}", name, ToString(wanted),
                         ToString(radiusMap->mapping));
                }
            }
            args.Finish();
            constexpr std::array<std::string_view, 4> kShapes = {"square", "octagon", "diamond", "disk"};
            const auto found = std::find(kShapes.begin(), kShapes.end(), shapeName);
            if (found == kShapes.end()) {
                Fail(location, "{} expects shape to be square, octagon, diamond or disk, got '{}'", name,
                     shapeName);
            }
            const auto shape = static_cast<terrain::BlurShape>(found - kShapes.begin());
            constexpr std::array<std::string_view, 3> kFalloffs = {"flat", "cone", "round"};
            const auto falloff = std::find(kFalloffs.begin(), kFalloffs.end(), falloffName);
            if (falloff == kFalloffs.end()) {
                Fail(location, "{} expects falloff to be flat, cone or round, got '{}'", name, falloffName);
            }
            return Handle{Unwrap(graph.AddBlur(
                input, radius, location, mode, shape, radiusMap,
                static_cast<terrain::BlurFalloff>(falloff - kFalloffs.begin()), slope))};
        };
    };
    filters.set_function("Expand", morphology("Filters.Expand", terrain::BlurMode::Maximum));
    filters.set_function("Shrink", morphology("Filters.Shrink", terrain::BlurMode::Minimum));
    // A finished field read where `by` (metres along x and z) moves each sample, the move capped at
    // `radius` samples: World Machine's Displacement, bending what an expander or a blur already made.
    filters.set_function("Displace", [&graph](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Filters.Displace", location);
        const Value          input  = args.Input("input");
        const Value          by     = args.Input("by");
        const u32_t          radius = args.Count("radius", 8, 1, terrain::kMaxBlurRadius);
        args.Finish();
        return Handle{Unwrap(graph.AddDisplace(input, by, radius, location))};
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

    // A field from a grid of numbers the script computed, sampled smoothly at `position`. The values are
    // listed x fastest; `origin` is where the first one is, in metres, and `cell` the spacing.
    terrain.set_function("Grid", [&graph](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Terrain.Grid", location);
        const std::array<f64_t, 2> size   = args.Pair("size", {0.0, 0.0});
        const std::array<f64_t, 2> origin = args.Pair("origin", {0.0, 0.0});
        Graph::GridParams          params;
        params.width   = static_cast<u32_t>(size[0]);
        params.depth   = static_cast<u32_t>(size[1]);
        params.originX = static_cast<f32_t>(origin[0]);
        params.originZ = static_cast<f32_t>(origin[1]);
        params.cell    = args.Float("cell", 1.0);
        params.wrap    = args.Boolean("wrap", false);
        if (params.width != size[0] || params.depth != size[1] || params.width < 2 || params.depth < 2) {
            Fail(location, "Terrain.Grid expects size as a pair of whole numbers of at least 2");
        }
        std::pmr::vector<f32_t> values(&memory::General().Resource());
        args.Numbers("values", static_cast<usize_t>(params.width) * params.depth, values);
        const Value position = PositionOrCoords(args, graph, location);
        args.Finish();
        return Handle{Unwrap(graph.AddGrid(position, values, params, location))};
    });

    // The value a script built, evaluated on the CPU over a grid, so it can look at its own terrain before
    // deciding something (see sampler.hpp). Returns a list, x fastest, the same layout Terrain.Grid takes.
    terrain.set_function("Sample", [&graph](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Terrain.Sample", location);
        const Value          value  = args.Input("value");
        const f64_t          cell   = args.Number("cell", 50.0);
        const f64_t          seed   = args.Number("seed", 0.0);
        std::pmr::vector<std::array<f32_t, 2>> positions(&memory::General().Resource());
        // Either a grid (`origin`, `size`, `cell`) or a list of points (`xs`, `zs`), `cell` then being only
        // the step a gradient is measured over.
        if (args.Has("xs")) {
            const usize_t           count = args.Length("xs");
            std::pmr::vector<f64_t> xs(&memory::General().Resource());
            std::pmr::vector<f64_t> zs(&memory::General().Resource());
            args.Numbers("xs", count, xs);
            args.Numbers("zs", count, zs);
            for (usize_t i = 0; i < count; ++i) {
                positions.push_back({static_cast<f32_t>(xs[i]), static_cast<f32_t>(zs[i])});
            }
        } else {
            const auto  size   = args.Pair("size", {0.0, 0.0});
            const auto  origin = args.Pair("origin", {0.0, 0.0});
            const u32_t width  = static_cast<u32_t>(size[0]);
            const u32_t depth  = static_cast<u32_t>(size[1]);
            if (width != size[0] || depth != size[1] || width < 1 || depth < 1
                || static_cast<u64_t>(width) * depth > (4ull << 20)) {
                Fail(location, "Terrain.Sample expects size as whole numbers and at most 4 M samples");
            }
            positions.reserve(static_cast<usize_t>(width) * depth);
            for (u32_t z = 0; z < depth; ++z) {
                for (u32_t x = 0; x < width; ++x) {
                    positions.push_back({static_cast<f32_t>(origin[0] + x * cell), static_cast<f32_t>(origin[1] + z * cell)});
                }
            }
        }
        args.Finish();
        if (!(value.mapping == Mapping{Domain::R2, 1}) || !(cell > 0.0)) {
            Fail(location, "Terrain.Sample samples an R2->R1 value with a cell above zero, got {}", ToString(value.mapping));
        }
        std::pmr::vector<f32_t> out(positions.size(), 0.0f, &memory::General().Resource());
        if (Status ok = terrain::SampleGraph(graph, value, static_cast<u32_t>(static_cast<u64_t>(seed)),
                                             static_cast<f32_t>(cell), positions, out);
            !ok) {
            Fail(location, "{}", ok.error().Message());
        }
        sol::state_view view(lua);
        sol::table      list = view.create_table(static_cast<i32_t>(out.size()), 0);
        for (usize_t i = 0; i < out.size(); ++i) {
            list[i + 1] = static_cast<f64_t>(out[i]);
        }
        return list;
    });

    // Stream-power erosion of a whole grid on the CPU (see stream_power.hpp). Takes and returns plain
    // lists of numbers, x fastest, so a script builds the grid, erodes it and hands it to Terrain.Grid.
    erosion.set_function("StreamPower", [](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Erosion.StreamPower", location);
        const std::array<f64_t, 2> size = args.Pair("size", {0.0, 0.0});
        terrain::StreamPowerParams params;
        params.width = static_cast<u32_t>(size[0]);
        params.depth = static_cast<u32_t>(size[1]);
        if (params.width != size[0] || params.depth != size[1] || params.width < 3 || params.depth < 3
            || static_cast<u64_t>(params.width) * params.depth > (16ull << 20)) {
            Fail(location, "Erosion.StreamPower expects size as a pair of whole numbers of at least 3");
        }
        params.cell       = args.Number("cell", 50.0);
        params.iterations = args.Count("iterations", 100, 1, 100000);
        params.k          = args.Number("k", 1e-4);
        params.m          = args.Number("m", 0.5);
        params.talus      = args.Number("talus", 0.0);
        params.diffusion  = args.Number("diffusion", 0.0);
        params.refresh    = args.Count("refresh", 1, 1, 1000);
        const usize_t count = static_cast<usize_t>(params.width) * params.depth;
        std::pmr::vector<f64_t> height(&memory::General().Resource());
        std::pmr::vector<f64_t> uplift(&memory::General().Resource());
        args.Numbers("height", count, height);
        args.Numbers("uplift", count, uplift);
        args.Finish();
        if (Status eroded = terrain::StreamPowerErode(height, uplift, params); !eroded) {
            Fail(location, "{}", eroded.error().Message());
        }
        sol::state_view view(lua);
        sol::table      out = view.create_table(static_cast<i32_t>(count), 0);
        for (usize_t i = 0; i < count; ++i) {
            out[i + 1] = height[i];
        }
        return out;
    });

    // Whole-raster passes for the land prelude (see raster.hpp), over lists of `n * n` entries, x fastest.
    // A mask is read as Lua reads a condition (anything but false and nil is set). The lists go through the
    // raw stack, not sol's proxies: they are hundreds of thousands of entries, read and written a few dozen
    // times a map.
    sol::table rasters = state.create_named_table("Raster");
    const auto rasterSide = [](Args& args, const char* op, const SourceLocation& location) {
        const f64_t n = args.Number("n", 0.0);
        if (n != std::floor(n) || n < 1.0 || n > 4096.0) {
            Fail(location, "{} expects n, the raster's side, as a whole number from 1 to 4096", op);
        }
        return static_cast<u32_t>(n);
    };
    const auto readList = [](lua_State* L, const sol::table& list, usize_t count, auto&& put) {
        sol::stack::push(L, list);
        const int at = lua_gettop(L);
        for (usize_t i = 0; i < count; ++i) {
            lua_rawgeti(L, at, static_cast<lua_Integer>(i + 1));
            put(i, L);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    };
    const auto writeList = [](lua_State* L, auto&& get, usize_t count) {
        lua_createtable(L, static_cast<int>(count), 0);
        for (usize_t i = 0; i < count; ++i) {
            lua_pushnumber(L, get(i));
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        sol::table out(L, -1);
        lua_pop(L, 1);
        return out;
    };
    const auto readMask = [readList](lua_State* L, Args& args, const sol::table& table, const char* key,
                                     usize_t count, const char* op, const SourceLocation& location) {
        if (args.Length(key) != count) {
            Fail(location, "{} expects '{}' to hold {} entries", op, key, count);
        }
        std::pmr::vector<u8_t> mask(count, 0, &memory::General().Resource());
        readList(L, table[key], count, [&](usize_t i, lua_State* s) { mask[i] = lua_toboolean(s, -1) ? 1 : 0; });
        return mask;
    };
    rasters.set_function("Distance", [rasterSide, readMask, writeList](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        const sol::table     source   = table;
        Args                 args(std::move(table), "Raster.Distance", location);
        const u32_t          n        = rasterSide(args, "Raster.Distance", location);
        const usize_t        count    = static_cast<usize_t>(n) * n;
        const std::pmr::vector<u8_t> mask = readMask(lua, args, source, "mask", count, "Raster.Distance", location);
        args.Finish();
        std::pmr::vector<f64_t> out(count, 0.0, &memory::General().Resource());
        terrain::raster::Distance(mask, n, out);
        return writeList(lua, [&](usize_t i) { return out[i]; }, count);
    });
    // { parent = ..., area = ... } of the land's drainage (raster.hpp, `Drain`).
    rasters.set_function("Drain", [rasterSide, readMask, readList, writeList](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        const sol::table     source   = table;
        Args                 args(std::move(table), "Raster.Drain", location);
        const u32_t          n        = rasterSide(args, "Raster.Drain", location);
        const usize_t        count    = static_cast<usize_t>(n) * n;
        const std::pmr::vector<u8_t> land = readMask(lua, args, source, "land", count, "Raster.Drain", location);
        if (args.Length("height") != count) {
            Fail(location, "Raster.Drain expects 'height' to hold {} numbers", count);
        }
        args.Finish();
        std::pmr::vector<f64_t> height(count, 0.0, &memory::General().Resource());
        readList(lua, source["height"], count, [&](usize_t i, lua_State* s) { height[i] = lua_tonumber(s, -1); });
        std::pmr::vector<f64_t> parent(count, 0.0, &memory::General().Resource());
        std::pmr::vector<f64_t> area(count, 0.0, &memory::General().Resource());
        terrain::raster::Drain(land, height, n, parent, area);
        sol::state_view view(lua);
        sol::table      out = view.create_table();
        out["parent"] = writeList(lua, [&](usize_t i) { return parent[i]; }, count);
        out["area"]   = writeList(lua, [&](usize_t i) { return area[i]; }, count);
        return out;
    });
    // { corners = ..., weights = ..., height = ..., found = ... } of an `n` raster over a template's lattice
    // (raster.hpp, `Triangles`). `diag` is read entry by entry, as a condition: the prelude leaves holes in it.
    rasters.set_function("Triangles", [rasterSide, readList, writeList](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        const sol::table     source   = table;
        Args                 args(std::move(table), "Raster.Triangles", location);
        const u32_t          n        = rasterSide(args, "Raster.Triangles", location);
        const u32_t          nx       = args.Count("nx", 1, 2, 4096);
        const u32_t          nz       = args.Count("nz", 1, 2, 4096);
        const f64_t          spacing  = args.Number("spacing", 1.0);
        const f64_t          units    = args.Number("units", 1.0);
        const usize_t        points   = static_cast<usize_t>(nx) * nz;
        const auto           numbers  = [&](const char* key) {
            if (args.Length(key) != points) {
                Fail(location, "Raster.Triangles expects '{}' to hold {} numbers", key, points);
            }
            std::pmr::vector<f64_t> list(points, 0.0, &memory::General().Resource());
            readList(lua, source[key], points, [&](usize_t i, lua_State* s) { list[i] = lua_tonumber(s, -1); });
            return list;
        };
        const std::pmr::vector<f64_t> px = numbers("px"), pz = numbers("pz"), h = numbers("h");
        if (!args.Has("diag")) {
            Fail(location, "Raster.Triangles needs 'diag', the quads' diagonals");
        }
        args.Finish();
        std::pmr::vector<u8_t> diag(points, 0, &memory::General().Resource());
        readList(lua, source["diag"], points, [&](usize_t i, lua_State* s) { diag[i] = lua_toboolean(s, -1) ? 1 : 0; });
        const usize_t           count = static_cast<usize_t>(n) * n;
        std::pmr::vector<f64_t> corners(3 * count, 0.0, &memory::General().Resource());
        std::pmr::vector<f64_t> weights(3 * count, 0.0, &memory::General().Resource());
        std::pmr::vector<f64_t> height(count, 0.0, &memory::General().Resource());
        std::pmr::vector<u8_t>  found(count, 0, &memory::General().Resource());
        terrain::raster::Triangles({px, pz, h, diag, nx, nz, spacing, units}, n, corners, weights, height, found);
        sol::state_view view(lua);
        sol::table      out = view.create_table();
        out["corners"] = writeList(lua, [&](usize_t i) { return corners[i]; }, 3 * count);
        out["weights"] = writeList(lua, [&](usize_t i) { return weights[i]; }, 3 * count);
        out["height"]  = writeList(lua, [&](usize_t i) { return height[i]; }, count);
        lua_State* L   = lua;
        lua_createtable(L, static_cast<int>(count), 0);
        for (usize_t i = 0; i < count; ++i) {
            lua_pushboolean(L, found[i] != 0);
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        out["found"] = sol::table(L, -1);
        lua_pop(L, 1);
        return out;
    });
    // { region = ..., masses = { { cells, x, z, edge }, ... }, waters = ... } (raster.hpp, `Label`).
    rasters.set_function("Label", [rasterSide, readMask, writeList](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        const sol::table     source   = table;
        Args                 args(std::move(table), "Raster.Label", location);
        const u32_t          n        = rasterSide(args, "Raster.Label", location);
        const usize_t        count    = static_cast<usize_t>(n) * n;
        const std::pmr::vector<u8_t> land = readMask(lua, args, source, "land", count, "Raster.Label", location);
        args.Finish();
        std::pmr::vector<f64_t>                region(count, 0.0, &memory::General().Resource());
        std::pmr::vector<terrain::raster::Body> masses(&memory::General().Resource());
        std::pmr::vector<terrain::raster::Body> waters(&memory::General().Resource());
        terrain::raster::Label(land, n, region, masses, waters);
        sol::state_view view(lua);
        const auto      bodies = [&](const std::pmr::vector<terrain::raster::Body>& list) {
            sol::table out = view.create_table(static_cast<int>(list.size()), 0);
            for (usize_t i = 0; i < list.size(); ++i) {
                out[i + 1] = view.create_table_with("cells", list[i].cells, "x", list[i].x, "z", list[i].z, "edge",
                                                    list[i].edge);
            }
            return out;
        };
        sol::table out = view.create_table();
        out["region"] = writeList(lua, [&](usize_t i) { return region[i]; }, count);
        out["masses"] = bodies(masses);
        out["waters"] = bodies(waters);
        return out;
    });
    rasters.set_function("Soften", [rasterSide, readList, writeList](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        const sol::table     source   = table;
        Args                 args(std::move(table), "Raster.Soften", location);
        const u32_t          n        = rasterSide(args, "Raster.Soften", location);
        const usize_t        count    = static_cast<usize_t>(n) * n;
        if (args.Length("values") != count) {
            Fail(location, "Raster.Soften expects 'values' to hold {} numbers", count);
        }
        args.Finish();
        std::pmr::vector<f64_t> values(count, 0.0, &memory::General().Resource());
        readList(lua, source["values"], count, [&](usize_t i, lua_State* s) { values[i] = lua_tonumber(s, -1); });
        std::pmr::vector<f64_t> out(count, 0.0, &memory::General().Resource());
        terrain::raster::Soften(values, n, out);
        return writeList(lua, [&](usize_t i) { return out[i]; }, count);
    });
    rasters.set_function("Square", [rasterSide, readMask, writeList](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        const sol::table     source   = table;
        Args                 args(std::move(table), "Raster.Square", location);
        const u32_t          n        = rasterSide(args, "Raster.Square", location);
        const usize_t        count    = static_cast<usize_t>(n) * n;
        std::pmr::vector<u8_t> mask = readMask(lua, args, source, "mask", count, "Raster.Square", location);
        const u32_t            radius = args.Count("radius", 1, 0, 4096);
        // `steps` erodes (`true`) or dilates (`false`) one after another: an opening and a closing in one call.
        const usize_t steps = args.Length("steps");
        sol::table    list  = source["steps"];
        for (usize_t s = 1; s <= steps; ++s) {
            terrain::raster::Square(mask, n, radius, list[s].get<bool>());
        }
        args.Finish();
        lua_State* L = lua;
        lua_createtable(L, static_cast<int>(count), 0);
        for (usize_t i = 0; i < count; ++i) {
            lua_pushboolean(L, mask[i] != 0);
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        sol::table out(L, -1);
        lua_pop(L, 1);
        return out;
    });
    rasters.set_function("Box", [rasterSide, readList, writeList](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        const sol::table     source   = table;
        Args                 args(std::move(table), "Raster.Box", location);
        const u32_t          n        = rasterSide(args, "Raster.Box", location);
        const usize_t        count    = static_cast<usize_t>(n) * n;
        const u32_t          radius   = args.Count("radius", 1, 0, 4096);
        if (args.Length("values") != count) {
            Fail(location, "Raster.Box expects 'values' to hold {} numbers", count);
        }
        args.Finish();
        std::pmr::vector<f64_t> values(count, 0.0, &memory::General().Resource());
        readList(lua, source["values"], count, [&](usize_t i, lua_State* s) { values[i] = lua_tonumber(s, -1); });
        terrain::raster::Box(values, n, radius);
        return writeList(lua, [&](usize_t i) { return values[i]; }, count);
    });

    rasters.set_function("Project", [rasterSide, readList, writeList](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        const sol::table     source   = table;
        Args                 args(std::move(table), "Raster.Project", location);
        const u32_t          n        = rasterSide(args, "Raster.Project", location);
        const usize_t        count    = static_cast<usize_t>(n) * n;
        const f64_t          cell     = args.Number("cell", 1.0);
        const u32_t          reach    = args.Count("reach", 1, 1, 4096);
        if (args.Length("sdf") != count || args.Length("at_coast") != count || !(cell > 0.0)) {
            Fail(location, "Raster.Project expects 'sdf' and 'at_coast' of {} numbers and a cell above zero", count);
        }
        args.Finish();
        std::pmr::vector<f64_t> sdf(count, 0.0, &memory::General().Resource());
        std::pmr::vector<f64_t> atCoast(count, 0.0, &memory::General().Resource());
        std::pmr::vector<f64_t> out(count, 0.0, &memory::General().Resource());
        readList(lua, source["sdf"], count, [&](usize_t i, lua_State* s) { sdf[i] = lua_tonumber(s, -1); });
        readList(lua, source["at_coast"], count, [&](usize_t i, lua_State* s) { atCoast[i] = lua_tonumber(s, -1); });
        terrain::raster::Project(sdf, n, cell, reach, atCoast, out);
        return writeList(lua, [&](usize_t i) { return out[i]; }, count);
    });

    // A raster read at another raster's pixels: `from` and `to` are { n, x0, z0, cell }.
    rasters.set_function("Resample", [readList, writeList](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        const sol::table     source   = table;
        Args                 args(std::move(table), "Raster.Resample", location);
        const auto placement = [&](const char* key) {
            const usize_t length = args.Length(key);
            sol::table    list   = source[key];
            if (length != 4) {
                Fail(location, "Raster.Resample expects '{}' as {{ n, x0, z0, cell }}", key);
            }
            terrain::raster::Placement p;
            p.n    = static_cast<u32_t>(list[1].get<f64_t>());
            p.x0   = list[2].get<f64_t>();
            p.z0   = list[3].get<f64_t>();
            p.cell = list[4].get<f64_t>();
            if (p.n < 1 || p.n > 8192 || !(p.cell > 0.0)) {
                Fail(location, "Raster.Resample expects '{}' with n from 1 to 8192 and a cell above zero", key);
            }
            return p;
        };
        const terrain::raster::Placement from = placement("from");
        const terrain::raster::Placement to   = placement("to");
        const b8_t   nearest = args.Boolean("nearest", false);
        const f64_t  outside = args.Number("outside", 0.0);
        const usize_t count  = static_cast<usize_t>(from.n) * from.n;
        if (args.Length("values") != count) {
            Fail(location, "Raster.Resample expects 'values' to hold {} numbers", count);
        }
        args.Finish();
        std::pmr::vector<f64_t> values(count, 0.0, &memory::General().Resource());
        readList(lua, source["values"], count, [&](usize_t i, lua_State* s) { values[i] = lua_tonumber(s, -1); });
        std::pmr::vector<f64_t> out(static_cast<usize_t>(to.n) * to.n, 0.0, &memory::General().Resource());
        terrain::raster::Resample(values, from, to, nearest, outside, out);
        return writeList(lua, [&](usize_t i) { return out[i]; }, out.size());
    });

    // A list of numbers kept for the rest of the process under a key the script makes of everything that
    // went into it (see raster.hpp), and given back to the next run that asks for the same key.
    rasters.set_function("Keep", [readList](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        const sol::table     source   = table;
        Args                 args(std::move(table), "Raster.Keep", location);
        const std::string    key      = args.Text("key", "");
        const usize_t        count    = args.Length("numbers");
        args.Finish();
        if (key.empty()) {
            Fail(location, "Raster.Keep needs a 'key'");
        }
        std::pmr::vector<f64_t> numbers(count, 0.0, &memory::General().Resource());
        readList(lua, source["numbers"], count, [&](usize_t i, lua_State* s) { numbers[i] = lua_tonumber(s, -1); });
        terrain::raster::Keep(key, numbers);
    });
    rasters.set_function("Recall", [writeList](sol::this_state lua, sol::table table) -> sol::object {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Raster.Recall", location);
        const std::string    key      = args.Text("key", "");
        args.Finish();
        std::pmr::vector<f64_t> numbers(&memory::General().Resource());
        if (!terrain::raster::Recall(key, numbers)) {
            return sol::make_object(lua, sol::lua_nil);
        }
        return writeList(lua, [&](usize_t i) { return numbers[i]; }, numbers.size());
    });

    // Rivers and lakes of a whole grid on the CPU (see hydrology.hpp): the grid made to drain, its drainage
    // area, its lakes and the distance to its rivers, as plain lists of numbers, x fastest, for
    // `Terrain.Grid`. The `Water.*` prelude turns them into carved channels and water surfaces.
    sol::table hydrology = state.create_named_table("Hydrology");
    hydrology.set_function("Drain", [](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Hydrology.Drain", location);
        const std::array<f64_t, 2> size = args.Pair("size", {0.0, 0.0});
        terrain::HydrologyParams   params;
        params.width = static_cast<u32_t>(size[0]);
        params.depth = static_cast<u32_t>(size[1]);
        if (params.width != size[0] || params.depth != size[1] || params.width < 3 || params.depth < 3
            || static_cast<u64_t>(params.width) * params.depth > (16ull << 20)) {
            Fail(location, "Hydrology.Drain expects size as a pair of whole numbers of at least 3");
        }
        params.cell        = args.Number("cell", 50.0);
        params.seaLevel    = args.Number("sea_level", -1e30);
        params.breachDepth = args.Number("breach", 3.0);
        params.lakeArea    = args.Number("lake_area", 2e5);
        params.lakeDepth   = args.Number("lake_depth", 2.0);
        params.channelArea = args.Number("channel_area", 1e6);
        params.widthScale  = args.Number("width_scale", 10.0);
        params.depthScale  = args.Number("depth_scale", 1.5);
        const usize_t count = static_cast<usize_t>(params.width) * params.depth;
        std::pmr::vector<f64_t> height(&memory::General().Resource());
        args.Numbers("height", count, height);
        args.Finish();
        terrain::HydrologyResult drained{
            .height        = std::pmr::vector<f64_t>(&memory::General().Resource()),
            .area          = std::pmr::vector<f64_t>(&memory::General().Resource()),
            .lake          = std::pmr::vector<f64_t>(&memory::General().Resource()),
            .riverDistance = std::pmr::vector<f64_t>(&memory::General().Resource()),
            .riverWidth    = std::pmr::vector<f64_t>(&memory::General().Resource()),
            .riverDepth    = std::pmr::vector<f64_t>(&memory::General().Resource()),
            .riverLevel    = std::pmr::vector<f64_t>(&memory::General().Resource()),
            .receiver      = std::pmr::vector<i32_t>(&memory::General().Resource()),
            .sea           = std::pmr::vector<u8_t>(&memory::General().Resource()),
        };
        if (Status ok = terrain::Drain(height, params, drained); !ok) {
            Fail(location, "{}", ok.error().Message());
        }
        sol::state_view view(lua);
        const auto      list = [&](const std::pmr::vector<f64_t>& values) {
            sol::table out = view.create_table(static_cast<i32_t>(values.size()), 0);
            for (usize_t i = 0; i < values.size(); ++i) {
                out[i + 1] = values[i];
            }
            return out;
        };
        sol::table out = view.create_table();
        out["height"]         = list(drained.height);
        out["area"]           = list(drained.area);
        out["lake"]           = list(drained.lake);
        out["river_distance"] = list(drained.riverDistance);
        out["river_width"]    = list(drained.riverWidth);
        out["river_depth"]    = list(drained.riverDepth);
        out["river_level"]    = list(drained.riverLevel);
        // Lua indices: the list index of the cell each cell drains to, 0 at an outlet.
        sol::table receiver = view.create_table(static_cast<i32_t>(count), 0);
        sol::table sea      = view.create_table(static_cast<i32_t>(count), 0);
        for (usize_t i = 0; i < count; ++i) {
            receiver[i + 1] = drained.receiver[i] + 1;
            sea[i + 1]      = drained.sea[i] != 0;
        }
        out["receiver"] = receiver;
        out["sea"]      = sea;
        out["lakes"]    = drained.lakes;
        out["rivers"]   = drained.rivers;
        return out;
    });

    // The fields an analytic river or lake is drawn from (see hydrology.hpp), computed on the CPU over
    // the element's own bounds and handed to the sections as grids. Used by the `Water.*` prelude.
    const auto gridOf = [&graph](const terrain::FieldGrid& field, Value position, SourceLocation location) {
        Graph::GridParams params;
        params.width   = field.width;
        params.depth   = field.depth;
        params.originX = static_cast<f32_t>(field.originX);
        params.originZ = static_cast<f32_t>(field.originZ);
        params.cell    = static_cast<f32_t>(field.cell);
        std::pmr::vector<f32_t> values(field.values.size(), 0.0f, &memory::General().Resource());
        std::transform(field.values.begin(), field.values.end(), values.begin(),
                       [](f64_t v) { return static_cast<f32_t>(v); });
        return Handle{Unwrap(graph.AddGrid(position, values, params, location))};
    };
    const auto fieldGrid = [] {
        return terrain::FieldGrid{.values = std::pmr::vector<f64_t>(&memory::General().Resource())};
    };
    hydrology.set_function("CurveField", [&graph, gridOf, fieldGrid](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Hydrology.CurveField", location);
        const usize_t        count = args.Length("xs");
        std::pmr::vector<f64_t> xs(&memory::General().Resource());
        std::pmr::vector<f64_t> zs(&memory::General().Resource());
        std::pmr::vector<f64_t> levels(&memory::General().Resource());
        std::pmr::vector<f64_t> halves(&memory::General().Resource());
        args.Numbers("xs", count, xs);
        args.Numbers("zs", count, zs);
        args.Numbers("levels", count, levels);
        args.Numbers("halves", count, halves);
        // An optional axis the level is read from (see CurveFields).
        std::pmr::vector<f64_t> axisXs(&memory::General().Resource());
        std::pmr::vector<f64_t> axisZs(&memory::General().Resource());
        std::pmr::vector<f64_t> axisLevels(&memory::General().Resource());
        if (args.Has("axis_xs")) {
            const usize_t axis = args.Length("axis_xs");
            args.Numbers("axis_xs", axis, axisXs);
            args.Numbers("axis_zs", axis, axisZs);
            args.Numbers("axis_levels", axis, axisLevels);
        }
        const f64_t cell     = args.Number("cell", 10.0);
        const f64_t margin   = args.Number("margin", 500.0);
        const Value position = PositionOrCoords(args, graph, location);
        args.Finish();
        terrain::FieldGrid distance2 = fieldGrid();
        terrain::FieldGrid along     = fieldGrid();
        terrain::FieldGrid level     = fieldGrid();
        terrain::FieldGrid half      = fieldGrid();
        terrain::FieldGrid axisLevel = fieldGrid();
        if (Status ok = terrain::CurveFields(xs, zs, levels, halves, cell, margin, distance2, along, level, half,
                                             axisLevel, axisXs, axisZs, axisLevels);
            !ok) {
            Fail(location, "{}", ok.error().Message());
        }
        sol::state_view view(lua);
        sol::table      out = view.create_table();
        out["distance2"] = gridOf(distance2, position, location);
        out["level"]     = gridOf(level, position, location);
        out["half"]      = gridOf(half, position, location);
        if (!axisXs.empty()) {
            out["axis_level"] = gridOf(axisLevel, position, location);
        }
        return out;
    });
    // A river or a lake as one block of the grid buffer (see water.comp.glsl): a 16-value header, its
    // frame and `params` (the element's numbers, kind first), then four values a cell. `Hydrology.Carve`
    // reads it in one node. Both return the block's offset.
    // Header: 20 values, the frame, `params` from 5 to 15, the point count in 16 and where the cells
    // start in 17. Then the points, six values each (x, z, level, half-width, depth, bank skew), then the
    // cells, four values each.
    const auto block = [&graph](const std::array<const terrain::FieldGrid*, 4>& fields, const std::pmr::vector<f64_t>& params,
                                const std::array<const std::pmr::vector<f64_t>*, 6>& points, SourceLocation location) {
        const terrain::FieldGrid& frame = *fields[0];
        const usize_t             count = points[0] != nullptr ? points[0]->size() : 0;
        const usize_t             cells = 20 + count * 6;
        std::pmr::vector<f32_t>   values(cells + frame.values.size() * 4, 0.0f, &memory::General().Resource());
        values[16] = static_cast<f32_t>(count);
        values[17] = static_cast<f32_t>(cells);
        for (usize_t i = 0; i < count; ++i) {
            for (usize_t k = 0; k < 6; ++k) {
                values[20 + i * 6 + k] = static_cast<f32_t>((*points[k])[i]);
            }
        }
        values[0] = static_cast<f32_t>(frame.width);
        values[1] = static_cast<f32_t>(frame.depth);
        values[2] = static_cast<f32_t>(frame.originX);
        values[3] = static_cast<f32_t>(frame.originZ);
        values[4] = static_cast<f32_t>(frame.cell);
        for (usize_t i = 0; i < params.size() && i < 11; ++i) {
            values[5 + i] = static_cast<f32_t>(params[i]);
        }
        for (usize_t c = 0; c < frame.values.size(); ++c) {
            for (usize_t k = 0; k < 4; ++k) {
                values[cells + c * 4 + k] = fields[k] != nullptr ? static_cast<f32_t>(fields[k]->values[c]) : 0.0f;
            }
        }
        Result<u32_t> offset = graph.AddGridBlock(values, location);
        if (!offset) {
            Fail(location, "{}", offset.error().Message());
        }
        return static_cast<f64_t>(*offset);
    };
    hydrology.set_function("RiverBlock", [block, fieldGrid](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Hydrology.RiverBlock", location);
        const usize_t        count = args.Length("xs");
        const usize_t        axis  = args.Length("axis_xs");
        std::pmr::memory_resource* r = &memory::General().Resource();
        std::pmr::vector<f64_t> xs(r), zs(r), levels(r), halves(r), axisXs(r), axisZs(r), axisLevels(r), params(r);
        args.Numbers("xs", count, xs);
        args.Numbers("zs", count, zs);
        args.Numbers("levels", count, levels);
        args.Numbers("halves", count, halves);
        std::pmr::vector<f64_t> depths(r), skews(r);
        args.Numbers("depths", count, depths);
        args.Numbers("skews", count, skews);
        args.Numbers("axis_xs", axis, axisXs);
        args.Numbers("axis_zs", axis, axisZs);
        args.Numbers("axis_levels", axis, axisLevels);
        args.Numbers("params", args.Length("params"), params);
        const f64_t cell   = args.Number("cell", 10.0);
        const f64_t margin = args.Number("margin", 500.0);
        args.Finish();
        terrain::FieldGrid distance2 = fieldGrid(), along = fieldGrid(), level = fieldGrid(), half = fieldGrid(),
                           axisLevel = fieldGrid();
        if (Status ok = terrain::CurveFields(xs, zs, levels, halves, cell, margin, distance2, along, level, half,
                                             axisLevel, axisXs, axisZs, axisLevels);
            !ok) {
            Fail(location, "{}", ok.error().Message());
        }
        return block({&distance2, &along, &level, &axisLevel}, params, {&xs, &zs, &levels, &halves, &depths, &skews}, location);
    });
    hydrology.set_function("LakeBlock", [block, fieldGrid](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Hydrology.LakeBlock", location);
        const usize_t        count = args.Length("xs");
        std::pmr::memory_resource* r = &memory::General().Resource();
        std::pmr::vector<f64_t> xs(r), zs(r), params(r);
        args.Numbers("xs", count, xs);
        args.Numbers("zs", count, zs);
        args.Numbers("params", args.Length("params"), params);
        const f64_t cell   = args.Number("cell", 20.0);
        const f64_t margin = args.Number("margin", 500.0);
        // The basin's slope at every cell, steeper on the uphill side (a cirque's headwall) by `headwall`,
        // and a moraine on the downhill side `moraine` metres high: `uphill` is the direction, `centre` the
        // lake's middle.
        const f64_t basin    = args.Number("basin", 0.2);
        const f64_t headwall = args.Number("headwall", 0.0);
        const f64_t moraine  = args.Number("moraine", 0.0);
        const auto  uphill   = args.Pair("uphill", {0.0, 0.0});
        const auto  centre   = args.Pair("centre", {0.0, 0.0});
        args.Finish();
        terrain::FieldGrid distance = fieldGrid();
        if (Status ok = terrain::PolygonField(xs, zs, cell, margin, distance); !ok) {
            Fail(location, "{}", ok.error().Message());
        }
        terrain::FieldGrid slope = distance;
        terrain::FieldGrid rim   = distance;
        for (u32_t z = 0; z < distance.depth; ++z) {
            for (u32_t x = 0; x < distance.width; ++x) {
                const f64_t dx = distance.originX + x * cell - centre[0];
                const f64_t dz = distance.originZ + z * cell - centre[1];
                const f64_t to = (dx * uphill[0] + dz * uphill[1]) / std::max(1.0, std::sqrt(dx * dx + dz * dz));
                const usize_t i = static_cast<usize_t>(z) * distance.width + x;
                slope.values[i] = basin * (1.0 + headwall * std::max(0.0, to));
                rim.values[i]   = moraine * std::max(0.0, -to) * std::max(0.0, -to);
            }
        }
        return block({&distance, &slope, &rim, nullptr}, params, {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}, location);
    });
    hydrology.set_function("Carve", [&graph](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Hydrology.Carve", location);
        const f64_t          at       = args.Number("block", -1.0);
        const f64_t          receiver = args.Number("receiver", -1.0);
        const std::string    mode     = args.Text("mode", "cap");
        const Value          aux      = args.Input("aux");
        const Value          position = PositionOrCoords(args, graph, location);
        args.Finish();
        using Mode = Graph::WaterMode;
        const Mode chosen = mode == "fill" ? Mode::RiverFill : mode == "lake" ? Mode::LakeCap
                          : mode == "rim"  ? Mode::LakeRim   : Mode::RiverCap;
        if (mode != "cap" && mode != "fill" && mode != "lake" && mode != "rim") {
            Fail(location, "Hydrology.Carve expects mode 'cap', 'fill', 'lake' or 'rim', got '{}'", mode);
        }
        const u32_t own  = at < 0.0 ? Graph::kNoBlock : static_cast<u32_t>(at);
        const u32_t into = receiver < 0.0 ? Graph::kNoBlock : static_cast<u32_t>(receiver);
        return Handle{Unwrap(graph.AddWater(position, aux, own, into, chosen, location))};
    });

    hydrology.set_function("PolygonField",[&graph, gridOf, fieldGrid](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Hydrology.PolygonField", location);
        const usize_t        count = args.Length("xs");
        std::pmr::vector<f64_t> xs(&memory::General().Resource());
        std::pmr::vector<f64_t> zs(&memory::General().Resource());
        args.Numbers("xs", count, xs);
        args.Numbers("zs", count, zs);
        const f64_t cell     = args.Number("cell", 20.0);
        const f64_t margin   = args.Number("margin", 500.0);
        const Value position = PositionOrCoords(args, graph, location);
        args.Finish();
        terrain::FieldGrid distance = fieldGrid();
        if (Status ok = terrain::PolygonField(xs, zs, cell, margin, distance); !ok) {
            Fail(location, "{}", ok.error().Message());
        }
        return gridOf(distance, position, location);
    });

    // Aeolian sand transport over a whole periodic grid on the CPU (see wind.hpp): dunes from the wind.
    // Takes and returns a list of sand heights in metres, x fastest; the wind blows along +x.
    erosion.set_function("Wind", [](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Erosion.Wind", location);
        const std::array<f64_t, 2> size = args.Pair("size", {0.0, 0.0});
        terrain::WindParams        params;
        params.width = static_cast<u32_t>(size[0]);
        params.depth = static_cast<u32_t>(size[1]);
        if (params.width != size[0] || params.depth != size[1] || params.width < 8 || params.depth < 8
            || static_cast<u64_t>(params.width) * params.depth > (4ull << 20)) {
            Fail(location, "Erosion.Wind expects size as a pair of whole numbers of at least 8");
        }
        params.cell       = args.Number("cell", 20.0);
        params.slab       = args.Number("slab", 3.0);
        params.iterations = args.Count("iterations", 200, 1, 100000);
        params.hop        = args.Count("hop", 5, 1, 256);
        params.pSand      = args.Number("p_sand", 0.6);
        params.pBare      = args.Number("p_bare", 0.4);
        params.shadow     = args.Number("shadow", 15.0);
        params.repose     = args.Number("repose", 33.0);
        params.creep      = args.Number("creep", 0.2);
        params.smooth     = args.Count("smooth", 2, 0, 16);
        params.seed       = args.Count("seed", 0, 0, ~0u);
        std::pmr::vector<f64_t> height(&memory::General().Resource());
        args.Numbers("height", static_cast<usize_t>(params.width) * params.depth, height);
        args.Finish();
        if (Status moved = terrain::WindErode(height, params); !moved) {
            Fail(location, "{}", moved.error().Message());
        }
        sol::state_view view(lua);
        sol::table      out = view.create_table(static_cast<i32_t>(height.size()), 0);
        for (usize_t i = 0; i < height.size(); ++i) {
            out[i + 1] = height[i];
        }
        return out;
    });

    // Gullies across the slope of `input`, from its gradient: the look of erosion where the simulation
    // cannot reach (kilometres of drainage), as a pattern rather than a process. Pointwise, so it is the
    // same at every spacing and costs no halo beyond the gradient's.
    erosion.set_function("Gullies", [&graph](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Erosion.Gullies", location);
        const Value          input = args.Input("input");
        const Value gradient = args.Has("gradient") ? args.Input("gradient")
                                                    : Unwrap(graph.AddGradient(input, location));
        const Value position = PositionOrCoords(args, graph, location);

        Graph::GulliesParams params;
        params.cell       = args.Float("cell", params.cell);
        params.octaves    = args.Count("octaves", params.octaves, 1, 8);
        params.strength   = args.Float("strength", params.strength);
        params.full       = args.Float("full", params.full);
        params.gain       = args.Float("gain", params.gain);
        params.lacunarity = args.Float("lacunarity", params.lacunarity);
        params.seedSalt   = args.Count("seed", 0, 0, ~0u);
        args.Finish();

        return Handle{Unwrap(graph.AddGullies(input, gradient, position, params, location))};
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

    // A block of numbers in the grid buffer, for an op that reads a list of its own there: its offset.
    // The layout is the op's (`Geometry.Lands`: see lands.comp.glsl); `Shapes.Set` writes it.
    geometry.set_function("Block", [&graph](sol::this_state lua, sol::table table) {
        const SourceLocation    location = CallerLocation(lua);
        Args                    args(std::move(table), "Geometry.Block", location);
        std::pmr::vector<f64_t> numbers(&memory::General().Resource());
        args.Numbers("values", args.Length("values"), numbers);
        args.Finish();
        std::pmr::vector<f32_t> values(numbers.size(), 0.0f, &memory::General().Resource());
        std::transform(numbers.begin(), numbers.end(), values.begin(), [](f64_t v) { return static_cast<f32_t>(v); });
        Result<u32_t> offset = graph.AddGridBlock(values, location);
        if (!offset) {
            Fail(location, "{}", offset.error().Message());
        }
        return static_cast<f64_t>(*offset);
    });
    geometry.set_function("Lands", [&graph](sol::this_state lua, sol::table table) {
        const SourceLocation location = CallerLocation(lua);
        Args                 args(std::move(table), "Geometry.Lands", location);
        const f64_t          block    = args.Number("block", -1.0);
        const std::string    mode     = args.Text("mode", "coast");
        const Value          position = PositionOrCoords(args, graph, location);
        args.Finish();
        if (mode != "coast" && mode != "relief" && mode != "seabed" && mode != "margin") {
            Fail(location, "Geometry.Lands expects mode 'coast', 'relief', 'seabed' or 'margin', got '{}'", mode);
        }
        const u32_t at     = block < 0.0 ? Graph::kNoBlock : static_cast<u32_t>(block);
        const auto  chosen = mode == "relief" ? Graph::LandsMode::Relief
                           : mode == "seabed" ? Graph::LandsMode::Seabed
                           : mode == "margin" ? Graph::LandsMode::Margin
                                              : Graph::LandsMode::Coast;
        return Handle{Unwrap(graph.AddLands(position, at, chosen, location))};
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

// --- Drawn shapes --------------------------------------------------------------------------------

/// Strict JSON read straight into Lua values: objects as tables keyed by name, arrays as lists, `null`
/// as an absent field. Data only, so nothing in the file can run; nesting and size are bounded.
class JsonReader {
public:
    JsonReader(sol::state& state, std::string_view text) : m_state(state), m_text(text) {}

    [[nodiscard]] sol::object Document() {
        sol::object value = Value(0);
        Space();
        if (m_at < m_text.size()) {
            return Fail("text after the end of the document");
        }
        return value;
    }
    [[nodiscard]] const std::string& Problem() const { return m_problem; }
    [[nodiscard]] u32_t Line() const { return m_line; }

private:
    static constexpr u32_t kMaxDepth = 32;

    sol::object Fail(std::string_view what) {
        if (m_problem.empty()) {
            m_problem = what;
            m_line    = 1 + static_cast<u32_t>(std::count(m_text.begin(), m_text.begin() + static_cast<std::ptrdiff_t>(m_at), '\n'));
        }
        m_at = m_text.size() + 1;
        return sol::lua_nil;
    }
    void Space() {
        while (m_at < m_text.size() && std::strchr(" \t\r\n", m_text[m_at]) != nullptr) {
            ++m_at;
        }
    }
    [[nodiscard]] b8_t Eat(char c) {
        Space();
        if (m_at < m_text.size() && m_text[m_at] == c) {
            ++m_at;
            return true;
        }
        return false;
    }
    [[nodiscard]] b8_t Word(std::string_view word) {
        if (m_text.substr(m_at, word.size()) != word) {
            return false;
        }
        m_at += word.size();
        return true;
    }
    sol::object Value(u32_t depth) {
        Space();
        if (depth > kMaxDepth || m_at >= m_text.size()) {
            return Fail(depth > kMaxDepth ? "nested more than 32 deep" : "unexpected end of the document");
        }
        const char c = m_text[m_at];
        if (c == '{' || c == '[') {
            return Container(depth, c == '{');
        }
        if (c == '"') {
            std::string text;
            return String(text) ? sol::make_object(m_state, text) : sol::object(sol::lua_nil);
        }
        if (Word("true")) {
            return sol::make_object(m_state, true);
        }
        if (Word("false")) {
            return sol::make_object(m_state, false);
        }
        if (Word("null")) {
            return sol::lua_nil;
        }
        const usize_t start = m_at;
        while (m_at < m_text.size() && std::strchr("+-0123456789.eE", m_text[m_at]) != nullptr) {
            ++m_at;
        }
        f64_t number = 0.0;
        const auto [end, failure] = std::from_chars(m_text.data() + start, m_text.data() + m_at, number);
        if (m_at == start || failure != std::errc{} || end != m_text.data() + m_at || !std::isfinite(number)) {
            m_at = start;
            return Fail("expected a value");
        }
        return sol::make_object(m_state, number);
    }
    sol::object Container(u32_t depth, b8_t object) {
        ++m_at;
        sol::table table = m_state.create_table();
        const char close = object ? '}' : ']';
        if (Eat(close)) {
            return table;
        }
        for (usize_t index = 1;; ++index) {
            std::string key;
            if (object && (!(Space(), m_at < m_text.size() && m_text[m_at] == '"') || !String(key) || !Eat(':'))) {
                return Fail("expected \"name\": in an object");
            }
            sol::object value = Value(depth + 1);
            if (!m_problem.empty()) {
                return sol::lua_nil;
            }
            if (object) {
                table[key] = value;
            } else if (value == sol::lua_nil) {
                return Fail("null in a list");
            } else {
                table[index] = value;
            }
            if (Eat(close)) {
                return table;
            }
            if (!Eat(',')) {
                return Fail(object ? "expected , or } in an object" : "expected , or ] in a list");
            }
        }
    }
    [[nodiscard]] b8_t String(std::string& out) {
        ++m_at;
        while (m_at < m_text.size() && m_text[m_at] != '"') {
            const char c = m_text[m_at++];
            if (static_cast<unsigned char>(c) < 0x20) {
                Fail("a control character inside a string");
                return false;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            const char e = m_at < m_text.size() ? m_text[m_at++] : '\0';
            const char* from = "\"\\/bfnrt";
            const char* to   = "\"\\/\b\f\n\r\t";
            if (const char* hit = std::strchr(from, e); hit != nullptr && e != '\0') {
                out.push_back(to[hit - from]);
            } else if (e == 'u' && m_at + 4 <= m_text.size()) {
                u32_t code = 0;
                if (std::from_chars(m_text.data() + m_at, m_text.data() + m_at + 4, code, 16).ptr
                    != m_text.data() + m_at + 4) {
                    Fail("a bad \\u escape");
                    return false;
                }
                m_at += 4;
                // UTF-8 for the basic plane; names and kinds are ASCII in practice.
                if (code < 0x80) {
                    out.push_back(static_cast<char>(code));
                } else if (code < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                }
            } else {
                Fail("a bad escape in a string");
                return false;
            }
        }
        if (m_at >= m_text.size()) {
            Fail("a string that never ends");
            return false;
        }
        ++m_at;
        return true;
    }

    sol::state&      m_state;
    std::string_view m_text;
    usize_t          m_at = 0;
    std::string      m_problem;
    u32_t            m_line = 0;
};

/// Reads the drawn shapes: a GeoJSON FeatureCollection whose features are a `Point`, a `LineString` or a
/// one-ring `Polygon` with a string `properties.kind`. Each becomes a table with its properties (numbers,
/// strings and booleans), `geometry` (`"point"`, `"line"` or `"polygon"`), `points`, a list of `{ x, z }`
/// in world metres (GeoJSON's second coordinate is the world z), and `heights` when every position gives a
/// third value. A polygon's closing point, the same as its first, is dropped.
[[nodiscard]] Result<sol::table> ReadShapes(sol::state& state, std::string_view path) {
    constexpr usize_t kMaxBytes = 16u << 20;
    const std::string file(path);
    std::ifstream     stream(file, std::ios::binary);
    if (!stream) {
        return std::unexpected(MakeScriptError(ErrorCode::InvalidArgument, ErrorStage::Script, path, 0,
                                               "cannot read the shapes file"));
    }
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (text.size() > kMaxBytes) {
        return std::unexpected(MakeScriptError(ErrorCode::InvalidArgument, ErrorStage::Script, path, 0,
                                               "the shapes file is over 16 MiB"));
    }
    JsonReader        reader(state, text);
    const sol::object document = reader.Document();
    if (!reader.Problem().empty()) {
        return std::unexpected(MakeScriptError(ErrorCode::InvalidArgument, ErrorStage::Script, path,
                                               reader.Line(), "shapes: {}", reader.Problem()));
    }
    const auto fail = [&](usize_t feature, std::string_view what) {
        return std::unexpected(MakeScriptError(ErrorCode::InvalidArgument, ErrorStage::Script, path, 0,
                                               "shapes, feature {}: {}", feature, what));
    };
    const auto textOf = [](const sol::table& t, const char* key) {
        const sol::optional<std::string> value = t[key];
        return value.value_or(std::string{});
    };
    const sol::optional<sol::table> root = document.as<sol::optional<sol::table>>();
    if (!root || textOf(*root, "type") != "FeatureCollection" || (*root)["features"].get_type() != sol::type::table) {
        return std::unexpected(MakeScriptError(ErrorCode::InvalidArgument, ErrorStage::Script, path, 0,
                                               "shapes: expected a GeoJSON FeatureCollection with a 'features' list"));
    }
    sol::table       shapes   = state.create_table();
    const sol::table features = (*root)["features"];
    for (usize_t f = 1; f <= features.size(); ++f) {
        const sol::optional<sol::table> feature = features[f];
        if (!feature || textOf(*feature, "type") != "Feature") {
            return fail(f, "not a Feature");
        }
        const sol::optional<sol::table> geometry   = (*feature)["geometry"];
        const sol::optional<sol::table> properties = (*feature)["properties"];
        if (!geometry || !properties || textOf(*properties, "kind").empty()) {
            return fail(f, "needs a geometry and properties with a string 'kind'");
        }
        sol::table shape = state.create_table();
        for (const auto& [key, value] : *properties) {
            const std::string name = key.as<std::string>();
            if (value.get_type() == sol::type::table || name == "geometry" || name == "points"
                || name == "heights") {
                return fail(f, "properties are numbers, strings or booleans, and not named 'geometry', "
                               "'points' or 'heights'");
            }
            shape[name] = value;
        }

        // The positions: one for a Point, the list for a LineString, the one ring for a Polygon.
        const std::string         type    = textOf(*geometry, "type");
        const b8_t                polygon = type == "Polygon";
        sol::optional<sol::table> list    = (*geometry)["coordinates"];
        if (type == "Point") {
            list = state.create_table_with(1, (*geometry)["coordinates"]);
        } else if (polygon && list) {
            if (list->size() != 1) {
                return fail(f, "a Polygon with holes is not supported: give it one ring");
            }
            list = (*list)[1].get<sol::optional<sol::table>>();
        } else if (type != "LineString") {
            return fail(f, "the geometry is a Point, a LineString or a Polygon");
        }
        usize_t count = list ? list->size() : 0;
        // A ring repeats its first position at the end; the shapes take the ring without it.
        const auto coordinate = [&](usize_t i, usize_t k) {
            const sol::optional<sol::table> p = (*list)[i];
            return p ? (*p)[k].get<sol::optional<f64_t>>() : sol::nullopt;
        };
        if (polygon && count >= 2 && coordinate(1, 1) && coordinate(1, 1) == coordinate(count, 1)
            && coordinate(1, 2) == coordinate(count, 2)) {
            --count;
        }
        const usize_t least = type == "Point" ? 1 : polygon ? 3 : 2;
        if (count < least) {
            return fail(f, polygon ? "a Polygon needs at least three points" : "a LineString needs at least two points");
        }
        // Each position is [x, z] or [x, z, height]: the pair goes to `points` and the height, when every
        // position has one, to `heights`.
        sol::table points  = state.create_table();
        sol::table heights = state.create_table();
        usize_t    high    = 0;
        for (usize_t i = 1; i <= count; ++i) {
            const sol::optional<sol::table> p = (*list)[i];
            const usize_t                   n = p ? p->size() : 0;
            const sol::optional<f64_t>      x = p ? (*p)[1].get<sol::optional<f64_t>>() : sol::nullopt;
            const sol::optional<f64_t>      z = p ? (*p)[2].get<sol::optional<f64_t>>() : sol::nullopt;
            const sol::optional<f64_t>      y = n == 3 ? (*p)[3].get<sol::optional<f64_t>>() : sol::nullopt;
            if (n < 2 || n > 3 || !x || !z || (n == 3 && !y)) {
                return fail(f, "a position is [x, z] or [x, z, height]");
            }
            points[i] = state.create_table_with(1, *x, 2, *z);
            if (y) {
                heights[i] = *y;
                ++high;
            }
        }
        if (high != 0 && high != count) {
            return fail(f, "give every position a height or none");
        }
        shape["geometry"] = type == "Point" ? "point" : polygon ? "polygon" : "line";
        shape["points"]   = points;
        if (high != 0) {
            shape["heights"] = heights;
        }
        shapes[f] = shape;
    }
    return shapes;
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

        // `look` is not an output: it tells the viewer how to colour the terrain.
        if (*name == "look" && entry.second.is<sol::table>()) {
            const sol::table           look    = entry.second.as<sol::table>();
            const std::string          palette = look.get_or<std::string>("palette", "default");
            Graph::Look                parsed;
            if (palette == "sand") {
                parsed.palette = 1;
            } else if (palette == "coast") {
                parsed.palette = 2;
            } else if (palette == "elevation") {
                parsed.palette = 3;
            } else if (palette != "default") {
                return std::unexpected(MakeScriptError(
                    ErrorCode::ScriptError, ErrorStage::Script, location.file, location.line,
                    "look.palette '{}' is not known; expected 'default', 'sand', 'coast' or 'elevation'",
                    palette));
            }
            parsed.wind = static_cast<f32_t>(look.get_or("wind", 0.0));
            // Carried in the 15 bits above the wind: whole metres, 1 to 32767.
            const f64_t snow = look.get_or("snow", 0.0);
            if (!(snow >= 0.0 && snow <= 32767.0)) {
                return std::unexpected(MakeScriptError(
                    ErrorCode::ScriptError, ErrorStage::Script, location.file, location.line,
                    "look.snow must be an altitude between 0 and 32767 m, got {}", snow));
            }
            parsed.snow = static_cast<f32_t>(snow);
            graph.SetLook(parsed);
            continue;
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

constexpr const auto& kPreludeFiles = engine_prelude_files;

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
/// into the prelude, and `set_report(json)`, which hands the export the JSON object the prelude reports
/// (see `engine.report` in 05_common.lua). It is passed rather than made global so a script cannot call it.
[[nodiscard]] Status LoadPrelude(sol::state& state, Graph& graph) {
    sol::table engineApi = state.create_table();
    engineApi.set_function("fail", [](sol::this_state lua, const std::string& message) {
        Fail(CallerLocation(lua), "{}", message);
    });
    engineApi.set_function("set_report", [&graph](const std::string& json) { graph.SetReport(json); });

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
    if (Status prelude = LoadPrelude(state, graph); !prelude) {
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

    sol::table params = MakeParams(state, environment);
    if (!environment.shapes.empty()) {
        Result<sol::table> shapes = ReadShapes(state, environment.shapes);
        if (!shapes) {
            return std::unexpected(shapes.error());
        }
        params["shapes"] = *shapes;
    }
    const sol::protected_function_result returned = main(params);
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
