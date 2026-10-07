#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <engine/error.hpp>
#    include <engine/terrain/export.hpp>
#    include <engine/terrain/graph.hpp>

#    include <string_view>

/// Lua runtime entry points.
///
/// Nothing here mentions Lua or Sol2 types. The whole runtime lives in `src/lua.cpp`, which is the
/// only translation unit that includes Sol2, so its compile cost is paid once and no Sol2 type ever
/// reaches a header (spec section 8).
namespace engine::lua {

/// What a script's `main(params)` receives, besides the user parameters.
struct ScriptEnvironment {
    u64_t seed       = 0;
    f64_t minX       = 0.0;
    f64_t minY       = 0.0;
    f64_t minZ       = 0.0;
    f64_t maxX       = 0.0;
    f64_t maxY       = 0.0;
    f64_t maxZ       = 0.0;
    f64_t resolution = 1.0;
    /// User parameters, exposed to the script as named fields of `params`.
    const terrain::Params* params = nullptr;
};

/// Loads `path`, calls its `main(params)` and fills `graph` with the nodes and outputs it declared.
///
/// The Lua state is created and destroyed inside this call, so a reload is just another call and the
/// engine never holds a half-torn-down interpreter. Every allocation the runtime makes goes through
/// the `CPU/Lua` allocator.
///
/// Errors come back as `file:line: [script] message` with the script's own line, because every
/// binding reads the calling line before it touches the graph.
[[nodiscard]] Status RunScript(terrain::Graph& graph, std::string_view path,
                              const ScriptEnvironment& environment);

/// Bytes the Lua runtime holds right now, for the metadata sidecar and the memory plots.
[[nodiscard]] u64_t AllocatedBytes();

} // namespace engine::lua

#endif // IS_ENGINE
