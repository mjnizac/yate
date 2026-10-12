# Terrain engine

GPU-driven terrain generation engine in C++23 and Vulkan 1.4.

Lua scripts describe terrain as a lazy graph of operations — noises, erosion, masks, combinators.
The engine compiles that graph into a sequence of Vulkan compute dispatches, evaluates it section by
section and exports 16-bit PNG heightmaps with a metadata sidecar. The export path is the product;
the interactive viewer is a tool for iterating on it.

Everything is measured: all CPU time, GPU time, CPU memory and VRAM goes through Tracy, with no
allocation, buffer or compute pass invisible to the profiler.

## State

All nine milestones are done: build system, instrumented memory, a headless Vulkan 1.4 context, the
export path, the graph compiler, the Lua bindings, sectioned streaming, iterative erosion kernels and
an interactive viewer. Both configurations build warning-free and `ctest` passes 12 of 12 in each.

A 16384x16384 export is byte-identical across repeated runs and across section sizes, which is the
engine's one firm claim.

`docs/status.md` is the authoritative state; `docs/todo.md` is what is still open, and there is plenty.

## Build

Needs a C++23 compiler, CMake 3.25+, Ninja, the Vulkan SDK 1.4.x and a GPU exposing Vulkan 1.4.
Everything else is fetched by CMake, pinned to release tags.

```bash
./build.cmd Debug
```

```bash
cd build/Debug && ctest --output-on-failure
```

`docs/commands.md` covers the options, the CLI and the profiler.

## Documentation

| Where | What |
| --- | --- |
| [docs/spec.md](docs/spec.md) | The system specification this is built to |
| [docs/architecture.md](docs/architecture.md) | Layering and runtime flow |
| [docs/conventions.md](docs/conventions.md) | Code conventions |
| [docs/commands.md](docs/commands.md) | Configure, build, run, test, profile |
| [docs/lua_api.md](docs/lua_api.md) | Script API index |
| [docs/lua_reference.md](docs/lua_reference.md) | Every op, parameter by parameter, with pictures |
| [docs/status.md](docs/status.md) | Where the engine is right now |
| [docs/decisions.md](docs/decisions.md) | Why it is the way it is, and what went wrong on the way |
| [docs/roadmap.md](docs/roadmap.md) | Medium and long-term direction |
| [docs/todo.md](docs/todo.md) | Concrete open tasks |
