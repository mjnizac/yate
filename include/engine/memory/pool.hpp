#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <engine/memory/allocator.hpp>
#    include <engine/memory/tracy_memory.hpp>

namespace engine::memory {

/// Fixed-size slot allocator with an intrusive free list, for AST nodes, section descriptors
/// and other small fixed-size records.
///
/// Pages are obtained from a backing allocator and reported as reserved blocks; each slot
/// handed out is reported as a used event, so Tracy shows page slack as `unused`.
///
/// Not thread-safe.
class PoolAllocator final : public Allocator {
public:
    /// `slotSize` is rounded up to `slotAlign`, which must be a power of two and at least
    /// `alignof(void*)` so a free slot can hold the next-pointer.
    PoolAllocator(const char* name, Allocator& backing, usize_t slotSize, usize_t slotAlign,
                  usize_t slotsPerPage);
    ~PoolAllocator() override;

    /// `size` must fit in one slot and `align` must divide the slot alignment.
    [[nodiscard]] void* Allocate(usize_t size, usize_t align) override;
    void                Free(void* ptr) override;
    [[nodiscard]] AllocatorStats Stats() const override;

    [[nodiscard]] usize_t SlotSize() const noexcept { return m_slotSize; }

    /// Releases every page. Requires that no slot is live.
    void Trim() noexcept;

    void UpdatePlots() const noexcept;

private:
    struct Page {
        Page*   next;
        u8_t*   base;
        usize_t capacity;
    };

    struct FreeSlot {
        FreeSlot* next;
    };

    [[nodiscard]] b8_t AddPage();

    Allocator& m_backing;
    usize_t    m_slotSize = 0;
    usize_t    m_slotAlign;
    usize_t    m_slotsPerPage;
    Page*      m_pages    = nullptr;
    FreeSlot*  m_freeList = nullptr;
    usize_t    m_reserved = 0;
    usize_t    m_used     = 0;
    usize_t    m_peak     = 0;
    usize_t    m_count    = 0;
    TracyPool  m_tracy;
};

} // namespace engine::memory

#endif // IS_ENGINE
