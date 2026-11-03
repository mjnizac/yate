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
| [docs/design.md](docs/design.md) | The decisions that still hold, one bullet each, with where their story is |
| [docs/archive/decisions.md](docs/archive/decisions.md) | The history: why it is the way it is, and what went wrong on the way |
| [docs/roadmap.md](docs/roadmap.md) | Medium and long-term direction |
| [docs/todo.md](docs/todo.md) | Concrete open tasks |

Start with `docs/status.md`: it is short on purpose, the current state and the known defects. Before changing a
subsystem read only its section of `docs/design.md` (`tools/dev.sh headings docs/design.md` gives the line of each),
not the whole file; open `docs/archive/decisions.md` only for the story behind one of its bullets.

`docs/archive/decisions.md`, `docs/lua_reference.md`, `docs/archive/lands_plan.md` and `docs/spec.md` run to hundreds or
thousands of lines: grep for the heading or identifier and read only that section.
