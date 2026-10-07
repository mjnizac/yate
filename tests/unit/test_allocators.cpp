// Allocator behaviour: every allocator returns to `used == 0`, arena reset and pool reuse behave
// correctly, and `reserved >= used` holds at all times (spec section 14).

#include "test_support.hpp"

#include <engine/log.hpp>
#include <engine/memory/arena.hpp>
#include <engine/memory/general.hpp>
#include <engine/memory/pool.hpp>

#include <memory_resource>
#include <vector>

using namespace engine;
using namespace engine::memory;

namespace {

void CheckInvariants(const Allocator& allocator) {
    const AllocatorStats stats = allocator.Stats();
    CHECK(stats.reserved >= stats.used);
    CHECK(stats.peak >= stats.used);
}

void TestGeneral() {
    test::Section("GeneralAllocator");
    GeneralAllocator allocator("test/General");

    CHECK_EQ(allocator.Stats().used, usize_t{0});

    void* small = allocator.Allocate(64, 16);
    CHECK(small != nullptr);
    CHECK_EQ(reinterpret_cast<usize_t>(small) % 16, usize_t{0});
    CHECK(allocator.Stats().used >= 64);
    CHECK_EQ(allocator.Stats().allocationCount, usize_t{1});
    CheckInvariants(allocator);

    void* large = allocator.Allocate(1 << 20, 64);
    CHECK(large != nullptr);
    CHECK_EQ(reinterpret_cast<usize_t>(large) % 64, usize_t{0});
    CHECK_EQ(allocator.Stats().allocationCount, usize_t{2});
    CHECK(allocator.Stats().peak >= (1 << 20) + 64);

    // A reallocation is a free of the old block followed by an allocation of the new one.
    void* grown = allocator.Reallocate(small, 256, 16);
    CHECK(grown != nullptr);
    CHECK_EQ(allocator.Stats().allocationCount, usize_t{2});
    CheckInvariants(allocator);

    // The peak survives the frees: it is the highest value `used` ever reached.
    const usize_t peak = allocator.Stats().peak;
    allocator.Free(grown);
    allocator.Free(large);
    CHECK_EQ(allocator.Stats().used, usize_t{0});
    CHECK_EQ(allocator.Stats().allocationCount, usize_t{0});
    CHECK_EQ(allocator.Stats().peak, peak);

    // Freeing a null pointer is a no-op, and shrinking to zero frees.
    allocator.Free(nullptr);
    void* temporary = allocator.Allocate(32, 8);
    CHECK(allocator.Reallocate(temporary, 0, 8) == nullptr);
    CHECK_EQ(allocator.Stats().used, usize_t{0});
}

void TestArena() {
    test::Section("ArenaAllocator");
    GeneralAllocator backing("test/Arena backing");
    {
        // A small chunk size so the growth path is exercised.
        ArenaAllocator arena("test/Arena", backing, 1024);
        CHECK_EQ(arena.Stats().reserved, usize_t{0});

        u8_t* first = static_cast<u8_t*>(arena.Allocate(100, 8));
        CHECK(first != nullptr);
        CHECK(arena.Stats().reserved >= 1024);
        CHECK_EQ(arena.Stats().used, usize_t{100});
        CheckInvariants(arena);

        // Bump allocations inside one chunk are contiguous once alignment is applied.
        u8_t* second = static_cast<u8_t*>(arena.Allocate(8, 8));
        CHECK(second == first + 104);
        CHECK_EQ(arena.Stats().used, usize_t{108});

        // Free is a no-op: an arena is released as a whole.
        arena.Free(first);
        CHECK_EQ(arena.Stats().used, usize_t{108});

        // An allocation larger than the chunk size gets a chunk of its own.
        const usize_t reservedBefore = arena.Stats().reserved;
        void*         big            = arena.Allocate(4096, 16);
        CHECK(big != nullptr);
        CHECK(arena.Stats().reserved > reservedBefore);
        CheckInvariants(arena);

        const usize_t reservedAfterGrowth = arena.Stats().reserved;
        const usize_t peak                = arena.Stats().peak;

        // Reset rewinds every chunk and keeps them for reuse.
        arena.Reset();
        CHECK_EQ(arena.Stats().used, usize_t{0});
        CHECK_EQ(arena.Stats().allocationCount, usize_t{0});
        CHECK_EQ(arena.Stats().reserved, reservedAfterGrowth);
        CHECK_EQ(arena.Stats().peak, peak);

        // The first allocation after a reset lands at the start of the first chunk again.
        CHECK(static_cast<u8_t*>(arena.Allocate(100, 8)) == first);

        arena.Reset();
        arena.Trim();
        CHECK_EQ(arena.Stats().reserved, usize_t{0});
    }
    CHECK_EQ(backing.Stats().used, usize_t{0});
}

void TestPool() {
    test::Section("PoolAllocator");
    GeneralAllocator backing("test/Pool backing");
    {
        PoolAllocator pool("test/Pool", backing, 24, 8, 4);
        // The slot size is rounded up to the alignment.
        CHECK_EQ(pool.SlotSize(), usize_t{24});

        void* slots[6] = {};
        for (void*& slot : slots) {
            slot = pool.Allocate(24, 8);
            CHECK(slot != nullptr);
        }
        // Six slots over pages of four means two pages, so a quarter of the pages is slack.
        CHECK_EQ(pool.Stats().allocationCount, usize_t{6});
        CHECK_EQ(pool.Stats().used, usize_t{6 * 24});
        CHECK_EQ(pool.Stats().reserved, usize_t{8 * 24});
        CheckInvariants(pool);

        // A freed slot is handed out again before a new page is taken.
        void* recycled = slots[2];
        pool.Free(slots[2]);
        CHECK_EQ(pool.Stats().allocationCount, usize_t{5});
        const usize_t reserved = pool.Stats().reserved;
        slots[2]               = pool.Allocate(24, 8);
        CHECK(slots[2] == recycled);
        CHECK_EQ(pool.Stats().reserved, reserved);

        for (void* slot : slots) {
            pool.Free(slot);
        }
        CHECK_EQ(pool.Stats().used, usize_t{0});
        pool.Trim();
        CHECK_EQ(pool.Stats().reserved, usize_t{0});
    }
    CHECK_EQ(backing.Stats().used, usize_t{0});
}

void TestPmrAdapter() {
    test::Section("pmr adapter");
    GeneralAllocator allocator("test/pmr");
    {
        std::pmr::vector<u32_t> values(&allocator.Resource());
        for (u32_t i = 0; i < 1000; ++i) {
            values.push_back(i);
        }
        CHECK_EQ(values.size(), usize_t{1000});
        CHECK_EQ(values[999], u32_t{999});
        CHECK(allocator.Stats().used > 0);
    }
    CHECK_EQ(allocator.Stats().used, usize_t{0});
}

void TestLuaAlloc() {
    test::Section("lua_Alloc hook");
    GeneralAllocator allocator("test/Lua");

    // lua_Alloc semantics: nsize == 0 frees, ptr == nullptr allocates, anything else reallocates.
    void* block = LuaAlloc(&allocator, nullptr, 0, 128);
    CHECK(block != nullptr);
    CHECK_EQ(allocator.Stats().allocationCount, usize_t{1});

    block = LuaAlloc(&allocator, block, 128, 512);
    CHECK(block != nullptr);
    CHECK_EQ(allocator.Stats().allocationCount, usize_t{1});
    CHECK(allocator.Stats().used >= 512);

    CHECK(LuaAlloc(&allocator, block, 512, 0) == nullptr);
    CHECK_EQ(allocator.Stats().used, usize_t{0});
}

void TestUntrackedNew() {
    test::Section("global operator new");
    usize_t bytes = 0;
    usize_t count = 0;
    UntrackedStats(bytes, count);

    // Every escape from the engine allocators still goes through the tracked heap. The logger
    // thread allocates concurrently, so the counts are compared with inequalities.
    auto*   block      = new u64_t[16];
    usize_t afterBytes = 0;
    usize_t afterCount = 0;
    UntrackedStats(afterBytes, afterCount);
    CHECK(afterCount >= count + 1);
    CHECK(afterBytes >= bytes + 16 * sizeof(u64_t));

    delete[] block;
    usize_t finalBytes = 0;
    usize_t finalCount = 0;
    UntrackedStats(finalBytes, finalCount);
    CHECK(finalCount < afterCount);
    CHECK(finalBytes < afterBytes);
}

} // namespace

