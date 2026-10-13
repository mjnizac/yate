// Shapes, transforms and profiles against values worked out on paper (milestone 11).
//
// `test_kernels` checks every kernel against its CPU reference, and `test_seams` that sections agree.
// Neither says that a circle's distance is the distance to a circle: the reference is the same formula
// written a second time. This runs the exporter on scripts built from the public API and compares the
// raw floats with closed-form expressions, so what is under test is what a script author gets.
//
// The grid is 64 x 64 samples at 8 m over [-256, 256), so a sample is at an exact multiple of 8 and a
// vertex placed at a multiple of 8 lands exactly on one, which is the case a crossing rule gets wrong.

#include "test_support.hpp"

#include <engine/platform.hpp>
#include <engine/terrain/sha256.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#ifndef TERRAIN_EXPORT_EXE
#    error "TERRAIN_EXPORT_EXE is set by tests/CMakeLists.txt"
#endif
#ifndef ENGINE_SOURCE_ROOT
#    error "ENGINE_SOURCE_ROOT is set by tests/CMakeLists.txt"
#endif

namespace {

constexpr const char*  kDirectory = "out/test_geometry";
constexpr std::size_t  kSide      = 64;
constexpr double       kSpacing   = 8.0;
constexpr double       kOrigin    = -256.0;

/// Where a field was sampled. The default is the 64 x 64 grid at 8 m over [-256, 256) that most cases use.
struct Grid {
    double      originX = kOrigin;
    double      originZ = kOrigin;
    double      spacing = kSpacing;
    std::size_t side    = kSide;
    std::size_t section = 16;
};

struct Field {
    std::vector<float> values;
    std::string        error;
    Grid               grid;
    [[nodiscard]] bool Ok() const { return values.size() == grid.side * grid.side; }
};

[[nodiscard]] std::string ReadFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

/// Runs the exporter on `script` over `grid` with `--raw` and reads back the output named `output`.
///
/// `extra` is appended to the command line (seed, parameters). Any failure is in `Field::error`, which
/// carries the exporter's own text so a test can check what a user would have read.
[[nodiscard]] Field RunExport(const std::string& script, const std::string& directory, const Grid& grid,
                              const std::string& output, const std::string& extra = "") {
    Field field;
    field.grid = grid;

    char range[160];
    std::snprintf(range, sizeof(range), "--min %.6f,%.6f --max %.6f,%.6f --resolution %.6f --section %zu",
                  grid.originX, grid.originZ,
                  grid.originX + grid.spacing * static_cast<double>(grid.side),
                  grid.originZ + grid.spacing * static_cast<double>(grid.side), grid.spacing,
                  grid.section);

    const std::string capture = std::string(kDirectory) + "/capture.txt";
    std::string       command = std::string("\"") + TERRAIN_EXPORT_EXE + "\" --script \"" + script
                          + "\" " + range + " --raw " + extra + " --out \"" + directory + "\" > \""
                          + capture + "\" 2>&1";
#ifdef _WIN32
    command = "\"" + command + "\"";
#endif
    const int code = std::system(command.c_str());
    if (code != 0) {
        field.error = ReadFile(capture);
        return field;
    }
    const std::string bytes = ReadFile(directory + "/" + output + ".raw");
    if (bytes.size() == grid.side * grid.side * sizeof(float)) {
        field.values.resize(grid.side * grid.side);
        std::memcpy(field.values.data(), bytes.data(), bytes.size());
    } else {
        field.error = output + ".raw is " + std::to_string(bytes.size()) + " bytes";
    }
    return field;
}

/// Exports `body` (the text of a Lua `main`) over `grid`.
///
/// `body` returns its value as `local result`, and the wrapper names it the `d` output.
[[nodiscard]] Field Export(const char* name, const std::string& body, const Grid& grid = Grid{},
                           const std::string& extra = "") {
    const std::string script = std::string(kDirectory) + "/" + name + ".lua";
    {
        std::ofstream out(script, std::ios::binary);
        out << "local function main()\n" << body
            << "\n    return { d = { value = result, range = { -1000, 1000 } } }\nend\nreturn main\n";
    }
    return RunExport(script, std::string(kDirectory) + "/" + name, grid, "d", extra);
}

using Expected = std::function<double(double x, double z)>;

[[nodiscard]] double WorldX(const Field& field, std::size_t column) {
    return field.grid.originX + field.grid.spacing * static_cast<double>(column);
}
[[nodiscard]] double WorldZ(const Field& field, std::size_t row) {
    return field.grid.originZ + field.grid.spacing * static_cast<double>(row);
}

/// Compares every sample with `expected`, and reports the worst and where.
void Compare(const char* label, const Field& field, const Expected& expected, double tolerance) {
    ++test::g_checks;
    if (!field.Ok()) {
        ++test::g_failures;
        std::printf("FAIL %s: no field (%.200s)\n", label, field.error.c_str());
        return;
    }
    const std::size_t side    = field.grid.side;
    double            worst   = 0.0;
    std::size_t       worstAt = 0;
    for (std::size_t row = 0; row < side; ++row) {
        for (std::size_t column = 0; column < side; ++column) {
            const double error = std::fabs(static_cast<double>(field.values[row * side + column])
                                           - expected(WorldX(field, column), WorldZ(field, row)));
            if (error > worst) {
                worst   = error;
                worstAt = row * side + column;
            }
        }
    }
    if (worst > tolerance) {
        ++test::g_failures;
        std::printf("FAIL %s: worst error %.6g at sample (%zu, %zu), tolerance %.3g\n", label, worst,
                    worstAt % side, worstAt / side, tolerance);
        return;
    }
    std::printf("     %-34s worst error %.3g\n", label, worst);
}

[[nodiscard]] double At(const Field& field, double x, double z) {
    const std::size_t column =
        static_cast<std::size_t>(std::llround((x - field.grid.originX) / field.grid.spacing));
    const std::size_t row =
        static_cast<std::size_t>(std::llround((z - field.grid.originZ) / field.grid.spacing));
    return field.Ok() ? static_cast<double>(field.values[row * field.grid.side + column]) : 0.0;
}

[[nodiscard]] double BoxDistance(double x, double z, double hx, double hz) {
    const double qx = std::fabs(x) - hx;
    const double qz = std::fabs(z) - hz;
    return std::hypot(std::max(qx, 0.0), std::max(qz, 0.0)) + std::min(std::max(qx, qz), 0.0);
}

void TestPrimitives() {
    test::Section("primitives are the distances they say");

    Compare("circle at the origin",
            Export("circle", "    local result = Geometry.Circle{ radius = 100 }"),
            [](double x, double z) { return std::hypot(x, z) - 100.0; }, 2e-4);

    Compare("a circle of radius 0 is a point",
            Export("point", "    local result = Geometry.Circle{ radius = 0 }"),
            [](double x, double z) { return std::hypot(x, z); }, 2e-4);

    Compare("box, full size 240 by 80",
            Export("box", "    local result = Geometry.Box{ size = { 240, 80 } }"),
            [](double x, double z) { return BoxDistance(x, z, 120.0, 40.0); }, 2e-4);

    // Rounding pulls the corner in: the corner of a 120 x 40 half-size box rounded by 20 is 20 m from
    // the rounded-off centre square, so its distance there is that square's corner distance minus 20.
    Compare("rounded box",
            Export("rounded", "    local result = Geometry.Box{ size = { 240, 80 }, rounding = 20 }"),
            [](double x, double z) {
                const double qx = std::fabs(x) - 100.0;
                const double qz = std::fabs(z) - 20.0;
                return std::hypot(std::max(qx, 0.0), std::max(qz, 0.0))
                       + std::min(std::max(qx, qz), 0.0) - 20.0;
            },
            2e-4);

    Compare("Chebyshev box has square faces",
            Export("chebyshev", "    local result = Geometry.BoxChebyshev{ size = { 200, 120 } }"),
            [](double x, double z) { return std::max(std::fabs(x) - 100.0, std::fabs(z) - 60.0); },
            2e-4);

    // The ellipse distance is a bound and not exact, but it is exact on the axes and has the right
    // sign everywhere. Radii 128 and 64: on the x axis the edge is at 128, on the z axis at 64.
    const Field ellipse = Export("ellipse", "    local result = Geometry.Ellipse{ radii = { 128, 64 } }");
    ++test::g_checks;
    if (!ellipse.Ok()) {
        ++test::g_failures;
        std::printf("FAIL ellipse: no field (%.200s)\n", ellipse.error.c_str());
    } else {
        CHECK(std::fabs(At(ellipse, 128.0, 0.0)) < 2e-4);
        CHECK(std::fabs(At(ellipse, 0.0, 64.0)) < 2e-4);
        CHECK(At(ellipse, 0.0, 0.0) < 0.0);
        CHECK(At(ellipse, 200.0, 0.0) > 0.0);
        CHECK(At(ellipse, 0.0, 120.0) > 0.0);
        // Inside along x and outside along z at the same distance: the radii are not swapped.
        CHECK(At(ellipse, 96.0, 0.0) < 0.0);
        CHECK(At(ellipse, 0.0, 96.0) > 0.0);
    }
}

void TestPlacement() {
    test::Section("placement moves, turns and scales the shape, not the world");

    Compare("translated circle",
            Export("moved", "    local at = Terrain.Transform{ at = { 64, -32 } }\n"
                            "    local result = Geometry.Circle{ position = at, radius = 50 }"),
            [](double x, double z) { return std::hypot(x - 64.0, z + 32.0) - 50.0; }, 2e-4);

    // Quarter turn: the long axis of a 240 x 80 box, which is x, lies along z afterwards.
    Compare("box turned 90 degrees",
            Export("turned", "    local at = Terrain.Transform{ rotation = 90 }\n"
                             "    local result = Geometry.Box{ position = at, size = { 240, 80 } }"),
            [](double x, double z) { return BoxDistance(x, z, 40.0, 120.0); }, 5e-4);

    // Rotation about a pivot: a box whose left edge is its pivot, turned 90 degrees, hangs from the
    // pivot along +z instead of +x.
    Compare("box turned about its corner",
            Export("pivot", "    local at = Terrain.Transform{ at = { 0, 0 }, rotation = 90,\n"
                            "                                 pivot = { -120, 0 } }\n"
                            "    local result = Geometry.Box{ position = at, size = { 240, 80 } }"),
            [](double x, double z) {
                // The local box [-120, 120] x [-40, 40] has its pivot (-120, 0) at the origin and,
                // turned a quarter, runs from z = 0 to z = 240 and spans x in [-40, 40].
                return BoxDistance(x, z - 120.0, 40.0, 120.0);
            },
            5e-4);

    Compare("uniform scale",
            Export("scaled", "    local at = Terrain.Transform{ scale = 2 }\n"
                             "    local result = Geometry.Circle{ position = at, radius = 40 }"),
            [](double x, double z) { return std::hypot(x, z) / 2.0 - 40.0; }, 2e-4);
}

void TestPolygons() {
    test::Section("polygons and polylines");

    // Vertices at multiples of 8, so samples fall exactly on the corners and on the horizontal edges.
    Compare("square with vertices on samples",
            Export("square", "    local result = Geometry.Segments{ points = {\n"
                             "        { -104, -104 }, { 104, -104 }, { 104, 104 }, { -104, 104 } } }"),
            [](double x, double z) {
                const double qx = std::fabs(x) - 104.0;
                const double qz = std::fabs(z) - 104.0;
                return std::hypot(std::max(qx, 0.0), std::max(qz, 0.0))
                       + std::min(std::max(qx, qz), 0.0);
            },
            2e-4);

    // A "U": the mouth is outside and both arms and the base are inside. Parity gets this wrong if a
    // vertex is counted twice or not at all.
    const Field u = Export("u_shape",
                           "    local result = Geometry.Segments{ points = {\n"
                           "        { -104, -104 }, { 104, -104 }, { 104, 104 }, { 56, 104 },\n"
                           "        { 56, -24 }, { -56, -24 }, { -56, 104 }, { -104, 104 } } }");
    ++test::g_checks;
    if (!u.Ok()) {
        ++test::g_failures;
        std::printf("FAIL u shape: no field (%.200s)\n", u.error.c_str());
    } else {
        CHECK(At(u, 0.0, 56.0) > 0.0);    // The mouth.
        CHECK(At(u, -80.0, 40.0) < 0.0);  // The left arm.
        CHECK(At(u, 80.0, 40.0) < 0.0);   // The right arm.
        CHECK(At(u, 0.0, -64.0) < 0.0);   // The base.
        CHECK(At(u, 0.0, -112.0) > 0.0);  // Under the base.
        // Distance from the mouth to its nearest wall is 56 m to the arm at x = +-56.
        CHECK(std::fabs(At(u, 0.0, 56.0) - 56.0) < 2e-4);
    }

    // A polyline is unsigned: |z| beside the segment, and the distance to the end past it.
    Compare("polyline is unsigned",
            Export("line", "    local result = Geometry.Segments{ closed = false,\n"
                           "        points = { { -96, 0 }, { 96, 0 } } }"),
            [](double x, double z) {
                const double along = std::max(std::fabs(x) - 96.0, 0.0);
                return std::hypot(along, z);
            },
            2e-4);

    // Many segments: a regular 40-gon is within its own sagitta of the circle.
    std::string points;
    constexpr int kSides = 40;
    for (int i = 0; i < kSides; ++i) {
        const double angle = 2.0 * 3.14159265358979323846 * i / kSides;
        char         entry[64];
        std::snprintf(entry, sizeof(entry), "{ %.6f, %.6f }%s", 100.0 * std::cos(angle),
                      100.0 * std::sin(angle), i + 1 < kSides ? ", " : "");
        points += entry;
    }
    const Field ring = Export("ngon", "    local result = Geometry.Segments{ points = { " + points + " } }");
    Compare("a 40-gon is a circle to its sagitta", ring,
            [](double x, double z) { return std::hypot(x, z) - 100.0; }, 0.35);
}

void TestProfiles() {
    test::Section("profiles turn a distance into a landform");

    Compare("cone: linear profile of a circle",
            Export("cone", "    local base = Geometry.Circle{ radius = 128 }\n"
                           "    local result = Curves.Profile{ input = base, kind = 'linear',\n"
                           "                                   width = 128 } * 60.0"),
            [](double x, double z) {
                const double u = std::clamp((128.0 - std::hypot(x, z)) / 128.0, 0.0, 1.0);
                return 60.0 * u;
            },
            5e-3);

    Compare("plateau is flat on top",
            Export("plateau", "    local base = Geometry.Circle{ radius = 128 }\n"
                              "    local result = Curves.Profile{ input = base, kind = 'plateau',\n"
                              "                                   edge = 32, falloff = 32 } * 40.0"),
            [](double x, double z) {
                const double d = std::hypot(x, z) - 128.0;
                const double t = std::clamp((d + 32.0) / 64.0, 0.0, 1.0);
                return 40.0 * (1.0 - t * t * (3.0 - 2.0 * t));
            },
            5e-3);

    Compare("terrace snaps to steps",
            Export("terrace", "    local ramp = Terrain.Coords{}.x\n"
                              "    local result = Curves.Terrace{ input = ramp, step = 64,\n"
                              "                                   sharpness = 1.0 }"),
            [](double x, double) { return 64.0 * std::floor(x / 64.0 + 0.5); }, 2e-4);

    // Smooth minimum: with a width of 0 it would be min; at equal operands it dips by k / 4.
    const Field smooth = Export("smooth_min",
                                "    local a = Geometry.Circle{ radius = 60 }\n"
                                "    local b = Geometry.Box{ size = { 100, 100 } }\n"
                                "    local result = Combine.SmoothMin{ a = a, b = b, k = 40 }");
    Compare("smooth minimum never exceeds the minimum", smooth,
            [&](double x, double z) {
                const double a = std::hypot(x, z) - 60.0;
                const double b = BoxDistance(x, z, 50.0, 50.0);
                const double m = std::min(a, b);
                const double h = std::clamp(0.5 + 0.5 * (b - a) / 40.0, 0.0, 1.0);
                return b + (a - b) * h - 40.0 * h * (1.0 - h) <= m + 1e-9
                           ? (b + (a - b) * h - 40.0 * h * (1.0 - h))
                           : m;
            },
            2e-4);
}

void TestLandforms() {
    test::Section("landforms and shapes from the prelude");

    Compare("Landforms.Cone, placed",
            Export("l_cone", "    local result = Landforms.Cone{ at = { 48, -16 }, radius = 96,\n"
                             "                                  height = 70 }"),
            [](double x, double z) {
                const double u = std::clamp((96.0 - std::hypot(x - 48.0, z + 16.0)) / 96.0, 0.0, 1.0);
                return 70.0 * u;
            },
            5e-3);

    Compare("Landforms.Bowl digs",
            Export("l_bowl", "    local result = Landforms.Bowl{ radius = 100, depth = 30 }"),
            [](double x, double z) {
                const double u = std::clamp((100.0 - std::hypot(x, z)) / 100.0, 0.0, 1.0);
                return -30.0 * (u * u * (3.0 - 2.0 * u));
            },
            5e-3);

    Compare("Landforms.Pyramid has square faces",
            Export("l_pyramid", "    local result = Landforms.Pyramid{ size = { 160, 160 }, height = 40 }"),
            [](double x, double z) {
                const double d = std::max(std::fabs(x) - 80.0, std::fabs(z) - 80.0);
                return 40.0 * std::clamp(-d / 80.0, 0.0, 1.0);
            },
            5e-3);

    Compare("Landforms.Cylinder has a flat top",
            Export("l_cylinder", "    local result = Landforms.Cylinder{ radius = 96, height = 50, edge = 8 }"),
            [](double x, double z) {
                const double d = std::hypot(x, z) - 96.0;
                const double t = std::clamp((d + 8.0) / 16.0, 0.0, 1.0);
                return 50.0 * (1.0 - t * t * (3.0 - 2.0 * t));
            },
            5e-3);

    // A straight ridge along x: 64 m across, 50 m high, smooth in section, ending at x = +-192.
    Compare("Landforms.Ridge along a curve",
            Export("l_ridge", "    local result = Landforms.Ridge{\n"
                              "        points = { { -192, 0 }, { 192, 0 } }, width = 64, height = 50 }"),
            [](double x, double z) {
                const double along = std::max(std::fabs(x) - 192.0, 0.0);
                const double d     = std::hypot(along, z);
                const double u     = std::clamp((32.0 - d) / 32.0, 0.0, 1.0);
                return 50.0 * (u * u * (3.0 - 2.0 * u));
            },
            5e-3);

    Compare("Landforms.Trench is the same cut downwards",
            Export("l_trench", "    local result = Landforms.Trench{\n"
                               "        points = { { -192, 0 }, { 192, 0 } }, width = 64, depth = 20 }"),
            [](double x, double z) {
                const double along = std::max(std::fabs(x) - 192.0, 0.0);
                const double d     = std::hypot(along, z);
                const double u     = std::clamp((32.0 - d) / 32.0, 0.0, 1.0);
                return -20.0 * (u * u * (3.0 - 2.0 * u));
            },
            5e-3);

    Compare("Shapes.Union is the nearer of two",
            Export("s_union", "    local a = Shapes.Circle{ at = { -64, 0 }, radius = 50 }\n"
                              "    local b = Shapes.Circle{ at = { 64, 0 }, radius = 50 }\n"
                              "    local result = Shapes.Union{ a, b }"),
            [](double x, double z) {
                return std::min(std::hypot(x + 64.0, z) - 50.0, std::hypot(x - 64.0, z) - 50.0);
            },
            2e-4);

    Compare("Shapes.Subtract cuts one out of another",
            Export("s_subtract", "    local a = Shapes.Circle{ radius = 100 }\n"
                                 "    local b = Shapes.Circle{ at = { 60, 0 }, radius = 50 }\n"
                                 "    local result = Shapes.Subtract{ a = a, b = b }"),
            [](double x, double z) {
                return std::max(std::hypot(x, z) - 100.0, -(std::hypot(x - 60.0, z) - 50.0));
            },
            2e-4);

    Compare("Shapes.Mask is 1 inside and 0 outside",
            Export("s_mask", "    local c = Shapes.Circle{ radius = 80 }\n"
                             "    local result = Shapes.Mask{ shape = c, feather = 16 }"),
            [](double x, double z) {
                const double d = std::hypot(x, z) - 80.0;
                const double t = std::clamp((d + 16.0) / 32.0, 0.0, 1.0);
                return 1.0 - t * t * (3.0 - 2.0 * t);
            },
            2e-4);

    Compare("a warp of amplitude 0 changes nothing",
            Export("s_warp0", "    local position = Shapes.Warp{ amplitude = 0 }\n"
                              "    local result = Shapes.Circle{ position = position, radius = 100 }"),
            [](double x, double z) { return std::hypot(x, z) - 100.0; }, 1e-3);

    // A warp that is on bends the edge: some samples must differ from the plain circle, and the
    // field must stay close to it, within the amplitude.
    const Field warped = Export("s_warp", "    local position = Shapes.Warp{ amplitude = 30, seed = 3 }\n"
                                          "    local result = Shapes.Circle{ position = position, radius = 100 }");
    ++test::g_checks;
    if (!warped.Ok()) {
        ++test::g_failures;
        std::printf("FAIL warp: no field (%.200s)\n", warped.error.c_str());
    } else {
        std::size_t moved = 0;
        double      worst = 0.0;
        for (std::size_t row = 0; row < kSide; ++row) {
            for (std::size_t column = 0; column < kSide; ++column) {
                const double x = kOrigin + kSpacing * static_cast<double>(column);
                const double z = kOrigin + kSpacing * static_cast<double>(row);
                const double e =
                    std::fabs(static_cast<double>(warped.values[row * kSide + column])
                              - (std::hypot(x, z) - 100.0));
                moved += e > 0.5 ? 1 : 0;
                worst = std::max(worst, e);
            }
        }
        CHECK(moved > 100);
        // The warp moves a sample by at most `amplitude` on each axis, and a distance is 1-Lipschitz.
        CHECK(worst <= 30.0 * std::sqrt(2.0) + 1e-3);
    }
}

void TestPreludeErrors() {
    test::Section("a prelude error names the script line, not the prelude");

    struct Case {
        const char* name;
        const char* body;
        const char* fragment;
    };
    const Case cases[] = {
        {"p_missing_radius", "    local result = Landforms.Cone{ height = 10 }", "needs 'radius'"},
        {"p_unknown_key", "    local result = Landforms.Cone{ radius = 5, height = 1, slope = 2 }",
         "does not take 'slope'"},
        {"p_wrong_type", "    local result = Shapes.Circle{ radius = 'big' }", "got string"},
        {"p_bad_profile",
         "    local result = Landforms.Dome{ radius = 5, height = 1, profile = 'cliff' }",
         "does not know the profile"},
        {"p_inner_error", "    local result = Landforms.Cone{ radius = -5, height = 10 }", "radius"},
        {"p_one_shape", "    local a = Shapes.Circle{ radius = 5 }\n"
                       "    local result = Shapes.Union{ a }", "at least two"},
        {"p_spline_limit",
         "    local points = {}\n"
         "    for i = 1, 40 do points[i] = { i * 8, (i % 5) * 10 } end\n"
         "    local result = Shapes.Spline{ points = points, subdivisions = 8 }",
         "limit is 128"},
    };
    for (const Case& c : cases) {
        const Field field = Export(c.name, c.body);
        ++test::g_checks;
        const std::string script = std::string(c.name) + ".lua:";
        const bool        named  = field.error.find(script) != std::string::npos;
        const bool        inside = field.error.find("prelude/") != std::string::npos;
        if (field.Ok() || field.error.find(c.fragment) == std::string::npos || !named || inside) {
            ++test::g_failures;
            std::printf("FAIL %s: expected '%s' at the script's line and no prelude frame, got: %.300s\n",
                        c.name, c.fragment, field.Ok() ? "success" : field.error.c_str());
        }
    }

    // And the line is the line of the call, not just any line: the call below is on line 3.
    const Field second = Export("p_line", "    local a = 1\n"
                                          "    local result = Landforms.Cone{ height = 10 }");
    ++test::g_checks;
    if (second.Ok() || second.error.find("p_line.lua:3:") == std::string::npos) {
        ++test::g_failures;
        std::printf("FAIL p_line: expected line 3, got: %.200s\n", second.error.c_str());
    }
}

void TestPreludeHash() {
    test::Section("the prelude is part of the provenance");

    const std::string metadata = ReadFile(std::string(kDirectory) + "/l_cone/metadata.json");
    const std::string key      = "\"prelude_hash\": \"";
    const std::size_t at       = metadata.find(key);
    ++test::g_checks;
    if (at == std::string::npos) {
        ++test::g_failures;
        std::printf("FAIL the sidecar has no prelude_hash\n");
        return;
    }
    const std::string recorded = metadata.substr(at + key.size(), 64);

    // Recomputed here from the files next to this executable, in the engine's load order.
    // The prelude sits next to the exporter, so its directory is the exporter's.
    std::string directory = TERRAIN_EXPORT_EXE;
    directory.erase(directory.find_last_of("/\\") + 1);

    engine::terrain::Sha256 hash;
    for (const char* name : {"10_shapes.lua", "20_landforms.lua", "30_ranges.lua", "40_dunes.lua", "50_lands.lua"}) {
        const std::string text = ReadFile(directory + "lua/prelude/" + name);
        hash.Update(std::string_view{name});
        hash.Update("\n", 1);
        hash.Update(text.data(), text.size());
    }
    const std::array<char, 65> expected = hash.Final();
    CHECK(recorded == std::string(expected.data(), 64));
}

/// The four reference terrains are documentation, and documentation that does not run is a lie. Each is
/// exported over its own world at a coarse spacing, and what comes back has to have relief: a script
/// that quietly produced a flat field would pass an "exits 0" check.
void TestReferenceTerrains() {
    test::Section("the reference terrains run and have relief");

    const char* const scripts[] = {"volcanic_island", "mountain_range", "canyon", "mesas",
                                   "coast"};
    for (const char* name : scripts) {
        const std::string directory = std::string(kDirectory) + "/ref_" + name;
        const std::string capture   = std::string(kDirectory) + "/capture.txt";
        std::string       command =
            std::string("\"") + TERRAIN_EXPORT_EXE + "\" --script \"" + ENGINE_SOURCE_ROOT
            + "/assets/scripts/shapes/" + name
            + ".lua\" --min -2048,-2048 --max 2048,2048 --resolution 32 --section 64 --raw --out \""
            + directory + "\" > \"" + capture + "\" 2>&1";
#ifdef _WIN32
        command = "\"" + command + "\"";
#endif
        const int code = std::system(command.c_str());
        ++test::g_checks;
        if (code != 0) {
            ++test::g_failures;
            std::printf("FAIL %s: exit %d: %.300s\n", name, code, ReadFile(capture).c_str());
            continue;
        }
        const std::string bytes = ReadFile(directory + "/height.raw");
        ++test::g_checks;
        if (bytes.size() != 128 * 128 * sizeof(float)) {
            ++test::g_failures;
            std::printf("FAIL %s: height.raw is %zu bytes\n", name, bytes.size());
            continue;
        }
        std::vector<float> heights(128 * 128);
        std::memcpy(heights.data(), bytes.data(), bytes.size());
        const auto [low, high] = std::minmax_element(heights.begin(), heights.end());
        bool finite = true;
        for (const float value : heights) {
            finite = finite && std::isfinite(value);
        }
        std::printf("     %-18s heights %.1f to %.1f m\n", name, static_cast<double>(*low),
                    static_cast<double>(*high));
        CHECK(finite);
        // More than a hundred metres of relief over four kilometres: a flat field fails this.
        CHECK(*high - *low > 100.0f);
    }
}

// --- Audit: degenerate input, limits and numerical behaviour ---------------------------------------

struct Point {
    double x;
    double z;
};

/// The distance to a polygon and its inside, written again in double precision with the ordinary
/// even-odd rule. It shares no code with the kernel or with the chain the binding builds, so agreement
/// says the chain is assembled in the right order, the wrap is right and the parity is right.
[[nodiscard]] double ReferencePolygon(const std::vector<Point>& polygon, double px, double pz) {
    double best   = 1e300;
    bool   inside = false;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Point a = polygon[i];
        const Point b = polygon[(i + 1) % polygon.size()];

        const double bax = b.x - a.x;
        const double baz = b.z - a.z;
        const double pax = px - a.x;
        const double paz = pz - a.z;
        const double len = bax * bax + baz * baz;
        double       h   = len > 0.0 ? (pax * bax + paz * baz) / len : 0.0;
        h                = std::clamp(h, 0.0, 1.0);
        best             = std::min(best, std::hypot(pax - bax * h, paz - baz * h));

        if ((a.z > pz) != (b.z > pz)) {
            const double xAt = a.x + (pz - a.z) * (b.x - a.x) / (b.z - a.z);
            if (px < xAt) {
                inside = !inside;
            }
        }
    }
    return inside ? -best : best;
}

