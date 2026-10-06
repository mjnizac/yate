// Mapping types, VRAM value sizes and section-pool size classes (spec sections 7.3 and 8).

#include "test_support.hpp"

#include <engine/terrain/mapping.hpp>

using namespace engine;
using namespace engine::terrain;

namespace {

void TestValidity() {
    test::Section("mapping validity");
    CHECK(IsValid(Mapping{Domain::R2, 1}));
    CHECK(IsValid(Mapping{Domain::R3, 4}));
    CHECK(!IsValid(Mapping{Domain::R2, 0}));
    CHECK(!IsValid(Mapping{Domain::R3, 5}));

    // Brace initialisers carry commas, so the mappings are named before the checks.
    const Mapping heightmap{Domain::R2, 1};
    const Mapping normals{Domain::R2, 3};
    const Mapping density{Domain::R3, 1};
    CHECK_EQ(std::string_view{ToString(heightmap)}, std::string_view{"R2->R1"});
    CHECK_EQ(std::string_view{ToString(normals)}, std::string_view{"R2->R3"});
    CHECK_EQ(std::string_view{ToString(density)}, std::string_view{"R3->R1"});
    CHECK_EQ(heightmap.Axes(), u32_t{2});
    CHECK_EQ(density.Axes(), u32_t{3});
}

void TestValueSizes() {
    test::Section("value sizes");
    const SectionExtent tile{.x = 512, .y = 1, .z = 512};

    // One R2->R1 value on a 512^2 section is 1 MiB.
    CHECK_EQ(ValueSize(Mapping{Domain::R2, 1}, tile, 0), u64_t{512} * 512 * 4);
    CHECK_EQ(ValueSize(Mapping{Domain::R2, 1}, tile, 0), u64_t{1} * 1024 * 1024);

    // An R2->R3 sample is exactly 12 bytes, not 16: components are tightly packed f32.
    CHECK_EQ(ValueSize(Mapping{Domain::R2, 3}, tile, 0) / (u64_t{512} * 512), u64_t{12});

    // The halo applies on both axes of R2.
    CHECK_EQ(SampleCount(Mapping{Domain::R2, 1}, tile, 2), u64_t{516} * 516);
    // The y extent is ignored for R2, even when it is set.
    const SectionExtent tallTile{.x = 512, .y = 64, .z = 512};
    CHECK_EQ(SampleCount(Mapping{Domain::R2, 1}, tallTile, 2),
             SampleCount(Mapping{Domain::R2, 1}, tile, 2));

    // One R3->R1 value on a 512^3 section is 512 MiB, which is why R3 bricks are much smaller.
    const SectionExtent cube{.x = 512, .y = 512, .z = 512};
    CHECK_EQ(ValueSize(Mapping{Domain::R3, 1}, cube, 0), u64_t{512} * 1024 * 1024);

    const SectionExtent brick{.x = 64, .y = 64, .z = 64};
    CHECK_EQ(ValueSize(Mapping{Domain::R3, 1}, brick, 0), u64_t{64} * 64 * 64 * 4);
    // The halo applies on all three axes of R3.
    CHECK_EQ(SampleCount(Mapping{Domain::R3, 1}, brick, 1), u64_t{66} * 66 * 66);

    // Two values of the same mapping have different sizes when their halos differ.
    CHECK(ValueSize(Mapping{Domain::R2, 1}, tile, 0) < ValueSize(Mapping{Domain::R2, 1}, tile, 4));
}

void TestSizeClasses() {
    test::Section("size classes");
    const SectionExtent tile{.x = 512, .y = 1, .z = 512};
    const SectionExtent brick{.x = 64, .y = 64, .z = 64};

    // A value is never placed in a slot smaller than its computed size.
    const SizeClass heightClass = ClassOf(Mapping{Domain::R2, 1}, tile, 0);
    CHECK_EQ(heightClass.slotSize, u64_t{1} * 1024 * 1024);
    CHECK(heightClass.slotSize >= ValueSize(Mapping{Domain::R2, 1}, tile, 0));
    CHECK(heightClass.domain == Domain::R2);

    // A halo pushes the value into the next power-of-two class.
    const SizeClass haloClass = ClassOf(Mapping{Domain::R2, 1}, tile, 2);
    CHECK_EQ(haloClass.slotSize, u64_t{2} * 1024 * 1024);
    CHECK(haloClass.slotSize >= ValueSize(Mapping{Domain::R2, 1}, tile, 2));

    // R2 and R3 never share a size class, even when the byte sizes coincide.
    const SectionExtent matchedBrick{.x = 64, .y = 64, .z = 64};
    const SizeClass     r3Class = ClassOf(Mapping{Domain::R3, 1}, matchedBrick, 0);
    CHECK_EQ(r3Class.slotSize, u64_t{1} * 1024 * 1024);
    CHECK(r3Class.domain == Domain::R3);
    CHECK(!(r3Class == heightClass));
    CHECK(!r3Class.Accepts(Domain::R2, 1024));
    CHECK(!heightClass.Accepts(Domain::R3, 1024));
    CHECK(heightClass.Accepts(Domain::R2, heightClass.slotSize));
    CHECK(!heightClass.Accepts(Domain::R2, heightClass.slotSize + 1));

    const SizeClass vectorClass = ClassOf(Mapping{Domain::R3, 3}, brick, 1);
    CHECK(vectorClass.slotSize >= ValueSize(Mapping{Domain::R3, 3}, brick, 1));
    CHECK(IsPowerOfTwo(static_cast<usize_t>(vectorClass.slotSize)));
}

} // namespace

int main() {
    TestValidity();
    TestValueSizes();
    TestSizeClasses();
    return test::Summary("test_mapping");
}
