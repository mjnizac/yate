// Lands.Compose on the CPU through the sampler (no GPU): elements sized by their area are that area, ids
// name the element a sample belongs to, land and water combine in order, and a map may be all land or
// all sea. What a script author asks for, measured on what the engine builds.
#include "test_support.hpp"

#include <engine/log.hpp>
#include <engine/lua.hpp>
#include <engine/memory/allocator.hpp>
#include <engine/platform.hpp>
#include <engine/terrain/graph.hpp>
#include <engine/terrain/sampler.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using namespace engine;
using terrain::Graph;
using terrain::Value;

namespace {

constexpr const char* kDirectory = "out/test_compose";

// Lua the scatter cases check with: `expect`, the elements a scatter generated, and that land elements keep
// `spacing` between their bounding circles.
constexpr const char* kScatterChecks = R"(
local function expect(ok, what) if not ok then error(what, 2) end end
local function generated(land, id)
    local out = {}
    for _, e in ipairs(land.info.elements) do
        if type(e.id) == "string" and e.id:sub(1, #id + 1) == id .. "/" then out[#out + 1] = e end
    end
    return out
end
local function apart(elements, spacing)
    for i = 1, #elements do
        for j = i + 1, #elements do
            local a, b = elements[i], elements[j]
            if a.kind == "land" and b.kind == "land" then
                local d = math.sqrt((a.at[1] - b.at[1]) ^ 2 + (a.at[2] - b.at[2]) ^ 2)
                expect(d >= a.radius + b.radius + spacing - 1e-6, tostring(a.id) .. " and " .. tostring(b.id) .. " are " .. d .. " m apart")
            end
        end
    end
end
)";

[[nodiscard]] std::string Write(const char* name, std::string_view source) {
    const std::string path = std::string(kDirectory) + "/" + name + ".lua";
    if (std::FILE* file = std::fopen(path.c_str(), "wb"); file != nullptr) {
        (void)std::fwrite(source.data(), 1, source.size(), file);
        (void)std::fclose(file);
    }
    return path;
}

/// Runs a script whose `main` returns `land:outputs{ ... }` for the `Lands.Compose` table in `compose`.
[[nodiscard]] Status Run(Graph& graph, const char* name, std::string_view compose, std::string_view call = "Lands.Compose") {
    const std::string source = "return function(params)\n    local land = " + std::string(call) + std::string(compose)
                             + "\n    return land:outputs{ 'coast_sdf', 'region_id', 'land_mask', 'land_height', 'bathymetry' }\nend\n";
    const std::string      path = Write(name, source);
    lua::ScriptEnvironment environment;
    environment.maxX = environment.maxZ = 1000.0;
    return lua::RunScript(graph, path, environment);
}

[[nodiscard]] Value Output(const Graph& graph, std::string_view name) {
    for (usize_t i = 0; i < graph.OutputCount(); ++i) {
        if (std::string_view{graph.Output(i).name.data()} == name) {
            return graph.Output(i).value;
        }
    }
    return Value{};
}

/// Output `name` over a square grid of `side` samples `cell` apart, centred on the origin.
[[nodiscard]] std::vector<f32_t> Grid(const Graph& graph, std::string_view name, usize_t side, f32_t cell) {
    std::vector<std::array<f32_t, 2>> positions;
    const f32_t                       origin = -0.5f * cell * static_cast<f32_t>(side - 1);
    for (usize_t z = 0; z < side; ++z) {
        for (usize_t x = 0; x < side; ++x) {
            positions.push_back({origin + cell * static_cast<f32_t>(x), origin + cell * static_cast<f32_t>(z)});
        }
    }
    std::vector<f32_t> out(positions.size(), 0.0f);
    if (!terrain::SampleGraph(graph, Output(graph, name), 0, cell, positions, out)) {
        out.assign(positions.size(), 1e30f);
    }
    return out;
}

[[nodiscard]] f32_t At(const Graph& graph, std::string_view name, f32_t x, f32_t z) {
    const std::array<f32_t, 2> p{x, z};
    f32_t                      out = 1e30f;
    (void)terrain::SampleGraph(graph, Output(graph, name), 0, 4.0f, {&p, 1}, {&out, 1});
    return out;
}

/// Land area in square metres: the samples whose signed distance is negative.
[[nodiscard]] double LandArea(const Graph& graph, usize_t side, f32_t cell) {
    usize_t land = 0;
    for (const f32_t d : Grid(graph, "coast_sdf", side, cell)) {
        land += d < 0.0f ? 1 : 0;
    }
    return static_cast<double>(land) * static_cast<double>(cell) * static_cast<double>(cell);
}

void TestAreas() {
    test::Section("an element sized by its area has that area");
    struct Case {
        const char* name;
        const char* element;
    };
    // 4 km^2 each, on a 4 km map sampled every 10 m: the fringe costs well under 1 %.
    const Case cases[] = {
        {"circle", "{ shape = 'circle', area = 4e6 }"},
        {"ellipse", "{ shape = 'ellipse', area = 4e6, aspect = 2.5, rotation = 30 }"},
        {"superellipse", "{ shape = 'superellipse', area = 4e6, aspect = 1.5, exponent = 4 }"},
        {"superellipse_low", "{ shape = 'superellipse', area = 4e6, exponent = 1.5 }"},
        {"box", "{ shape = 'box', area = 4e6, aspect = 0.5, rotation = 10 }"},
        {"radial", "{ shape = 'radial', area = 4e6, harmonics = { { 3, 0.25, 0.4 }, { 5, 0.1, 1 } } }"},
        {"ring", "{ shape = 'ring', area = 4e6, width = 300 }"},
        {"capsule", "{ shape = 'capsule', area = 4e6, length = 1500, rotation = 45 }"},
        {"scaled", "{ shape = 'circle', area = 4e6, scale = 1.7 }"},
    };
    for (const Case& c : cases) {
        Graph        graph;
        const Status ran = Run(graph, c.name, std::string("{ frame = { size = 6000 }, profile = 'flat', elements = { ") + c.element + " } }");
        if (!ran) {
            std::printf("FAIL %s: %s\n", c.name, ran.error().Format().data());
            CHECK(false);
            continue;
        }
        const double area = LandArea(graph, 600, 10.0f);
        std::printf("     %-18s %.4g m2 (%.2f %%)\n", c.name, area, 100.0 * (area / 4e6 - 1.0));
        CHECK(std::fabs(area / 4e6 - 1.0) < 0.01);
    }
}

void TestIdsAndOrder() {
    test::Section("ids name the element a sample is nearest, and water cuts what came before it");
    Graph graph;
    REQUIRE_OK_VOID(Run(graph, "ids", R"({ frame = { size = 8000 }, profile = 'dome', elements = {
        { id = 'west', shape = 'circle', radius = 1200, at = { -1500, 0 } },
        { id = 'east', shape = 'ellipse', radii = { 900, 600 }, at = { 1800, 400 } },
        { id = 'bay', shape = 'circle', radius = 400, at = { -1500, 0 }, water = true },
        { id = 'islet', shape = 'circle', radius = 150, at = { -1500, 0 } } } })"));
    CHECK_EQ(At(graph, "region_id", -1500.0f, 900.0f), 1.0f);
    CHECK_EQ(At(graph, "region_id", 1800.0f, 400.0f), 2.0f);
    // The bay is water, and the islet put after it is land again.
    CHECK(At(graph, "coast_sdf", -1500.0f, 300.0f) > 0.0f);
    CHECK(At(graph, "coast_sdf", -1500.0f, 0.0f) < 0.0f);
    CHECK_EQ(At(graph, "region_id", -1500.0f, 0.0f), 4.0f);
    CHECK(At(graph, "land_mask", 0.0f, -3000.0f) < 0.01f);
}