[[nodiscard]] std::string LuaPoints(const std::vector<Point>& points) {
    std::string text = "{ ";
    for (std::size_t i = 0; i < points.size(); ++i) {
        char entry[64];
        std::snprintf(entry, sizeof(entry), "{ %.6f, %.6f }%s", points[i].x, points[i].z,
                      i + 1 < points.size() ? ", " : "");
        text += entry;
    }
    return text + " }";
}

/// A deterministic pseudo-random polygon: a fixed generator, so a failure names a reproducible case.
[[nodiscard]] std::vector<Point> RandomPolygon(std::uint32_t seed, std::size_t count, bool star) {
    std::uint32_t state = seed * 2654435761u + 12345u;
    const auto    next  = [&]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<double>(state >> 8) / 16777216.0;
    };
    std::vector<Point> points;
    for (std::size_t i = 0; i < count; ++i) {
        if (star) {
            // Around a centre, so it is simple but concave.
            const double angle  = 2.0 * 3.14159265358979323846 * static_cast<double>(i)
                                  / static_cast<double>(count);
            const double radius = 60.0 + 140.0 * next();
            points.push_back({std::round(radius * std::cos(angle) * 1e6) / 1e6,
                              std::round(radius * std::sin(angle) * 1e6) / 1e6});
        } else {
            // Anywhere: usually self-intersecting, which the even-odd rule still defines.
            points.push_back({std::round((next() * 400.0 - 200.0) * 1e6) / 1e6,
                              std::round((next() * 400.0 - 200.0) * 1e6) / 1e6});
        }
    }
    return points;
}

