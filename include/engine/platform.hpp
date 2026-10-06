#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <engine/error.hpp>

#    include <string_view>

/// The only module allowed to contain OS-specific code (spec section 3).
namespace engine::platform {

/// Queries the host description and prepares per-process state. Must run first.
[[nodiscard]] Status Init();

void Shutdown() noexcept;

/// Names the calling OS thread, for debuggers and profilers.
void SetThreadName(const char* name) noexcept;

/// Absolute directory the running executable lives in, with a trailing separator.
/// Shader `.spv` files and the pipeline cache are resolved against it.
[[nodiscard]] std::string_view ExecutableDirectory() noexcept;

/// Host description, valid after `Init`. Used by log banners and the metadata sidecar.
[[nodiscard]] std::string_view CpuName() noexcept;
[[nodiscard]] std::string_view OsName() noexcept;
[[nodiscard]] u32_t            LogicalCoreCount() noexcept;
[[nodiscard]] u64_t            PhysicalMemoryBytes() noexcept;

} // namespace engine::platform

#endif // IS_ENGINE
