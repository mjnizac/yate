#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <engine/memory/allocator.hpp>
#    include <engine/memory/tracy_memory.hpp>

#    include <atomic>

namespace engine::memory {

/// mimalloc-backed allocator for long-lived, variable-size objects.
///
/// Allocations go through mimalloc's thread-local heaps, so instances are thread-safe and
/// a block may be freed from any thread. Statistics and the leak check are kept per instance
/// with atomics. mimalloc does not expose its segment reservations per instance, so `reserved`
/// tracks the usable size of the live blocks and the `unused` plot stays at zero.
class GeneralAllocator final : public Allocator {
public:
    explicit GeneralAllocator(const char* name);
    ~GeneralAllocator() override;

    [[nodiscard]] void* Allocate(usize_t size, usize_t align) override;
    void                Free(void* ptr) override;
    [[nodiscard]] AllocatorStats Stats() const override;

    /// Grows or shrinks a block in place when possible. Reported to Tracy as a free of the
    /// old pointer followed by an allocation of the new one (spec section 7.4 rule 4).
    [[nodiscard]] void* Reallocate(void* ptr, usize_t newSize, usize_t align);

    void UpdatePlots() const noexcept;

private:
    TracyPool             m_tracy;
    std::atomic<usize_t>  m_used{0};
    std::atomic<usize_t>  m_peak{0};
    std::atomic<usize_t>  m_count{0};
};

} // namespace engine::memory

#endif // IS_ENGINE