void TestPolygonsAgainstReference() {
    test::Section("polygons agree with an independent double-precision reference");

    struct Case {
        std::uint32_t seed;
        std::size_t   count;
        bool          star;
    };
    // Counts straddle the two-segments-per-node boundary (odd and even), and include the smallest
    // polygon and one of 63 vertices, which is 32 nodes.
    const Case cases[] = {{1, 3, true},  {2, 4, true},  {3, 5, true},  {4, 8, true},
                          {5, 21, true}, {6, 63, true}, {7, 6, false}, {8, 17, false},
                          {9, 40, false}};
    for (const Case& c : cases) {
        const std::vector<Point> polygon = RandomPolygon(c.seed, c.count, c.star);
        char                     name[48];
        std::snprintf(name, sizeof(name), "ref_polygon_%u", c.seed);
        const Field field = Export(
            name, "    local result = Geometry.Segments{ points = " + LuaPoints(polygon) + " }");
        char label[96];
        std::snprintf(label, sizeof(label), "%zu-gon, %s", c.count,
                      c.star ? "star-shaped" : "random, self-intersecting");
        // Both sides see the same decimal vertices. The GPU holds them as f32, so a sample within
        // ~1e-4 m of an edge can flip sign, which changes the value by that much and no more.
        Compare(label, field,
                [&](double x, double z) { return ReferencePolygon(polygon, x, z); }, 2e-3);
    }
}

