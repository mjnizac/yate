# Roadmap

## Near term

Milestones 4 to 9 in order, as listed in `docs/status.md`. Each one ends with its acceptance
criterion met and this file plus `status.md` and `todo.md` updated.

The order is deliberate: the export path is the product, so it becomes real before the graph gets
interesting, and the viewer comes last because it is a tool for iterating on an export path that
already works.

## Kernel fusion

Postponed on purpose. One node is one dispatch today, which means one intermediate buffer per node
and a buffer memory barrier between dependent dispatches. Fusion would merge chains of pointwise
nodes into one shader generated and compiled at runtime.

It will only be built if measurements on real scripts justify it. The numbers to collect first:

- GPU time in pointwise dispatches versus total GPU time per section.
- Intermediate VRAM per section, and how many sections in flight that costs.
- Memory bandwidth per section, from the dispatch timings and the buffer sizes.

The design keeps the door open: pointwise math lives in reusable functions inside
`assets/shaders/lib/`, separate from the buffer load/store boilerplate, and the compiler keeps
classification and halo propagation as separate stages so fusion can be inserted between them and
scheduling.

## Further out

- **Async compute.** A dedicated compute family is already preferred during device selection and
  queues already signal timeline values, so overlapping evaluation with rendering is a scheduling
  change rather than an architectural one.
- **R3 volumes end to end.** The mapping system, the size classes and the kernel interface already
  cover `R3`; what is missing is the brick iteration, the raw `f32` writer and ops that are actually
  interesting in three dimensions.
- **More erosion models.** Hydraulic and thermal first (milestone 8). Anything further has to
  declare an honest effective influence radius, or sections stop being seamless.
- **Distributed export.** Sections are already independent and deterministic, so splitting a job
  across machines is an orchestration problem. Worth doing only once single-machine throughput is
  measured and understood.

## Non-goals

- Runtime shader compilation outside the fusion experiment above.
- A general-purpose node editor. Scripts are the authoring surface.
- Dynamic rendering. The viewer uses the classic render pass path on purpose, so pipelines stay
  render-pass compatible.
