#include <engine/memory/pool.hpp>

#include <engine/assert.hpp>

namespace engine::memory {

PoolAllocator::PoolAllocator(const char* name, Allocator& backing, usize_t slotSize,
                             usize_t slotAlign, usize_t slotsPerPage)
    : Allocator(name),
      m_backing(backing),
      m_slotAlign(slotAlign < alignof(FreeSlot) ? alignof(FreeSlot) : slotAlign),
      m_slotsPerPage(slotsPerPage),
      m_tracy(name) {
    ENGINE_ASSERT(IsPowerOfTwo(slotAlign), "pool '{}' alignment {} is not a power of two", name,
                  slotAlign);
    ENGINE_ASSERT(slotsPerPage > 0, "pool '{}' needs at least one slot per page", name);
    const usize_t minimum = slotSize < sizeof(FreeSlot) ? sizeof(FreeSlot) : slotSize;
    m_slotSize            = AlignUp(minimum, m_slotAlign);
}

PoolAllocator::~PoolAllocator() {
    ReportLeaks();
    m_freeList = nullptr;
    m_used     = 0;
    m_count    = 0;
    Trim();
}

b8_t PoolAllocator::AddPage() {
    const usize_t header   = AlignUp(sizeof(Page), m_slotAlign);
    const usize_t capacity = m_slotSize * m_slotsPerPage;
    auto*         memory = static_cast<u8_t*>(m_backing.Allocate(header + capacity, m_slotAlign));
    ENGINE_ASSERT_RETURN(false, memory != nullptr, "pool '{}' could not reserve {} bytes", Name(),
                         header + capacity);

    auto* page     = reinterpret_cast<Page*>(memory);
    page->next     = m_pages;
    page->base     = memory + header;
    page->capacity = capacity;
    m_pages        = page;

    // Thread the new slots onto the free list, front to back, so allocation order is stable.
    for (usize_t i = m_slotsPerPage; i-- > 0;) {
        auto* slot = reinterpret_cast<FreeSlot*>(page->base + i * m_slotSize);
        slot->next = m_freeList;
        m_freeList = slot;
    }

    m_reserved += capacity;
    m_tracy.BeginBlock(page, capacity);
    m_tracy.PlotUnused(m_reserved, m_used);
    return true;
}

void* PoolAllocator::Allocate(usize_t size, usize_t align) {
    ENGINE_ASSERT_RETURN(nullptr, size <= m_slotSize,
                         "pool '{}' slot is {} bytes, requested {}", Name(), m_slotSize, size);
    ENGINE_ASSERT_RETURN(nullptr, align <= m_slotAlign,
                         "pool '{}' slot alignment is {}, requested {}", Name(), m_slotAlign,
                         align);

    if (m_freeList == nullptr && !AddPage()) {
        return nullptr;
    }

    FreeSlot* slot = m_freeList;
    m_freeList     = slot->next;
    m_used += m_slotSize;
    ++m_count;
    if (m_used > m_peak) {
        m_peak = m_used;
    }
    m_tracy.Acquire(slot, m_slotSize);
    m_tracy.PlotUnused(m_reserved, m_used);
    return slot;
}

void PoolAllocator::Free(void* ptr) {
    if (ptr == nullptr) {
        return;
    }
    m_tracy.Release(ptr);
    auto* slot = static_cast<FreeSlot*>(ptr);
    slot->next = m_freeList;
    m_freeList = slot;
    m_used -= m_slotSize;
    --m_count;
    m_tracy.PlotUnused(m_reserved, m_used);
}

AllocatorStats PoolAllocator::Stats() const {
    return AllocatorStats{.reserved        = m_reserved,
                          .used            = m_used,
                          .peak            = m_peak,
                          .allocationCount = m_count};
}

void PoolAllocator::Trim() noexcept {
    ENGINE_ASSERT(m_count == 0, "pool '{}' trimmed with {} live slots", Name(), m_count);
    Page* page = m_pages;
    while (page != nullptr) {
        Page* next = page->next;
        m_tracy.EndBlock(page);
        m_backing.Free(page);
        page = next;
    }
    m_pages    = nullptr;
    m_freeList = nullptr;
    m_reserved = 0;
    m_tracy.PlotUnused(0, m_used);
}

void PoolAllocator::UpdatePlots() const noexcept { m_tracy.PlotUnused(m_reserved, m_used); }

} // namespace engine::memory