void TestDegenerateGeometry() {
    test::Section("degenerate shapes behave");

    // A repeated vertex makes a zero-length segment: its distance is the distance to the point and it
    // never crosses a scanline, so the polygon is the same polygon.
    const std::vector<Point> square{{-104, -104}, {104, -104}, {104, 104}, {-104, 104}};
    const std::vector<Point> repeated{{-104, -104}, {-104, -104}, {104, -104}, {104, 104},
                                      {104, 104},   {-104, 104}};
    Compare("repeated vertices change nothing",
            Export("degen_repeat",
                   "    local result = Geometry.Segments{ points = " + LuaPoints(repeated) + " }"),
            [&](double x, double z) { return ReferencePolygon(square, x, z); }, 2e-3);

    // Collinear vertices along an edge: the same square again.
    const std::vector<Point> collinear{{-104, -104}, {0, -104}, {104, -104}, {104, 0},
                                       {104, 104},   {0, 104},  {-104, 104}, {-104, 0}};
    Compare("collinear vertices change nothing",
            Export("degen_collinear",
                   "    local result = Geometry.Segments{ points = " + LuaPoints(collinear) + " }"),
            [&](double x, double z) { return ReferencePolygon(square, x, z); }, 2e-3);

    // A polygon whose area is zero has no inside: the sign must never be negative.
    const Field flat = Export(
        "degen_flat", "    local result = Geometry.Segments{ points = { { -100, 0 }, { 100, 0 }, "
                      "{ 0, 0 } } }");
    ++test::g_checks;
    if (!flat.Ok()) {
        ++test::g_failures;
        std::printf("FAIL zero-area polygon: no field (%.200s)\n", flat.error.c_str());
    } else {
        bool negative = false;
        for (const float value : flat.values) {
            negative = negative || value < -1e-3f;
        }
        CHECK(!negative);
    }

    // Exactly the centre of an ellipse: the distance bound divides by a zero gradient there, and the
    // kernel falls back to the smaller radius, which is the true distance to the edge.
    const Field ellipse = Export("degen_ellipse_centre",
                                 "    local result = Geometry.Ellipse{ radii = { 128, 64 } }");
    CHECK(std::fabs(At(ellipse, 0.0, 0.0) + 64.0) < 1e-4);

    // A box exactly as rounded as it is wide is a stadium-free "pill": its smaller half-size is the
    // rounding, so it is a circle on one axis. The distance is still the exact box distance.
    Compare("a box rounded by its smaller half-size",
            Export("degen_pill", "    local result = Geometry.Box{ size = { 200, 80 }, rounding = 40 }"),
            [](double x, double z) {
                // Two semicircles of radius 40 on a 120 m bar.
                const double along = std::max(std::fabs(x) - 60.0, 0.0);
                return std::hypot(along, z) - 40.0;
            },
            2e-4);
}

