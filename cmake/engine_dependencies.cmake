# Third-party dependencies, every one pinned to a release tag (spec section 4).

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- spdlog -----------------------------------------------------------------------------------

set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.1
    GIT_SHALLOW    TRUE
)

# --- Tracy ------------------------------------------------------------------------------------
# Built as a shared library so the engine DLL and the executables report into one session.

set(TRACY_ENABLE ${ENGINE_TRACY} CACHE BOOL "" FORCE)
set(TRACY_STATIC OFF CACHE BOOL "" FORCE)
set(TRACY_ON_DEMAND OFF CACHE BOOL "" FORCE)   # Memory graphs need every event from startup.
set(TRACY_CALLSTACK ${ENGINE_TRACY_CALLSTACKS} CACHE BOOL "" FORCE)
FetchContent_Declare(tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG        v0.14.1
    GIT_SHALLOW    TRUE
)

# --- mimalloc ---------------------------------------------------------------------------------
# Static and without the global override: the engine installs its own operator new.

set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
set(MI_INSTALL_TOPLEVEL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(mimalloc
    GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
    GIT_TAG        v2.1.9
    GIT_SHALLOW    TRUE
)

# --- Vulkan Memory Allocator -------------------------------------------------------------------

set(VMA_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(VMA_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
FetchContent_Declare(vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.2.1
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(spdlog tracy mimalloc vma)

# --- zlib ---------------------------------------------------------------------------------------
# Only needed by libspng, whose CMake requires ZLIB unconditionally (its miniz option exists only
# in the meson build). OVERRIDE_FIND_PACKAGE makes libspng's find_package(ZLIB) resolve here
# instead of looking for a system install.

set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG        v1.3.1
    GIT_SHALLOW    TRUE
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(zlib)

# zlib does not export a namespaced target, which is what libspng links against.
if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()

# --- libspng ------------------------------------------------------------------------------------

set(SPNG_SHARED OFF CACHE BOOL "" FORCE)
set(SPNG_STATIC ON CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spng
    GIT_REPOSITORY https://github.com/randy408/libspng.git
    GIT_TAG        v0.7.4
    GIT_SHALLOW    TRUE
)

# libspng has no install toggle, and its install(EXPORT) refuses to run because the fetched
# zlibstatic is not in an export set. Nothing here is ever installed, so its rules are skipped.
set(CMAKE_SKIP_INSTALL_RULES ON)
FetchContent_MakeAvailable(spng)
set(CMAKE_SKIP_INSTALL_RULES OFF)

# --- Lua 5.4 ------------------------------------------------------------------------------------
# The upstream repository ships no CMake, so the library target is declared here from its own source
# list. `lua.c` and `luac.c` are the standalone interpreter and compiler and are deliberately left
# out. Compiled as C: error handling then uses longjmp, which is what Sol2's protected calls expect,
# and the engine never lets a longjmp cross a frame holding a C++ destructor (spec section 3).

FetchContent_Declare(lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG        v5.4.7
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(lua)

set(ENGINE_LUA_SOURCES
    lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldblib.c ldebug.c ldo.c ldump.c
    lfunc.c lgc.c linit.c liolib.c llex.c lmathlib.c lmem.c loadlib.c lobject.c lopcodes.c
    loslib.c lparser.c lstate.c lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c lutf8lib.c
    lvm.c lzio.c
)
list(TRANSFORM ENGINE_LUA_SOURCES PREPEND "${lua_SOURCE_DIR}/")

add_library(lua_static STATIC ${ENGINE_LUA_SOURCES})
target_include_directories(lua_static SYSTEM PUBLIC "${lua_SOURCE_DIR}")
set_target_properties(lua_static PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    # Not ours to fix, and Lua is warning-clean only under its own flags.
    target_compile_options(lua_static PRIVATE /W0)
endif()

# --- Sol2 ---------------------------------------------------------------------------------------
# Header-only. Every Sol2 include is confined to src/lua.cpp, so the compile cost is paid once and
# no Sol2 type reaches a header (spec section 8).

set(SOL2_BUILD_LUA OFF CACHE BOOL "" FORCE)
set(SOL2_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(sol2
    GIT_REPOSITORY https://github.com/ThePhD/sol2.git
    GIT_TAG        v3.3.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(sol2)

# --- Vulkan SDK ---------------------------------------------------------------------------------
# Looked up last so a missing SDK is the only thing a fresh configure can fail on, after the
# fetched dependencies are already in place.

find_package(Vulkan 1.4 REQUIRED COMPONENTS glslc)

# --- Aggregated usage requirements -------------------------------------------------------------

# VMA's single-header implementation does not compile clean under /W4, and it is not ours to fix.
# Marking the fetched include directories as SYSTEM keeps third-party warnings out of our build log
# without lowering the warning level on engine code.
foreach(dependency spdlog TracyClient mimalloc-static VulkanMemoryAllocator spng_static sol2)
    if(TARGET ${dependency})
        get_target_property(_dirs ${dependency} INTERFACE_INCLUDE_DIRECTORIES)
        if(_dirs)
            set_property(TARGET ${dependency} PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
                         "${_dirs}")
        endif()
    endif()
endforeach()

add_library(engine_third_party INTERFACE)
target_compile_definitions(engine_third_party INTERFACE
    # std::fopen and friends are used deliberately for binary file I/O; the _s variants are not
    # portable and the engine checks every return value.
    _CRT_SECURE_NO_WARNINGS
)
target_link_libraries(engine_third_party INTERFACE
    Vulkan::Vulkan
    GPUOpen::VulkanMemoryAllocator
    spng_static
    spdlog::spdlog
    mimalloc-static
    Tracy::TracyClient
    lua_static
    sol2::sol2
)
target_compile_definitions(engine_third_party INTERFACE
    $<$<BOOL:${ENGINE_TRACY_CALLSTACKS}>:ENGINE_TRACY_CALLSTACKS>
    $<$<BOOL:${ENGINE_VULKAN_VALIDATION}>:ENGINE_VULKAN_VALIDATION>
    $<$<BOOL:${ENGINE_VULKAN_VALIDATION_VERBOSE}>:ENGINE_VULKAN_VALIDATION_VERBOSE>
)

# VMA is header-only here; the implementation is compiled once in src/vulkan/allocator.cpp.
target_compile_definitions(engine_third_party INTERFACE
    VMA_STATIC_VULKAN_FUNCTIONS=1
    VMA_DYNAMIC_VULKAN_FUNCTIONS=0
)