void TestAllLandAndAllSea() {
    test::Section("a map may be all land, with lakes, or all sea");
    Graph land;
    REQUIRE_OK_VOID(Run(land, "all_land", R"({ frame = { size = 8000 }, profile = 'dome', elements = {
        { shape = 'fill' }, { shape = 'circle', radius = 500, at = { 1000, 1000 }, water = true, role = 'lake' } } })"));
    usize_t dry = 0, samples = 0;
    for (const f32_t m : Grid(land, "land_mask", 64, 120.0f)) {
        dry += m > 0.99f ? 1 : 0;
        ++samples;
    }
    CHECK(dry > samples * 9 / 10);
    CHECK(At(land, "land_mask", 1000.0f, 1000.0f) < 0.01f);
    CHECK(At(land, "land_mask", -3000.0f, 3500.0f) > 0.99f);

    Graph sea;
    REQUIRE_OK_VOID(Run(sea, "all_sea", "{ frame = { size = 8000 }, elements = {} }"));
    f32_t most = 0.0f;
    for (const f32_t m : Grid(sea, "land_mask", 32, 240.0f)) {
        most = std::max(most, m);
    }
    CHECK(most < 0.01f);
}

void TestProfiles() {
    test::Section("the profile says how land rises, and nothing else does");
    // Three islands of radius 2 km and height 500 m: a dome peaks at its centre, a rim's crest stands near
    // its coast and over its interior, a caldera's floor is under the sea.
    Graph graph;
    REQUIRE_OK_VOID(Run(graph, "profiles", R"({ frame = { size = 20000 }, height = 500, elements = {
        { id = 'dome', shape = 'circle', radius = 2000, at = { -6000, 0 }, profile = 'dome' },
        { id = 'rim', shape = 'circle', radius = 2000, at = { 0, 0 }, profile = 'rim' },
        { id = 'caldera', shape = 'circle', radius = 2000, at = { 6000, 0 }, profile = 'caldera' } } })"));
    const auto height = [&](f32_t x) { return At(graph, "land_height", x, 0.0f); };
    // The dome: highest at the centre, and the full height there.
    CHECK(height(-6000.0f) > height(-6000.0f + 1000.0f));
    CHECK(std::fabs(height(-6000.0f) - 500.0f) < 5.0f);
    // The rim: its crest, 0.12 of the radius inland (240 m), is the highest ground and stands 300 m over the
    // middle.
    f32_t crest = 0.0f, crestAt = 0.0f;
    for (f32_t x = 0.0f; x <= 2000.0f; x += 20.0f) {
        if (height(x) > crest) {
            crest   = height(x);
            crestAt = x;
        }
    }
    CHECK(std::fabs(crestAt - 1760.0f) < 60.0f);
    CHECK(crest > height(0.0f) + 300.0f);
    // The caldera: a floor below the sea in the middle, inside a ring of high ground.
    CHECK(height(6000.0f) < 0.0f);
    CHECK(height(6000.0f + 1750.0f) > 300.0f);
    // At sea, no ground was raised.
    CHECK(At(graph, "land_mask", -3000.0f, 0.0f) < 0.01f);

    // Profiles over profiles: a plateau standing on a plain that covers the map, a lake cut into the plain.
    // Each rises over its own ground and only where it is.
    Graph layered;
    REQUIRE_OK_VOID(Run(layered, "layered", R"({ frame = { size = 20000 }, height = 400, elements = {
        { shape = 'fill', profile = { 'flat', height = 40, edge = 300 } },
        { shape = 'circle', radius = 3000, at = { -5000, 0 }, smooth = 800, profile = { 'plateau', height = 300, edge = 500 } },
        { shape = 'circle', radius = 1000, at = { 5000, 0 }, water = true } } })"));
    CHECK(std::fabs(At(layered, "land_height", -5000.0f, 0.0f) - 300.0f) < 2.0f);
    CHECK(std::fabs(At(layered, "land_height", 0.0f, 5000.0f) - 40.0f) < 2.0f);
    CHECK(At(layered, "land_mask", 5000.0f, 0.0f) < 0.01f);
    CHECK(At(layered, "land_height", 3950.0f, 0.0f) < 20.0f);  // 50 m from the lake shore.

    // A lobed outline has no star at its centre: its distance there is direction-dependent unless blended
    // into the inscribed circle's, and a dome over it would have spokes and a pit.
    Graph lobed;
    REQUIRE_OK_VOID(Run(lobed, "lobed", R"({ frame = { size = 20000 }, height = 500, profile = 'dome', elements = {
        { shape = 'radial', radius = 4000, harmonics = { { 3, 0.2, 0.5 }, { 5, 0.08, 1 } } },
        { shape = 'superellipse', radii = { 2500, 1500 }, exponent = 1.5, at = { 8000, 0 } } } })"));
    for (const f32_t cx : {0.0f, 8000.0f}) {
        f32_t low = 1e30f, high = -1e30f;
        for (int k = 0; k < 16; ++k) {
            const f32_t a = 0.3926991f * static_cast<f32_t>(k);
            const f32_t h = At(lobed, "land_height", cx + 60.0f * std::cos(a), 60.0f * std::sin(a));
            low = std::min(low, h), high = std::max(high, h);
        }
        std::printf("     around the centre at x = %.0f: %.2f to %.2f m\n", static_cast<double>(cx), static_cast<double>(low),
                    static_cast<double>(high));
        CHECK(high - low < 1.0f);
        CHECK(At(lobed, "land_height", cx, 0.0f) >= high - 0.5f);
    }
}

