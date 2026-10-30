// Rivers, lakes and the sea from Water.Network, checked on the CPU through the sampler (no GPU).
//
// A drawn river never rises and its water lies over its bed and nowhere far from it; a lake drawn in a
// closed basin under the sea level holds its own level and the sea does not reach it; the generator makes
// as many rivers as it is asked for, none when asked for none, and a river traced from a source runs down.

#include "test_support.hpp"

#include <engine/log.hpp>
#include <engine/lua.hpp>
#include <engine/memory/allocator.hpp>
#include <engine/platform.hpp>
#include <engine/terrain/graph.hpp>
#include <engine/terrain/sampler.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

using namespace engine;
using terrain::Graph;

namespace {

constexpr const char* kDirectory = "out/test_water";

[[nodiscard]] std::string Write(const char* name, std::string_view source) {
    const std::string path = std::string(kDirectory) + "/" + name + ".lua";
    if (std::FILE* file = std::fopen(path.c_str(), "wb"); file != nullptr) {
        (void)std::fwrite(source.data(), 1, source.size(), file);
        (void)std::fclose(file);
    }
    return path;
}

/// Output `name` of `graph` at (x, z).
[[nodiscard]] f32_t At(const Graph& graph, std::string_view name, f32_t x, f32_t z) {
    for (usize_t i = 0; i < graph.OutputCount(); ++i) {
        if (std::string_view{graph.Output(i).name.data()} == name) {
            const std::array<f32_t, 2> p{x, z};
            f32_t                      out = 0.0f;
            if (!terrain::SampleGraph(graph, graph.Output(i).value, 0, 4.0f, {&p, 1}, {&out, 1})) {
                return -1e30f;
            }
            return out;
        }
    }
    return -1e30f;
}

[[nodiscard]] b8_t Run(Graph& graph, const char* name, std::string_view source) {
    const std::string      path = Write(name, source);
    lua::ScriptEnvironment environment;
    environment.maxX = environment.maxZ = 1000.0;
    const Status ran = lua::RunScript(graph, path, environment);
    if (!ran) {
        std::printf("     %s: %s\n", name, ran.error().Format().data());
    }
    return ran.has_value();
}

// Every river's levels never rise, which the script checks for all of them, generated or drawn.
constexpr const char* kMonotone = R"(
local function monotone(net)
    for _, r in ipairs(net.rivers) do
        for i = 2, #r.levels do
            if r.levels[i] > r.levels[i - 1] + 1e-6 then
                error("a " .. r.kind .. " river rises by " .. (r.levels[i] - r.levels[i - 1]) .. " m")
            end
        end
    end
end
)";

void TestRiver() {
    test::Section("a drawn river never rises, and its water is over its bed and nowhere else");
    Graph graph;
    // A plane falling 4 % towards +x, and a river straight across it along z = 0.
    const std::string source = std::string(kMonotone) + R"(
return function(params)
    local base = Terrain.Coords{}.x * -0.04 + 200.0
    local net = Water.Network{ terrain = base, region = { -4000, -2000, 4000, 2000 }, cell = 50,
        rivers = { { points = { { -3500, 0 }, { 3500, 0 } }, kind = "gorge", width = 20, depth = 3, roughness = 0 } } }
    monotone(net)
    local wet = net:apply(base)
    return { height = { value = wet.height, range = { -200, 600 } }, rivers = { value = wet.rivers, range = { -30000, 600 } } }
end
)";
    if (!Run(graph, "river", source)) {
        CHECK(false);
        return;
    }
    f32_t previous = 1e30f;
    b8_t  falls    = true;
    b8_t  wet      = true;
    for (f32_t x = -3000.0f; x <= 3000.0f; x += 250.0f) {
        const f32_t water  = At(graph, "rivers", x, 0.0f);
        const f32_t ground = At(graph, "height", x, 0.0f);
        wet                = wet && water > ground + 0.5f;
        falls              = falls && water <= previous + 1e-3f;
        previous           = water;
    }
    CHECK(wet);
    CHECK(falls);
    // 600 m off the channel, on the plane: dry.
    CHECK(At(graph, "rivers", 0.0f, 600.0f) < At(graph, "height", 0.0f, 600.0f));
    // The channel is cut into the plane, not laid on it.
    CHECK(At(graph, "height", 0.0f, 0.0f) < 200.0f - 2.0f);
}

