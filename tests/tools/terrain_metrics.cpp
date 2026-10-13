// terrain_metrics: measures an exported height field as land and sea.
//
//   terrain_metrics <export dir or height.raw> [--width N --depth N --resolution M] [--level L]
//                   [--box kmin,kmax] [--json]
//
// Given a directory it reads `metadata.json` for the size and the spacing. The numbers are the ones
// `test_lands` accepts the Lands macros against (see tests/unit/terrain_metrics.hpp).

#include "terrain_metrics.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

[[nodiscard]] std::string ReadAll(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

[[nodiscard]] bool Number(const std::string& text, const std::string& key, double& out) {
    const std::string needle = "\"" + key + "\":";
    const std::size_t at     = text.find(needle);
    if (at == std::string::npos) {
        return false;
    }
    out = std::strtod(text.c_str() + at + needle.size(), nullptr);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: terrain_metrics <export dir or height.raw> [--width N --depth N "
                     "--resolution M] [--level L] [--box kmin,kmax] [--json]\n";
        return 2;
    }
    std::string path = argv[1];
    double      width = 0, depth = 0, resolution = 0, level = 0;
    int         boxMin = 2, boxMax = 7;
    bool        json = false;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto        next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : "0"; };
        if (arg == "--width") {
            width = std::atof(next());
        } else if (arg == "--depth") {
            depth = std::atof(next());
        } else if (arg == "--resolution") {
            resolution = std::atof(next());
        } else if (arg == "--level") {
            level = std::atof(next());
        } else if (arg == "--box") {
            const std::string range = next();
            const std::size_t comma = range.find(',');
            if (comma != std::string::npos) {
                boxMin = std::atoi(range.substr(0, comma).c_str());
                boxMax = std::atoi(range.substr(comma + 1).c_str());
            }
        } else if (arg == "--json") {
            json = true;
        }
    }

    const bool isRaw = path.size() > 4 && path.compare(path.size() - 4, 4, ".raw") == 0;
    const std::string directory = isRaw ? path.substr(0, path.find_last_of("/\\")) : path;
    if (!isRaw) {
        path += "/height.raw";
    }
    const std::string sidecar = ReadAll(directory + "/metadata.json");
    // An R2 export is `width` x `height` samples with a `depth` of 1: the z axis is "height".
    if (width == 0 && !Number(sidecar, "width", width)) {
        width = 0;
    }
    if (depth == 0 && !Number(sidecar, "height", depth)) {
        depth = 0;
    }
    if (resolution == 0 && !Number(sidecar, "resolution", resolution)) {
        resolution = 0;
    }
    if (width < 2 || depth < 2 || resolution <= 0) {
        std::cerr << "terrain_metrics: need the size and the spacing (no usable metadata.json in '"
                  << directory << "'; pass --width, --depth and --resolution)\n";
        return 2;
    }

    metrics::Field field;
    if (!metrics::LoadRaw(path, static_cast<std::size_t>(width), static_cast<std::size_t>(depth),
                          resolution, field)) {
        std::cerr << "terrain_metrics: cannot read " << width << " x " << depth << " floats from " << path << "\n";
        return 2;
    }
    const metrics::Report r = metrics::Measure(field, level, boxMin, boxMax);

    const double shelf = metrics::MeanSlopeInDepth(field, level, 0.0, 120.0);
    const double slope = metrics::MeanSlopeInDepth(field, level, 120.0, 2500.0);
    const double flat  = metrics::MeanSlopeInDepth(field, level, 2500.0, 1e9);

    if (json) {
        std::printf("{\"width\": %zu, \"depth\": %zu, \"resolution\": %g, \"level\": %g,\n", field.width,
                    field.depth, resolution, level);
        std::printf(" \"land_fraction\": %.4f, \"islands\": %zu, \"dominant_share\": %.4f, \"lagoons\": %zu,\n",
                    r.landAreaFraction, r.islands, r.dominantShare, r.lagoons);
        std::printf(" \"island_areas_km2\": [");
        for (std::size_t i = 0; i < r.islandAreas.size() && i < 16; ++i) {
            std::printf("%s%.4f", i == 0 ? "" : ", ", r.islandAreas[i] / 1e6);
        }
        std::printf("],\n \"coast_length_km\": %.3f, \"coast_per_km2\": %.4f,\n", r.coastLength / 1000.0,
                    r.coastPerKm2);
        std::printf(" \"box_dimension\": %.3f, \"box_fit\": %.3f, \"box_scales\": [%d, %d],\n", r.boxDimension,
                    r.boxFit, boxMin, boxMax);
        std::printf(" \"min_channel_m\": %.1f,\n \"hypsometry_m\": {\"p10\": %.1f, \"p50\": %.1f, \"p90\": %.1f, \"p99\": %.1f},\n",
                    r.minChannel, r.hypsometry[0], r.hypsometry[1], r.hypsometry[2], r.hypsometry[3]);
        std::printf(" \"slope_deg\": {\"shelf_0_120m\": %.2f, \"slope_120_2500m\": %.2f, \"abyssal_2500m_up\": %.2f}}\n",
                    shelf, slope, flat);
        return 0;
    }
    std::printf("%zu x %zu at %g m, sea level %g\n", field.width, field.depth, resolution, level);
    std::printf("land %.1f %% of the area in %zu mass(es), the largest %.1f %% of the land; %zu lagoon(s)\n",
                r.landAreaFraction * 100.0, r.islands, r.dominantShare * 100.0, r.lagoons);
    std::printf("coast %.1f km (%.2f km per km2); box-counting dimension %.3f over 2^%d..2^%d cells (R2 %.3f)\n",
                r.coastLength / 1000.0, r.coastPerKm2, r.boxDimension, boxMin, boxMax, r.boxFit);
    std::printf("narrowest gap between masses %.0f m; land heights p10 %.0f p50 %.0f p90 %.0f p99 %.0f m\n",
                r.minChannel, r.hypsometry[0], r.hypsometry[1], r.hypsometry[2], r.hypsometry[3]);
    std::printf("mean slope: shelf (0-120 m deep) %.2f deg, slope (120-2500 m) %.2f deg, below %.2f deg\n", shelf,
                slope, flat);
    return 0;
}
