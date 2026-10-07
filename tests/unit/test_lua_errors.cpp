// What a bad script says. This is the milestone 6 acceptance test (spec section 15).
//
// A terrain script is the only surface a user writes against, so the quality of these messages is the
// quality of the engine from where they sit. Every case below asserts the whole contract: the exit is
// a failure, the message carries the script's own file and line, the stage is `[script]`, and the
// text names what was wrong specifically enough to fix without reading engine code.
//
// The scripts are written to a temporary directory rather than kept as files, so each case reads as
// one unit: the source that breaks and the message it must produce, side by side.

#include "test_support.hpp"

#include <engine/log.hpp>
#include <engine/lua.hpp>
#include <engine/memory/allocator.hpp>
#include <engine/platform.hpp>
#include <engine/terrain/graph.hpp>

#include <cstdio>
#include <string>
#include <string_view>

using namespace engine;
using terrain::Graph;

namespace {

constexpr const char* kDirectory = "out/test_lua_errors";

/// Records one expectation, labelled by case and by what was expected.
///
/// Local rather than in test_support.hpp because every check here is "case X must say Y", and a
/// failure is only useful if it prints both.
void Expect(b8_t condition, const char* name, const char* what) {
    ++test::g_checks;
    if (!condition) {
        ++test::g_failures;
        std::printf("FAIL %s: %s\n", name, what);
    }
}

/// Writes `source` to a file named after the case, so the reported path is the case name.
[[nodiscard]] std::string WriteScript(const char* name, std::string_view source) {
    std::string path = std::string(kDirectory) + "/" + name + ".lua";
    std::FILE*  file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return {};
    }
    (void)std::fwrite(source.data(), 1, source.size(), file);
    (void)std::fclose(file);
    return path;
}

/// Runs `source` and checks the error against the expected stage, line and substrings.
///
/// `line` is the script line the message must point at; 0 means the message is about the script as a
/// whole and no line is asserted. Substrings rather than whole messages, so a case pins the parts
/// that matter and does not break when a list of accepted keys grows.
///
/// The stage is part of the contract and it is not always `[script]`: a binding that rejects its own
/// arguments reports `[script]`, while a value of the wrong mapping is caught by the graph builder
/// and reports `[validation]`, which is the stage the spec's own example shows. Both carry the script
/// line, which is the part that matters to whoever has to fix it.
void CheckFailure(const char* name, std::string_view source, u32_t line,
                  std::initializer_list<const char*> fragments,
                  ErrorStage expectedStage = ErrorStage::Script) {
    const std::string path = WriteScript(name, source);
    if (path.empty()) {
        Expect(false, name, "the script file could be written");
        return;
    }

    Graph                    graph;
    lua::ScriptEnvironment   environment;
    environment.maxX       = 256.0;
    environment.maxZ       = 256.0;
    environment.resolution = 1.0;

    const Status result = lua::RunScript(graph, path, environment);
    Expect(!result.has_value(), name, "the script is rejected");
    if (result.has_value()) {
        return;
    }

    const Error&                              error     = result.error();
    const std::array<char, Error::kMaxFormat> formatted = error.Format();
    const std::string_view                    message{formatted.data()};

    Expect(error.stage == expectedStage, name, "the stage is the expected one");
    if (error.stage != expectedStage) {
        std::printf("     %s: expected [%s], got: %s\n", name, ToString(expectedStage),
                    formatted.data());
    }
    if (line != 0) {
        Expect(error.line == line, name, "the message points at the right line");
        if (error.line != line) {
            std::printf("     %s: expected line %u, got %u in: %s\n", name, line, error.line,
                        formatted.data());
        }
    }
    for (const char* fragment : fragments) {
        const b8_t found = message.find(fragment) != std::string_view::npos;
        Expect(found, name, fragment);
        if (!found) {
            std::printf("     %s: expected '%s' in: %s\n", name, fragment, formatted.data());
        }
    }
}

// --- Mapping mismatches -------------------------------------------------------------------------
//
// The class of error the spec singles out: a value of the wrong mapping reaching an op. Each one must
// name the op, what it wanted and what it got.