void TestLimitsAndInvalidInput() {
    test::Section("limits and invalid input are refused, with the line");

    // The segment limit is exact: 128 segments is the largest polygon and 129 is refused.
    const auto ngon = [](int sides) {
        std::vector<Point> points;
        for (int i = 0; i < sides; ++i) {
            const double angle = 2.0 * 3.14159265358979323846 * i / sides;
            points.push_back({std::round(100.0 * std::cos(angle) * 1e6) / 1e6,
                              std::round(100.0 * std::sin(angle) * 1e6) / 1e6});
        }
        return points;
    };
    Compare("a 128-gon is the largest polygon",
            Export("limit_128",
                   "    local result = Geometry.Segments{ points = " + LuaPoints(ngon(128)) + " }"),
            [&](double x, double z) { return ReferencePolygon(ngon(128), x, z); }, 2e-3);

    struct Refused {
        const char* name;
        std::string body;
        const char* fragment;
    };
    const Refused refused[] = {
        {"limit_129", "    local result = Geometry.Segments{ points = " + LuaPoints(ngon(129)) + " }",
         "at most 128"},
        {"limit_line_130",
         "    local result = Geometry.Segments{ closed = false, points = " + LuaPoints(ngon(130))
             + " }",
         "at most 129 points"},
        {"nan_radius", "    local result = Geometry.Circle{ radius = 0 / 0 }", "radius"},
        {"inf_radius", "    local result = Geometry.Circle{ radius = math.huge }", "non-finite"},
        {"inf_placement",
         "    local p = Terrain.Transform{ at = { math.huge, 0 } }\n"
         "    local result = Geometry.Circle{ position = p, radius = 5 }",
         "finite"},
        {"zero_box", "    local result = Geometry.Box{ size = { 0, 10 } }", "size"},
        {"over_rounded_box", "    local result = Geometry.Box{ size = { 10, 10 }, rounding = 6 }",
         "rounding"},
        {"flat_ellipse", "    local result = Geometry.Ellipse{ radii = { 0, 5 } }", "radii"},
        {"empty_polygon", "    local result = Shapes.Polygon{ points = {} }", "empty list"},
        {"short_point", "    local result = Shapes.Polygon{ points = { { 0, 0 }, { 1 }, { 2, 2 } } }",
         "pair"},
        {"empty_union", "    local result = Shapes.Union{}", "at least two"},
        {"negative_feather",
         "    local result = Shapes.Mask{ shape = Shapes.Circle{ radius = 5 }, feather = -1 }",
         "feather"},
        {"warp_unknown_key", "    local result = Shapes.Warp{ amplitud = 5 }", "does not take"},
        {"zero_profile_width",
         "    local result = Curves.Profile{ input = Geometry.Circle{ radius = 5 }, kind = 'linear',\n"
         "                                   width = 0 }",
         "width"},
        {"negative_terrace", "    local result = Curves.Terrace{ input = Terrain.Coords{}.x, step = -4 }",
         "step"},
        {"terrace_sharpness",
         "    local result = Curves.Terrace{ input = Terrain.Coords{}.x, step = 4, sharpness = 2 }",
         "sharpness"},
        {"negative_smooth",
         "    local c = Geometry.Circle{ radius = 5 }\n"
         "    local result = Combine.SmoothMax{ a = c, b = c, k = -3 }",
         "greater than zero"},
        {"ridge_one_point",
         "    local result = Landforms.Ridge{ points = { { 0, 0 } }, width = 50, height = 10 }",
         "at least 2"},
        {"zero_ridge_width",
         "    local result = Landforms.Ridge{ points = { { 0, 0 }, { 50, 0 } }, width = 0, height = 10 }",
         "width"},
    };
    for (const Refused& r : refused) {
        const Field field = Export(r.name, r.body);
        ++test::g_checks;
        const bool named = field.error.find(std::string(r.name) + ".lua:") != std::string::npos;
        if (field.Ok() || field.error.find(r.fragment) == std::string::npos || !named) {
            ++test::g_failures;
            std::printf("FAIL %s: expected '%s' with the script's line, got: %.260s\n", r.name,
                        r.fragment, field.Ok() ? "success" : field.error.c_str());
        }
    }
}

void TestOriginAndResolution() {
    test::Section("far from the origin, and at other resolutions");

    // Positions that are multiples of 8 m are exact in f32 out to 2^21, so a shape far from the origin
    // is as exact as one at it.
    Grid far;
    far.originX = 500000.0;
    far.originZ = 500000.0;
    const std::string placed =
        "    local at = Terrain.Transform{ at = { 500256, 500256 } }\n"
        "    local result = Geometry.Circle{ position = at, radius = 100 }";
    Compare("5e5 m from the origin, exact spacing", Export("far_exact", placed, far),
            [](double x, double z) { return std::hypot(x - 500256.0, z - 500256.0) - 100.0; }, 1e-3);

    // A spacing f32 cannot hold exactly puts each sample position off by up to half a ulp, which at
    // 5e5 m is 0.016 m. That is a property of positions in f32, not of the shape, and the bound is the
    // ulp: the test says it rather than hiding it in a loose tolerance.
    // The exporter snaps `--min` to the nearest sample index, and a sample's position is that integer
    // times the spacing, both in f32. The reference does the same, so what is measured is the shape
    // and not where the grid happened to land.
    Grid fine = far;
    fine.spacing = 0.7;
    const long long firstIndex = std::llround(far.originX / fine.spacing);
    Compare("5e5 m from the origin, inexact spacing", Export("far_inexact", placed, fine),
            [&](double x, double z) {
                const long long column = std::llround((x - fine.originX) / fine.spacing);
                const long long row    = std::llround((z - fine.originZ) / fine.spacing);
                const float     px = static_cast<float>(firstIndex + column) * 0.7f;
                const float     pz = static_cast<float>(firstIndex + row) * 0.7f;
                return std::hypot(static_cast<double>(px) - 500256.0,
                                  static_cast<double>(pz) - 500256.0) - 100.0;
            },
            2e-3);

    // The same graph at 8 m and 16 m over the same world: the coarse samples are a subset of the fine
    // ones, and a pointwise graph must agree on them exactly. Noise is in it, through the warp.
    const std::string body =
        "    local position = Shapes.Warp{ amplitude = 40, seed = 5 }\n"
        "    local cone = Landforms.Cone{ at = { 30, -20 }, radius = 180, height = 90,\n"
        "                                 position = position }\n"
        "    local ridge = Landforms.Ridge{ points = { { -220, 80 }, { 0, 140 }, { 200, 60 } },\n"
        "                                   width = 70, height = 40, position = position }\n"
        "    local result = Combine.SmoothMax{ a = cone, b = ridge, k = 15 }";
    Grid coarse;
    coarse.spacing = 16.0;
    coarse.side    = 32;
    const Field fineField   = Export("res_8", body);
    const Field coarseField = Export("res_16", body, coarse);
    ++test::g_checks;
    if (!fineField.Ok() || !coarseField.Ok()) {
        ++test::g_failures;
        std::printf("FAIL resolutions: no field (%.200s)\n",
                    (fineField.Ok() ? coarseField.error : fineField.error).c_str());
    } else {
        std::size_t differing = 0;
        for (std::size_t row = 0; row < coarse.side; ++row) {
            for (std::size_t column = 0; column < coarse.side; ++column) {
                const float a = coarseField.values[row * coarse.side + column];
                const float b = fineField.values[(2 * row) * kSide + 2 * column];
                differing += std::memcmp(&a, &b, sizeof(float)) != 0 ? 1 : 0;
            }
        }
        std::printf("     8 m against 16 m: %zu of %zu coincident samples differ\n", differing,
                    coarse.side * coarse.side);
        CHECK_EQ(differing, std::size_t{0});
    }
}

void TestContinuity() {
    test::Section("profiles are continuous, across section borders too");

    // 2 m samples and 16-sample sections: a border every 32 m. A step in the profile, or a seam
    // between sections, would show as a jump larger than the steepest slope times the spacing.
    Grid fine;
    fine.spacing = 2.0;
    fine.side    = 256;
    fine.section = 16;

    struct Landform {
        const char* name;
        const char* body;
        double      steepest; // metres of height per metre of ground
    };
    const Landform landforms[] = {
        // Linear cone: 100 m over 200 m.
        {"cont_cone", "    local result = Landforms.Cone{ radius = 200, height = 100 }", 0.5},
        // Smoothstep: the steepest point is 1.5 x the average slope.
        {"cont_dome",
         "    local result = Landforms.Dome{ radius = 200, height = 100, profile = 'smooth' }", 0.75},
        // The scarp: plateau profile over edge + falloff = 44 m, smoothstep, so 1.5 / 44 per metre.
        {"cont_plateau",
         "    local result = Landforms.Plateau{ size = { 300, 200 }, height = 100, rotation = 25,\n"
         "                                     edge = 4, falloff = 40 }",
         100.0 * 1.5 / 44.0},
        {"cont_ridge",
         "    local result = Landforms.Ridge{ points = { { -200, -40 }, { 0, 60 }, { 200, -20 } },\n"
         "                                   width = 120, height = 60, profile = 'linear' }",
         60.0 / 60.0 * 1.2},
    };
    for (const Landform& l : landforms) {
        const Field field = Export(l.name, l.body, fine);
        ++test::g_checks;
        if (!field.Ok()) {
            ++test::g_failures;
            std::printf("FAIL %s: no field (%.200s)\n", l.name, field.error.c_str());
            continue;
        }
        double widest = 0.0;
        for (std::size_t row = 0; row < fine.side; ++row) {
            for (std::size_t column = 0; column + 1 < fine.side; ++column) {
                const double a = field.values[row * fine.side + column];
                const double b = field.values[row * fine.side + column + 1];
                const double c = column < fine.side && row + 1 < fine.side
                                     ? field.values[(row + 1) * fine.side + column]
                                     : a;
                widest = std::max({widest, std::fabs(b - a), std::fabs(c - a)});
            }
        }
        const double bound = l.steepest * fine.spacing * 1.02 + 1e-3;
        std::printf("     %-14s largest step %.3f m between neighbours, bound %.3f m\n", l.name,
                    widest, bound);
        CHECK(widest <= bound);
    }
}

