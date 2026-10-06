# Code conventions

## Language and portability

- C++23, no compiler extensions in shared code. Integer aliases (`u32_t`, `f32_t`, `usize_t`, …)
  come from `engine/common.hpp` and are `static_assert`ed on size.
- Include paths match real casing, so the project builds on case-sensitive filesystems.
- OS-specific code lives only in `src/platform/platform.cpp`. The single exception is
  `ENGINE_DEBUG_BREAK` in `engine/assert.hpp`.

## Header placement

- Anything used outside the `.cpp` that implements it goes in a header under `include/engine/`,
  public API and engine internals alike.
- Internal declarations are wrapped in `#ifdef IS_ENGINE`. Mixed headers put the public part first
  and a single internal block at the end.
- A public declaration never depends on a type declared only inside an `IS_ENGINE` block. Where the
  public API must reference engine state, it does so through a publicly declared incomplete type —
  see `engine::ApplicationState`.
- Third-party headers needed only by internal declarations (Vulkan, VMA, Tracy, spdlog, mimalloc)
  are included inside the guard, so consumers do not need those include paths.

## Include order

Each `.cpp` starts with the header it implements, then — separated by blank lines — other engine
headers (`<engine/...>`, always angle brackets), third-party headers, standard library headers.

## Errors

- Fallible operations return `Result<T>` or `Status`, i.e. `std::expected<T, Error>`.
- `Error` stores its message and script path in fixed inline buffers, so it never owns heap memory
  and can cross the shared-library boundary without violating the module boundary rule.
- Every error renders as `file:line: [stage] message`, or `[stage] message` without a location.
  That one format is what the viewer panel, stderr and the JSON log all print.
- `ENGINE_FAIL(code, stage, fmt, ...)` returns an `std::unexpected` from the current function.
- Invariants use `ENGINE_ASSERT` (debug only), `ENGINE_ASSERT_RETURN` and `ENGINE_ASSERT_X`.
- Vulkan calls use `VK_CHECK`, `VK_CHECK_RETURN`, `VK_CHECK_X`, or `VK_TRY` when the enclosing
  function returns a `Result`/`Status` and the `VkResult` should become the error.
- Never throw across a C boundary. The Vulkan debug messenger, the Lua allocator and GLFW callbacks
  log and return normally.

## Memory

- Engine code never calls `malloc`, `free`, `new` or `delete`. Allocations come from the memory
  system; placement construction into engine-owned storage is how objects are built.
- `std::pmr` containers are used in engine systems, backed by an allocator's `Resource()`.
- Memory allocated inside a module is freed inside that module, which is why
  `src/memory/new_delete.cpp` is compiled into the engine *and* into every executable and test.
- Every allocator asserts `used == 0` on destruction and logs its name, live count and bytes on
  failure.

## Nullability and ownership

References mean non-null. A raw pointer means "may be null" and is checked. Owning raw pointers are
forbidden: ownership is RAII, with explicit move-only wrappers for Vulkan handles.

A movable owner that others point into must re-point them on move. `vulkan::Context` does this for
the section pool and the rings through `Rebind`, because they hold an `Allocator*`.

## Profiling

Every non-trivial function on a hot or long-running path opens a Tracy zone, and every GPU pass
opens a Tracy GPU zone. Log entries are mirrored into the timeline with `TracyMessage`.

Tracy identifies a memory pool by the *pointer* of its name, so derived names
(`"<name> [reserved]"`, `"<name> unused"`) are interned once and never released. Pool objects for
VRAM categories are process-wide function-local statics for the same reason.

## Naming

`PascalCase` for types and functions, `camelCase` for variables and struct fields, `m_camelCase`
for private members, `k` prefix for constants, `g_` prefix for file-scope statics. Code, comments
and log messages are in English.

## Commits

One commit per concrete step, Conventional Commits, and never amend: a bug found after a commit is
fixed in a new `fix:` commit that names the commit which introduced it. Every commit builds and
passes the existing tests, and a commit that changes a milestone's state updates `docs/status.md`
and `docs/todo.md`.