void TestMappingErrors() {
    test::Section("mapping mismatches");

    CheckFailure("normals_from_scalar", R"(local function main()
    local n = Noises.fBm{ frequency = 0.01 }
    return { normals = { value = Terrain.Normals{ input = Filters.Blur{ input = n.value,
                                                                       radius = 2 } } } }
end
return main
)",
                 3, {"Terrain.Normals", "R2->R2", "R2->R1", ".gradient"});

    CheckFailure("slope_from_normals", R"(local function main()
    local n = Noises.fBm{ frequency = 0.01 }
    local normals = Terrain.Normals{ input = n.gradient }
    local mask = Masks.Slope{ input = normals, min = 0.1, max = 0.5 }
    return { height = { value = mask, range = { 0, 1 } } }
end
return main
)",
                 4, {"Masks.Slope", "R2->R2", "R2->R3"});

    CheckFailure("blend_mismatched_operands", R"(local function main()
    local n = Noises.fBm{ frequency = 0.01 }
    local mask = Masks.Slope{ input = n.gradient, min = 0.1, max = 0.5 }
    return { height = { value = Combine.Blend{ a = n.value, b = n.gradient, mask = mask },
                        range = { 0, 1 } } }
end
return main
)",
                 4, {"Blend", "R2->R1", "R2->R2"}, ErrorStage::Validation);

    CheckFailure("arith_across_domains", R"(local function main()
    local flat = Noises.fBm{ frequency = 0.01 }
    local solid = Noises.fBm{ domain = 3, frequency = 0.01 }
    return { height = { value = flat.value + solid.value, range = { 0, 1 } } }
end
return main
)",
                 4, {"R2->R1", "R3->R1"}, ErrorStage::Validation);

    CheckFailure("no_gradient_channel", R"(local function main()
    local c = Terrain.Const{ value = 1.0 }
    return { height = { value = c.gradient, range = { 0, 1 } } }
end
return main
)",
                 3, {"gradient"});
}

// --- Parameter errors ---------------------------------------------------------------------------
//
// A named-table API lives or dies on rejecting what it was not asked for. A silently ignored key is
// the one failure mode that produces a plausible wrong result instead of a message.

void TestParameterErrors() {
    test::Section("parameters");

    CheckFailure("misspelled_key", R"(local function main()
    local n = Noises.fBm{ frequency = 0.01, persistance = 0.3 }
    return { height = { value = n.value, range = { 0, 1 } } }
end
return main
)",
                 2, {"Noises.fBm", "persistance", "persistence"});

    CheckFailure("zero_frequency", R"(local function main()
    local n = Noises.fBm{ frequency = 0.0 }
    return { height = { value = n.value, range = { 0, 1 } } }
end
return main
)",
                 2, {"frequency"}, ErrorStage::Validation);

    CheckFailure("zero_persistence", R"(local function main()
    local n = Noises.fBm{ frequency = 0.01, persistence = 0.0 }
    return { height = { value = n.value, range = { 0, 1 } } }
end
return main
)",
                 2, {"persistence"}, ErrorStage::Validation);

    CheckFailure("fractional_octaves", R"(local function main()
    local n = Noises.fBm{ frequency = 0.01, octaves = 2.5 }
    return { height = { value = n.value, range = { 0, 1 } } }
end
return main
)",
                 2, {"octaves", "whole number"});

    CheckFailure("octaves_out_of_range", R"(local function main()
    local n = Noises.fBm{ frequency = 0.01, octaves = 0 }
    return { height = { value = n.value, range = { 0, 1 } } }
end
return main
)",
                 2, {"octaves"});

    CheckFailure("unknown_noise_kind", R"(local function main()
    local n = Noises.fBm{ kind = "worley", frequency = 0.01 }
    return { height = { value = n.value, range = { 0, 1 } } }
end
return main
)",
                 2, {"worley", "simplex"});

    CheckFailure("frequency_is_a_string", R"(local function main()
    local n = Noises.fBm{ frequency = "fast" }
    return { height = { value = n.value, range = { 0, 1 } } }
end
return main
)",
                 2, {"frequency", "number"});

    CheckFailure("blur_without_input", R"(local function main()
    return { height = { value = Filters.Blur{ radius = 2 }, range = { 0, 1 } } }
end
return main
)",
                 2, {"Filters.Blur", "input"});

    CheckFailure("blur_radius_too_large", R"(local function main()
    local n = Noises.fBm{ frequency = 0.01 }
    return { height = { value = Filters.Blur{ input = n.value, radius = 1000 },
                        range = { 0, 1 } } }
end
return main
)",
                 3, {"radius"});

    CheckFailure("inverted_slope_range", R"(local function main()
    local n = Noises.fBm{ frequency = 0.01 }
    local mask = Masks.Slope{ input = n.gradient, min = 0.9, max = 0.2 }
    return { height = { value = mask, range = { 0, 1 } } }
end
return main
)",
                 3, {"Masks.Slope", "min < max"});

    CheckFailure("combine_too_few", R"(local function main()
    local n = Noises.fBm{ frequency = 0.01 }
    return { height = { value = Vec.Combine{ n.value }, range = { 0, 1 } } }
end
return main
)",
                 3, {"Vec.Combine", "2 to 4"});

    CheckFailure("unknown_field", R"(local function main()
    local n = Noises.fBm{ frequency = 0.01 }
    return { height = { value = n.magnitude, range = { 0, 1 } } }
end
return main
)",
                 3, {"magnitude", ".value", ".gradient"});
}

