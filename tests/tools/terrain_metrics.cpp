// terrain_metrics: measures an exported height field as land and sea, and its relief.
//
//   terrain_metrics <export dir or height.raw> [--width N --depth N --resolution M] [--level L]
//                   [--no-sea] [--box kmin,kmax] [--block M] [--channel M2] [--json]
//                   [--hillshade out.png [--sun azimuth,elevation]]
//   terrain_metrics <reference.png> [--resolution M] [--vertical M] ...
//
// Given a directory it reads `metadata.json` for the size and the spacing. Given a PNG (8 or 16 bits,
// grey or colour, read as luminance) it measures it as a reference heightmap: white is `--vertical`
// metres (default 255, so 8-bit levels read as metres) and a sample is `--resolution` metres (default
// 1). With an unknown scale only the scale-free numbers (spectral slope, hypsometric integral) mean
// anything; check first that the image is heights and not shaded relief. The land and sea numbers are
// the ones `test_lands` accepts the Lands macros against (tests/unit/terrain_metrics.hpp); the relief
// ones are tests/unit/relief_metrics.hpp. `--no-sea` treats every sample as land. `--hillshade` writes
// the field as a grey image lit from `--sun` (default 315,35: the top left, 35 degrees up), with cast
// shadows, so the geometry can be judged without the viewer's lighting.

#include "relief_metrics.hpp"

#include <spng.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

[[nodiscard]] std::string ReadAll(const std::string& path);

