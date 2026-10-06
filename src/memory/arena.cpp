#include <engine/memory/arena.hpp>

#include <engine/assert.hpp>

#include <cstring>

namespace engine::memory {

ArenaAllocator::ArenaAllocator(const char* name, Allocator& backing, usize_t chunkSize)
    : Allocator(name), m_backing(backing), m_chunkSize(chunkSize), m_tracy(name) {
    ENGINE_ASSERT(chunkSize > 0, "arena '{}' needs a non-zero chunk size", name);
}

ArenaAllocator::~ArenaAllocator() {
    ReportLeaks();
    Reset();
    Trim();
}

ArenaAllocator::Chunk* ArenaAllocator::AcquireChunk(usize_t minimumSize) {
    // Reuse the next already-reserved chunk when it is large enough.
    Chunk* next = m_current != nullptr ? m_current->next : m_first;
    while (next != nullptr) {
        if (next->capacity >= minimumSize) {
            m_current = next;
            return next;
        }
        next = next->next;
    }

    const usize_t capacity = minimumSize > m_chunkSize ? minimumSize : m_chunkSize;
    const usize_t total    = AlignUp(sizeof(Chunk), kDefaultAlign) + capacity;
    auto*         memory   = static_cast<u8_t*>(m_backing.Allocate(total, kDefaultAlign));
    ENGINE_ASSERT_RETURN(nullptr, memory != nullptr, "arena '{}' could not reserve {} bytes",
                         Name(), total);

    auto* chunk     = reinterpret_cast<Chunk*>(memory);
    chunk->next     = nullptr;
    chunk->base     = memory + AlignUp(sizeof(Chunk), kDefaultAlign);
    chunk->capacity = capacity;
    chunk->offset   = 0;

    if (m_current != nullptr) {
        m_current->next = chunk;
    } else {
        m_first = chunk;
    }
    m_current = chunk;
    m_reserved += capacity;
    m_tracy.BeginBlock(chunk, capacity);
    m_tracy.PlotUnused(m_reserved, m_used);
    return chunk;
}

void* ArenaAllocator::Allocate(usize_t size, usize_t align) {
    ENGINE_ASSERT(IsPowerOfTwo(align), "alignment {} is not a power of two", align);
    if (size == 0) {
        return nullptr;
    }

    Chunk* chunk = m_current;
    if (chunk != nullptr) {
        const usize_t aligned = AlignUp(reinterpret_cast<usize_t>(chunk->base + chunk->offset), align)
                                - reinterpret_cast<usize_t>(chunk->base);
        if (aligned + size <= chunk->capacity) {
            void* ptr     = chunk->base + aligned;
            m_used += size;
            chunk->offset = aligned + size;
            ++m_count;
            if (m_used > m_peak) {
                m_peak = m_used;
            }
            m_tracy.PlotUsed(m_used);
            return ptr;
        }
    }

    chunk = AcquireChunk(size + align);
    if (chunk == nullptr) {
        return nullptr;
    }
    const usize_t aligned = AlignUp(reinterpret_cast<usize_t>(chunk->base), align)
                            - reinterpret_cast<usize_t>(chunk->base);
    void* ptr     = chunk->base + aligned;
    chunk->offset = aligned + size;
    m_used += size;
    ++m_count;
    if (m_used > m_peak) {
        m_peak = m_used;
    }
    m_tracy.PlotUsed(m_used);
    return ptr;
}

void ArenaAllocator::Free(void* ptr) { (void)ptr; }

AllocatorStats ArenaAllocator::Stats() const {
    return AllocatorStats{.reserved        = m_reserved,
                          .used            = m_used,
                          .peak            = m_peak,
                          .allocationCount = m_count};
}

void ArenaAllocator::Reset() noexcept {
    for (Chunk* chunk = m_first; chunk != nullptr; chunk = chunk->next) {
        chunk->offset = 0;
    }
    m_current = m_first;
    m_used    = 0;
    m_count   = 0;
    m_tracy.PlotUsed(0);
    m_tracy.PlotUnused(m_reserved, 0);
}

void ArenaAllocator::Trim() noexcept {
    Chunk* chunk = m_first;
    while (chunk != nullptr) {
        Chunk* next = chunk->next;
        m_tracy.EndBlock(chunk);
        m_backing.Free(chunk);
        chunk = next;
    }
    m_first    = nullptr;
    m_current  = nullptr;
    m_reserved = 0;
    m_tracy.PlotUnused(0, m_used);
}

void ArenaAllocator::UpdatePlots() const noexcept {
    m_tracy.PlotUsed(m_used);
    m_tracy.PlotUnused(m_reserved, m_used);
}

} // namespace engine::memory