// --- Script-level errors ------------------------------------------------------------------------

void TestScriptErrors() {
    test::Section("script shape");

    CheckFailure("syntax_error", R"(local function main(
    return {}
end
return main
)",
                 0, {"could not load the script", "syntax_error.lua"});

    CheckFailure("no_main", R"(local x = 1
)",
                 0, {"main"});

    CheckFailure("main_returns_nothing", R"(local function main()
end
return main
)",
                 0, {"main must return a table"});

    CheckFailure("empty_outputs", R"(local function main()
    return {}
end
return main
)",
                 0, {"no outputs"});

    CheckFailure("output_without_value", R"(local function main()
    local n = Noises.fBm{ frequency = 0.01 }
    return { height = { range = { 0, 1 } } }
end
return main
)",
                 0, {"height", "no value"});

    CheckFailure("scalar_output_without_range", R"(local function main()
    local n = Noises.fBm{ frequency = 0.01 }
    return { height = { value = n.value } }
end
return main
)",
                 0, {"height", "range", "R2->R1"});

    CheckFailure("runtime_error_in_main", R"(local function main(params)
    local n = Noises.fBm{ frequency = params.nothing.at_all }
    return { height = { value = n.value, range = { 0, 1 } } }
end
return main
)",
                 0, {"nothing"});

    CheckFailure("missing_file", "", 0, {});

    // `io` and `os` are deliberately not opened, so a script cannot read files or start processes.
    CheckFailure("no_io_library", R"(local function main()
    local f = io.open("secret.txt", "r")
    return { height = { value = f, range = { 0, 1 } } }
end
return main
)",
                 0, {"io"});
}

/// A script that is fine must stay fine. Without this the suite would pass if `RunScript` simply
/// failed on everything.
void TestGoodScriptPasses() {
    test::Section("a correct script is accepted");

    const std::string path = WriteScript("good", R"(local function main(params)
    local base = Noises.fBm{ frequency = 0.002, octaves = 5, amplitude = params.amplitude }
    local peaks = Noises.Ridged{ frequency = 0.01, octaves = 3, seed = 7 }
    local height = Combine.Blend{
        a = base.value,
        b = Curves.Clamp{ input = peaks.value, min = 0.0, max = 1.0 },
        mask = Masks.Slope{ input = base.gradient, min = 0.1, max = 0.6 },
    }
    return {
        height  = { value = height * 2.0 - 1.0, range = { -100, 900 } },
        normals = { value = Terrain.Normals{ input = base.gradient } },
    }
end
return main
)");

    Graph                  graph;
    lua::ScriptEnvironment environment;
    environment.maxX       = 256.0;
    environment.maxZ       = 256.0;
    environment.resolution = 1.0;
    terrain::Params params;
    CHECK(params.Set("amplitude", "400"));
    environment.params = &params;

    REQUIRE_OK_VOID(lua::RunScript(graph, path, environment));
    CHECK_EQ(graph.OutputCount(), usize_t{2});
    // Name order, not table order: a Lua table has no iteration order and the request order reaches
    // the buffer planner.
    CHECK(std::string_view{graph.Output(0).name.data()} == "height");
    CHECK(std::string_view{graph.Output(1).name.data()} == "normals");
    CHECK(graph.NodeCount() >= 7);
}

} // namespace

int main() {
    REQUIRE_OK(log::Init(log::Config{}));
    REQUIRE_OK(memory::Init());
    // MakeDirectory does not create parents, and ctest runs from the build directory where `out`
    // does not exist yet.
    REQUIRE_OK(platform::MakeDirectory("out"));
    REQUIRE_OK(platform::MakeDirectory(kDirectory));

    TestMappingErrors();
    TestParameterErrors();
    TestScriptErrors();
    TestGoodScriptPasses();

    // Every state is created and destroyed inside RunScript, so nothing may be left behind.
    CHECK_EQ(lua::AllocatedBytes(), u64_t{0});

    memory::Shutdown();
    log::Shutdown();
    return test::Summary("test_lua_errors");
}