void TestSectionsAndInteraction() {
    test::Section("shapes through erosion, noise and sections");

    // A graph that mixes every kind of op: shapes, noise, a mask, a terrace, hydraulic and thermal
    // erosion with a blur between. Exported twice with different section sizes, which has to agree
    // to the byte, because the halo of the erosion crosses every border.
    const std::string script = std::string(kDirectory) + "/interaction.lua";
    {
        std::ofstream out(script, std::ios::binary);
        out << R"(local function main(params)
    local position = Shapes.Warp{ amplitude = 30, frequency = 0.01, seed = params.seed }
    local hill = Landforms.Cone{ at = { 20, 10 }, radius = 150, height = 80, position = position }
    local mesa = Landforms.Plateau{ at = { -90, -70 }, size = { 120, 90 }, height = 60, rotation = 30,
                                    edge = 3, falloff = 25, position = position }
    local noise = Noises.fBm{ frequency = 0.02, octaves = 4, amplitude = 12, seed = params.seed + 1 }
    local mask = Shapes.Mask{ shape = Shapes.Circle{ radius = 170, position = position }, feather = 40 }
    local land = Combine.SmoothMax{ a = hill, b = mesa, k = 14 } + noise.value * mask
    local stepped = Curves.Terrace{ input = land, step = 12, sharpness = 0.6 }
    local carved = Erosion.Hydraulic{ input = stepped, iterations = 6, rain = 0.3, capacity = 20 }
    local smooth = Filters.Blur{ input = carved, radius = 1 }
    local settled = Erosion.Thermal{ input = smooth, iterations = 5, talus = 1.0, strength = 0.3 }
    return { d = { value = settled, range = { -50, 200 } } }
end
return main
)";
    }
    Grid wide;
    wide.spacing = 4.0;
    wide.side    = 128;
    Grid small = wide;
    small.section = 16;
    wide.section  = 64;
    const Field whole  = RunExport(script, std::string(kDirectory) + "/interaction_64", wide, "d",
                                   "--seed 11");
    const Field pieces = RunExport(script, std::string(kDirectory) + "/interaction_16", small, "d",
                                   "--seed 11");
    ++test::g_checks;
    if (!whole.Ok() || !pieces.Ok()) {
        ++test::g_failures;
        std::printf("FAIL interaction: no field (%.200s)\n",
                    (whole.Ok() ? pieces.error : whole.error).c_str());
        return;
    }
    const bool identical = std::memcmp(whole.values.data(), pieces.values.data(),
                                       whole.values.size() * sizeof(float)) == 0;
    std::printf("     sections of 64 and 16 samples: %s\n", identical ? "byte-identical" : "DIFFER");
    CHECK(identical);

    // And it is deterministic: the same job twice gives the same bytes.
    const Field again = RunExport(script, std::string(kDirectory) + "/interaction_64b", wide, "d",
                                  "--seed 11");
    ++test::g_checks;
    CHECK(again.Ok() && std::memcmp(whole.values.data(), again.values.data(),
                                    whole.values.size() * sizeof(float)) == 0);
}

/// `position` on a noise: the unwarped path is unchanged, the units are metres applied once, and a
/// position field of the wrong dimension is refused with the line.
void TestWarpedNoise() {
    test::Section("noise sampled at a position field");

    const char* const kinds[] = {"fBm", "Ridged", "Billow"};
    const auto noise = [](const char* kind, const std::string& extra) {
        return std::string("    local result = Noises.") + kind
               + "{ frequency = 0.01, octaves = 4, amplitude = 50, seed = 3" + extra + " }.value";
    };

    // A shift by whole samples, so the shifted grid lands on the same lattice and the comparison is
    // exact up to the rounding of one addition.
    constexpr double kShiftX = 136.0;
    constexpr double kShiftZ = -96.0;
    Grid             shifted;
    shifted.originX += kShiftX;
    shifted.originZ += kShiftZ;

    for (const char* kind : kinds) {
        char label[64];

        const Field plain = Export((std::string("wn_plain_") + kind).c_str(), noise(kind, ""));
        const Field world = Export((std::string("wn_coords_") + kind).c_str(),
                                   "    local c = Terrain.Coords{}\n"
                                       + noise(kind, ", position = Vec.Combine{ c.x, c.z }"));
        ++test::g_checks;
        const bool same = plain.Ok() && world.Ok()
                          && std::memcmp(plain.values.data(), world.values.data(),
                                         plain.values.size() * sizeof(float)) == 0;
        std::snprintf(label, sizeof(label), "%s at its own position is the plain noise", kind);
        std::printf("     %-34s %s\n", label, same ? "byte-identical" : "DIFFER");
        CHECK(same);

        // Moved by a constant: the same field read somewhere else.
        const Field moved = Export(
            (std::string("wn_moved_") + kind).c_str(),
            "    local c = Terrain.Coords{}\n"
                + noise(kind, ", position = Vec.Combine{ c.x + 136.0, c.z - 96.0 }"));
        const Field there = Export((std::string("wn_there_") + kind).c_str(), noise(kind, ""), shifted);
        std::snprintf(label, sizeof(label), "%s moved by a constant", kind);
        Compare(label, moved,
                [&](double x, double z) { return At(there, x + kShiftX, z + kShiftZ); }, 1e-3);

        // Scale is applied once: sampling at 2p with frequency f is sampling at p with frequency 2f.
        const Field doubled = Export(
            (std::string("wn_double_") + kind).c_str(),
            "    local c = Terrain.Coords{}\n"
                + noise(kind, ", position = Vec.Combine{ c.x * 2.0, c.z * 2.0 }"));
        const Field fast = Export(
            (std::string("wn_fast_") + kind).c_str(),
            std::string("    local result = Noises.") + kind
                + "{ frequency = 0.02, octaves = 4, amplitude = 50, seed = 3 }.value");
        std::snprintf(label, sizeof(label), "%s with the position doubled", kind);
        Compare(label, doubled, [&](double x, double z) { return At(fast, x, z); }, 5e-2);
    }

    // The real API: Shapes.Warp straight into each noise, and the result agrees across section sizes.
    const std::string script = std::string(kDirectory) + "/warped_noise.lua";
    {
        std::ofstream out(script, std::ios::binary);
        out << R"(local function main(params)
    local w = Shapes.Warp{ amplitude = 60, frequency = 0.004, octaves = 3, seed = params.seed }
    local a = Noises.fBm{ frequency = 0.01, octaves = 4, amplitude = 30, position = w }
    local b = Noises.Ridged{ frequency = 0.008, octaves = 5, amplitude = 40, position = w, seed = 1 }
    local c = Noises.Billow{ frequency = 0.006, octaves = 3, amplitude = 20, position = w, seed = 2 }
    local sum = a.value + b.value + c.value
    local carved = Erosion.Hydraulic{ input = sum, iterations = 4, rain = 0.3, capacity = 20 }
    return { d = { value = carved, range = { -200, 200 } } }
end
return main
)";
    }
    Grid wide;
    wide.spacing = 4.0;
    wide.side    = 128;
    Grid small   = wide;
    small.section = 16;
    wide.section  = 64;
    const Field whole  = RunExport(script, std::string(kDirectory) + "/warped_64", wide, "d", "--seed 5");
    const Field pieces = RunExport(script, std::string(kDirectory) + "/warped_16", small, "d", "--seed 5");
    ++test::g_checks;
    const bool identical = whole.Ok() && pieces.Ok()
                           && std::memcmp(whole.values.data(), pieces.values.data(),
                                          whole.values.size() * sizeof(float)) == 0;
    std::printf("     Shapes.Warp into every noise, sections 64 and 16: %s\n",
                identical ? "byte-identical" : "DIFFER");
    CHECK(identical);
    if (whole.Ok()) {
        double lowest = 1e30, highest = -1e30;
        for (const float v : whole.values) {
            lowest  = std::min(lowest, static_cast<double>(v));
            highest = std::max(highest, static_cast<double>(v));
        }
        ++test::g_checks;
        CHECK(std::isfinite(lowest) && std::isfinite(highest) && highest - lowest > 10.0);
    }

    // Erosion.Gullies is pointwise: the same bytes whatever the section size, and nothing on flat ground.
    const std::string gullies = std::string(kDirectory) + "/gullies.lua";
    {
        std::ofstream out(gullies, std::ios::binary);
        out << R"(local function main(params)
    local cone = Curves.Profile{ input = Shapes.Point{} - 240, kind = "linear", width = 240 } * 200
    local cut = Erosion.Gullies{ input = cone, cell = 60, octaves = 4, strength = 12, seed = 3 }
    return { d = { value = cut, range = { -50, 250 } } }
end
return main
)";
    }
    const Field g64 = RunExport(gullies, std::string(kDirectory) + "/gullies_64", wide, "d", "--seed 5");
    const Field g16 = RunExport(gullies, std::string(kDirectory) + "/gullies_16", small, "d", "--seed 5");
    ++test::g_checks;
    const bool gully_same = g64.Ok() && g16.Ok()
                            && std::memcmp(g64.values.data(), g16.values.data(),
                                           g64.values.size() * sizeof(float)) == 0;
    std::printf("     Erosion.Gullies, sections 64 and 16: %s\n",
                gully_same ? "byte-identical" : "DIFFER");
    CHECK(gully_same);
    if (g64.Ok()) {
        // Outside the cone the ground is flat, so nothing is cut; on the cone something is.
        ++test::g_checks;
        CHECK(At(g64, -250.0, -250.0) == 0.0);
        double changed = 0.0;
        for (double x = -150.0; x <= 150.0; x += 4.0) {
            const double plain = std::max(0.0, (240.0 - std::fabs(x)) / 240.0) * 200.0;
            changed = std::max(changed, std::fabs(At(g64, x, 0.0) - plain));
        }
        ++test::g_checks;
        CHECK(changed > 2.0);
    }

    // Terrain.Grid passes through its values and is the same whatever the section size; a grid from
    // Erosion.StreamPower, which must have cut below what uplift alone would give.
    const std::string grid = std::string(kDirectory) + "/grid.lua";
    {
        std::ofstream out(grid, std::ios::binary);
        out << R"(local function main(params)
    local n, cell = 24, 25.0
    local height, uplift = {}, {}
    for z = 0, n - 1 do
        for x = 0, n - 1 do
            local edge = x == 0 or z == 0 or x == n - 1 or z == n - 1
            height[#height + 1] = ((x * 7 + z * 13) % 5) * 0.1
            uplift[#uplift + 1] = edge and 0 or 2.0
        end
    end
    local eroded = Erosion.StreamPower{ size = { n, n }, cell = cell, height = height, uplift = uplift,
                                        iterations = 60, k = 0.02, talus = 1.0, diffusion = 5 }
    -- Uplift alone would raise the interior by 120 m; erosion has to have taken a good part of it.
    local total, raised = 0, 0
    for i = 1, #eroded do total = total + eroded[i] raised = raised + height[i] + uplift[i] * 60 end
    if not (total < raised * 0.8 and total > 0) then error("stream power kept " .. total / raised) end
    local g = Terrain.Grid{ values = eroded, size = { n, n }, origin = { -300, -300 }, cell = cell }
    return { d = { value = g, range = { -10, 200 } } }
end
return main
)";
    }
    const Field grid64 = RunExport(grid, std::string(kDirectory) + "/grid_64", wide, "d");
    const Field grid16 = RunExport(grid, std::string(kDirectory) + "/grid_16", small, "d");
    ++test::g_checks;
    const bool grid_same = grid64.Ok() && grid16.Ok()
                           && std::memcmp(grid64.values.data(), grid16.values.data(),
                                          grid64.values.size() * sizeof(float)) == 0;
    std::printf("     Terrain.Grid of Erosion.StreamPower, sections 64 and 16: %s\n",
                grid_same ? "byte-identical" : (grid64.Ok() ? "DIFFER" : grid64.error.c_str()));
    CHECK(grid_same);

    struct Case {
        const char* name;
        const char* body;
        const char* fragment;
    };
    const Case cases[] = {
        {"grid_wrong_count",
         "    local result = Terrain.Grid{ values = { 1, 2, 3 }, size = { 2, 2 }, cell = 1 }",
         "to hold 4 numbers"},
        {"gullies_bad_gradient",
         "    local result = Erosion.Gullies{ input = Terrain.Coords{}.x, gradient = Terrain.Coords{}.x }",
         "Gullies gradient must be R2->R2"},
        {"wn_scalar_position",
         "    local result = Noises.fBm{ position = Terrain.Const{ value = 1.0 } }.value",
         "position field, got R2->R1"},
        {"wn_3d_with_2d_position",
         "    local c = Terrain.Coords{}\n"
         "    local result = Noises.fBm{ domain = 3, position = Vec.Combine{ c.x, c.z } }.value",
         "an R3->R3 position field, got R2->R2"},
    };
    for (const Case& c : cases) {
        const Field field = Export(c.name, c.body);
        ++test::g_checks;
        if (field.Ok() || field.error.find(c.fragment) == std::string::npos
            || field.error.find(".lua:") == std::string::npos) {
            ++test::g_failures;
            std::printf("FAIL %s: expected an error naming '%s' with a line, got: %.240s\n", c.name,
                        c.fragment, field.Ok() ? "success" : field.error.c_str());
        }
    }
}