void TestSeabed() {
    test::Section("the sea floor has its own forms: seamounts, guyots and a trench off an active margin");
    // The geologic margin: a shelf to 130 m over 40 km, a slope to 3500 m over 60 km. A seamount and a
    // guyot on the abyssal plain, and an island whose margin is active.
    Graph graph;
    REQUIRE_OK_VOID(Run(graph, "seabed", R"({ frame = { size = 400000 }, margin = 'geologic', height = 800, elements = {
        { id = 'island', shape = 'circle', radius = 20000, at = { -100000, 0 }, profile = 'dome', margin = 'active' },
        { id = 'seamount', shape = 'circle', radius = 12000, at = { 100000, 60000 }, seabed = true,
          profile = { 'cone', height = 2500 } },
        { id = 'guyot', shape = 'circle', radius = 12000, at = { 100000, -60000 }, seabed = true,
          profile = { 'plateau', height = 2000, edge = 3000 } } } })"));
    const auto floor = [&](f32_t x, f32_t z) { return At(graph, "bathymetry", x, z); };
    const f32_t plain = floor(150000.0f, 0.0f);
    std::printf("     abyssal %.0f m, seamount top %.0f m, guyot top %.0f and %.0f m, trench %.0f m\n",
                static_cast<double>(plain), static_cast<double>(floor(100000.0f, 60000.0f)),
                static_cast<double>(floor(100000.0f, -60000.0f)), static_cast<double>(floor(104000.0f, -60000.0f)),
                static_cast<double>(floor(-100000.0f, 114000.0f)));
    CHECK(std::fabs(plain + 3500.0f) < 200.0f);
    // The seamount's summit stands 2500 m over the plain; the guyot is flat on top.
    CHECK(std::fabs(floor(100000.0f, 60000.0f) - (plain + 2500.0f)) < 250.0f);
    CHECK(std::fabs(floor(100000.0f, -60000.0f) - floor(104000.0f, -60000.0f)) < 60.0f);
    CHECK(floor(100000.0f, -60000.0f) > plain + 1800.0f);
    // The trench lies at the foot of the slope, 94 km off the coast, deeper than the plain.
    CHECK(floor(-100000.0f, 114000.0f) < plain - 1500.0f);
    CHECK(floor(-100000.0f, 175000.0f) > floor(-100000.0f, 114000.0f) + 1000.0f);
}

