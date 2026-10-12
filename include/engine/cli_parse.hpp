#pragma once

#include <engine/common.hpp>
#include <engine/error.hpp>

#include <charconv>
#include <cmath>
#include <string_view>
#include <system_error>

/// Strict parsing of command-line numbers, shared by both executables.
///
/// The `strtod`/`strtoul` calls these replace stopped at the first character they did not
/// understand and said nothing, so `--resolution 123abc` ran at 123, `--section 200abc` ran at 200
/// until the power-of-two check caught it for an unrelated reason, and `--resolution nan` or `-1`
/// got as far as the first script that happened to divide by it. A token is a number or it is an
/// error that names the option and the text.
namespace engine::cli {

/// A whole, non-negative integer no larger than `maximum`. No sign, no whitespace, no suffix.
[[nodiscard]] inline Result<u64_t> ParseUnsigned(std::string_view option, std::string_view text,
                                                 u64_t maximum = ~0ull) {
    u64_t      value = 0;
    const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (text.empty() || code != std::errc{} || end != text.data() + text.size()) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                    "{} expects a whole number, got '{}'", option, text);
    }
    if (value > maximum) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                    "{} expects at most {}, got {}", option, maximum, value);
    }
    return value;
}

/// A finite floating-point number. `nan`, `inf` and anything that overflows a double are rejected,
/// because each of them would otherwise reach an integer conversion that is undefined behaviour.
[[nodiscard]] inline Result<f64_t> ParseFinite(std::string_view option, std::string_view text) {
    f64_t      value = 0.0;
    const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || code != std::errc{} || end != text.data() + text.size()
        || !std::isfinite(value)) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                    "{} expects a finite number, got '{}'", option, text);
    }
    return value;
}

/// A finite number strictly greater than zero.
[[nodiscard]] inline Result<f64_t> ParsePositive(std::string_view option, std::string_view text) {
    Result<f64_t> value = ParseFinite(option, text);
    if (!value) {
        return value;
    }
    if (!(*value > 0.0)) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                    "{} expects a number greater than zero, got {}", option, *value);
    }
    return value;
}

} // namespace engine::cli
