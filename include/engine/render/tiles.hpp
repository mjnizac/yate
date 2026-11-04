#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <engine/render/renderer.hpp>
#    include <engine/terrain/compiler.hpp>
#    include <engine/terrain/evaluator.hpp>
#    include <engine/terrain/graph.hpp>
#    include <engine/vulkan/context.hpp>
#    include <engine/memory/general.hpp>

#    include <array>
#    include <memory_resource>
#    include <span>
#    include <string_view>
#    include <vector>

namespace engine::terrain {
class Params;
}

// The viewer's tiles: a script loaded into a graph, compiled for a level, and evaluated a batch of tiles at a
// time into section-pool slots. `ViewerLayer` (renderer.cpp) decides which tiles and draws them.
namespace engine::render {

/// Index of an output the script does not have.
constexpr usize_t kNoOutput = static_cast<usize_t>(-1);

/// Which piece of terrain a tile holds.
///
/// Coordinates are in tile units *at its level*, so the same `x` and `z` mean a different patch of world
/// at a different level. The level is part of the key for exactly that reason.
struct TileKey {
    i32_t x     = 0;
    i32_t z     = 0;
    u32_t level = 0;

    [[nodiscard]] b8_t operator==(const TileKey&) const noexcept = default;
};

/// One evaluated piece of terrain, resident on the GPU.
///
/// Height and normals live in their own section-pool slots rather than in the evaluator's reusable
/// buffers, because the evaluator reuses one set for every section and the viewer needs them all at once.
struct Tile {
    TileKey             key;
    vulkan::SectionSlot height;
    vulkan::SectionSlot normals;
    /// World position of the tile's first sample.
    std::array<f32_t, 3> origin{};
    /// Metres between samples at this tile's level.
    f32_t spacing = 1.0f;
    u32_t samples = 0;
    /// The cache's generation it was evaluated in (see `TileCache`).
    u64_t generation = 0;
    /// Its normals slot carries the shown output as a fourth plane after the normals.
    b8_t shown = false;
};

/// A graph the script produced, plus what the viewer needs to draw it.
struct LoadedGraph {
    terrain::Graph graph;
    /// Index of the height output and of the normals output in the graph's output list.
    usize_t heightOutput  = 0;
    usize_t normalsOutput = 0;
    /// The `lakes` and `rivers` outputs, `kNoOutput` where the script has none.
    std::array<usize_t, 3> waterOutputs{kNoOutput, kNoOutput, kNoOutput};
    f32_t   heightMinimum = 0.0f;
    f32_t   heightMaximum = 0.0f;

