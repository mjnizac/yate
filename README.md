# Terrain engine

GPU-driven terrain generation engine in C++23 and Vulkan 1.4.

Lua scripts describe terrain as a lazy graph of operations — noises, erosion, masks, combinators.
The engine compiles that graph into a sequence of Vulkan compute dispatches, evaluates it section by
section and exports 16-bit PNG heightmaps with a metadata sidecar. The export path is the product;
the interactive viewer is a tool for iterating on it.

Everything is measured: all CPU time, GPU time, CPU memory and VRAM goes through Tracy, with no
allocation, buffer or compute pass invisible to the profiler.

## State

Milestones 1 to 3 are done and verified on hardware: build system, instrumented memory system, and a
headless Vulkan 1.4 context whose compute kernels write section buffers that read back correctly.
Export, the graph compiler, the Lua bindings, streaming, erosion and the viewer are next.

`docs/status.md` is the authoritative list.

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
| [docs/lua_api.md](docs/lua_api.md) | Script API reference |
| [docs/status.md](docs/status.md) | Current state, milestone by milestone |
| [docs/roadmap.md](docs/roadmap.md) | Medium and long-term direction |
| [docs/todo.md](docs/todo.md) | Concrete open tasks |