void TestEditsAndReport() {
    test::Section("an element is changed by its id, and the export is told what was built");
    constexpr const char* kBase = R"({ frame = { size = 12000 }, profile = 'dome', elements = {
        { id = 'big', shape = 'circle', area = 6e6, at = { -2500, 0 } },
        { id = 'small', shape = 'ellipse', area = 2e6, aspect = 2, at = { 3000, 1000 } },
        { id = 'gone', shape = 'circle', radius = 400, at = { 3000, -3500 } } })";
    Graph before;
    REQUIRE_OK_VOID(Run(before, "edit_before", std::string(kBase) + " }"));
    // "Make the small one 20 % bigger, move the big one 1 km north, and take the third away."
    Graph after;
    REQUIRE_OK_VOID(Run(after, "edit_after", std::string(kBase)
                                                 + ", edit = { small = { scale = 1.2 }, big = { offset = { 0, 1000 } } }, remove = { 'gone' } }"));
    const auto areaOf = [](const Graph& graph, f32_t id) {
        usize_t cells = 0;
        const std::vector<f32_t> ids  = Grid(graph, "region_id", 600, 20.0f);
        const std::vector<f32_t> dist = Grid(graph, "coast_sdf", 600, 20.0f);
        for (usize_t k = 0; k < ids.size(); ++k) {
            cells += dist[k] < 0.0f && ids[k] == id ? 1 : 0;
        }
        return static_cast<double>(cells) * 400.0;
    };
    const double grown = areaOf(after, 2.0f) / areaOf(before, 2.0f);
    std::printf("     the small island grew by %.3f in area (1.44 asked)\n", grown);
    CHECK(std::fabs(grown - 1.44) < 0.02);
    CHECK(At(after, "land_mask", -2500.0f, 1000.0f + 1300.0f) > 0.99f);  // The big one, moved north.
    CHECK(At(after, "land_mask", -2500.0f, -1300.0f + 200.0f) < 0.01f);
    CHECK(At(after, "land_mask", 3000.0f, -3500.0f) < 0.01f);            // The third, gone.
    CHECK(At(before, "land_mask", 3000.0f, -3500.0f) > 0.99f);

    // The report names every element with what it is and what it measured.
    const std::string_view report = after.Report();
    std::printf("     report: %.160s...\n", std::string(report).c_str());
    CHECK(report.find(R"("id":"big")") != std::string_view::npos);
    CHECK(report.find(R"("id":"small")") != std::string_view::npos);
    CHECK(report.find(R"("id":"gone")") == std::string_view::npos);
    CHECK(report.find(R"("area_measured":)") != std::string_view::npos);
    CHECK(report.find(R"("kind":"land")") != std::string_view::npos);
}