/// Decodes any PNG as 16-bit RGBA and keeps the luminance, white at `vertical`.
[[nodiscard]] bool LoadPng(const std::string& path, double resolution, double vertical, metrics::Field& out) {
    const std::string encoded = ReadAll(path);
    if (encoded.empty()) {
        return false;
    }
    spng_ctx* context = spng_ctx_new(0);
    spng_ihdr header{};
    size_t    bytes   = 0;
    bool      ok      = context != nullptr && spng_set_png_buffer(context, encoded.data(), encoded.size()) == 0
                        && spng_get_ihdr(context, &header) == 0
                        && spng_decoded_image_size(context, SPNG_FMT_RGBA16, &bytes) == 0;
    std::vector<std::uint16_t> rgba(bytes / sizeof(std::uint16_t));
    ok = ok && spng_decode_image(context, rgba.data(), bytes, SPNG_FMT_RGBA16, 0) == 0;
    spng_ctx_free(context);
    if (!ok) {
        return false;
    }
    out.width      = header.width;
    out.depth      = header.height;
    out.resolution = resolution;
    out.values.resize(out.width * out.depth);
    for (std::size_t i = 0; i < out.values.size(); ++i) {
        const double luminance = 0.299 * rgba[4 * i] + 0.587 * rgba[4 * i + 1] + 0.114 * rgba[4 * i + 2];
        out.values[i]          = static_cast<float>(luminance / 65535.0 * vertical);
    }
    return true;
}

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
                     "--resolution M] [--level L] [--no-sea] [--box kmin,kmax] [--block M] [--channel M2] "
                     "[--json] [--hillshade out.png [--sun azimuth,elevation]]\n";
        return 2;
    }
    std::string path = argv[1];
    double      width = 0, depth = 0, resolution = 0, level = 0;
    int         boxMin = 2, boxMax = 7;
    bool        json = false;
    metrics::ReliefOptions options;
    std::string hillshade;
    double      azimuth = 315.0, elevation = 35.0;
    double      vertical = 255.0;
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
        } else if (arg == "--no-sea") {
            level = -std::numeric_limits<double>::infinity();
        } else if (arg == "--block") {
            options.block = std::atof(next());
        } else if (arg == "--channel") {
            options.channelArea = std::atof(next());
        } else if (arg == "--hillshade") {
            hillshade = next();
        } else if (arg == "--vertical") {
            vertical = std::atof(next());
        } else if (arg == "--sun") {
            const std::string sun   = next();
            const std::size_t comma = sun.find(',');
            if (comma != std::string::npos) {
                azimuth   = std::atof(sun.substr(0, comma).c_str());
                elevation = std::atof(sun.substr(comma + 1).c_str());
            }
        }
    }
    options.level = level;

    const auto endsWith = [&](const char* suffix) {
        const std::size_t n = std::strlen(suffix);
        return path.size() > n && path.compare(path.size() - n, n, suffix) == 0;
    };
    metrics::Field field;
    if (endsWith(".png") || endsWith(".PNG")) {
        if (!LoadPng(path, resolution > 0 ? resolution : 1.0, vertical, field)) {
            std::cerr << "terrain_metrics: cannot decode " << path << "\n";
            return 2;
        }
        resolution = field.resolution;
    } else {
        const bool isRaw = endsWith(".raw");
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
        if (!metrics::LoadRaw(path, static_cast<std::size_t>(width), static_cast<std::size_t>(depth),
                              resolution, field)) {
            std::cerr << "terrain_metrics: cannot read " << width << " x " << depth << " floats from " << path
                      << "\n";
            return 2;
        }
    }
    const metrics::Report r = metrics::Measure(field, level, boxMin, boxMax);
    const metrics::Relief relief = metrics::MeasureRelief(field, options);

    const double shelf = metrics::MeanSlopeInDepth(field, level, 0.0, 120.0);
    const double slope = metrics::MeanSlopeInDepth(field, level, 120.0, 2500.0);
    const double flat  = metrics::MeanSlopeInDepth(field, level, 2500.0, 1e9);

    if (!hillshade.empty()
        && !metrics::WriteGreyPng(hillshade, metrics::Hillshade(field, azimuth, elevation), field.width, field.depth)) {
        std::cerr << "terrain_metrics: cannot write " << hillshade << "\n";
        return 2;
    }

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
        std::printf(" \"slope_deg\": {\"shelf_0_120m\": %.2f, \"slope_120_2500m\": %.2f, \"abyssal_2500m_up\": %.2f},\n",
                    shelf, slope, flat);
        std::printf(" \"relief\": {\"slope_p50\": %.2f, \"slope_p90\": %.2f, \"slope_p99\": %.2f, \"slope_p999\": %.2f,\n",
                    relief.slope[0], relief.slope[1], relief.slope[2], relief.slope[3]);
        std::printf("  \"steep35\": %.4f, \"steep45\": %.4f, \"local_relief_m\": %.1f, \"block_m\": %g,\n",
                    relief.steep35, relief.steep45, relief.localRelief, options.block);
        std::printf("  \"hypsometric_integral\": %.4f, \"curvature_p90\": %.6f,\n", relief.hypsometricIntegral,
                    relief.curvatureP90);
        std::printf("  \"pits\": %zu, \"pit_share\": %.5f, \"pit_depth_m\": %.2f,\n", relief.pits, relief.pitShare,
                    relief.pitDepth);
        std::printf("  \"drainage_density\": %.4f, \"channel_area_m2\": %g,\n", relief.drainageDensity,
                    options.channelArea);
        std::printf("  \"spectral_slope\": %.3f, \"spectral_fit\": %.3f, \"axis_diagonal_ratio\": %.3f}}\n",
                    relief.spectralSlope, relief.spectralFit, relief.axisDiagonalRatio);
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
    std::printf("relief: slope p50 %.1f p90 %.1f p99 %.1f p99.9 %.1f deg; %.2f %% over 35 deg, %.2f %% over 45 deg\n",
                relief.slope[0], relief.slope[1], relief.slope[2], relief.slope[3], relief.steep35 * 100.0,
                relief.steep45 * 100.0);
    std::printf("relief %.0f m per %g m block; hypsometric integral %.3f; |laplacian| p90 %.2e per m\n",
                relief.localRelief, options.block, relief.hypsometricIntegral, relief.curvatureP90);
    std::printf("%zu closed depression(s), %.3f %% of the land, deepest %.1f m; drainage %.2f km per km2 "
                "(channels from %g m2)\n",
                relief.pits, relief.pitShare * 100.0, relief.pitDepth, relief.drainageDensity, options.channelArea);
    std::printf("spectral slope %.2f (R2 %.3f); axis/diagonal power %.2f\n", relief.spectralSlope,
                relief.spectralFit, relief.axisDiagonalRatio);
    return 0;
}
