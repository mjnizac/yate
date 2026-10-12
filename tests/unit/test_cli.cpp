// Black-box tests of the two executables: the program, not the library behind it.
//
// Everything else in the suite links the engine and calls into it. This is the only place that runs
// `terrain_export` and `terrain_viewer` as a user does, and checks what a user can see: the exit code,
// the text of the error and the files left behind. It exists because the viewer was found, by hand,
// to exit 0 when its script did not exist -- a defect no in-process test could have reached, since
// the exit code is assembled in `main`.
//
// The executables' paths come from CMake as compile definitions, so this builds only after both.

#include "test_support.hpp"

#include <engine/platform.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>

#ifndef TERRAIN_EXPORT_EXE
#    error "TERRAIN_EXPORT_EXE is set by tests/CMakeLists.txt"
#endif
#ifndef TERRAIN_VIEWER_EXE
#    error "TERRAIN_VIEWER_EXE is set by tests/CMakeLists.txt"
#endif

namespace {

constexpr const char* kDirectory = "out/test_cli";

struct Run {
    int         exitCode = -1;
    std::string output;
};

[[nodiscard]] std::string ReadFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

/// Runs `executable` with `arguments`, capturing stdout and stderr together.
[[nodiscard]] Run Execute(const char* executable, const std::string& arguments) {
    const std::string capture = std::string(kDirectory) + "/capture.txt";
    std::string       command = std::string("\"") + executable + "\" " + arguments + " > \"" + capture
                          + "\" 2>&1";
#ifdef _WIN32
    // `cmd /c` strips the outermost pair of quotes, which would eat the executable's own.
    command = "\"" + command + "\"";
#endif
    Run run;
    run.exitCode = std::system(command.c_str());
    run.output   = ReadFile(capture);
    return run;
}

[[nodiscard]] bool Contains(const std::string& text, std::string_view fragment) {
    return text.find(fragment) != std::string::npos;
}

/// Writes a Lua script under the test directory and returns its path.
[[nodiscard]] std::string WriteScript(const char* name, std::string_view source) {
    const std::string path = std::string(kDirectory) + "/" + name + ".lua";
    std::ofstream     out(path, std::ios::binary);
    out.write(source.data(), static_cast<std::streamsize>(source.size()));
    return path;
}

void Expect(bool condition, const std::string& label, const char* what, const Run& run) {
    ++test::g_checks;
    if (!condition) {
        ++test::g_failures;
        std::printf("FAIL %s: %s (exit %d)\n     output: %.300s\n", label.c_str(), what,
                    run.exitCode, run.output.c_str());
    }
}

/// An invocation that must fail, with an exit code that is not zero and a message containing every
/// fragment. The message is the contract: it has to name the option and the offending text.
void MustFail(const char* executable, const std::string& label, const std::string& arguments,
              std::initializer_list<const char*> fragments) {
    const Run run = Execute(executable, arguments);
    Expect(run.exitCode != 0, label, "the exit code is not zero", run);
    for (const char* fragment : fragments) {
        Expect(Contains(run.output, fragment), label, fragment, run);
    }
}

const std::string kGood = std::string(kDirectory) + "/good.lua";

void TestExporterArguments() {
    test::Section("terrain_export rejects bad arguments, naming the option and the text");

    const std::string base = "--script \"" + kGood + "\" --out \"" + kDirectory + "/o\" ";

    MustFail(TERRAIN_EXPORT_EXE, "no_arguments", "", {"--script is required"});
    MustFail(TERRAIN_EXPORT_EXE, "unknown_option", base + "--bogus 1", {"unknown option", "--bogus"});
    MustFail(TERRAIN_EXPORT_EXE, "missing_value", base + "--seed", {"--seed needs a value"});

    // The class of input the first version of the parser let through.
    MustFail(TERRAIN_EXPORT_EXE, "resolution_trailing_text", base + "--resolution 123abc",
             {"--resolution", "finite number", "123abc"});
    MustFail(TERRAIN_EXPORT_EXE, "resolution_nan", base + "--resolution nan", {"--resolution", "nan"});
    MustFail(TERRAIN_EXPORT_EXE, "resolution_inf", base + "--resolution inf", {"--resolution", "inf"});
    MustFail(TERRAIN_EXPORT_EXE, "resolution_zero", base + "--resolution 0",
             {"--resolution", "greater than zero"});
    MustFail(TERRAIN_EXPORT_EXE, "resolution_negative", base + "--resolution -1",
             {"--resolution", "greater than zero"});
    MustFail(TERRAIN_EXPORT_EXE, "section_trailing_text", base + "--section 200abc",
             {"--section", "whole number", "200abc"});
    MustFail(TERRAIN_EXPORT_EXE, "section_not_power_of_two", base + "--section 7",
             {"power of two"});
    MustFail(TERRAIN_EXPORT_EXE, "section_enormous", base + "--section 99999999", {"--section"});
    MustFail(TERRAIN_EXPORT_EXE, "seed_negative", base + "--seed -3", {"--seed", "whole number"});
    MustFail(TERRAIN_EXPORT_EXE, "png_level_over", base + "--png-level 12", {"--png-level", "9"});
    MustFail(TERRAIN_EXPORT_EXE, "min_overflows", base + "--min 1e400,0 --max 1,1",
             {"--min", "finite number"});
    MustFail(TERRAIN_EXPORT_EXE, "min_four_components", base + "--min 1,2,3,4 --max 5,5",
             {"--min", "<x>,<z>"});
    MustFail(TERRAIN_EXPORT_EXE, "min_empty_component", base + "--min 1, --max 5,5", {"--min"});
    MustFail(TERRAIN_EXPORT_EXE, "param_without_equals", base + "--param noequals",
             {"--param", "key=value"});
}

void TestExporterJobLimits() {
    test::Section("terrain_export refuses a job nobody meant");

    const std::string base = "--script \"" + kGood + "\" --out \"" + kDirectory + "/o\" ";
    MustFail(TERRAIN_EXPORT_EXE, "absurd_bounds", base + "--max 1e9,1e9",
             {"over the limit", "--max-samples"});
    MustFail(TERRAIN_EXPORT_EXE, "unrepresentable_bound", base + "--max 1e30,1e30",
             {"not representable"});
    MustFail(TERRAIN_EXPORT_EXE, "empty_bounds", base + "--min 10,10 --max 10,10",
             {"bounds span"});
}

void TestScripts() {
    test::Section("a missing or broken script is a failure the user can act on");

    const std::string out = std::string("--out \"") + kDirectory + "/o\" ";
    MustFail(TERRAIN_EXPORT_EXE, "script_missing", "--script nope.lua " + out,
             {"[script]", "nope.lua"});

    const std::string syntax = WriteScript("syntax", "local function main(\nend\nreturn main\n");
    MustFail(TERRAIN_EXPORT_EXE, "script_syntax_error", "--script \"" + syntax + "\" " + out,
             {"[script]", "syntax.lua"});

    const std::string loops = WriteScript("loops", "local function main()\n  while true do end\nend\n"
                                                   "return main\n");
    MustFail(TERRAIN_EXPORT_EXE, "script_loops_forever", "--script \"" + loops + "\" " + out,
             {"time budget", "infinite loop"});

    const std::string loads = WriteScript("loads", "local function main()\n  dofile('x.lua')\nend\n"
                                                   "return main\n");
    MustFail(TERRAIN_EXPORT_EXE, "script_reads_a_file", "--script \"" + loads + "\" " + out,
             {"attempt to call a nil value", "dofile"});
}

void TestViewerExitCodes() {
    test::Section("terrain_viewer reports failure through its exit code");

    // The defect this suite was written for: a missing script used to leave the exit code at 0.
    MustFail(TERRAIN_VIEWER_EXE, "viewer_script_missing", "--script nope.lua --frames 1",
             {"[script]", "nope.lua"});
    MustFail(TERRAIN_VIEWER_EXE, "viewer_no_arguments", "", {"--script is required"});
    MustFail(TERRAIN_VIEWER_EXE, "viewer_ring_enormous", "--script \"" + kGood + "\" --ring 99",
             {"--ring", "at most"});
    MustFail(TERRAIN_VIEWER_EXE, "viewer_width_trailing", "--script \"" + kGood + "\" --width 12x",
             {"--width", "whole number"});
}

void TestSuccessIsReproducible() {
    test::Section("a good export succeeds and is reproducible");

    const std::string dirA = std::string(kDirectory) + "/a";
    const std::string dirB = std::string(kDirectory) + "/b";
    const std::string common = "--script \"" + kGood + "\" --min 0,0 --max 256,256 --resolution 2 "
                               "--section 64 ";

    const Run first  = Execute(TERRAIN_EXPORT_EXE, common + "--out \"" + dirA + "\"");
    const Run second = Execute(TERRAIN_EXPORT_EXE, common + "--out \"" + dirB + "\"");
    Expect(first.exitCode == 0, "export_first", "the export exits 0", first);
    Expect(second.exitCode == 0, "export_second", "the export exits 0", second);
    if (first.exitCode != 0 || second.exitCode != 0) {
        return;
    }

    const std::string heightA = ReadFile(dirA + "/height.png");
    const std::string heightB = ReadFile(dirB + "/height.png");
    CHECK(heightA.size() > 100);
    // PNG signature: eight fixed bytes, so a file that is merely non-empty is not accepted.
    CHECK(heightA.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0);
    CHECK(heightA == heightB);

    const std::string metadata = ReadFile(dirA + "/metadata.json");
    CHECK(Contains(metadata, "\"script_hash\""));
    CHECK(Contains(metadata, "\"engine_version\""));
    CHECK(Contains(metadata, "\"height\""));

    // A different section size must not change a single byte of the heightmap. This is the engine's
    // one firm claim, checked from the outside with the real program.
    const std::string dirC = std::string(kDirectory) + "/c";
    const Run third = Execute(TERRAIN_EXPORT_EXE, "--script \"" + kGood
                                                      + "\" --min 0,0 --max 256,256 --resolution 2 "
                                                        "--section 128 --out \""
                                                      + dirC + "\"");
    Expect(third.exitCode == 0, "export_other_section", "the export exits 0", third);
    CHECK(ReadFile(dirC + "/height.png") == heightA);
}

} // namespace

int main() {
    // `out` does not exist when ctest runs from a fresh build directory, and MakeDirectory makes one
    // level at a time.
    for (const char* path : {"out", kDirectory}) {
        REQUIRE_OK(engine::platform::MakeDirectory(path));
    }

    {
        std::ofstream good(kGood, std::ios::binary);
        good << "local function main(params)\n"
                "  local n = Noises.fBm{ frequency = 0.01, octaves = 4, amplitude = 100.0 }\n"
                "  return { height = { value = n.value, range = { -200, 200 } } }\n"
                "end\n"
                "return main\n";
    }

    TestExporterArguments();
    TestExporterJobLimits();
    TestScripts();
    TestViewerExitCodes();
    TestSuccessIsReproducible();

    return test::Summary("test_cli");
}