void TestPresets() {
    test::Section("every island preset builds, at the land fraction asked for, and edits by id");
    for (const char* preset : {"high_island", "low_island", "volcano", "caldera", "rim_island", "mesa_island", "atoll"}) {
        Graph        graph;
        const Status ran = Run(graph, preset, std::string("{ preset = '") + preset + "', seed = 4, size = 12000 }", "Lands.Island");
        if (!ran) {
            std::printf("FAIL %s: %s\n", preset, ran.error().Format().data());
            CHECK(false);
            continue;
        }
        const double fraction = LandArea(graph, 400, 30.0f) / (12000.0 * 12000.0);
        std::printf("     %-12s land %.3f of the map\n", preset, fraction);
        CHECK(fraction > 0.02 && fraction < 0.5);
    }
    // One element, the asked-for fraction of the map within the fringe; then the main mass grown by an edit.
    Graph plain, grown;
    REQUIRE_OK_VOID(Run(plain, "rim_fraction", "{ preset = 'rim_island', seed = 2, size = 12000, land_fraction = 0.2 }", "Lands.Island"));
    REQUIRE_OK_VOID(Run(grown, "rim_grown", "{ preset = 'rim_island', seed = 2, size = 12000, land_fraction = 0.2, "
                                            "edit = { main = { scale = 1.1 } } }", "Lands.Island"));
    const double before = LandArea(plain, 400, 30.0f), after = LandArea(grown, 400, 30.0f);
    CHECK(std::fabs(before / (12000.0 * 12000.0) - 0.2) < 0.005);
    CHECK(std::fabs(after / before - 1.21) < 0.02);
    Graph retired;
    const Status refused = Run(retired, "retired", "{ preset = 'volcano', roughness = 0.5 }", "Lands.Island");
    CHECK(!refused && std::string_view{refused.error().Format().data()}.find("no longer takes 'roughness'") != std::string_view::npos);
    Graph unnamed;
    const Status needs = Run(unnamed, "unnamed", "{ seed = 1 }", "Lands.Island");
    CHECK(!needs && std::string_view{needs.error().Format().data()}.find("needs a 'preset'") != std::string_view::npos);
}

/// Runs `body`, Lua that builds `land` and may `error()` on what it checks, and returns its outputs.
[[nodiscard]] Status RunBody(Graph& graph, const char* name, std::string_view body) {
    const std::string source = std::string(kScatterChecks) + "return function(params)\n" + std::string(body)
                             + "\n    return land:outputs{ 'coast_sdf', 'region_id', 'land_mask', 'land_height' }\nend\n";
    const std::string      path = Write(name, source);
    lua::ScriptEnvironment environment;
    environment.maxX = environment.maxZ = 1000.0;
    return lua::RunScript(graph, path, environment);
}

