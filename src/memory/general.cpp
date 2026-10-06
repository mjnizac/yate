#include <engine/memory/general.hpp>

#include <engine/assert.hpp>
#include <engine/log.hpp>
#include <engine/memory/arena.hpp>

#include <mimalloc.h>

#include <cstddef>
#include <new>

namespace engine::memory {

// --- Allocator base -------------------------------------------------------------------------

Allocator::Allocator(const char* name) noexcept : m_name(name), m_resource(*this) {}

Allocator::~Allocator() = default;

void Allocator::ReportLeaks() const noexcept {
    const AllocatorStats stats = Stats();
    if (stats.used == 0 && stats.allocationCount == 0) {
        return;
    }
    LOG_ERROR("allocator '{}' leaked {} bytes in {} live allocations", m_name, stats.used,
              stats.allocationCount);
    ENGINE_ASSERT(stats.used == 0, "allocator '{}' destroyed with live allocations", m_name);
}

void* Allocator::PmrResource::do_allocate(usize_t bytes, usize_t align) {
    void* ptr = m_owner.Allocate(bytes, align < kDefaultAlign ? kDefaultAlign : align);
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void Allocator::PmrResource::do_deallocate(void* ptr, usize_t, usize_t) { m_owner.Free(ptr); }

bool Allocator::PmrResource::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

// --- GeneralAllocator -----------------------------------------------------------------------

GeneralAllocator::GeneralAllocator(const char* name) : Allocator(name), m_tracy(name) {}

GeneralAllocator::~GeneralAllocator() { ReportLeaks(); }

void* GeneralAllocator::Allocate(usize_t size, usize_t align) {
    ENGINE_ASSERT(IsPowerOfTwo(align), "alignment {} is not a power of two", align);
    void* ptr = mi_malloc_aligned(size, align);
    if (ptr == nullptr) [[unlikely]] {
        return nullptr;
    }
    const usize_t usable = mi_usable_size(ptr);
    const usize_t used   = m_used.fetch_add(usable, std::memory_order_relaxed) + usable;
    m_count.fetch_add(1, std::memory_order_relaxed);
    BumpPeak(used);
    m_tracy.Acquire(ptr, usable);
    return ptr;
}

void GeneralAllocator::BumpPeak(usize_t used) noexcept {
    usize_t peak = m_peak.load(std::memory_order_relaxed);
    while (peak < used && !m_peak.compare_exchange_weak(peak, used, std::memory_order_relaxed)) {
    }
}

void GeneralAllocator::Free(void* ptr) {
    if (ptr == nullptr) {
        return;
    }
    m_tracy.Release(ptr);
    m_used.fetch_sub(mi_usable_size(ptr), std::memory_order_relaxed);
    m_count.fetch_sub(1, std::memory_order_relaxed);
    mi_free(ptr);
}

void* GeneralAllocator::Reallocate(void* ptr, usize_t newSize, usize_t align) {
    if (newSize == 0) {
        Free(ptr);
        return nullptr;
    }
    if (ptr == nullptr) {
        return Allocate(newSize, align);
    }
    // Reported as free(old) followed by alloc(new), even when mimalloc grows in place.
    m_tracy.Release(ptr);
    const usize_t oldUsable = mi_usable_size(ptr);
    void*         result    = mi_realloc_aligned(ptr, newSize, align);
    if (result == nullptr) [[unlikely]] {
        m_tracy.Acquire(ptr, oldUsable); // The old block survives a failed reallocation.
        return nullptr;
    }
    const usize_t newUsable = mi_usable_size(result);
    // Unsigned arithmetic wraps correctly on a shrink, because the sum is taken modulo 2^64.
    const usize_t used = m_used.fetch_add(newUsable - oldUsable, std::memory_order_relaxed)
                         + newUsable - oldUsable;
    BumpPeak(used);
    m_tracy.Acquire(result, newUsable);
    return result;
}

AllocatorStats GeneralAllocator::Stats() const {
    const usize_t used = m_used.load(std::memory_order_relaxed);
    return AllocatorStats{.reserved        = used,
                          .used            = used,
                          .peak            = m_peak.load(std::memory_order_relaxed),
                          .allocationCount = m_count.load(std::memory_order_relaxed)};
}

void GeneralAllocator::UpdatePlots() const noexcept {
    const usize_t used = m_used.load(std::memory_order_relaxed);
    m_tracy.PlotUnused(used, used);
}

// --- Untracked heap -------------------------------------------------------------------------

namespace {

/// Leaked on purpose: global `operator delete` may run after static destructors, so this
/// allocator must outlive every other object in the process.
GeneralAllocator& Untracked() noexcept {
    alignas(GeneralAllocator) static std::byte storage[sizeof(GeneralAllocator)];
    static GeneralAllocator* instance = ::new (storage) GeneralAllocator("CPU/Untracked new");
    return *instance;
}

} // namespace

void* UntrackedAllocate(usize_t size, usize_t align) noexcept {
    return Untracked().Allocate(size, align);
}

void UntrackedFree(void* ptr) noexcept { Untracked().Free(ptr); }

void UntrackedStats(usize_t& outBytes, usize_t& outCount) noexcept {
    const AllocatorStats stats = Untracked().Stats();
    outBytes                   = stats.used;
    outCount                   = stats.allocationCount;
}

// --- Engine allocators ----------------------------------------------------------------------

namespace {

/// Minimum size of a frame-arena chunk. Scratch for one frame or tick.
constexpr usize_t kFrameArenaChunkSize = 1u << 20;

template <typename T>
struct Singleton {
    alignas(T) std::byte storage[sizeof(T)];
    T* instance = nullptr;

    template <typename... Args>
    T& Construct(Args&&... args) {
        instance = ::new (storage) T(std::forward<Args>(args)...);
        return *instance;
    }

    void Destroy() noexcept {
        if (instance != nullptr) {
            instance->~T();
            instance = nullptr;
        }
    }
};

Singleton<GeneralAllocator> g_general;
Singleton<GeneralAllocator> g_lua;
Singleton<ArenaAllocator>   g_frameArena;
b8_t                        g_initialized = false;

} // namespace

GeneralAllocator& General() noexcept {
    ENGINE_ASSERT(g_general.instance != nullptr, "memory system is not initialized");
    return *g_general.instance;
}

GeneralAllocator& Lua() noexcept {
    ENGINE_ASSERT(g_lua.instance != nullptr, "memory system is not initialized");
    return *g_lua.instance;
}

ArenaAllocator& FrameArena() noexcept {
    ENGINE_ASSERT(g_frameArena.instance != nullptr, "memory system is not initialized");
    return *g_frameArena.instance;
}

void* LuaAlloc(void* userdata, void* ptr, usize_t oldSize, usize_t newSize) noexcept {
    (void)oldSize;
    auto* allocator = static_cast<GeneralAllocator*>(userdata);
    ENGINE_ASSERT_RETURN(nullptr, allocator != nullptr, "LuaAlloc called without an allocator");
    return allocator->Reallocate(ptr, newSize, kDefaultAlign);
}

Status Init() {
    ENGINE_ASSERT_RETURN(Status{}, !g_initialized, "memory system is already initialized");
    g_general.Construct("CPU/General");
    g_lua.Construct("CPU/Lua");
    g_frameArena.Construct("CPU/Frame arena", *g_general.instance, kFrameArenaChunkSize);
    g_initialized = true;
    LOG_INFO("memory system ready (general, lua, frame arena)");
    return {};
}

void Shutdown() noexcept {
    if (!g_initialized) {
        return;
    }
    g_frameArena.instance->Reset(); // An arena must be empty when it is destroyed.
    g_frameArena.Destroy();
    g_lua.Destroy();
    g_general.Destroy();
    g_initialized = false;

    usize_t untrackedBytes = 0;
    usize_t untrackedCount = 0;
    UntrackedStats(untrackedBytes, untrackedCount);
    if (untrackedCount != 0) {
        LOG_INFO("'CPU/Untracked new' still holds {} bytes in {} allocations at shutdown",
                 untrackedBytes, untrackedCount);
    }
}

void UpdatePlots() noexcept {
    if (!g_initialized) {
        return;
    }
    g_general.instance->UpdatePlots();
    g_lua.instance->UpdatePlots();
    g_frameArena.instance->UpdatePlots();
    Untracked().UpdatePlots();
}

} // namespace engine::memory
