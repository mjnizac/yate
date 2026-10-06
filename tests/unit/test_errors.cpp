// Error payload and the single `file:line: [stage] message` format every stage reports
// (spec section 11). Errors must be specific enough to act on without reading engine code.

#include "test_support.hpp"

#include <engine/engine.hpp>
#include <engine/error.hpp>

#include <string_view>

using namespace engine;

namespace {

void TestFormatWithoutLocation() {
    test::Section("format without a script location");
    const Error error =
        MakeError(ErrorCode::Unsupported, ErrorStage::Vulkan, "device {} does not support {}",
                  "GeForce GTX 1070", "Vulkan 1.4");
    CHECK_EQ(std::string_view{error.Format().data()},
             std::string_view{"[vulkan] device GeForce GTX 1070 does not support Vulkan 1.4"});
    CHECK(error.code == ErrorCode::Unsupported);
    CHECK(error.stage == ErrorStage::Vulkan);
    CHECK_EQ(error.line, u32_t{0});
    CHECK(error.File().empty());
}

void TestFormatWithLocation() {
    test::Section("format with a script location");
    // The exact wording the spec gives as the reference example.
    const Error error = MakeScriptError(ErrorCode::ScriptError, ErrorStage::Validation,
                                       "terrain.lua", 42,
                                       "Erosion.Hydraulic expects {} input, got {}", "R2->R1",
                                       "R3->R1");
    CHECK_EQ(std::string_view{error.Format().data()},
             std::string_view{"terrain.lua:42: [validation] Erosion.Hydraulic expects R2->R1 "
                              "input, got R3->R1"});
    CHECK_EQ(error.line, u32_t{42});
    CHECK_EQ(error.File(), std::string_view{"terrain.lua"});
}

void TestTruncation() {
    test::Section("truncation");
    // An over-long message is truncated, never overruns and stays NUL-terminated.
    std::array<char, 1024> wide{};
    wide.fill('x');
    wide[wide.size() - 1] = '\0';
    const Error error =
        MakeScriptError(ErrorCode::InternalError, ErrorStage::Compile,
                        std::string_view{wide.data()}, 7, "{}", std::string_view{wide.data()});
    CHECK(error.Message().size() == Error::kMaxMessage - 1);
    CHECK(error.File().size() == Error::kMaxFile - 1);
    CHECK(error.Format()[Error::kMaxFormat - 1] == '\0');
}

void TestExitCodes() {
    test::Section("exit codes");
    // Bit flags, so a combined failure is still readable.
    ExitCode exit = ExitCode::Success;
    CHECK_EQ(static_cast<i32_t>(exit), 0);
    exit |= ExitCode::FailedToEvaluate;
    exit |= ExitCode::FailedToShutdown;
    CHECK(HasFlag(exit, ExitCode::FailedToEvaluate));
    CHECK(HasFlag(exit, ExitCode::FailedToShutdown));
    CHECK(!HasFlag(exit, ExitCode::FailedToExport));
    CHECK_EQ(static_cast<i32_t>(exit), static_cast<i32_t>(ExitCode::FailedToEvaluate)
                                           | static_cast<i32_t>(ExitCode::FailedToShutdown));
}

void TestResultPropagation() {
    test::Section("Result propagation");
    const auto fallible = [](b8_t ok) -> Result<u32_t> {
        if (!ok) {
            ENGINE_FAIL(ErrorCode::NotFound, ErrorStage::Export, "output {} is missing", "height");
        }
        return 7u;
    };
    CHECK(fallible(true).has_value());
    CHECK_EQ(*fallible(true), u32_t{7});

    const Result<u32_t> failed = fallible(false);
    CHECK(!failed.has_value());
    CHECK(failed.error().code == ErrorCode::NotFound);
    CHECK_EQ(std::string_view{failed.error().Format().data()},
             std::string_view{"[export] output height is missing"});
}

} // namespace

int main() {
    TestFormatWithoutLocation();
    TestFormatWithLocation();
    TestTruncation();
    TestExitCodes();
    TestResultPropagation();
    return test::Summary("test_errors");
}
