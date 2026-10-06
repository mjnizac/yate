#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <engine/memory/allocator.hpp>
#    include <engine/memory/tracy_memory.hpp>

namespace engine::memory {

/// Linear bump allocator over chunks, reset as a whole.
///
/// `Free` is a no-op: an arena is released by `Reset`, which rewinds every chunk and keeps
/// them for reuse. Per-bump allocations are reported as a plot rather than as Tracy events,
/// because their rate would bloat the trace (spec section 7.4 rule 3). Chunk reservations are
/// reported as events.
///
/// Not thread-safe; one arena belongs to one thread or one phase.
class ArenaAllocator final : public Allocator {
public:
    /// `chunkSize` is the minimum size of each chunk obtained from `backing`. A single
    /// allocation larger than it gets a chunk of its own.
    ArenaAllocator(const char* name, Allocator& backing, usize_t chunkSize);
    ~ArenaAllocator() override;

    [[nodiscard]] void* Allocate(usize_t size, usize_t align) override;

    /// No-op. Present to satisfy the allocator interface and the pmr adapter.
    void Free(void* ptr) override;

    [[nodiscard]] AllocatorStats Stats() const override;

    /// Rewinds every chunk without releasing it, and emits a single plot update.
    void Reset() noexcept;

    /// Releases every chunk back to the backing allocator.
    void Trim() noexcept;

    void UpdatePlots() const noexcept;

private:
    struct Chunk {
        Chunk*  next;
        u8_t*   base;
        usize_t capacity;
        usize_t offset;
    };

    [[nodiscard]] Chunk* AcquireChunk(usize_t minimumSize);

    Allocator& m_backing;
    usize_t    m_chunkSize;
    Chunk*     m_first   = nullptr;
    Chunk*     m_current = nullptr;
    usize_t    m_reserved = 0;
    usize_t    m_used     = 0;
    usize_t    m_peak     = 0;
    usize_t    m_count    = 0;
    TracyPool  m_tracy;
};

} // namespace engine::memory

#endif // IS_ENGINE
