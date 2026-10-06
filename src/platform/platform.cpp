// The only translation unit with OS-specific code (spec section 3).

#include <engine/platform.hpp>

#ifdef TRACY_ENABLE
#    include <tracy/Tracy.hpp>
#endif

#include <array>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <Windows.h>
#else
#    include <pthread.h>
#    include <sys/sysinfo.h>
#    include <sys/utsname.h>
#    include <unistd.h>
#endif

namespace engine::platform {

namespace {

constexpr usize_t kMaxText = 256;
constexpr usize_t kMaxPath = 1024;

std::array<char, kMaxText> g_cpuName{};
std::array<char, kMaxText> g_osName{};
std::array<char, kMaxPath> g_executableDirectory{};
u32_t                      g_coreCount  = 0;
u64_t                      g_memoryBytes = 0;

void SetText(std::array<char, kMaxText>& out, const char* text) noexcept {
    const usize_t length = text != nullptr ? std::strlen(text) : 0;
    const usize_t count  = length < kMaxText - 1 ? length : kMaxText - 1;
    if (count > 0) {
        std::memcpy(out.data(), text, count);
    }
    out[count] = '\0';
}

#if defined(_WIN32)

/// Reads one REG_SZ value, returning false when it is missing.
b8_t ReadRegistryString(HKEY root, const char* subKey, const char* value,
                        std::array<char, kMaxText>& out) noexcept {
    DWORD size = static_cast<DWORD>(kMaxText);
    DWORD type = 0;
    const LSTATUS status =
        ::RegGetValueA(root, subKey, value, RRF_RT_REG_SZ, &type, out.data(), &size);
    if (status != ERROR_SUCCESS) {
        out[0] = '\0';
        return false;
    }
    out[kMaxText - 1] = '\0';
    return true;
}

void QueryHost() noexcept {
    if (!ReadRegistryString(HKEY_LOCAL_MACHINE,
                            R"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)",
                            "ProcessorNameString", g_cpuName)) {
        SetText(g_cpuName, "unknown CPU");
    }

    std::array<char, kMaxText> product{};
    std::array<char, kMaxText> build{};
    ReadRegistryString(HKEY_LOCAL_MACHINE, R"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)",
                       "ProductName", product);
    ReadRegistryString(HKEY_LOCAL_MACHINE, R"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)",
                       "CurrentBuildNumber", build);
    if (product[0] == '\0') {
        SetText(g_osName, "Windows");
    } else {
        std::array<char, kMaxText> combined{};
        std::snprintf(combined.data(), kMaxText, "%s (build %s)", product.data(), build.data());
        SetText(g_osName, combined.data());
    }

    SYSTEM_INFO systemInfo{};
    ::GetSystemInfo(&systemInfo);
    g_coreCount = systemInfo.dwNumberOfProcessors;

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (::GlobalMemoryStatusEx(&memory) != 0) {
        g_memoryBytes = memory.ullTotalPhys;
    }
}

void QueryExecutableDirectory() noexcept {
    std::array<wchar_t, kMaxPath> wide{};
    const DWORD length = ::GetModuleFileNameW(nullptr, wide.data(), static_cast<DWORD>(kMaxPath));
    if (length == 0 || length == kMaxPath) {
        g_executableDirectory[0] = '\0';
        return;
    }
    for (DWORD i = length; i-- > 0;) {
        if (wide[i] == L'\\' || wide[i] == L'/') {
            wide[i + 1] = L'\0';
            break;
        }
    }
    const int written = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), -1,
                                              g_executableDirectory.data(),
                                              static_cast<int>(kMaxPath), nullptr, nullptr);
    if (written <= 0) {
        g_executableDirectory[0] = '\0';
    }
}

#else

void QueryHost() noexcept {
    SetText(g_cpuName, "unknown CPU");
    if (std::FILE* info = std::fopen("/proc/cpuinfo", "r"); info != nullptr) {
        std::array<char, 512> line{};
        while (std::fgets(line.data(), static_cast<int>(line.size()), info) != nullptr) {
            if (std::strncmp(line.data(), "model name", 10) == 0) {
                const char* colon = std::strchr(line.data(), ':');
                if (colon != nullptr) {
                    const char* start = colon + 1;
                    while (*start == ' ') {
                        ++start;
                    }
                    SetText(g_cpuName, start);
                    char* newline = std::strchr(g_cpuName.data(), '\n');
                    if (newline != nullptr) {
                        *newline = '\0';
                    }
                }
                break;
            }
        }
        std::fclose(info);
    }

    utsname host{};
    if (::uname(&host) == 0) {
        std::array<char, kMaxText> combined{};
        std::snprintf(combined.data(), kMaxText, "%s %s", host.sysname, host.release);
        SetText(g_osName, combined.data());
    } else {
        SetText(g_osName, "unknown OS");
    }

    g_coreCount = static_cast<u32_t>(::sysconf(_SC_NPROCESSORS_ONLN));
    g_memoryBytes = static_cast<u64_t>(::sysconf(_SC_PHYS_PAGES))
                    * static_cast<u64_t>(::sysconf(_SC_PAGE_SIZE));
}

void QueryExecutableDirectory() noexcept {
    const ssize_t length =
        ::readlink("/proc/self/exe", g_executableDirectory.data(), kMaxPath - 1);
    if (length <= 0) {
        g_executableDirectory[0] = '\0';
        return;
    }
    g_executableDirectory[static_cast<usize_t>(length)] = '\0';
    for (usize_t i = static_cast<usize_t>(length); i-- > 0;) {
        if (g_executableDirectory[i] == '/') {
            g_executableDirectory[i + 1] = '\0';
            break;
        }
    }
}

#endif

} // namespace

Status Init() {
    QueryHost();
    QueryExecutableDirectory();
    SetThreadName("engine-main");
    // Logging is not up yet at this point of the startup order, so the host banner is
    // emitted by engine::init once the loggers exist.
    return {};
}

void Shutdown() noexcept {}

void SetThreadName(const char* name) noexcept {
    if (name == nullptr) {
        return;
    }
#ifdef TRACY_ENABLE
    tracy::SetThreadName(name);
#endif
#if defined(_WIN32)
    std::array<wchar_t, 64> wide{};
    const int written = ::MultiByteToWideChar(CP_UTF8, 0, name, -1, wide.data(),
                                              static_cast<int>(wide.size()));
    if (written > 0) {
        ::SetThreadDescription(::GetCurrentThread(), wide.data());
    }
#else
    ::pthread_setname_np(::pthread_self(), name);
#endif
}

std::string_view ExecutableDirectory() noexcept { return {g_executableDirectory.data()}; }
std::string_view CpuName() noexcept { return {g_cpuName.data()}; }
std::string_view OsName() noexcept { return {g_osName.data()}; }
u32_t            LogicalCoreCount() noexcept { return g_coreCount; }
u64_t            PhysicalMemoryBytes() noexcept { return g_memoryBytes; }

} // namespace engine::platform
