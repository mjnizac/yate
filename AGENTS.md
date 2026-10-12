# Terrain engine

GPU-driven terrain generation engine. Lua scripts describe terrain as a lazy graph of operations;
the engine compiles that graph into Vulkan compute dispatches, evaluates it section by section and
exports 16-bit PNG heightmaps with a metadata sidecar.

The export path is the product. The viewer is a tool for iterating on it.

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

Start with `docs/status.md`. It is short on purpose: current state, the numbers that matter and the
known defects. Read `docs/decisions.md` only when you need to know *why* something is the way it is —
it is the long history and it is not meant to be read front to back.
