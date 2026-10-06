#pragma once

#include <engine/api.h>

#include <cstddef>
#include <cstdint>

namespace engine {

using u8_t  = std::uint8_t;
using u16_t = std::uint16_t;
using u32_t = std::uint32_t;
using u64_t = std::uint64_t;
using i8_t  = std::int8_t;
using i16_t = std::int16_t;
using i32_t = std::int32_t;
using i64_t = std::int64_t;
using f32_t = float;
using f64_t = double;

using usize_t = std::size_t;
using isize_t = std::ptrdiff_t;
using b8_t    = bool;

static_assert(sizeof(u8_t) == 1 && sizeof(i8_t) == 1);
static_assert(sizeof(u16_t) == 2 && sizeof(i16_t) == 2);
static_assert(sizeof(u32_t) == 4 && sizeof(i32_t) == 4);
static_assert(sizeof(u64_t) == 8 && sizeof(i64_t) == 8);
static_assert(sizeof(f32_t) == 4 && sizeof(f64_t) == 8);
static_assert(sizeof(usize_t) == sizeof(void*) && sizeof(isize_t) == sizeof(void*));
static_assert(sizeof(b8_t) == 1);

/// Deleted copy operations. Placed in the public section of a class body.
#define ENGINE_NO_COPY(Type)                                                                        \
    Type(const Type&) = delete;                                                                     \
    Type& operator=(const Type&) = delete

/// Deleted move operations. Placed in the public section of a class body.
#define ENGINE_NO_MOVE(Type)                                                                        \
    Type(Type&&) = delete;                                                                          \
    Type& operator=(Type&&) = delete

/// Number of elements of a C array, as usize_t.
template <typename T, usize_t N>
constexpr usize_t ArrayCount(const T (&)[N]) noexcept {
    return N;
}

/// Smallest multiple of `align` that is >= `value`. `align` must be a power of two.
constexpr usize_t AlignUp(usize_t value, usize_t align) noexcept {
    return (value + align - 1) & ~(align - 1);
}

/// True when `value` is zero or a power of two.
constexpr b8_t IsPowerOfTwo(usize_t value) noexcept {
    return (value & (value - 1)) == 0;
}

/// Smallest power of two that is >= `value`, for `value` >= 1.
constexpr usize_t NextPowerOfTwo(usize_t value) noexcept {
    usize_t result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

} // namespace engine
