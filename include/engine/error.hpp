#pragma once

#include <engine/common.hpp>

#include <array>
#include <expected>
#include <format>
#include <string_view>

namespace engine {

enum class ErrorCode : u32_t {
    None = 0,
    InvalidArgument,
    NotFound,
    Unsupported,
    OutOfMemory,
    VulkanError,
    IoError,
    ScriptError,
    InternalError,
};

/// Pipeline phase an error originated from. Printed between brackets so that the
/// `file:line: [stage] message` format is machine-parseable (see spec section 11).
enum class ErrorStage : u32_t {
    None = 0,
    Init,
    Platform,
    Memory,
    Vulkan,
    Script,
    Validation,
    Compile,
    Evaluate,
    Export,
    Shutdown,
};

ENGINE_API const char* ToString(ErrorCode code) noexcept;
ENGINE_API const char* ToString(ErrorStage stage) noexcept;

/// Fallible-operation payload.
///
/// Every field is stored inline: an `Error` never owns heap memory, so it can cross the
/// shared-library boundary without violating the module boundary rule (spec section 7.2).
struct Error {
    static constexpr usize_t kMaxMessage = 384;
    static constexpr usize_t kMaxFile    = 112;
    static constexpr usize_t kMaxFormat  = kMaxMessage + kMaxFile + 64;

    ErrorCode  code  = ErrorCode::None;
    ErrorStage stage = ErrorStage::None;
    /// Source line inside `file`, or 0 when the error has no script location.
    u32_t line = 0;
    /// Script path the error points at. Empty for errors with no script location.
    std::array<char, kMaxFile>    file{};
    std::array<char, kMaxMessage> message{};

    [[nodiscard]] std::string_view Message() const noexcept { return {message.data()}; }
    [[nodiscard]] std::string_view File() const noexcept { return {file.data()}; }

    /// Renders `file:line: [stage] message`, or `[stage] message` with no location.
    /// Always NUL-terminated.
    [[nodiscard]] ENGINE_API std::array<char, kMaxFormat> Format() const noexcept;
};

/// Result of a fallible operation that produces a value.
template <typename T>
using Result = std::expected<T, Error>;

/// Result of a fallible operation that produces nothing.
using Status = std::expected<void, Error>;

namespace detail {

/// Copies `text` into `out`, truncating and always NUL-terminating.
template <usize_t N>
constexpr void CopyBounded(std::array<char, N>& out, std::string_view text) noexcept {
    const usize_t count = text.size() < N - 1 ? text.size() : N - 1;
    for (usize_t i = 0; i < count; ++i) {
        out[i] = text[i];
    }
    out[count] = '\0';
}

} // namespace detail

/// Builds an error with no script location.
template <typename... Args>
[[nodiscard]] Error MakeError(ErrorCode code, ErrorStage stage,
                              std::format_string<Args...> fmt, Args&&... args) {
    Error error{.code = code, .stage = stage};
    const auto result = std::format_to_n(error.message.data(), Error::kMaxMessage - 1, fmt,
                                         std::forward<Args>(args)...);
    *result.out = '\0';
    return error;
}

/// Builds an error that points at a script location.
template <typename... Args>
[[nodiscard]] Error MakeScriptError(ErrorCode code, ErrorStage stage, std::string_view file,
                                    u32_t line, std::format_string<Args...> fmt, Args&&... args) {
    Error error = MakeError(code, stage, fmt, std::forward<Args>(args)...);
    detail::CopyBounded(error.file, file);
    error.line = line;
    return error;
}

/// Shorthand for returning an error from a function whose result is `Result<T>` or `Status`.
#define ENGINE_FAIL(code, stage, ...)                                                               \
    return ::std::unexpected(::engine::MakeError((code), (stage), __VA_ARGS__))

} // namespace engine