void TestScatter() {
    test::Section("a scatter makes the islands asked for, apart, stable as the count grows, and the same in any window");
    struct Case {
        const char* name;
        const char* body;
    };
    const Case cases[] = {
        // N islands of a range of areas, the spacing kept between every pair and from the declared land.
        {"scatter_count", R"(
    local land = Lands.Compose{ seed = 3, frame = { size = 48000 }, profile = 'dome', elements = {
        { id = 'main', shape = 'circle', radius = 5000 },
        { id = 'isles', shape = 'radial', scatter = { count = 9, area = { 2e6, 20e6 }, spacing = 1200, margin = 1500 } } } }
    local isles = generated(land, 'isles')
    expect(#isles == 9, 'placed ' .. #isles .. ' of 9')
    for _, e in ipairs(isles) do expect(e.area >= 2e6 * 0.999 and e.area <= 20e6 * 1.001, e.id .. ' area ' .. e.area) end
    apart(land.info.elements, 1200)
    expect(land.info.scatters.isles.skipped == 0, 'skipped some'))"},
        // One more asked for leaves the first nine where they were.
        {"scatter_stable", R"(
    local function build(n)
        return Lands.Compose{ seed = 3, frame = { size = 48000 }, profile = 'dome', elements = {
            { id = 'isles', shape = 'radial', scatter = { count = n, area = { 2e6, 20e6 }, spacing = 1200 } } } }
    end
    local a, land = generated(build(9), 'isles'), build(10)
    local b = generated(land, 'isles')
    expect(#b == 10, 'placed ' .. #b .. ' of 10')
    for k = 1, 9 do
        expect(a[k].id == b[k].id and a[k].at[1] == b[k].at[1] and a[k].at[2] == b[k].at[2] and a[k].area == b[k].area,
               a[k].id .. ' moved when one more was asked for')
    end)"},
        // A share of the map: the scatter's sizes are scaled until the land is 30 %.
        {"scatter_fraction", R"(
    local land = Lands.Compose{ seed = 5, frame = { size = 48000 }, profile = 'dome', land_fraction = 0.3, elements = {
        { id = 'isles', shape = 'radial', scatter = { count = 30, area = { 2e6, 20e6 }, spacing = 800 } } } }
    expect(math.abs(land.info.land_fraction - 0.3) < 0.02, 'land fraction ' .. land.info.land_fraction))"},
        // Patterns: a chain shrinking along its path, a ring, islands at given places, clusters, a grid.
        {"scatter_patterns", R"(
    local land = Lands.Compose{ seed = 7, frame = { size = 96000 }, profile = 'cone', elements = {
        { id = 'chain', shape = 'ellipse', aspect = 1.6, scatter = { pattern = 'line', count = 6, area = { 8e6, 8e6 },
          decay = 0.8, spacing = 600, path = { { -40000, -30000 }, { 0, -25000 }, { 40000, -35000 } } } },
        { id = 'ring', shape = 'circle', scatter = { pattern = 'ring', count = 8, area = { 1e6, 1e6 }, center = { 25000, 20000 },
          radius = 9000 } },
        { id = 'given', shape = 'circle', scatter = { pattern = 'centers', centers = { { -30000, 25000, 4e6 }, { -20000, 30000, 2e6 } } } },
        { id = 'clusters', shape = 'radial', scatter = { pattern = 'cluster', count = 10, clusters = 2, spread = 3000,
          area = { 0.5e6, 3e6 }, spacing = 300 } } } }
    local chain = generated(land, 'chain')
    expect(#chain == 6, 'chain placed ' .. #chain)
    for k = 2, #chain do expect(chain[k].area < chain[k - 1].area, 'the chain does not shrink along its path') end
    expect(#generated(land, 'ring') == 8, 'ring placed ' .. #generated(land, 'ring'))
    local given = generated(land, 'given')
    expect(#given == 2 and given[1].at[1] == -30000 and math.abs(given[1].area - 4e6) < 1, 'centers not where given')
    expect(#generated(land, 'clusters') >= 8, 'clusters placed ' .. #generated(land, 'clusters'))
    apart(land.info.elements, 0))"},
        // The unbounded world: two windows sharing world cells keep the same islands in them.
        {"scatter_world", R"(
    local function build(region)
        return Lands.Compose{ seed = 9, frame = { world_cell = 16000, region = region }, profile = 'dome', elements = {
            { id = 'isles', shape = 'radial', scatter = { count = 3, area = { 2e6, 12e6 }, spacing = 1000 } } } }
    end
    local a = generated(build({ 0, 0, 64000, 64000 }), 'isles')
    local b = generated(build({ 32000, 0, 96000, 64000 }), 'isles')
    local seen, shared = {}, 0
    for _, e in ipairs(a) do seen[e.id] = e end
    for _, e in ipairs(b) do
        local o = seen[e.id]
        if o then
            shared = shared + 1
            expect(o.at[1] == e.at[1] and o.at[2] == e.at[2] and o.area == e.area, e.id .. ' differs between windows')
        elseif e.at[1] >= 32000 and e.at[1] < 64000 then
            error(e.id .. ' is in both windows but only one has it')
        end
    end
    expect(shared >= 6, 'only ' .. shared .. ' islands shared')
    local land = build({ 0, 0, 64000, 64000 }))"},
    };
    for (const Case& c : cases) {
        Graph        graph;
        const Status ran = RunBody(graph, c.name, c.body);
        ++test::g_checks;
        if (!ran) {
            ++test::g_failures;
            std::printf("FAIL %s: %s\n", c.name, ran.error().Format().data());
        } else {
            std::printf("     %s\n", c.name);
        }
    }
}

void TestArchipelagoPresets() {
    test::Section("every archipelago preset places nearly all its islands, and takes count, area and edits");
    struct Case {
        const char* name;
        const char* body;
    };
    const Case cases[] = {
        {"arch_presets", R"(
    for _, name in ipairs({ 'scattered', 'cluster', 'hotspot', 'arc', 'atolls', 'shattered', 'chain' }) do
        local land = Lands.Archipelago{ preset = name, seed = 2, size = 48000 }
        for id, s in pairs(land.info.scatters) do
            expect(s.placed >= 0.75 * (s.placed + s.skipped), name .. ': ' .. id .. ' placed ' .. s.placed .. ' of ' .. (s.placed + s.skipped))
        end
    end
    local land = Lands.Archipelago{ preset = 'scattered', seed = 2, size = 48000 })"},
        {"arch_request", R"(
    -- "Six islands of 3 to 5 km2, and make the third one larger."
    local land = Lands.Archipelago{ preset = 'scattered', seed = 2, size = 48000, count = 6, area = { 3e6, 5e6 },
                                    edit = { ['isles/3'] = { scale = 1.3 } } }
    local isles = generated(land, 'isles')
    expect(#isles == 6, 'placed ' .. #isles)
    for _, e in ipairs(isles) do
        local hi = e.id == 'isles/3' and 5e6 * 1.69 or 5e6
        expect(e.area >= 3e6 * 0.999 and e.area <= hi * 1.001, e.id .. ' area ' .. e.area)
    end
    expect(generated(land, 'isles')[3].area > 3e6 * 1.69 * 0.999, 'isles/3 did not grow'))"},
    };
    for (const Case& c : cases) {
        Graph        graph;
        const Status ran = RunBody(graph, c.name, c.body);
        ++test::g_checks;
        if (!ran) {
            ++test::g_failures;
            std::printf("FAIL %s: %s\n", c.name, ran.error().Format().data());
        } else {
            std::printf("     %s\n", c.name);
        }
    }
    Graph        retired;
    const Status refused = Run(retired, "arch_retired", "{ layout = 'arc' }", "Lands.Archipelago");
    CHECK(!refused && std::string_view{refused.error().Format().data()}.find("no longer takes 'layout'") != std::string_view::npos);
}

void TestRidgeAndLands() {
    test::Section("a ridge is as high as asked, with its saddles and its spurs");
    struct Case {
        const char* name;
        const char* body;
    };
    const Case cases[] = {
        // "A range 1000 m high along this line, two saddles, spurs every 2400 m."
        {"ridge", R"(
    local land = Lands.Compose{ seed = 1, frame = { size = 40000 }, profile = { 'flat', height = 100, edge = 400 }, elements = {
        { shape = 'fill' },
        { id = 'r', ridge = { points = { { -15000, -2000 }, { 15000, 2000 } }, width = 2000, height = 1000, saddles = 2,
                              spacing = 2400 } } } }
    local xs, zs = {}, {}
    for k = 0, 3000 do xs[#xs + 1], zs[#zs + 1] = -15000 + 10 * k, -2000 + 4000 * k / 3000 end
    local h = Terrain.Sample{ value = land.land_height, xs = xs, zs = zs, cell = 4 }
    local top = -math.huge
    for k = 1, #h do top = math.max(top, h[k]) end
    expect(math.abs(top - 1100) <= 20, 'the crest stands ' .. (top - 100) .. ' m over the land, 1000 asked')
    for _, at in ipairs({ 1000, 2000 }) do
        local low = math.huge
        for k = at - 100, at + 100 do low = math.min(low, h[k + 1]) end
        expect(low < 100 + 750, 'no saddle near ' .. (10 * at) .. ' m along the crest: ' .. low)
    end
    local spurs = {}
    for _, e in ipairs(land.info.elements) do
        if tostring(e.id):find('^r/spur/') then spurs[#spurs + 1] = e end
    end
    expect(#spurs == 13, #spurs .. ' spurs along 30.3 km at 2400 m')
    local mids = { xs = {}, zs = {} }
    for k, e in ipairs(spurs) do
        mids.xs[k], mids.zs[k] = e.at[1], e.at[2]
        if k > 1 then
            local d = math.sqrt((e.from[1] - spurs[k - 1].from[1]) ^ 2 + (e.from[2] - spurs[k - 1].from[2]) ^ 2)
            expect(math.abs(d - 2400) <= 240, e.id .. ' is ' .. d .. ' m from the last')
        end
    end
    for k, v in ipairs(Terrain.Sample{ value = land.land_height, xs = mids.xs, zs = mids.zs, cell = 4 }) do
        expect(v > 100 + 400, spurs[k].id .. ' does not stand out: ' .. v)
    end)"},
    };
    const auto check = [](const char* name, const std::string& body) {
        Graph        graph;
        const Status ran = RunBody(graph, name, body);
        ++test::g_checks;
        if (!ran) {
            ++test::g_failures;
            std::printf("FAIL %s: %s\n", name, ran.error().Format().data());
        } else {
            std::printf("     %s\n", name);
        }
    };
    for (const Case& c : cases) {
        check(c.name, c.body);
    }
}

void TestErrors() {
    test::Section("a wrong composition is refused with what was expected");
    struct Case {
        const char* name;
        const char* compose;
        const char* fragment;
    };
    const Case cases[] = {
        {"unknown_key", "{ frame = { size = 1000 }, elements = {}, colour = 1 }", "does not take 'colour'"},
        {"no_frame", "{ elements = {} }", "needs 'frame'"},
        {"repeated_id", "{ frame = { size = 1000 }, profile = 'cone', elements = { { id = 'a', shape = 'circle', radius = 5 }, { id = 'a', shape = 'circle', radius = 5 } } }",
         "repeats the id 'a'"},
        {"no_profile", "{ frame = { size = 1000 }, elements = { { shape = 'circle', radius = 50 } } }", "needs a profile"},
        {"bad_preset", "{ frame = { size = 1000 }, profile = 'volcano', elements = { { shape = 'circle', radius = 50 } } }", "has profile 'volcano'"},
        {"bad_profile_key", "{ frame = { size = 1000 }, profile = { 'rim', crest = 3 }, elements = { { shape = 'circle', radius = 50 } } }", "does not take 'crest'"},
        {"water_profile", "{ frame = { size = 1000 }, profile = 'dome', elements = { { shape = 'fill' }, { shape = 'circle', radius = 50, water = true, profile = 'dome' } } }", "takes no profile"},
        {"edit_unknown", "{ frame = { size = 1000 }, profile = 'cone', elements = { { id = 'a', shape = 'circle', radius = 5 } }, edit = { b = { scale = 2 } } }", "names 'b', which is no element's id; the ids are a"},
        {"remove_unknown", "{ frame = { size = 1000 }, profile = 'cone', elements = { { shape = 'circle', radius = 5 } }, remove = { 'x' } }", "names 'x'"},
        {"area_polygon", "{ frame = { size = 1000 }, profile = 'cone', elements = { { shape = 'polygon', area = 5, points = { { 0, 0 }, { 1, 0 }, { 0, 1 } } } } }",
         "cannot be sized by 'area'"},
        {"ridge_key", "{ frame = { size = 1000 }, profile = 'cone', elements = { { ridge = { points = { { 0, 0 }, { 100, 0 } }, width = 10, height = 5, crest = 2 } } } }",
         "does not take 'crest'"},
    };
    for (const Case& c : cases) {
        Graph        graph;
        const Status ran = Run(graph, c.name, c.compose);
        ++test::g_checks;
        if (ran || std::string_view{ran.error().Format().data()}.find(c.fragment) == std::string_view::npos) {
            ++test::g_failures;
            std::printf("FAIL %s: expected an error naming '%s', got: %.240s\n", c.name, c.fragment,
                        ran ? "success" : ran.error().Format().data());
        }
    }
}

} // namespace

int main() {
    REQUIRE_OK(log::Init(log::Config{}));
    REQUIRE_OK(memory::Init());
    REQUIRE_OK(platform::MakeDirectory("out"));
    REQUIRE_OK(platform::MakeDirectory(kDirectory));
    TestAreas();
    TestIdsAndOrder();
    TestAllLandAndAllSea();
    TestProfiles();
    TestSeabed();
    TestEditsAndReport();
    TestPresets();
    TestScatter();
    TestArchipelagoPresets();
    TestRidgeAndLands();
    TestErrors();
    CHECK_EQ(lua::AllocatedBytes(), u64_t{0});
    memory::Shutdown();
    log::Shutdown();
    return test::Summary("test_compose");
}
