#include <engine/terrain/wind.hpp>

#include <engine/log.hpp>
#include <engine/memory/general.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <memory_resource>

namespace engine::terrain {

namespace {

// The viewer re-runs a script on every level change; the same inputs come back from here at once.
constexpr usize_t kCacheEntries = 4;

struct CacheEntry {
    u64_t                    key    = 0;
    std::pmr::vector<f64_t>* result = nullptr;
};

std::mutex                            g_cacheMutex;
std::array<CacheEntry, kCacheEntries> g_cache{};
usize_t                               g_cacheNext = 0;

[[nodiscard]] u64_t Mix(u64_t h, const void* data, usize_t bytes) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (usize_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

[[nodiscard]] u64_t Key(std::span<const f64_t> height, const WindParams& params) {
    u64_t       h         = 0xCBF29CE484222325ull;
    const f64_t numbers[] = {params.cell,   params.slab,   params.pSand, params.pBare,
                             params.shadow, params.repose, params.creep};
    const u32_t counts[]  = {params.width, params.depth, params.iterations, params.hop, params.seed,
                             params.smooth};
    h                     = Mix(h, numbers, sizeof(numbers));
    h                     = Mix(h, counts, sizeof(counts));
    return Mix(h, height.data(), height.size_bytes());
}

class Simulation {
public:
    Simulation(std::span<f64_t> height, const WindParams& params)
        : m_width(static_cast<i32_t>(params.width)),
          m_depth(static_cast<i32_t>(params.depth)),
          m_h(height.size(), &memory::General().Resource()),
          m_params(params),
          m_state(0x9E3779B97F4A7C15ull ^ (static_cast<u64_t>(params.seed) * 0xBF58476D1CE4E5B9ull)) {
        for (usize_t i = 0; i < height.size(); ++i) {
            m_h[i] = static_cast<i32_t>(std::max(0.0, std::round(height[i] / params.slab)));
            m_highest = std::max(m_highest, m_h[i]);
        }
        const f64_t perCell = params.cell / params.slab;
        m_shadow            = std::tan(params.shadow * 0.017453292519943295) * perCell;
        m_repose            = std::tan(params.repose * 0.017453292519943295) * perCell;
    }

    void Run() {
        const u64_t picks = static_cast<u64_t>(m_params.iterations) * m_h.size();
        for (u64_t p = 0; p < picks; ++p) {
            const i32_t x = static_cast<i32_t>(Next() % static_cast<u64_t>(m_width));
            const i32_t z = static_cast<i32_t>(Next() % static_cast<u64_t>(m_depth));
            const usize_t i = Index(x, z);
            if (m_h[i] <= 0 || InShadow(x, z)) {
                continue;
            }
            --m_h[i];
            Relax(x, z);
            // Reptation: a short creep to the next cell downwind instead of a saltation hop.
            const b8_t creep = Uniform() < m_params.creep;
            i32_t      at    = x;
            for (u32_t hops = 0; hops < 256; ++hops) {
                at = Wrap(at + (creep ? 1 : static_cast<i32_t>(m_params.hop)), m_width);
                const usize_t j = Index(at, z);
                const f64_t   p_ = m_h[j] > 0 ? m_params.pSand : m_params.pBare;
                if (creep || InShadow(at, z) || Uniform() < p_) {
                    ++m_h[j];
                    m_highest = std::max(m_highest, m_h[j]);
                    Relax(at, z);
                    break;
                }
            }
        }
    }

    void Write(std::span<f64_t> height) const {
        for (usize_t i = 0; i < height.size(); ++i) {
            height[i] = m_h[i] * m_params.slab;
        }
        // Centre-weighted (1 2 1)^2 / 16, wrapping like the simulation.
        std::pmr::vector<f64_t> copy(height.size(), &memory::General().Resource());
        for (u32_t pass = 0; pass < m_params.smooth; ++pass) {
            std::copy(height.begin(), height.end(), copy.begin());
            for (i32_t z = 0; z < m_depth; ++z) {
                for (i32_t x = 0; x < m_width; ++x) {
                    f64_t sum = 0.0;
                    for (i32_t dz = -1; dz <= 1; ++dz) {
                        for (i32_t dx = -1; dx <= 1; ++dx) {
                            sum += copy[Index(x + dx, z + dz)] * ((dx == 0 ? 2 : 1) * (dz == 0 ? 2 : 1));
                        }
                    }
                    height[Index(x, z)] = sum / 16.0;
                }
            }
        }
    }

private:
    [[nodiscard]] static i32_t Wrap(i32_t v, i32_t n) { return ((v % n) + n) % n; }
    [[nodiscard]] usize_t Index(i32_t x, i32_t z) const {
        return static_cast<usize_t>(Wrap(z, m_depth)) * static_cast<usize_t>(m_width)
               + static_cast<usize_t>(Wrap(x, m_width));
    }
    [[nodiscard]] u64_t Next() {
        m_state ^= m_state << 13;
        m_state ^= m_state >> 7;
        m_state ^= m_state << 17;
        return m_state;
    }
    [[nodiscard]] f64_t Uniform() { return static_cast<f64_t>(Next() >> 11) * (1.0 / 9007199254740992.0); }

    // In the shadow of the sand upwind: some cell `d` cells upwind stands more than `d` times the
    // shadow slope above this one. Nothing is that high further than the tallest stack allows.
    [[nodiscard]] b8_t InShadow(i32_t x, i32_t z) const {
        const i32_t here = m_h[Index(x, z)];
        const i32_t reach =
            std::min(m_width - 1, static_cast<i32_t>((m_highest - here) / std::max(m_shadow, 1e-6)) + 1);
        for (i32_t d = 1; d <= reach; ++d) {
            if (m_h[Index(x - d, z)] - here > m_shadow * d) {
                return true;
            }
        }
        return false;
    }

    // Slabs slide wherever a neighbour is lower by more than the angle of repose allows.
    void Relax(i32_t x0, i32_t z0) {
        m_stack.clear();
        m_stack.push_back({x0, z0});
        u32_t guard = 0;
        while (!m_stack.empty() && guard++ < 4096) {
            const auto [x, z] = m_stack.back();
            m_stack.pop_back();
            const usize_t i = Index(x, z);
            for (i32_t dz = -1; dz <= 1; ++dz) {
                for (i32_t dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dz == 0) {
                        continue;
                    }
                    const usize_t j     = Index(x + dx, z + dz);
                    const f64_t   limit = m_repose * (dx != 0 && dz != 0 ? 1.41421356 : 1.0);
                    if (m_h[i] - m_h[j] > limit) {
                        --m_h[i];
                        ++m_h[j];
                        m_stack.push_back({x, z});
                        m_stack.push_back({x + dx, z + dz});
                    } else if (m_h[j] - m_h[i] > limit) {
                        --m_h[j];
                        ++m_h[i];
                        m_stack.push_back({x, z});
                        m_stack.push_back({x + dx, z + dz});
                    }
                }
            }
        }
    }

    i32_t                                   m_width;
    i32_t                                   m_depth;
    std::pmr::vector<i32_t>                 m_h;
    WindParams                              m_params;
    u64_t                                   m_state;
    f64_t                                   m_shadow  = 1.0;
    f64_t                                   m_repose  = 2.0;
    i32_t                                   m_highest = 0;
    std::pmr::vector<std::array<i32_t, 2>>  m_stack{&memory::General().Resource()};
};

} // namespace

Status WindErode(std::span<f64_t> height, const WindParams& params) {
    const usize_t count = static_cast<usize_t>(params.width) * params.depth;
    if (params.width < 8 || params.depth < 8 || height.size() != count || !(params.cell > 0.0)
        || !(params.slab > 0.0) || params.hop == 0 || !(params.pSand >= 0.0 && params.pSand <= 1.0)
        || !(params.pBare >= 0.0 && params.pBare <= 1.0) || !(params.shadow > 0.0)
        || !(params.repose > params.shadow) || !(params.creep >= 0.0 && params.creep <= 1.0)) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Script,
                    "wind needs a grid of at least 8 x 8 with {} heights, cell and slab greater than "
                    "zero, a hop of at least 1, probabilities in [0, 1] and repose above shadow",
                    count);
    }
    const u64_t key = Key(height, params);
    {
        std::lock_guard lock(g_cacheMutex);
        for (const CacheEntry& entry : g_cache) {
            if (entry.result != nullptr && entry.key == key && entry.result->size() == count) {
                std::memcpy(height.data(), entry.result->data(), height.size_bytes());
                LOG_INFO("wind: {}x{} grid, from the cache", params.width, params.depth);
                return {};
            }
        }
    }
    const auto started = std::chrono::steady_clock::now();
    Simulation simulation(height, params);
    simulation.Run();
    simulation.Write(height);
    LOG_INFO("wind: {}x{} grid, {} pick(s) per cell in {:.0f} ms", params.width, params.depth,
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
    slot.result->assign(height.begin(), height.end());
    return {};
}

void ReleaseWindCache() noexcept {
    std::lock_guard lock(g_cacheMutex);
    for (CacheEntry& entry : g_cache) {
        if (entry.result != nullptr) {
            entry.result->~vector();
            memory::General().Free(entry.result);
            entry.result = nullptr;
        }
    }
}

} // namespace engine::terrain