void TestLakeUnderSea() {
    test::Section("a lake in a closed basin under the sea level keeps its level, and the sea stays out");
    Graph graph;
    // Ground at +20 m with a closed bowl down to -60 m; the sea at 0; a lake drawn in the bowl at -30 m.
    CHECK(Run(graph, "lake", R"(
return function(params)
    local c = Terrain.Coords{}
    local bowl = Geometry.Circle{ position = c, radius = 1500 }
    local base = Combine.Max{ a = Curves.Clamp{ input = bowl + 1500.0, min = 0.0, max = 100000.0 } * 0.06 - 60.0,
                              b = Terrain.Const{ value = -60.0 } }
    base = Combine.Min{ a = base, b = Terrain.Const{ value = 20.0 } }
    local net = Water.Network{ terrain = base, region = { -4000, -4000, 4000, 4000 }, cell = 50, sea_level = 0,
        lakes = { { at = { 0, 0 }, radius = 700, level = -30, depth = 20, kind = "trench" } } }
    local wet = net:apply(base)
    return { height = { value = wet.height, range = { -200, 600 } }, lakes = { value = wet.lakes, range = { -30000, 600 } },
             sea = { value = wet.sea, range = { -30000, 600 } } }
end
)"));
    CHECK(std::fabs(At(graph, "lakes", 0.0f, 0.0f) + 30.0f) < 0.01f);
    CHECK(At(graph, "lakes", 0.0f, 0.0f) > At(graph, "height", 0.0f, 0.0f) + 5.0f);
    // The sea surface is far under the basin floor: the basin does not reach the edge.
    CHECK(At(graph, "sea", 0.0f, 0.0f) < At(graph, "height", 0.0f, 0.0f));
    CHECK(At(graph, "sea", 1100.0f, 0.0f) < At(graph, "height", 1100.0f, 0.0f));
}

void TestGenerate() {
    test::Section("the generator makes what it is asked for, and a traced river runs down");
    // Hills falling to the west; the script fails if a count or a level is wrong.
    const std::string body = std::string(kMonotone) + R"(
return function(params)
    local c = Terrain.Coords{}
    local base = Noises.fBm{ frequency = 0.0005, octaves = 4, amplitude = 250.0, seed = 3 }.value + c.x * 0.05 + 300.0
    local function make(count)
        return Water.Network{ terrain = base, region = { -6000, -6000, 6000, 6000 }, cell = 100,
                              generate = { rivers = { count = count } } }
    end
    local none = make(0)
    if #none.rivers ~= 0 then error("count = 0 made " .. #none.rivers .. " rivers") end
    local two = make(2)
    if #two.rivers < 1 or #two.rivers > 2 then error("count = 2 made " .. #two.rivers .. " rivers") end
    monotone(two)
    local traced = Water.Network{ terrain = base, region = { -6000, -6000, 6000, 6000 }, cell = 100,
                                  rivers = { { source = { 4000, 0 }, kind = "mountain", width = 10 } } }
    if #traced.rivers < 1 then error("a river from a source was not traced") end
    monotone(traced)
    local r = traced.rivers[1]
    if r.levels[#r.levels] >= r.levels[1] - 20.0 then error("the traced river barely fell") end
    return { height = { value = base, range = { -200, 900 } } }
end
)";
    Graph graph;
    CHECK(Run(graph, "generate", body));
}

void TestAudit() {
    test::Section("walked down every channel, the water never rises and the ground never stands over it");
    // Generated rivers with tributaries, a drawn river that crosses one of them from higher ground, and
    // detail put back over the cuts: confluences, floodplains and detail are what used to break it.
    Graph graph;
    CHECK(Run(graph, "audit", R"(
return function(params)
    local c = Terrain.Coords{}
    local base = Noises.fBm{ frequency = 0.0004, octaves = 5, amplitude = 350.0, seed = 5 }.value + c.x * 0.05 + 400.0
    local net = Water.Network{ terrain = base, region = { -6000, -6000, 6000, 6000 }, cell = 100,
        rivers = { { points = { { 3000, -5000 }, { 2500, 0 }, { 3000, 5000 } }, kind = "lowland", width = 30 } },
        generate = { rivers = { count = 3 }, tributaries = 0.5 } }
    local detail = Noises.fBm{ frequency = 0.005, octaves = 3, amplitude = 5.0, seed = 6 }.value
    local wet = net:apply(base + detail, { detail = detail })
    local a = net:audit(wet, 0)
    if #net.rivers < 3 then error("only " .. #net.rivers .. " rivers were made") end
    if a.rises > 0 or a.dry > 0 then
        error(string.format("over %d points the water rose %d times (worst %.1f m) and ran dry %d times",
                            a.points, a.rises, a.worst, a.dry))
    end
    return { height = { value = wet.height, range = { -200, 900 } } }
end
)"));
}

void TestMany() {
    test::Section("dozens of rivers and lakes fit one script, and their water stays right");
    // Before the fused carve each river cost about thirty nodes and a script held twenty or so elements.
    Graph graph;
    CHECK(Run(graph, "many", R"(
return function(params)
    local c = Terrain.Coords{}
    local base = Noises.fBm{ frequency = 0.0003, octaves = 6, amplitude = 550.0, seed = 8 }.value + c.x * 0.03 + 600.0
    local net = Water.Network{ terrain = base, region = { -12000, -12000, 12000, 12000 }, cell = 150, sea_level = 0,
        generate = { rivers = { count = 25 }, tributaries = 0.6, river = { spacing = 1500 }, lakes = { count = 10 } } }
    local wet = net:apply(base)
    if #net.rivers + #net.lakes < 35 then error("only " .. (#net.rivers + #net.lakes) .. " elements were made") end
    local a = net:audit(wet, 0)
    if a.rises > 0 or a.dry > 0 then
        error(string.format("over %d points the water rose %d times (worst %.1f m) and ran dry %d times",
                            a.points, a.rises, a.worst, a.dry))
    end
    return { height = { value = wet.height, range = { -300, 1800 } }, rivers = { value = wet.rivers, range = { -30000, 1800 } },
             lakes = { value = wet.lakes, range = { -30000, 1800 } }, sea = { value = wet.sea, range = { -30000, 1800 } } }
end
)"));
    CHECK(graph.NodeCount() < 600);
}

void TestShape() {
    test::Section("meanders loop, a cliff gives a fall, a calm mouth a delta and an open one an estuary");
    Graph graph;
    CHECK(Run(graph, "shape", R"(
local function sinuosity(r)
    local length = 0.0
    for i = 2, #r.points do
        length = length + math.sqrt((r.points[i][1] - r.points[i - 1][1]) ^ 2 + (r.points[i][2] - r.points[i - 1][2]) ^ 2)
    end
    local a, b = r.points[1], r.points[#r.points]
    return length / math.sqrt((b[1] - a[1]) ^ 2 + (b[2] - a[2]) ^ 2)
end
return function(params)
    local c = Terrain.Coords{}
    -- A plain falling to a sea along x = -5000, with a 120 m cliff along z = 3000 on its north side.
    local base = (c.x + 5000.0) * 0.004 - 8.0 + Curves.Smoothstep{ input = c.y, min = 2950.0, max = 3050.0 } * 120.0
    local net = Water.Network{ terrain = base, region = { -8000, -6000, 8000, 8000 }, cell = 100, sea_level = 0,
        rivers = { { points = { { 7000, -2000 }, { -7000, -2000 } }, kind = "lowland", width = 40, roughness = 0.8 },
                   { points = { { 3000, 7000 }, { 3000, 3500 }, { 2500, 0 } }, kind = "mountain", width = 14 },
                   { points = { { 6000, -4500 }, { -7000, -4500 } }, kind = "gorge", width = 12, mouth = "estuary" } } }
    local lowland, mountain, gorge = net.rivers[1], net.rivers[2], net.rivers[3]
    if sinuosity(lowland) < 1.4 then error("the lowland river barely meanders: " .. sinuosity(lowland)) end
    if sinuosity(gorge) > 1.15 then error("the gorge meanders like a lowland river: " .. sinuosity(gorge)) end
    if mountain.falls < 1 then error("the mountain river has no fall over the cliff") end
    local distributaries, fans = 0, 0
    for _, r in ipairs(net.rivers) do
        if r.distributary then distributaries = distributaries + 1 end
    end
    for _, l in ipairs(net.lakes) do
        if l.kind == "fan" then fans = fans + 1 end
    end
    if distributaries < 2 or fans < 1 then error("no delta: " .. distributaries .. " distributaries, " .. fans .. " fans") end
    if gorge.widths[#gorge.widths] < gorge.widths[#gorge.widths // 2] * 2.5 then error("the estuary did not widen") end
    local wet = net:apply(base)
    local a = net:audit(wet, 0)
    if a.rises > 0 or a.dry > 0 then
        error(string.format("over %d points the water rose %d times (worst %.1f m) and ran dry %d times",
                            a.points, a.rises, a.worst, a.dry))
    end
    return { height = { value = wet.height, range = { -300, 900 } } }
end
)"));
}

void TestWorldCells() {
    test::Section("with world cells, two regions in the same cells generate the same rivers");
    Graph graph;
    CHECK(Run(graph, "world", R"(
return function(params)
    local c = Terrain.Coords{}
    local base = Noises.fBm{ frequency = 0.0004, octaves = 5, amplitude = 350.0, seed = 5 }.value + c.x * 0.05 + 400.0
    local function make(region)
        return Water.Network{ terrain = base, region = region,
                              generate = { world_cell = 8000, rivers = { count = 2 }, lakes = { count = 1 } } }
    end
    local a = make({ -6000, -6000, 6000, 6000 })
    local b = make({ -7000, -5000, 5000, 7000 })
    if #a.rivers == 0 then error("nothing was generated") end
    if #a.rivers ~= #b.rivers or #a.lakes ~= #b.lakes then
        error(string.format("%d rivers and %d lakes against %d and %d", #a.rivers, #a.lakes, #b.rivers, #b.lakes))
    end
    for k, r in ipairs(a.rivers) do
        local s = b.rivers[k]
        if #r.points ~= #s.points then error("river " .. k .. " has a different course") end
        for i = 1, #r.points do
            if r.points[i][1] ~= s.points[i][1] or r.points[i][2] ~= s.points[i][2] or r.levels[i] ~= s.levels[i] then
                error("river " .. k .. " differs at point " .. i)
            end
        end
    end
    return { height = { value = base, range = { -200, 900 } } }
end
)"));
}

} // namespace

int main() {
    REQUIRE_OK(log::Init(log::Config{}));
    REQUIRE_OK(memory::Init());
    REQUIRE_OK(platform::MakeDirectory("out"));
    REQUIRE_OK(platform::MakeDirectory(kDirectory));
    TestRiver();
    TestLakeUnderSea();
    TestGenerate();
    TestAudit();
    TestMany();
    TestShape();
    TestWorldCells();
    CHECK_EQ(lua::AllocatedBytes(), u64_t{0});
    memory::Shutdown();
    log::Shutdown();
    return test::Summary("test_water");
}
