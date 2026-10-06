// Global operator new / delete for one module.
//
// Compiled into the engine library and into every executable (see cmake/engine.cmake), because
// replacing these operators only affects the module they are linked into. Both route to the
// exported `CPU/Untracked new` heap, so a block allocated inside a module is also freed there
// and nothing escapes Tracy.

#include <engine/memory/allocator.hpp>

#include <cstddef>
#include <new>

namespace {

void* Allocate(std::size_t size, std::size_t align) {
    void* ptr = engine::memory::UntrackedAllocate(size == 0 ? 1 : size, align);
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

} // namespace

void* operator new(std::size_t size) { return Allocate(size, engine::memory::kDefaultAlign); }
void* operator new[](std::size_t size) { return Allocate(size, engine::memory::kDefaultAlign); }
void* operator new(std::size_t size, std::align_val_t align) {
    return Allocate(size, static_cast<std::size_t>(align));
}
void* operator new[](std::size_t size, std::align_val_t align) {
    return Allocate(size, static_cast<std::size_t>(align));
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return engine::memory::UntrackedAllocate(size == 0 ? 1 : size, engine::memory::kDefaultAlign);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return engine::memory::UntrackedAllocate(size == 0 ? 1 : size, engine::memory::kDefaultAlign);
}
void* operator new(std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return engine::memory::UntrackedAllocate(size == 0 ? 1 : size,
                                             static_cast<std::size_t>(align));
}
void* operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return engine::memory::UntrackedAllocate(size == 0 ? 1 : size,
                                             static_cast<std::size_t>(align));
}

void operator delete(void* ptr) noexcept { engine::memory::UntrackedFree(ptr); }
void operator delete[](void* ptr) noexcept { engine::memory::UntrackedFree(ptr); }
void operator delete(void* ptr, std::align_val_t) noexcept { engine::memory::UntrackedFree(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept { engine::memory::UntrackedFree(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { engine::memory::UntrackedFree(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { engine::memory::UntrackedFree(ptr); }
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
    engine::memory::UntrackedFree(ptr);
}
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
    engine::memory::UntrackedFree(ptr);
}
void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    engine::memory::UntrackedFree(ptr);
}
void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    engine::memory::UntrackedFree(ptr);
}
void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
    engine::memory::UntrackedFree(ptr);
}
void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
    engine::memory::UntrackedFree(ptr);
}
