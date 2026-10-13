#include <engine/terrain/stream_power.hpp>

#include <engine/log.hpp>
#include <engine/memory/general.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <chrono>
#include <cstring>
#include <mutex>
#include <utility>

namespace engine::terrain {

Status StreamPowerRun(std::span<f64_t> height, std::span<const f64_t> uplift,
                      const StreamPowerParams& params);

namespace {

constexpr std::array<i32_t, 8> kDx = {-1, 0, 1, -1, 1, -1, 0, 1};
constexpr std::array<i32_t, 8> kDz = {-1, -1, -1, 0, 0, 1, 1, 1};

// The viewer re-runs a script whenever the level of detail changes, because the sample spacing is part
// of what a script sees; a simulation over the whole terrain does not depend on it, and took seconds
// every time the camera crossed a level. The last few results are kept, keyed by a hash of everything
// that goes in, so the same inputs come back at once. The hash is over the exact bits, so a cached
// result is the result.
constexpr usize_t kCacheEntries = 4;

struct CacheEntry {
    u64_t                    key = 0;
    u64_t                    check = 0;
    std::pmr::vector<f64_t>* result = nullptr;
};

std::mutex                          g_cacheMutex;
std::array<CacheEntry, kCacheEntries> g_cache{};
usize_t                             g_cacheNext = 0;

/// A second, independent hash over the same bytes. A cached result is used only when both match, so a
/// collision of the first cannot hand back another terrain.
[[nodiscard]] u64_t MixCheck(u64_t h, const void* data, usize_t bytes) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (usize_t i = 0; i < bytes; ++i) {
        h = (h ^ p[i]) * 0x9E3779B185EBCA87ull;
        h ^= h >> 29;
    }
    return h;
}

[[nodiscard]] u64_t Mix(u64_t h, const void* data, usize_t bytes) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (usize_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

[[nodiscard]] u64_t Key(std::span<const f64_t> height, std::span<const f64_t> uplift,
                        const StreamPowerParams& params) {
    u64_t h = 0xCBF29CE484222325ull;
    const f64_t numbers[] = {params.cell, params.k, params.m, params.talus, params.diffusion};
    const u32_t counts[]  = {params.width, params.depth, params.iterations, params.refresh};
    h = Mix(h, numbers, sizeof(numbers));
    h = Mix(h, counts, sizeof(counts));
    h = Mix(h, height.data(), height.size_bytes());
    return Mix(h, uplift.data(), uplift.size_bytes());
}

[[nodiscard]] u64_t Check(std::span<const f64_t> height, std::span<const f64_t> uplift,
                          const StreamPowerParams& params) {
    u64_t h = 0x84222325CBF29CE4ull;
    const f64_t numbers[] = {params.cell, params.k, params.m, params.talus, params.diffusion};
    const u32_t counts[]  = {params.width, params.depth, params.iterations, params.refresh};
    h = MixCheck(h, numbers, sizeof(numbers));
    h = MixCheck(h, counts, sizeof(counts));
    h = MixCheck(h, height.data(), height.size_bytes());
    return MixCheck(h, uplift.data(), uplift.size_bytes());
}

} // namespace

Status StreamPowerErode(std::span<f64_t> height, std::span<const f64_t> uplift,
                        const StreamPowerParams& params) {
    const u64_t key   = Key(height, uplift, params);
    const u64_t check = Check(height, uplift, params);
    {
        std::lock_guard lock(g_cacheMutex);
        for (const CacheEntry& entry : g_cache) {
            if (entry.result != nullptr && entry.key == key && entry.check == check && entry.result->size() == height.size()) {
                std::memcpy(height.data(), entry.result->data(), height.size_bytes());
                LOG_INFO("stream power: {}x{} grid, {} step(s), from the cache", params.width,
                         params.depth, params.iterations);
                return {};
            }
        }
    }
    const auto started = std::chrono::steady_clock::now();
    if (Status run = StreamPowerRun(height, uplift, params); !run) {
        return run;
    }
    LOG_INFO("stream power: {}x{} grid, {} step(s) in {:.0f} ms", params.width, params.depth,
             params.iterations,
             std::chrono::duration<f64_t, std::milli>(std::chrono::steady_clock::now() - started)
                 .count());

    std::lock_guard lock(g_cacheMutex);
    CacheEntry& slot = g_cache[g_cacheNext];
    g_cacheNext      = (g_cacheNext + 1) % kCacheEntries;
    if (slot.result == nullptr) {
        void* storage = memory::General().Allocate(sizeof(std::pmr::vector<f64_t>),
                                                   alignof(std::pmr::vector<f64_t>));
        slot.result   = ::new (storage) std::pmr::vector<f64_t>(&memory::General().Resource());
    }
    slot.key = key;
    slot.check = check;
    slot.result->assign(height.begin(), height.end());
    return {};
}

void ReleaseStreamPowerCache() noexcept {
    std::lock_guard lock(g_cacheMutex);
    for (CacheEntry& entry : g_cache) {
        if (entry.result != nullptr) {
            entry.result->~vector();
            memory::General().Free(entry.result);
            entry.result = nullptr;
        }
    }
}

Status StreamPowerRun(std::span<f64_t> height, std::span<const f64_t> uplift,
                      const StreamPowerParams& params) {
    const usize_t count = static_cast<usize_t>(params.width) * params.depth;
    if (params.width < 3 || params.depth < 3 || height.size() != count || uplift.size() != count
        || !(params.cell > 0.0) || !(params.k >= 0.0) || !(params.m >= 0.0)
        || !(params.talus >= 0.0) || !(params.diffusion >= 0.0) || params.refresh == 0) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Script,
                    "stream power needs a grid of at least 3 x 3 with height and uplift of {} values, "
                    "a cell greater than zero and k, m and talus of zero or more",
                    count);
    }

    std::pmr::memory_resource& resource = memory::General().Resource();
    std::pmr::vector<f64_t>    filled(count, &resource);
    std::pmr::vector<f64_t>    area(count, &resource);
    std::pmr::vector<u32_t>    order(&resource);
    std::pmr::vector<i32_t>    receiver(count, -1, &resource);
    std::pmr::vector<u8_t>     done(count, 0, &resource);
    std::pmr::vector<f64_t>    laplacian(params.diffusion > 0.0 ? count : 0, &resource);
    order.reserve(count);

    const i32_t width  = static_cast<i32_t>(params.width);
    const i32_t depth  = static_cast<i32_t>(params.depth);
    const f64_t cell   = params.cell;
    const f64_t diag   = cell * std::sqrt(2.0);
    const f64_t cellArea = cell * cell;
    const auto  onEdge = [&](i32_t x, i32_t z) {
        return x == 0 || z == 0 || x == width - 1 || z == depth - 1;
    };

    // A min-heap on filled height, kept in a vector of our own so it is allocated once.
    using Entry = std::pair<f64_t, u32_t>;
    std::pmr::vector<Entry> open(&resource);
    open.reserve(count);
    const auto push = [&](f64_t level, u32_t i) {
        open.emplace_back(level, i);
        std::push_heap(open.begin(), open.end(), std::greater<>{});
    };

    for (u32_t step = 0; step < params.iterations; ++step) {
        for (usize_t i = 0; i < count; ++i) {
            const i32_t x = static_cast<i32_t>(i % params.width);
            const i32_t z = static_cast<i32_t>(i / params.width);
            if (!onEdge(x, z)) {
                height[i] += uplift[i];
            }
        }

        const b8_t rebuild = step < 20 || step % params.refresh == 0;
        if (rebuild) {
            // Priority flood from the edge: `order` comes out in increasing filled height, so every cell's
            // receiver precedes it, and the small epsilon gives every pit a way out.
            std::fill(done.begin(), done.end(), u8_t{0});
            order.clear();
            open.clear();
            for (i32_t z = 0; z < depth; ++z) {
                for (i32_t x = 0; x < width; ++x) {
                    if (onEdge(x, z)) {
                        const u32_t i = static_cast<u32_t>(z * width + x);
                        filled[i]     = height[i];
                        done[i]       = 1;
                        push(filled[i], i);
                    }
                }
            }
            while (!open.empty()) {
                std::pop_heap(open.begin(), open.end(), std::greater<>{});
                const auto [level, i] = open.back();
                open.pop_back();
                order.push_back(i);
                const i32_t x = static_cast<i32_t>(i % params.width);
                const i32_t z = static_cast<i32_t>(i / params.width);
                for (usize_t d = 0; d < 8; ++d) {
                    const i32_t xx = x + kDx[d];
                    const i32_t zz = z + kDz[d];
                    if (xx < 0 || zz < 0 || xx >= width || zz >= depth) {
                        continue;
                    }
                    const u32_t q = static_cast<u32_t>(zz * width + xx);
                    if (done[q] == 0) {
                        done[q]   = 1;
                        filled[q] = std::max(height[q], level + 1e-6);
                        push(filled[q], q);
                    }
                }
            }

            // Steepest descent on the filled surface, and drainage area accumulated from the top down.
            for (usize_t i = 0; i < count; ++i) {
                const i32_t x = static_cast<i32_t>(i % params.width);
                const i32_t z = static_cast<i32_t>(i / params.width);
                receiver[i]   = -1;
                area[i]       = cellArea;
                if (onEdge(x, z)) {
                    continue;
                }
                f64_t best = 0.0;
                for (usize_t d = 0; d < 8; ++d) {
                    const u32_t q    = static_cast<u32_t>((z + kDz[d]) * width + (x + kDx[d]));
                    const f64_t drop = (filled[i] - filled[q]) / (kDx[d] != 0 && kDz[d] != 0 ? diag : cell);
                    if (drop > best) {
                        best        = drop;
                        receiver[i] = static_cast<i32_t>(q);
                    }
                }
            }
            for (usize_t n = order.size(); n-- > 0;) {
                const u32_t i = order[n];
                if (receiver[i] >= 0) {
                    area[static_cast<usize_t>(receiver[i])] += area[i];
                }
            }

        }

        // Depressions fill with sediment up to their outlet. Left as lakes they never incise, and a
        // pit that uplift keeps raising becomes a flat plateau with no valley in it.
        if (rebuild) {
            for (usize_t i = 0; i < count; ++i) {
                height[i] = std::max(height[i], filled[i]);
            }
        }

        // Implicit stream-power step, outlets first. A cell still not above its receiver is left
        // alone; after the filling above that is only a cell on a flat, which has nothing to erode.
        for (const u32_t i : order) {
            if (receiver[i] < 0) {
                continue;
            }
            const usize_t r = static_cast<usize_t>(receiver[i]);
            if (height[i] <= height[r]) {
                continue;
            }
            const i32_t dx       = static_cast<i32_t>(i % params.width) - static_cast<i32_t>(r % params.width);
            const i32_t dz       = static_cast<i32_t>(i / params.width) - static_cast<i32_t>(r / params.width);
            const f64_t distance = dx != 0 && dz != 0 ? diag : cell;
            const f64_t factor   = params.k * std::pow(area[i], params.m) / distance;
            height[i]            = (height[i] + factor * height[r]) / (1.0 + factor);
        }

        // Soil creep: explicit diffusion, with the coefficient clamped to stay stable.
        if (params.diffusion > 0.0) {
            const f64_t rate = std::min(params.diffusion / cellArea, 0.2);
            for (i32_t z = 1; z < depth - 1; ++z) {
                for (i32_t x = 1; x < width - 1; ++x) {
                    const usize_t i = static_cast<usize_t>(z * width + x);
                    laplacian[i]    = height[i - 1] + height[i + 1] + height[i - params.width]
                                   + height[i + params.width] - 4.0 * height[i];
                }
            }
            for (i32_t z = 1; z < depth - 1; ++z) {
                for (i32_t x = 1; x < width - 1; ++x) {
                    const usize_t i = static_cast<usize_t>(z * width + x);
                    height[i] += rate * laplacian[i];
                }
            }
        }

        // Hillslopes: nothing steeper than the talus, settled in the same order so a collapse
        // propagates uphill in one pass.
        if (params.talus > 0.0) {
            for (const u32_t i : order) {
                const i32_t x = static_cast<i32_t>(i % params.width);
                const i32_t z = static_cast<i32_t>(i / params.width);
                if (onEdge(x, z)) {
                    continue;
                }
                for (usize_t d = 0; d < 8; ++d) {
                    const usize_t q     = static_cast<usize_t>((z + kDz[d]) * width + (x + kDx[d]));
                    const f64_t   limit = params.talus * (kDx[d] != 0 && kDz[d] != 0 ? diag : cell);
                    height[i]           = std::min(height[i], height[q] + limit);
                }
            }
        }
    }
    return {};
}

} // namespace engine::terrain
