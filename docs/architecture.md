# Architecture

## Layering

```
executables        terrain_export (Headless)        terrain_viewer (Graphics, milestone 9)
                             |                                |
engine public API    engine::init / shutdown, Application, Layer, Error, log macros
                             |
engine internals     terrain/  (graph, compiler, kernels, evaluator, output writer)
                     render/   (viewer rendering)
                     vulkan/   (context, device, VMA allocator, section pool, pipelines)
                     memory/   (general, arena, pool, Tracy reporting)
                     platform/ (the only OS-specific code)
```

Everything a `.cpp` exposes beyond itself lives in a header under `include/engine/`. Internal
declarations sit inside `#ifdef IS_ENGINE`, and `IS_ENGINE` is a PRIVATE compile definition of the
engine target, so executables never see them. `src/` holds only `.cpp` files.

## Run modes

The mode is chosen once, in `AppInfo::mode`:

- **Graphics** (viewer): the GPU is selected for rendering and presentation. The main window and
  its surface exist before device selection, so presentation support can be verified.
- **Headless** (export): the GPU is selected for compute. No window system is touched.

Every system except windowing, swapchain, render passes and presentation behaves identically in
both modes.

## Startup order

`engine::init` runs these steps, logging the start and end of each one. `engine::shutdown` runs the
exact reverse.

1. Platform (`platform::Init`): host description, executable directory, main thread name.
2. Tracy: the session is named with `TracyAppInfo`.
3. Logging (`log::Init`): two asynchronous loggers, `engine` and `app`, on one worker thread.
4. Memory system (`memory::Init`): `CPU/General`, `CPU/Lua`, `CPU/Frame arena`.
5. *Graphics only:* GLFW, main window, surface. **Milestone 9.**
6. Vulkan context: instance, debug messenger, device, queues, VMA allocator, section pool,
   staging and readback rings, pipeline cache.
7. *Graphics only:* swapchain, render pass, depth buffer, framebuffers. **Milestone 9.**
8. Lua runtime. **Milestone 6.**
9. Application and its layer stack.

The host banner is logged by `engine::init`, not by `platform::Init`, because logging is not up yet
at step 1.

## Application and layers

`Application` owns an ordered stack of `Layer`s and calls `OnUpdate()` on each one per frame or
tick. `PushLayer<T>(args...)` constructs the layer inside engine-owned storage and returns `T&`.
Layers are non-copyable and non-movable.

At the end of every tick the loop resets the frame arena, refreshes the CPU memory plots, queries
the VRAM budgets and collects the Tracy GPU zones. In Graphics mode it also emits `FrameMark`.

## Where the data lives

- **CPU:** every allocation goes through the memory system. `operator new` is replaced per module
  and routed to `CPU/Untracked new`, so nothing is invisible to Tracy.
- **VRAM:** VMA is the only path to `vkAllocateMemory`. Buffers carry a category, which is also
  their Tracy pool name. Section values are sub-allocated from large per-domain blocks with VMA
  virtual blocks, in power-of-two size classes derived from the mapping, section size and halo.
  `R2` and `R3` values never share a block.

## Kernel interface

Each graph node maps to one precompiled compute kernel. Kernels take no descriptor sets: inputs
and outputs arrive as buffer device addresses inside a 128-byte push-constant block
(`vulkan::KernelPushConstants`), together with the integer section origin, the section extent, the
halo, the domain, a channel mask and a 10-word parameter block.

Workgroup sizes are fixed project-wide and come from specialization constants 0..2, so one SPIR-V
module serves both domains: 8x8x1 for `R2`, 4x4x4 for `R3`. Op variants use specialization ids 3
and up. `assets/shaders/lib/kernel.glsl` implements the GLSL side and keeps the buffer load/store
boilerplate separate from the math, so kernel fusion stays possible later.

World positions are always derived from the integer section origin plus a local offset, which is
what makes neighbouring sections agree bit-exactly at their borders.
