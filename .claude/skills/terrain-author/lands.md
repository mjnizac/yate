## Islands, coasts and any land and sea: `Lands.Compose`

Read from `SKILL.md` when the request is about land and sea: islands, archipelagos, continents, coasts.

Shapes first, detail after. Translate the request into elements, never into noise:

| The request | The composition |
| --- | --- |
| "N islands of A to B km2" | `{ id = "isles", shape = "radial", profile = ..., scatter = { count = N, area = { A, B }, spacing = ... } }`, or `Lands.Archipelago{ preset = ..., count = N, area = { A, B } }`; they are `isles/1`..`isles/N` |
| "grouped", "in a chain", "on an arc", "here and here" | `pattern = "cluster"`, `"line"` (with `decay` for a hotspot), `"arc"`, `"centers"` |
| "a third of the map is land" | `land_fraction = 0.33` (bounded) or a scatter's `count` per world cell (unbounded) |
| "make island X bigger" | `edit = { X = { scale = 1.15 } }` (area x 1.32), or `{ area = ... }` |
| "move it", "remove it" | `edit = { X = { offset = { dx, dz } } }`, `remove = { "X" }` |
| "cliffs higher than the interior" | `profile = { "rim", inner = ... }`; a flat cay `"flat"`, a mesa `"plateau"`, a volcano `"cone"`, a caldera `"caldera"`, a cuesta `{ "tilt", direction = ... }` |
| "a bay", "a strait", "a lake" | a water element: `sector`/`ellipse`, `capsule`/`band`, `circle` with `role = "lake"` |
| "all land", "no land" | a `fill` element (cut lakes into it); no land elements |
| "a lagoon" | a water element with `role = "lagoon"` |
| "seamount", "guyot", "trench" | `seabed = true` with `cone`/`plateau`; `margin = "active"` on the island |
| "anywhere in the world" | `frame = { world_cell = 16000, region = ... }` and absolute positions |
| "a continent", "a supercontinent", "an inland sea", "a peninsula", "an isthmus", "a closed basin", "lakes everywhere" | `Lands.Continent{ preset = ... }`: `continent`, `pangea`, `old_world`, `mediterranean`, `inland_sea`, `peninsula`, `isthmus`, `fractious`, `taklamakan`, `all_land`, `lakeland`, `ocean_only` |
| "an island like these", "an archipelago", "a continent" judged by its outline | the presets with an Azgaar template draw it by default (`Lands.Template`: his generator, then bend, fjords and fray); `outline = "shapes"` for the composition with ids and `edit`; `land_fraction` sets the sea level; `Lands.Template{ steps = [[Hill 1 90-100 44-56 40-60 ...]] }` (his syntax, one step a line) for a template of your own |
| "a range N m high", "with spurs every S m", "two passes" | `{ id = "range", ridge = { points = ..., width = W, height = N, spacing = S, saddles = 2 } }`; its pieces are `range/crest/K`, `range/spur/K` |
| "a massif", "a basin" | `relief = true` with a profile (a negative `dome` for a basin): it changes no coast |
| "here, a third of the way across" | in a bounded map, `at = { "33%", "50%" }`; `frame.edge_margin` keeps scatters off the edge |

Start from `Lands.Preset{ name = ... }` when one fits, change its table, build it. Water bodies come out classed in
`land.info.waters` (`sea`, `lake`, `ocean`, or the element's `role`). Then check before saying
it is done: `tools/dev.sh lands-check <script> --bounds ... --expect count=N,area=A..B,land=F..G` exports
it and compares what each id measured with what was asked; `tools/dev.sh lands` prints the resolved
elements (`script_report.lands` in `metadata.json`) next to the measurements. Give each new example a
header with its viewer and export commands at a resolution suited to its size, and a golden in
`tests/datasets/lands_<name>/`.

A low land under a cliffed coast must stand above the cliff (`cliff_height`) and rise within the distance the
cliff fades in (`size * 0.01`), or a closed trough runs behind the coast; check the pits of every new composition
(`terrain_metrics <dir> --level 0 --pits pits.png`) and review it at the seed its golden uses.
