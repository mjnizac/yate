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

#include <algorithm>
#include <cmath>
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

namespace {

constexpr const char*  kDirectory = "out/test_geometry";
constexpr std::size_t  kSide      = 64;
constexpr double       kSpacing   = 8.0;
constexpr double       kOrigin    = -256.0;

struct Field {
    std::vector<float> values;
    std::string        error;
    [[nodiscard]] bool Ok() const { return values.size() == kSide * kSide; }
};

[[nodiscard]] std::string ReadFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

/// Exports `body` (the text of a Lua `main`) over the test grid with `--raw`.
///
/// `body` returns its value as `local result`, and the wrapper names it the `d` output.
[[nodiscard]] Field Export(const char* name, const std::string& body) {
    Field             field;
    const std::string script = std::string(kDirectory) + "/" + name + ".lua";
    {
        std::ofstream out(script, std::ios::binary);
        out << "local function main()\n" << body
            << "\n    return { d = { value = result, range = { -1000, 1000 } } }\nend\nreturn main\n";
    }
    const std::string directory = std::string(kDirectory) + "/" + name;
    const std::string capture   = std::string(kDirectory) + "/capture.txt";
    std::string       command   = std::string("\"") + TERRAIN_EXPORT_EXE + "\" --script \"" + script
                          + "\" --min -256,-256 --max 256,256 --resolution 8 --section 16 --raw "
                            "--out \""
                          + directory + "\" > \"" + capture + "\" 2>&1";
#ifdef _WIN32
    command = "\"" + command + "\"";
#endif
    const int code = std::system(command.c_str());
    if (code != 0) {
        field.error = ReadFile(capture);
        return field;
    }
    const std::string bytes = ReadFile(directory + "/d.raw");
    if (bytes.size() == kSide * kSide * sizeof(float)) {
        field.values.resize(kSide * kSide);
        std::memcpy(field.values.data(), bytes.data(), bytes.size());
    } else {
        field.error = "d.raw is " + std::to_string(bytes.size()) + " bytes";
    }
    return field;
}

using Expected = std::function<double(double x, double z)>;

/// Compares every sample with `expected`, and reports the worst and where.
void Compare(const char* label, const Field& field, const Expected& expected, double tolerance) {
    ++test::g_checks;
    if (!field.Ok()) {
        ++test::g_failures;
        std::printf("FAIL %s: no field (%.200s)\n", label, field.error.c_str());
        return;
    }
    double      worst   = 0.0;
    std::size_t worstAt = 0;
    for (std::size_t row = 0; row < kSide; ++row) {
        for (std::size_t column = 0; column < kSide; ++column) {
            const double x = kOrigin + kSpacing * static_cast<double>(column);
            const double z = kOrigin + kSpacing * static_cast<double>(row);
            const double error =
                std::fabs(static_cast<double>(field.values[row * kSide + column]) - expected(x, z));
            if (error > worst) {
                worst   = error;
                worstAt = row * kSide + column;
            }
        }
    }
    if (worst > tolerance) {
        ++test::g_failures;
        std::printf("FAIL %s: worst error %.6g at sample (%zu, %zu), tolerance %.3g\n", label, worst,
                    worstAt % kSide, worstAt / kSide, tolerance);
        return;
    }
    std::printf("     %-34s worst error %.3g\n", label, worst);
}

[[nodiscard]] double At(const Field& field, double x, double z) {
    const std::size_t column = static_cast<std::size_t>(std::llround((x - kOrigin) / kSpacing));
    const std::size_t row    = static_cast<std::size_t>(std::llround((z - kOrigin) / kSpacing));
    return field.Ok() ? static_cast<double>(field.values[row * kSide + column]) : 0.0;
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

    return test::Summary("test_geometry");
}