    /// Empties it for reuse. A `Graph` is neither copyable nor movable, by design: it owns
    /// fixed-capacity node storage and handles point into it by index, so assigning one would quietly
    /// invalidate every handle anyone still held. Clearing in place is the only way to reuse it, and the
    /// only way that keeps the 150 KiB of storage allocated once instead of per reload.
    void Reset() {
        graph.Clear();
        heightOutput  = 0;
        normalsOutput = 0;
        waterOutputs  = {kNoOutput, kNoOutput, kNoOutput};
        heightMinimum = 0.0f;
        heightMaximum = 0.0f;
    }
};

/// Everything needed to evaluate one tile, rebuilt on a load and on a level change.
///
/// The compiled graph, the evaluator's shared buffers and the query pool used to live inside the function
/// that built the whole preview, because the whole preview was built at once. Tiles are now evaluated a
/// couple at a time as the camera moves, so these have to outlive a frame.
struct Terrain {
    /// `CompiledGraph` has no default constructor: its vectors need a memory resource, which is the
    /// engine's way of making sure nothing allocates from the global default by accident. So this one
    /// names the resource too, and is assigned from a compile rather than built empty.
    terrain::CompiledGraph    compiled{memory::General().Resource()};
    terrain::SectionResources resources;
    terrain::DispatchTimers   timers;
    u32_t                     samples = 0;
    /// Level this was compiled for, and the sample spacing that implies.
    u32_t level   = 0;
    f32_t spacing = 1.0f;
    /// Index into `compiled.outputs`.
    usize_t heightOutput  = 0;
    usize_t normalsOutput = 0;
    /// The script's `look`, packed for the push constants.
    u32_t look = 0;
    /// Halo each output buffer carries, which is not always zero: a value some other op reads with a
    /// radius keeps that halo even when it is also a requested output.
    u32_t heightHalo  = 0;
    u32_t normalsHalo = 0;
    /// The `lakes` and `rivers` outputs, `kNoOutput` where the script has none. Drawn from the height
    /// slot, which carries them after the height (see `EvaluateTile`).
    std::array<usize_t, 3> waterOutputs{kNoOutput, kNoOutput, kNoOutput};
    /// The output painted over the terrain (`ViewerLayer::Settings::show`), `kNoOutput` for none, and
    /// the range the script declared for it. Drawn from the normals slot, a fourth plane after the three
    /// components: the slot's power-of-two size class has room for it.
    usize_t shownOutput  = kNoOutput;
    f32_t   shownMinimum = 0.0f;
    f32_t   shownMaximum = 1.0f;
    /// Bytes one tight tile takes, with no halo.
    u64_t heightBytes   = 0;
    u64_t normalsBytes  = 0;
    f32_t heightMinimum = 0.0f;
    f32_t heightMaximum = 0.0f;
    b8_t  valid         = false;
};

/// The resident tiles.
struct TileCache {
    std::pmr::vector<Tile> tiles{&memory::General().Resource()};
    u64_t                  evaluated = 0;
    /// Bumped by a reload: tiles of an older generation are stale, drawn until a fresh one of the same
    /// key replaces them, so a reload never empties the screen nor stops it while the ring is refilled.
    u64_t generation = 0;

    void Release(vulkan::Context& context) {
        for (Tile& tile : tiles) {
            context.Sections().Release(tile.height);
            context.Sections().Release(tile.normals);
        }
        tiles.clear();
    }

    /// Holds a fresh tile of this key (a stale one is still to be evaluated again).
    [[nodiscard]] b8_t Holds(const TileKey& key) const {
        for (const Tile& tile : tiles) {
            if (tile.key == key && tile.generation == generation) {
                return true;
            }
        }
        return false;
    }

    /// Drops the stale tiles of this key, now that a fresh one is in.
    void Retire(vulkan::Context& context, const TileKey& key) {
        for (usize_t i = tiles.size(); i-- > 0;) {
            if (tiles[i].key == key && tiles[i].generation != generation) {
                context.Sections().Release(tiles[i].height);
                context.Sections().Release(tiles[i].normals);
                tiles.erase(tiles.begin() + static_cast<isize_t>(i));
            }
        }
    }
};

/// Sample spacing of a level. Doubling per level, so a level's lattice is a subset of the one below it.
[[nodiscard]] inline f32_t SpacingOf(f64_t baseResolution, u32_t level) {
    return static_cast<f32_t>(baseResolution) * static_cast<f32_t>(1u << level);
}

/// Runs the script into `loaded` and resolves its outputs. CPU only, safe on a worker thread.
[[nodiscard]] Status LoadScript(LoadedGraph& loaded, std::string_view path, std::string_view shapes,
                                u64_t seed, const terrain::Params& params, f64_t resolution,
                                const f32_t (&centre)[2], f32_t half);

/// Compiles `loaded` for `level` and creates the evaluator's resources for it.
[[nodiscard]] Result<Terrain> BuildTerrain(vulkan::Context& context, const LoadedGraph& loaded,
                                           const ViewerLayer::Settings& settings, u32_t level);

/// Evaluates `keys` into freshly acquired slots, appended to `out`, in one submit.
[[nodiscard]] Status EvaluateTiles(vulkan::Context& context, terrain::KernelLibrary& kernels, Terrain& built,
                                   std::span<const TileKey> keys, u32_t seed, std::pmr::vector<Tile>& out);

} // namespace engine::render

#endif