/// Every allocator the engine exposes by name, checked as a set rather than one at a time.
///
/// This is what stands in for reading the Tracy memory view: the graphs there are drawn from exactly
/// these numbers, so asserting the invariants in process says more than looking at a picture, and it
/// keeps saying it on every run. What a human still has to do once is confirm the pools *appear* in the
/// profiler under the names below; what they no longer have to do is eyeball the values.
void TestEveryPoolReports() {
    test::Section("every named CPU pool reports consistent values");

    // The other tests construct allocators directly; these three are the process-wide ones the Tracy
    // view groups by, so they need the memory system up.
    REQUIRE_OK_VOID(memory::Init());

    struct Named {
        const char*          expected;
        memory::Allocator*   allocator;
    };
    const Named pools[] = {
        {"CPU/General", &memory::General()},
        {"CPU/Lua", &memory::Lua()},
        {"CPU/Frame arena", &memory::FrameArena()},
    };

    u64_t peakSum = 0;
    for (const Named& pool : pools) {
        // The name is the Tracy pool name, and Tracy requires the same pointer every time, so it must
        // be a literal and it must be the one the view is grouped by.
        CHECK(std::string_view{pool.allocator->Name()} == pool.expected);

        const memory::AllocatorStats stats = pool.allocator->Stats();
        // Reserved >= used is the invariant the `unused` plot is drawn from: a negative difference would
        // render as a gap in the graph rather than as an error.
        CHECK(stats.reserved >= stats.used);
        CHECK(stats.peak >= stats.used);
        if (stats.used == 0) {
            CHECK_EQ(stats.allocationCount, usize_t{0});
        } else {
            CHECK(stats.allocationCount != 0);
        }
        peakSum += stats.peak;
    }

    // `PeakCpuBytes` is what the metadata sidecar reports, and it is the sum of the named pools plus
    // the untracked `operator new` pool, so it can never be below the named pools alone.
    CHECK(memory::PeakCpuBytes() >= peakSum);

    memory::Shutdown();
}

int main() {
    REQUIRE_OK(log::Init(log::Config{}));

    TestGeneral();
    TestArena();
    TestPool();
    TestPmrAdapter();
    TestLuaAlloc();
    TestUntrackedNew();
    TestEveryPoolReports();

    log::Shutdown();
    return test::Summary("test_allocators");
}
