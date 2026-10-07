#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <engine/assert.hpp>
#    include <engine/error.hpp>

#    include <vulkan/vulkan.h>

namespace engine::vulkan {

/// Spelling of a `VkResult` enumerator, or `VK_RESULT_<number>` for unknown values.
[[nodiscard]] const char* ResultName(VkResult result) noexcept;

/// Number of error-severity messages the debug messenger has reported this process.
///
/// Exposed so a test can fail on a validation error instead of only logging it. Every GPU test
/// checks it, which turns the whole suite into a validation run.
[[nodiscard]] u64_t ValidationErrorCount() noexcept;

/// Builds an `Error` that names the failing call and the result.
[[nodiscard]] Error MakeVulkanError(VkResult result, const char* call) noexcept;

} // namespace engine::vulkan

/// Hard invariant on a Vulkan call. Aborts in debug builds (see ENGINE_ASSERT).
/// The call itself always runs; only the check disappears in release builds, which is why the
/// result is explicitly discarded there.
#    define VK_CHECK(call)                                                                          \
        do {                                                                                        \
            const VkResult vkResult_ = (call);                                                      \
            ENGINE_ASSERT(vkResult_ == VK_SUCCESS, "{} returned {}", #call,                          \
                          ::engine::vulkan::ResultName(vkResult_));                                  \
            (void)vkResult_;                                                                        \
        } while (false)

/// Logs and returns `ret` when the call fails.
#    define VK_CHECK_RETURN(ret, call)                                                              \
        do {                                                                                        \
            const VkResult vkResult_ = (call);                                                      \
            ENGINE_ASSERT_RETURN(ret, vkResult_ == VK_SUCCESS, "{} returned {}", #call,              \
                                 ::engine::vulkan::ResultName(vkResult_));                           \
        } while (false)

/// Logs, runs `cleanup` and returns `ret` when the call fails.
#    define VK_CHECK_X(ret, call, cleanup)                                                          \
        do {                                                                                        \
            const VkResult vkResult_ = (call);                                                      \
            ENGINE_ASSERT_X(ret, vkResult_ == VK_SUCCESS, cleanup, "{} returned {}", #call,          \
                            ::engine::vulkan::ResultName(vkResult_));                                \
        } while (false)

/// Propagates a failing call as `std::unexpected(Error)`. For functions returning
/// `Result<T>` or `Status`, which is the engine's error-handling policy (spec section 3).
#    define VK_TRY(call)                                                                            \
        do {                                                                                        \
            const VkResult vkResult_ = (call);                                                      \
            if (vkResult_ != VK_SUCCESS) [[unlikely]] {                                             \
                return ::std::unexpected(::engine::vulkan::MakeVulkanError(vkResult_, #call));      \
            }                                                                                       \
        } while (false)

#endif // IS_ENGINE
