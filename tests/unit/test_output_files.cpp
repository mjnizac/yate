// Checks a written PNG against values that were worked out by hand, not against another file the
// engine produced.
//
// The dataset runner compares decoded samples between two PNGs the engine wrote, which is exactly
// what let one defect live from the first export to the ninth milestone: every 16-bit sample was
// written byte-reversed, so the golden was reversed too and the comparison passed. A smooth height
// field read as white noise and nothing in the suite could say why.
//
// So the expectations below are arithmetic, written down before the file exists:
//
//     0.0 over [-100000, 100000]  ->  0.5 * 65535 + 0.5 = 32768  ->  bytes 0x80 0x00
//
// and the decoder is libspng's, not anything of the engine's.

#include "test_support.hpp"

#include <engine/log.hpp>
#include <engine/memory/allocator.hpp>
#include <engine/platform.hpp>
#include <engine/terrain/output_writer.hpp>
#include <engine/terrain/sha256.hpp>

#include <spng.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::terrain;

namespace {

constexpr const char* kDirectory = "out/test_output_files";

struct Decoded {
    u32_t                   width  = 0;
    u32_t                   height = 0;
    u8_t                    depth  = 0;
    u8_t                    color  = 0;
    /// The image exactly as stored in the file: 16-bit samples big-endian, which is what PNG says.
    std::vector<u8_t>       bytes;
};

[[nodiscard]] Decoded Decode(const std::string& path) {
    Decoded       out;
    std::ifstream stream(path, std::ios::binary);
    const std::vector<char> file((std::istreambuf_iterator<char>(stream)),
                                 std::istreambuf_iterator<char>());
    if (file.empty()) {
        return out;
    }
    spng_ctx* context = spng_ctx_new(0);
    spng_set_png_buffer(context, file.data(), file.size());

    spng_ihdr header{};
    if (spng_get_ihdr(context, &header) == 0) {
        usize_t size = 0;
        // SPNG_FMT_RAW, not SPNG_FMT_PNG: libspng's own header says RAW is the one that is big-endian
        // and "everything else is host-endian", so FMT_PNG hands back little-endian samples on this
        // machine and would make a correct file look byte-reversed. RAW is the file's own layout.
        if (spng_decoded_image_size(context, SPNG_FMT_RAW, &size) == 0) {
            out.bytes.resize(size);
            if (spng_decode_image(context, out.bytes.data(), size, SPNG_FMT_RAW, 0) != 0) {
                out.bytes.clear();
            }
        }
        out.width  = header.width;
        out.height = header.height;
        out.depth  = header.bit_depth;
        out.color  = header.color_type;
    }
    spng_ctx_free(context);
    return out;
}

/// The 16-bit sample at (`x`, `channel`) of row 0, read as the two bytes the file holds.
[[nodiscard]] u32_t Sample(const Decoded& image, u32_t x, u32_t channel, u32_t channels) {
    const usize_t at = (static_cast<usize_t>(x) * channels + channel) * 2;
    return (static_cast<u32_t>(image.bytes[at]) << 8) | image.bytes[at + 1];
}

void TestGrayscaleBytes() {
    test::Section("a 16-bit grayscale file holds the bytes arithmetic says it should");

    const std::string path = std::string(kDirectory) + "/gray.png";
    constexpr u32_t   kWidth = 5;
    {
        Result<PngWriter> writer = PngWriter::Create(path, kWidth, 1, Mapping{Domain::R2, 1},
                                                     -100000.0f, 100000.0f, PngCompression{});
        REQUIRE_OK_VOID(writer);
        // Zero over a symmetric range is the centre of it; the others are the corners and quarters.
        const std::array<f32_t, kWidth> row{0.0f, -100000.0f, 100000.0f, -50000.0f, 50000.0f};
        REQUIRE_OK_VOID(writer->WriteRow(row.data()));
        REQUIRE_OK_VOID(writer->Finish());
    }

    const Decoded image = Decode(path);
    CHECK_EQ(image.width, kWidth);
    CHECK_EQ(image.height, u32_t{1});
    CHECK_EQ(static_cast<u32_t>(image.depth), u32_t{16});
    CHECK_EQ(static_cast<u32_t>(image.color), u32_t{0}); // grayscale
    if (image.bytes.size() < kWidth * 2) {
        CHECK(false);
        return;
    }

    // The first two bytes are the whole point: 0x80 then 0x00. Byte-reversed it is 0x00 then 0x80,
    // which is 128 read as a 16-bit value, a black pixel where mid-grey belongs.
    CHECK_EQ(static_cast<u32_t>(image.bytes[0]), u32_t{0x80});
    CHECK_EQ(static_cast<u32_t>(image.bytes[1]), u32_t{0x00});

    CHECK_EQ(Sample(image, 0, 0, 1), u32_t{32768});  //   0 -> 0.5
    CHECK_EQ(Sample(image, 1, 0, 1), u32_t{0});      // min -> 0
    CHECK_EQ(Sample(image, 2, 0, 1), u32_t{65535});  // max -> 65535
    CHECK_EQ(Sample(image, 3, 0, 1), u32_t{16384});  // quarter: 0.25 * 65535 + 0.5 = 16384.25
    CHECK_EQ(Sample(image, 4, 0, 1), u32_t{49151});  // three quarters: 49151.25 + 0.5 truncates
}

void TestClampingIsCountedAndSaturates() {
    test::Section("out-of-range samples saturate and are counted");

    const std::string path = std::string(kDirectory) + "/clamp.png";
    Result<PngWriter> writer = PngWriter::Create(path, 3, 1, Mapping{Domain::R2, 1}, 0.0f, 1.0f,
                                                 PngCompression{});
    REQUIRE_OK_VOID(writer);
    const std::array<f32_t, 3> row{-5.0f, 0.5f, 7.0f};
    REQUIRE_OK_VOID(writer->WriteRow(row.data()));
    REQUIRE_OK_VOID(writer->Finish());

    CHECK_EQ(writer->ClampedSamples(), u64_t{2});
    const Decoded image = Decode(path);
    if (image.bytes.size() < 6) {
        CHECK(false);
        return;
    }
    CHECK_EQ(Sample(image, 0, 0, 1), u32_t{0});
    CHECK_EQ(Sample(image, 1, 0, 1), u32_t{32768});
    CHECK_EQ(Sample(image, 2, 0, 1), u32_t{65535});
}

void TestVectorThroughTheBandEncoder() {
    test::Section("a three-component value keeps all three through the band encoder");

    // The path the exporter takes: the writer is created, then *moved* into the encoder before any row
    // is written. A field dropped in that move assignment wrote every vector output as one component
    // in red with green and blue forced to zero, for the whole life of the project, and the only
    // thing that noticed was a person looking at a normal map the colour of a stop sign.
    const std::string path = std::string(kDirectory) + "/normal.png";
    constexpr u32_t   kWidth = 4;
    constexpr u32_t   kRows  = 2;

    Result<PngWriter> writer = PngWriter::Create(path, kWidth, kRows, Mapping{Domain::R2, 3}, -1.0f,
                                                 1.0f, PngCompression{});
    REQUIRE_OK_VOID(writer);

    BandEncoder encoder;
    REQUIRE_OK_VOID(encoder.Start(std::move(*writer), kWidth, kRows, 3));

    // A flat field's normal is (0, 1, 0) -- this is the value whose three components all differ.
    std::array<f32_t, kWidth * kRows * 3> section{};
    for (usize_t i = 0; i < kWidth * kRows; ++i) {
        section[i * 3 + 0] = 0.0f;
        section[i * 3 + 1] = 1.0f;
        section[i * 3 + 2] = 0.0f;
    }
    encoder.Filling().Absorb(section.data(), kWidth, 0, kRows, kWidth);
    REQUIRE_OK_VOID(encoder.Submit(kRows));
    REQUIRE_OK_VOID(encoder.Finish());

    const Decoded image = Decode(path);
    CHECK_EQ(static_cast<u32_t>(image.color), u32_t{2}); // truecolor
    CHECK_EQ(image.width, kWidth);
    CHECK_EQ(image.height, kRows);
    if (image.bytes.size() < kWidth * kRows * 6) {
        CHECK(false);
        return;
    }
    for (u32_t x = 0; x < kWidth; ++x) {
        CHECK_EQ(Sample(image, x, 0, 3), u32_t{32768});  // x =  0 -> 0.5
        CHECK_EQ(Sample(image, x, 1, 3), u32_t{65535});  // y =  1 -> 1.0
        CHECK_EQ(Sample(image, x, 2, 3), u32_t{32768});  // z =  0 -> 0.5
    }
}

[[nodiscard]] std::string HashOf(std::string_view text) {
    Sha256 hash;
    hash.Update(text);
    return std::string(hash.Final().data());
}

void TestSha256AndHashFile() {
    test::Section("the recorded hash is SHA-256, and an unreadable file is an error");

    // FIPS 180-4 test vectors, including the 56-byte message whose padding spills into a second block.
    CHECK(HashOf("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(HashOf("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(HashOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
          == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // Fed in two pieces it must equal the same bytes fed at once: a boundary bug shows up here.
    Sha256 split;
    split.Update(std::string_view{"abcdbcdecdefdefgefghfghighijhijkijkl"});
    split.Update(std::string_view{"jklmklmnlmnomnopnopq"});
    CHECK(std::string(split.Final().data())
          == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // The failure that used to be silent: sixteen zeros and no message.
    const Result<std::array<char, 65>> missing = HashFile("out/test_output_files/no_such_file.lua");
    CHECK(!missing.has_value());
    if (!missing.has_value()) {
        CHECK(std::string_view{missing.error().Format().data()}.find("could not open")
              != std::string_view::npos);
    }
}

} // namespace

int main() {
    REQUIRE_OK(log::Init(log::Config{}));
    REQUIRE_OK(memory::Init());
    REQUIRE_OK(platform::MakeDirectory("out"));
    REQUIRE_OK(platform::MakeDirectory(kDirectory));

    TestGrayscaleBytes();
    TestClampingIsCountedAndSaturates();
    TestVectorThroughTheBandEncoder();
    TestSha256AndHashFile();

    memory::Shutdown();
    log::Shutdown();
    return test::Summary("test_output_files");
}
