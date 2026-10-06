#pragma once

#include <engine/common.hpp>

namespace engine::terrain {

/// Dimension of the domain a value is defined over.
/// `R2` is `(x, z)`; `R3` is `(x, y, z)`.
enum class Domain : u8_t {
    R2 = 2,
    R3 = 3,
};

/// Mapping type `Rn -> Rm` of every value in the graph (spec section 8).
///
/// `components` is 1 to 4. Components are stored tightly packed as `f32`, using scalar block
/// layout, so an `R2 -> R3` sample takes exactly 12 bytes rather than 16.
struct Mapping {
    Domain domain     = Domain::R2;
    u8_t   components = 1;

    [[nodiscard]] constexpr b8_t operator==(const Mapping&) const noexcept = default;

    [[nodiscard]] constexpr u32_t Axes() const noexcept { return static_cast<u32_t>(domain); }
};

inline constexpr usize_t kComponentSize = sizeof(f32_t);
inline constexpr u8_t    kMaxComponents = 4;

/// Interior size of a section, in samples per axis. `y` is 1 for `R2` sections.
struct SectionExtent {
    u32_t x = 0;
    u32_t y = 1;
    u32_t z = 0;

    [[nodiscard]] constexpr b8_t operator==(const SectionExtent&) const noexcept = default;
};

/// Samples a value occupies on a section with halo `halo`, counting the halo on every axis of
/// its domain:
///   `R2 -> Rm`: `(sx + 2h) * (sz + 2h)`
///   `R3 -> Rm`: `(sx + 2h) * (sy + 2h) * (sz + 2h)`
[[nodiscard]] constexpr u64_t SampleCount(Mapping mapping, SectionExtent extent,
                                          u32_t halo) noexcept {
    const u64_t padded = 2 * static_cast<u64_t>(halo);
    const u64_t sx     = static_cast<u64_t>(extent.x) + padded;
    const u64_t sz     = static_cast<u64_t>(extent.z) + padded;
    if (mapping.domain == Domain::R2) {
        return sx * sz;
    }
    return sx * (static_cast<u64_t>(extent.y) + padded) * sz;
}

/// Bytes a value occupies in VRAM (spec section 7.3). Two values of the same mapping can have
/// different sizes, because each one carries the halo it needs.
[[nodiscard]] constexpr u64_t ValueSize(Mapping mapping, SectionExtent extent,
                                        u32_t halo) noexcept {
    return SampleCount(mapping, extent, halo) * mapping.components * kComponentSize;
}

/// Spelling used in log entries and error messages, for example `R2->R1`.
ENGINE_API const char* ToString(Mapping mapping) noexcept;

/// True when `mapping` is a valid graph mapping.
[[nodiscard]] constexpr b8_t IsValid(Mapping mapping) noexcept {
    return (mapping.domain == Domain::R2 || mapping.domain == Domain::R3)
           && mapping.components >= 1 && mapping.components <= kMaxComponents;
}

/// Size class a value of this size belongs to inside the section buffer pool.
///
/// `R2` and `R3` values never share a size class, so a 512x512 heightmap and a 64x64x64 brick
/// cannot be placed in each other's slots. Within a domain, sizes are rounded up to the next
/// power of two, and a value is never placed in a slot smaller than its computed size.
struct SizeClass {
    Domain  domain = Domain::R2;
    /// Slot size in bytes, a power of two that is >= the value size.
    u64_t slotSize = 0;

    [[nodiscard]] constexpr b8_t operator==(const SizeClass&) const noexcept = default;

    /// True when a value of `size` bytes and domain `valueDomain` fits in this class.
    [[nodiscard]] constexpr b8_t Accepts(Domain valueDomain, u64_t size) const noexcept {
        return valueDomain == domain && size <= slotSize;
    }
};

/// Smallest power-of-two slot in `mapping`'s domain that holds the value.
[[nodiscard]] constexpr SizeClass ClassOf(Mapping mapping, SectionExtent extent,
                                          u32_t halo) noexcept {
    const u64_t size = ValueSize(mapping, extent, halo);
    u64_t       slot = 1;
    while (slot < size) {
        slot <<= 1;
    }
    return SizeClass{.domain = mapping.domain, .slotSize = slot};
}

} // namespace engine::terrain