/// `Dunes.Field`: the procedural style is pointwise, so sections agree to the byte, and a key of
/// another style is an error that names the caller's line.
void TestDuneFields() {
    test::Section("procedural dune fields");

    const std::string script = std::string(kDirectory) + "/dunes_procedural.lua";
    {
        std::ofstream out(script, std::ios::binary);
        out << R"(local function main(params)
    local field = Dunes.Field{ seed = params.seed, wavelength = 300, height = 40, small_height = 3 }
    return field
end
return main
)";
    }
    Grid wide;
    wide.spacing = 4.0;
    wide.side    = 128;
    Grid small   = wide;
    small.section = 16;
    wide.section  = 64;
    const Field whole  = RunExport(script, std::string(kDirectory) + "/dunes_64", wide, "height", "--seed 3");
    const Field pieces = RunExport(script, std::string(kDirectory) + "/dunes_16", small, "height", "--seed 3");
    ++test::g_checks;
    const bool identical = whole.Ok() && pieces.Ok()
                           && std::memcmp(whole.values.data(), pieces.values.data(),
                                          whole.values.size() * sizeof(float)) == 0;
    std::printf("     Dunes.Field procedural, sections 64 and 16: %s\n",
                identical ? "byte-identical" : (whole.Ok() ? "DIFFER" : whole.error.c_str()));
    CHECK(identical);

    // Another seed is another field.
    const Field other = RunExport(script, std::string(kDirectory) + "/dunes_seed", wide, "height", "--seed 4");
    ++test::g_checks;
    CHECK(whole.Ok() && other.Ok()
          && std::memcmp(whole.values.data(), other.values.data(), whole.values.size() * sizeof(float)) != 0);

    // A region: relief inside the polygon, nothing outside it, and the same bytes across sections.
    const std::string region = std::string(kDirectory) + "/dunes_region.lua";
    {
        std::ofstream out(region, std::ios::binary);
        out << R"(local function main(params)
    local field = Dunes.Field{
        seed = params.seed, wavelength = 120, height = 14, small_height = 1,
        area = { { -150, -150 }, { 150, -150 }, { 150, 150 }, { -150, 150 } }, feather = 60, edge_wander = 10,
    }
    return field
end
return main
)";
    }
    const Field inside = RunExport(region, std::string(kDirectory) + "/dunes_region_64", wide, "height", "--seed 3");
    const Field cut    = RunExport(region, std::string(kDirectory) + "/dunes_region_16", small, "height", "--seed 3");
    ++test::g_checks;
    CHECK(inside.Ok() && cut.Ok()
          && std::memcmp(inside.values.data(), cut.values.data(), inside.values.size() * sizeof(float)) == 0);
    if (inside.Ok()) {
        // The tile is 512 m across about the origin: the middle is in the region, the corner is not.
        double relief = 0.0;
        for (double x = -60.0; x <= 60.0; x += 4.0) {
            relief = std::max(relief, std::fabs(At(inside, x, 0.0) - At(inside, 0.0, 0.0)));
        }
        ++test::g_checks;
        CHECK(relief > 1.0);
        ++test::g_checks;
        CHECK(std::fabs(At(inside, 250.0, 250.0)) < 0.01);
    }

    const Field wrong = Export("dunes_wrong_key", "    local result = Dunes.Field{ flats = 0.3 }.height");
    ++test::g_checks;
    if (wrong.Ok() || wrong.error.find("'procedural' does not take 'flats'") == std::string::npos
        || wrong.error.find("belongs to style 'star'") == std::string::npos
        || wrong.error.find("dunes_wrong_key.lua:") == std::string::npos) {
        ++test::g_failures;
        std::printf("FAIL dunes_wrong_key: got %.240s\n", wrong.Ok() ? "success" : wrong.error.c_str());
    }
}

/// Numbers that say whether a terrain is *sane*. Not whether it is convincing: that is a person's eye.
struct Indicators {
    double      low = 0.0;
    double      high = 0.0;
    double      below = 0.0;  // fraction under sea level
    double      steepest = 0.0;
    double      p99 = 0.0;
    std::size_t bad = 0;       // NaN or infinite
};

[[nodiscard]] Indicators Measure(const Field& field) {
    Indicators out;
    const std::size_t side = field.grid.side;
    out.low  = 1e300;
    out.high = -1e300;
    std::size_t below = 0;
    std::vector<double> slopes;
    slopes.reserve(side * side);
    for (std::size_t row = 0; row < side; ++row) {
        for (std::size_t column = 0; column < side; ++column) {
            const double v = field.values[row * side + column];
            if (!std::isfinite(v)) {
                ++out.bad;
                continue;
            }
            out.low  = std::min(out.low, v);
            out.high = std::max(out.high, v);
            below += v < 0.0 ? 1 : 0;
            if (column + 1 < side && row + 1 < side) {
                const double dx = (field.values[row * side + column + 1] - v) / field.grid.spacing;
                const double dz = (field.values[(row + 1) * side + column] - v) / field.grid.spacing;
                slopes.push_back(std::hypot(dx, dz));
            }
        }
    }
    out.below = static_cast<double>(below) / static_cast<double>(side * side);
    std::sort(slopes.begin(), slopes.end());
    out.steepest = slopes.empty() ? 0.0 : slopes.back();
    out.p99      = slopes.empty() ? 0.0 : slopes[slopes.size() * 99 / 100];
    return out;
}

