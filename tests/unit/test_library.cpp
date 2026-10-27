// Library cases: a terrain script from assets/scripts exported as a user would, measured with the relief
// and land metrics, and every number its case declares checked against its range.
//
// A case is tests/library/<name>.json:
//
//   { "script": "assets/scripts/terrains/alpine.lua", "seed": 0, "resolution": 8, "section": 512,
//     "bounds": { "min_x": ..., "min_z": ..., "max_x": ..., "max_z": ... },
//     "level": 0,              (the sea level; leave it out for a field without sea)
//     "metrics": [ { "name": "pit_share", "max": 0.01 }, { "name": "slope_p99", "min": 30 },
//                  { "name": "spectral_slope", "min": 3.6, "max": 4.3, "target": true } ] }
//
// Golden files catch any change; these catch a change for the worse, which is the question a terrain
// example has to answer. The names are the keys `terrain_metrics --json` prints.

#include "test_support.hpp"
#include "relief_metrics.hpp"

#include "json_reader.hpp"

#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <string>

#ifndef TERRAIN_EXPORT_EXE
#    error "TERRAIN_EXPORT_EXE is set by tests/CMakeLists.txt"
#endif

namespace {

[[nodiscard]] std::map<std::string, double> Values(const metrics::Report& l, const metrics::Relief& r) {
    return {
        {"land_fraction", l.landAreaFraction},
        {"slope_p50", r.slope[0]},
        {"slope_p90", r.slope[1]},
        {"slope_p99", r.slope[2]},
        {"slope_p999", r.slope[3]},
        {"steep35", r.steep35},
        {"steep45", r.steep45},
        {"talus_share", r.talusShare},
        {"wall_edge_density", r.wallEdgeDensity},
        {"local_relief_m", r.localRelief},
        {"hypsometric_integral", r.hypsometricIntegral},
        {"curvature_p90", r.curvatureP90},
        {"pits", static_cast<double>(r.pits)},
        {"pit_share", r.pitShare},
        {"pit_depth_m", r.pitDepth},
        {"drainage_density", r.drainageDensity},
        {"spectral_slope", r.spectralSlope},
        {"spectral_fit", r.spectralFit},
        {"axis_diagonal_ratio", r.axisDiagonalRatio},
    };
}

void RunCase(const std::filesystem::path& file) {
    const std::string name = file.stem().string();
    test::Section(name.c_str());
    const engine::Result<test::Json> json = test::Json::Load(file.string());
    CHECK(json.has_value());
    if (!json) {
        return;
    }
    const std::string directory = "out/test_library/" + name;
    std::filesystem::create_directories(directory);
    const std::string range = "--min " + std::to_string(json->Number("bounds.min_x", 0.0)) + ","
                              + std::to_string(json->Number("bounds.min_z", 0.0)) + " --max "
                              + std::to_string(json->Number("bounds.max_x", 0.0)) + ","
                              + std::to_string(json->Number("bounds.max_z", 0.0));
    const std::string command = std::string("\"\"") + TERRAIN_EXPORT_EXE + "\" --script \"" + ENGINE_SOURCE_ROOT + "/"
                                + std::string(json->Text("script")) + "\" " + range + " --resolution "
                                + std::to_string(json->Number("resolution", 8.0)) + " --section "
                                + std::to_string(static_cast<int>(json->Number("section", 512.0))) + " --seed "
                                + std::to_string(static_cast<long long>(json->Number("seed", 0.0)))
                                + " --raw --out \"" + directory + "\" > \"" + directory + "/log.txt\" 2>&1\"";
    const int code = std::system(command.c_str());
    CHECK_EQ(code, 0);
    if (code != 0) {
        return;
    }

    // The size of the field is in the sidecar: an R2 export is `width` x `height` samples.
    const engine::Result<test::Json> sidecar = test::Json::Load(directory + "/metadata.json");
    CHECK(sidecar.has_value());
    if (!sidecar) {
        return;
    }
    const auto     width  = static_cast<std::size_t>(sidecar->Number("width", 0.0));
    const auto     depth  = static_cast<std::size_t>(sidecar->Number("height", 0.0));
    metrics::Field field;
    CHECK(metrics::LoadRaw(directory + "/height.raw", width, depth, json->Number("resolution", 8.0), field));
    if (field.values.empty()) {
        return;
    }
    const double level = json->Number("level", -std::numeric_limits<double>::infinity());
    const auto   values = Values(metrics::Measure(field, level), metrics::MeasureRelief(field, {.level = level}));

    for (std::size_t i = 0; i < json->ArraySize("metrics"); ++i) {
        const std::string key    = "metrics." + std::to_string(i);
        const std::string metric = std::string(json->Text(key + ".name"));
        const auto        found  = values.find(metric);
        if (found == values.end()) {
            std::printf("FAIL %s: no metric named '%s'\n", name.c_str(), metric.c_str());
            CHECK(false);
            continue;
        }
        const double lo = json->Number(key + ".min", -std::numeric_limits<double>::infinity());
        const double hi = json->Number(key + ".max", std::numeric_limits<double>::infinity());
        const bool   ok = found->second >= lo && found->second <= hi;
        // A target is a range measured on references that the example does not meet yet: reported, so
        // the gap is visible on every run, but it only becomes a check once the family is validated.
        const bool target = json->Text(key + ".target") == "true";
        std::printf("     %s %-22s %10.4f in [%g, %g]%s\n", ok ? "  " : target ? "->" : "!!", metric.c_str(),
                    found->second, lo, hi, target ? " (target)" : "");
        if (!target) {
            CHECK(ok);
        }
    }
}

} // namespace

int main() {
    std::vector<std::filesystem::path> cases;
    for (const auto& entry : std::filesystem::directory_iterator(std::string(ENGINE_SOURCE_ROOT) + "/tests/library")) {
        if (entry.path().extension() == ".json") {
            cases.push_back(entry.path());
        }
    }
    std::sort(cases.begin(), cases.end());
    CHECK(!cases.empty());
    for (const auto& file : cases) {
        RunCase(file);
    }
    return test::Summary("test_library");
}
