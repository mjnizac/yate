#pragma once

#include <engine/common.hpp>
#include <engine/error.hpp>

#include <cstddef>
#include <memory_resource>

namespace engine::memory {

/// Default alignment for untyped allocations.
inline constexpr usize_t kDefaultAlign = alignof(std::max_align_t);

struct AllocatorStats {
    /// Bytes obtained from the OS or the driver, including slack not handed to callers.
    usize_t reserved = 0;
    /// Bytes currently handed to callers.
    usize_t used = 0;
    /// Highest value `used` ever reached.
    usize_t peak = 0;
    /// Live allocation count.
    usize_t allocationCount = 0;
};

/// Common interface of every CPU allocator. Each instance owns a stable Tracy pool name,
/// so instances must not be copied or moved.
class ENGINE_API Allocator {
public:
    explicit Allocator(const char* name) noexcept;
    virtual ~Allocator();

    ENGINE_NO_COPY(Allocator);
    ENGINE_NO_MOVE(Allocator);

    /// Returns `nullptr` on failure. `align` must be a power of two.
    [[nodiscard]] virtual void* Allocate(usize_t size, usize_t align) = 0;

    /// Accepts `nullptr`.
    virtual void Free(void* ptr) = 0;

    [[nodiscard]] virtual AllocatorStats Stats() const = 0;

    /// Stable string literal, also used as the Tracy pool name.
    [[nodiscard]] const char* Name() const noexcept { return m_name; }

    /// Adapter so `std::pmr` containers can allocate from this allocator.
    [[nodiscard]] std::pmr::memory_resource& Resource() noexcept { return m_resource; }

protected:
    /// Logs name, live allocation count and live bytes when `used != 0`.
    /// Leaf destructors call it, because `Stats()` is pure here.
    void ReportLeaks() const noexcept;

private:
    class PmrResource final : public std::pmr::memory_resource {
    public:
        explicit PmrResource(Allocator& owner) noexcept : m_owner(owner) {}

    private:
        void* do_allocate(usize_t bytes, usize_t align) override;
        void  do_deallocate(void* ptr, usize_t bytes, usize_t align) override;
        bool  do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

        Allocator& m_owner;
    };

    const char* m_name;
    PmrResource m_resource;
};

/// Fallback heap for anything that escapes the engine allocators, including global
/// `operator new`. Reported to Tracy under `CPU/Untracked new`.
///
/// Exported so that each module can route its own `operator new` here and keep the
/// module boundary rule: memory allocated by this function is freed by `UntrackedFree`.
[[nodiscard]] ENGINE_API void* UntrackedAllocate(usize_t size, usize_t align) noexcept;
ENGINE_API void                UntrackedFree(void* ptr) noexcept;

/// Live bytes and allocation count still held by `UntrackedAllocate`.
ENGINE_API void UntrackedStats(usize_t& outBytes, usize_t& outCount) noexcept;

} // namespace engine::memory

#ifdef IS_ENGINE

namespace engine::memory {

class ArenaAllocator;
class GeneralAllocator;

/// Long-lived, variable-size engine objects. Reported as `CPU/General`.
[[nodiscard]] GeneralAllocator& General() noexcept;

/// Memory owned by the Lua runtime. Reported as `CPU/Lua`.
[[nodiscard]] GeneralAllocator& Lua() noexcept;

/// Per-frame / per-tick scratch, reset by the application loop. Reported as `CPU/Frame arena`.
[[nodiscard]] ArenaAllocator& FrameArena() noexcept;

/// `lua_Alloc`-compatible hook for `sol::state(panic, alloc, userdata)`.
/// `userdata` must be the `GeneralAllocator` to forward to, normally `&Lua()`.
[[nodiscard]] void* LuaAlloc(void* userdata, void* ptr, usize_t oldSize, usize_t newSize) noexcept;

/// Creates the engine allocators and configures their Tracy plots.
[[nodiscard]] Status Init();

/// Destroys the engine allocators, asserting that each one is empty.
void Shutdown() noexcept;

/// Refreshes every allocator's `unused` plot. Called once per frame or tick.
void UpdatePlots() noexcept;

} // namespace engine::memory

#endif // IS_ENGINE
