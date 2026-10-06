# Terrain engine

GPU-driven terrain generation engine. Lua scripts describe terrain as a lazy graph of operations;
the engine compiles that graph into Vulkan compute dispatches, evaluates it section by section and
exports 16-bit PNG heightmaps with a metadata sidecar.

The export path is the product. The viewer is a tool for iterating on it.

| Where | What |
| --- | --- |
| [docs/architecture.md](docs/architecture.md) | Layering and runtime flow |
| [docs/conventions.md](docs/conventions.md) | Code conventions |
| [docs/commands.md](docs/commands.md) | How to configure, build, run and test |
| [docs/lua_api.md](docs/lua_api.md) | Script API reference |
| [docs/status.md](docs/status.md) | Current state, milestone by milestone |
| [docs/roadmap.md](docs/roadmap.md) | Medium and long-term direction |
| [docs/todo.md](docs/todo.md) | Concrete open tasks |

Start with `docs/status.md`: it says which milestones are done and what the next one is.