void TestTerrainIndicators() {
    test::Section("complete terrains: finite, in range, sane slopes, deterministic");

    // 4 km at 16 m: 256 x 256 samples. Slopes are rise over run between neighbours at that spacing, so
    // a scarp reads as steep and a cliff steeper; the bounds are what each terrain's own design allows.
    Grid grid;
    grid.originX = -2048.0;
    grid.originZ = -2048.0;
    grid.spacing = 16.0;
    grid.side    = 256;
    grid.section = 128;

    struct Terrain {
        const char* name;
        double      lowest;      // declared range of the script, which the result must stay inside
        double      highest;
        double      steepest;    // an upper bound on neighbour slope at 16 m
        double      seaFraction; // expected share below 0 m, +- 0.15
    };
    const Terrain terrains[] = {
        // The land is a disc of about 1100 m radius (where the cone crosses 0 m) in a 4096 m window:
        // 23% of it, so 77% under the sea.
        {"volcanic_island", -150.0, 760.0, 4.5, 0.77},
        // 73 degrees at the 99th percentile at 16 m spacing, and 80 at the worst sample. The ridged detail
        // has a 240 m wavelength and four octaves, so its finest is about 30 m, two samples; and erosion
        // at 16 m is one iteration now (iterations are scaled to the spacing), so nothing relaxes it.
        // Steep, and measured, and it is what a 16 m preview of that terrain honestly is.
        {"mountain_range", -300.0, 1785.0, 6.0, 0.0},
        {"canyon", 0.0, 640.0, 12.0, 0.0},
        {"mesas", -120.0, 660.0, 12.0, 0.0},
        {"coast", -110.0, 255.0, 4.5, 0.4},
    };
    for (const Terrain& t : terrains) {
        const std::string script =
            std::string(ENGINE_SOURCE_ROOT) + "/assets/scripts/shapes/" + t.name + ".lua";
        const std::string directory = std::string(kDirectory) + "/ind_" + t.name;
        const Field       first     = RunExport(script, directory, grid, "height", "--seed 7");
        ++test::g_checks;
        if (!first.Ok()) {
            ++test::g_failures;
            std::printf("FAIL %s: no field (%.260s)\n", t.name, first.error.c_str());
            continue;
        }
        const Indicators m = Measure(first);
        std::printf("     %-16s %7.1f to %7.1f m | %4.0f%% under 0 | slope p99 %.2f max %.2f\n", t.name,
                    m.low, m.high, m.below * 100.0, m.p99, m.steepest);
        CHECK_EQ(m.bad, std::size_t{0});
        CHECK(m.low >= t.lowest);
        CHECK(m.high <= t.highest);
        CHECK(m.high - m.low > 100.0);
        CHECK(m.steepest <= t.steepest);
        CHECK(std::fabs(m.below - t.seaFraction) <= 0.12);

        // The same seed and bounds again: identical bytes.
        const Field second = RunExport(script, directory + "_b", grid, "height", "--seed 7");
        ++test::g_checks;
        CHECK(second.Ok() && std::memcmp(first.values.data(), second.values.data(),
                                         first.values.size() * sizeof(float)) == 0);
    }
}

/// The sidecar's `statistics` block for the single output `d`, read back as text.
struct SidecarStatistics {
    bool   found     = false;
    double minimum   = 0.0;
    double maximum   = 0.0;
    double mean      = 0.0;
    double samples   = 0.0;
    double nonFinite = 0.0;
};

[[nodiscard]] SidecarStatistics ReadStatistics(const std::string& directory) {
    SidecarStatistics out;
    const std::string text = ReadFile(directory + "/metadata.json");
    const std::size_t at   = text.find("\"statistics\"");
    if (at == std::string::npos) {
        return out;
    }
    const auto number = [&](const char* key) {
        const std::string needle = std::string("\"") + key + "\": ";
        const std::size_t k      = text.find(needle, at);
        return k == std::string::npos ? 0.0 : std::strtod(text.c_str() + k + needle.size(), nullptr);
    };
    out.found     = true;
    out.minimum   = number("min");
    out.maximum   = number("max");
    out.mean      = number("mean");
    out.samples   = number("samples");
    out.nonFinite = number("non_finite");
    return out;
}

void TestStatistics() {
    test::Section("the sidecar says what the samples came to");

    // A constant: everything is the value and there is nothing else.
    (void)Export("stat_const", "    local result = Terrain.Const{ value = -50 }");
    const SidecarStatistics flat = ReadStatistics(std::string(kDirectory) + "/stat_const");
    CHECK(flat.found);
    CHECK_EQ(flat.samples, static_cast<double>(kSide * kSide));
    CHECK_EQ(flat.minimum, -50.0);
    CHECK_EQ(flat.maximum, -50.0);
    CHECK(std::fabs(flat.mean + 50.0) < 1e-9);
    CHECK_EQ(flat.nonFinite, 0.0);

    // A cone: the apex is a sample, the foot is zero, and the mean is the sum of the same formula.
    (void)Export("stat_cone", "    local result = Landforms.Cone{ radius = 150, height = 100 }");
    const SidecarStatistics cone = ReadStatistics(std::string(kDirectory) + "/stat_cone");
    double                  sum  = 0.0;
    for (std::size_t row = 0; row < kSide; ++row) {
        for (std::size_t column = 0; column < kSide; ++column) {
            const double x = kOrigin + kSpacing * static_cast<double>(column);
            const double z = kOrigin + kSpacing * static_cast<double>(row);
            sum += 100.0 * std::clamp((150.0 - std::hypot(x, z)) / 150.0, 0.0, 1.0);
        }
    }
    CHECK(cone.found);
    CHECK(std::fabs(cone.maximum - 100.0) < 1e-3);
    CHECK_EQ(cone.minimum, 0.0);
    CHECK(std::fabs(cone.mean - sum / static_cast<double>(kSide * kSide)) < 1e-3);

    // A field of NaN: the 16-bit PNG would store it as terrain, so the count is what says it is not.
    (void)Export("stat_nan", "    local result = Terrain.Const{ value = 0 / 0 }");
    const SidecarStatistics broken = ReadStatistics(std::string(kDirectory) + "/stat_nan");
    CHECK(broken.found);
    CHECK_EQ(broken.nonFinite, static_cast<double>(kSide * kSide));
    CHECK_EQ(broken.samples, 0.0);
}

void TestErrors() {
    test::Section("bad geometry is refused with the line");

    struct Case {
        const char* name;
        const char* body;
        const char* fragment;
    };
    const Case cases[] = {
        {"negative_radius", "    local result = Geometry.Circle{ radius = -1 }", "radius"},
        {"zero_scale",
         "    local result = Geometry.Circle{ position = Terrain.Transform{ scale = 0 }, radius = 5 }",
         "scale"},
        {"short_polygon",
         "    local result = Geometry.Segments{ points = { { 0, 0 }, { 1, 1 } } }", "at least 3"},
        {"unknown_key", "    local result = Geometry.Circle{ radii = 5 }", "does not take"},
        {"bad_profile_kind",
         "    local result = Curves.Profile{ input = Geometry.Circle{ radius = 5 }, kind = 'cliff',\n"
         "                                   width = 5 }",
         "does not know the kind"},
        {"profile_missing_width",
         "    local result = Curves.Profile{ input = Geometry.Circle{ radius = 5 }, kind = 'linear' }",
         "needs 'width'"},
        {"profile_wrong_key",
         "    local result = Curves.Profile{ input = Geometry.Circle{ radius = 5 }, kind = 'linear',\n"
         "                                   width = 5, sigma = 2 }",
         "does not take 'sigma'"},
        {"zero_terrace_step",
         "    local result = Curves.Terrace{ input = Terrain.Coords{}.x, step = 0 }", "step"},
        {"smooth_k_zero",
         "    local c = Geometry.Circle{ radius = 5 }\n"
         "    local result = Combine.SmoothMin{ a = c, b = c, k = 0 }",
         "greater than zero"},
    };
    for (const Case& c : cases) {
        const Field field = Export(c.name, c.body);
        ++test::g_checks;
        if (field.Ok() || field.error.find(c.fragment) == std::string::npos
            || field.error.find(".lua:") == std::string::npos) {
            ++test::g_failures;
            std::printf("FAIL %s: expected an error naming '%s' with a line, got: %.240s\n", c.name,
                        c.fragment, field.Ok() ? "success" : field.error.c_str());
        }
    }
}

} // namespace

int main() {
    for (const char* path : {"out", kDirectory}) {
        REQUIRE_OK(engine::platform::MakeDirectory(path));
    }

    TestPrimitives();
    TestPlacement();
    TestPolygons();
    TestProfiles();
    TestErrors();
    TestWarpedNoise();
    TestDuneFields();
    TestLandforms();
    TestPreludeErrors();
    TestPreludeHash();
    TestReferenceTerrains();
    TestPolygonsAgainstReference();
    TestDegenerateGeometry();
    TestLimitsAndInvalidInput();
    TestOriginAndResolution();
    TestContinuity();
    TestSectionsAndInteraction();
    TestTerrainIndicators();
    TestStatistics();

    return test::Summary("test_geometry");
}
