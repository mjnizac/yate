#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <array>
#    include <cstring>
#    include <mutex>

#    ifdef TRACY_ENABLE
#        include <tracy/Tracy.hpp>
#    endif

namespace engine::memory {

/// Depth of the callstacks captured with ENGINE_TRACY_CALLSTACKS.
inline constexpr i32_t kCallstackDepth = 16;

namespace detail {

/// Stable storage for derived pool and plot names.
///
/// Tracy identifies a pool by the *pointer* of its name, and keeps referencing that pointer
/// for the whole session, so derived names must outlive every allocator. Interned names are
/// never released.
class NameTable {
public:
    static constexpr usize_t kMaxNames  = 128;
    static constexpr usize_t kMaxLength = 96;

    /// Returns a stable pointer to `base` followed by `suffix`, reusing an existing entry.
    [[nodiscard]] static const char* Intern(const char* base, const char* suffix) noexcept {
        static NameTable table;
        return table.Get(base, suffix);
    }

private:
    [[nodiscard]] const char* Get(const char* base, const char* suffix) noexcept {
        std::array<char, kMaxLength> wanted{};
        const usize_t                baseLength   = std::strlen(base);
        const usize_t                suffixLength = std::strlen(suffix);
        usize_t                      length = baseLength + suffixLength < kMaxLength - 1
                                                  ? baseLength + suffixLength
                                                  : kMaxLength - 1;
        std::memcpy(wanted.data(), base, baseLength < length ? baseLength : length);
        if (baseLength < length) {
            std::memcpy(wanted.data() + baseLength, suffix, length - baseLength);
        }
        wanted[length] = '\0';

        const std::lock_guard<std::mutex> lock(m_mutex);
        for (usize_t i = 0; i < m_count; ++i) {
            if (std::strcmp(m_names[i].data(), wanted.data()) == 0) {
                return m_names[i].data();
            }
        }
        if (m_count == kMaxNames) {
            return base; // Degrade to the base name rather than dangle.
        }
        m_names[m_count] = wanted;
        return m_names[m_count++].data();
    }

    std::mutex                                            m_mutex;
    std::array<std::array<char, kMaxLength>, kMaxNames>    m_names{};
    usize_t                                               m_count = 0;
};

} // namespace detail

/// Tracy bookkeeping shared by every allocator, CPU and GPU alike.
///
/// Reports two pools per allocator (spec section 7.4 rule 2):
///   * `<name>`            one event per live sub-allocation handed to callers
///   * `<name> [reserved]` one event per block obtained from the OS or the driver
/// plus the plots `<name> unused` (reserved minus used) and `<name> used`, the latter for
/// high-frequency allocators that report bytes as a plot instead of per-bump events.
class TracyPool {
public:
    explicit TracyPool(const char* name) noexcept
        : m_used(name),
          m_reserved(detail::NameTable::Intern(name, " [reserved]")),
          m_unusedPlot(detail::NameTable::Intern(name, " unused")),
          m_usedPlot(detail::NameTable::Intern(name, " used")) {
#    ifdef TRACY_ENABLE
        TracyPlotConfig(m_unusedPlot, tracy::PlotFormatType::Memory, true, true, 0);
        TracyPlotConfig(m_usedPlot, tracy::PlotFormatType::Memory, true, true, 0);
#    endif
    }

    ENGINE_NO_COPY(TracyPool);
    ENGINE_NO_MOVE(TracyPool);

    /// A whole block obtained from the OS or the driver: an arena chunk, a pool page,
    /// a `VkDeviceMemory` block or a section-pool `VkBuffer`.
    void BeginBlock(const void* key, usize_t bytes) const noexcept {
#    ifdef TRACY_ENABLE
#        ifdef ENGINE_TRACY_CALLSTACKS
        TracyAllocNS(key, bytes, kCallstackDepth, m_reserved);
#        else
        TracyAllocN(key, bytes, m_reserved);
#        endif
#    else
        (void)key;
        (void)bytes;
#    endif
    }

    void EndBlock(const void* key) const noexcept {
#    ifdef TRACY_ENABLE
#        ifdef ENGINE_TRACY_CALLSTACKS
        TracyFreeNS(key, kCallstackDepth, m_reserved);
#        else
        TracyFreeN(key, m_reserved);
#        endif
#    else
        (void)key;
#    endif
    }

    /// One sub-allocation handed to a caller. For GPU memory the key is the allocation
    /// handle, because a device sub-allocation has no CPU address (spec section 7.4 rule 5).
    void Acquire(const void* key, usize_t bytes) const noexcept {
#    ifdef TRACY_ENABLE
#        ifdef ENGINE_TRACY_CALLSTACKS
        TracyAllocNS(key, bytes, kCallstackDepth, m_used);
#        else
        TracyAllocN(key, bytes, m_used);
#        endif
#    else
        (void)key;
        (void)bytes;
#    endif
    }

    void Release(const void* key) const noexcept {
#    ifdef TRACY_ENABLE
#        ifdef ENGINE_TRACY_CALLSTACKS
        TracyFreeNS(key, kCallstackDepth, m_used);
#        else
        TracyFreeN(key, m_used);
#        endif
#    else
        (void)key;
#    endif
    }

    /// Used bytes as a plot, for allocators whose per-allocation rate would bloat the trace.
    void PlotUsed(usize_t used) const noexcept {
#    ifdef TRACY_ENABLE
        TracyPlot(m_usedPlot, static_cast<i64_t>(used));
#    else
        (void)used;
#    endif
    }

    /// Reserved minus used. Updated whenever either value changes and once per frame or tick.
    void PlotUnused(usize_t reserved, usize_t used) const noexcept {
#    ifdef TRACY_ENABLE
        TracyPlot(m_unusedPlot, static_cast<i64_t>(reserved) - static_cast<i64_t>(used));
#    else
        (void)reserved;
        (void)used;
#    endif
    }

    [[nodiscard]] const char* UsedPoolName() const noexcept { return m_used; }
    [[nodiscard]] const char* ReservedPoolName() const noexcept { return m_reserved; }

private:
    const char* m_used;
    const char* m_reserved;
    const char* m_unusedPlot;
    const char* m_usedPlot;
};

/// Plots a named value in memory format. Used for VMA heap usage and budgets.
inline void PlotMemory(const char* name, u64_t bytes) noexcept {
#    ifdef TRACY_ENABLE
    TracyPlot(name, static_cast<i64_t>(bytes));
#    else
    (void)name;
    (void)bytes;
#    endif
}

/// Plots a named count in plain number format.
inline void PlotCount(const char* name, i64_t value) noexcept {
#    ifdef TRACY_ENABLE
    TracyPlot(name, value);
#    else
    (void)name;
    (void)value;
#    endif
}

} // namespace engine::memory

#endif // IS_ENGINE
