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

# --- zlib-ng ------------------------------------------------------------------------------------
# Only needed by libspng, whose CMake requires ZLIB unconditionally (its miniz option exists only in the
# meson build). OVERRIDE_FIND_PACKAGE makes libspng's find_package(ZLIB) resolve here instead of looking
# for a system install.
#
# zlib-ng rather than zlib, in `ZLIB_COMPAT` mode so it provides zlib's API and libspng neither knows nor
# cares. The reason is measured: deflate is the single largest cost in an export. On a 2048x2048 export
# with erosion, the GPU is 5.7% of the wall time and the main loop spends 66% of it blocked waiting for a
# PNG encoder thread, so the compressor *is* the critical path and nothing else is close.

set(ZLIB_COMPAT ON CACHE BOOL "" FORCE)
set(ZLIB_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ZLIBNG_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zlib
    GIT_REPOSITORY https://github.com/zlib-ng/zlib-ng.git
    GIT_TAG        2.2.2
    GIT_SHALLOW    TRUE
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(zlib)

# Neither zlib nor zlib-ng exports the namespaced target libspng links against. zlib-ng in compat mode
# also provides `zlibstatic` as an alias of `zlib`, and CMake refuses an alias of an alias, so the real
# target has to be resolved rather than guessed by name.
if(NOT TARGET ZLIB::ZLIB)
    set(ENGINE_ZLIB_TARGET "")
    foreach(candidate zlib zlibstatic)
        if(TARGET ${candidate})
            get_target_property(_aliased ${candidate} ALIASED_TARGET)
            if(_aliased)
                set(ENGINE_ZLIB_TARGET "${_aliased}")
            else()
                set(ENGINE_ZLIB_TARGET "${candidate}")
            endif()
            break()
        endif()
    endforeach()
    if(NOT ENGINE_ZLIB_TARGET)
        message(FATAL_ERROR "the fetched zlib exports no usable target")
    endif()
    add_library(ZLIB::ZLIB ALIAS ${ENGINE_ZLIB_TARGET})
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

# --- GLFW ---------------------------------------------------------------------------------------
# Viewer only, and never initialized in Headless mode. Built without its examples, tests or docs; the
# Vulkan loader comes from the SDK, so GLFW does not need to find one itself.

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLFW_VULKAN_STATIC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glfw)

# --- Dear ImGui ---------------------------------------------------------------------------------
# The viewer's parameter panel and error panel. Not in the spec's dependency table, because the spec
# asks for "auto-generated UI for user parameters" without naming a library, so the choice was open.
#
# The alternative considered was drawing the panels with the engine's own pipeline and an embedded
# bitmap font, which would have added no dependency. It was rejected on the amount of work that buys
# nothing: a legible font is 95 hand-authored glyphs, and sliders and text entry would all be written
# from scratch, while ImGui ships its own atlas and its own Vulkan backend that manages its descriptor
# sets internally -- so it needs no image upload or sampler support from the engine, which has none.
#
# Its allocations are routed through `CPU/General` with `ImGui::SetAllocatorFunctions`, so it does not
# become another untracked pool.
#
# No CMakeLists upstream, so the target is declared here from the core sources plus the two backends.

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_static STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp"
)
target_include_directories(imgui_static SYSTEM PUBLIC
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)
target_link_libraries(imgui_static PUBLIC glfw Vulkan::Vulkan)
set_target_properties(imgui_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Deliberately *not* defining IMGUI_IMPL_VULKAN_NO_PROTOTYPES: the backend tests whether that symbol is
# defined, not what it is defined to, so setting it to 0 still switches it to the load-your-own-functions
# path and it then asserts that nobody loaded them. The engine links the SDK loader and has prototypes.
if(MSVC)
    # Not ours to fix.
    target_compile_options(imgui_static PRIVATE /W0)
endif()

# --- Vulkan SDK ---------------------------------------------------------------------------------
# Looked up last so a missing SDK is the only thing a fresh configure can fail on, after the
# fetched dependencies are already in place.

find_package(Vulkan 1.4 REQUIRED COMPONENTS glslc)

# --- Aggregated usage requirements -------------------------------------------------------------

# VMA's single-header implementation does not compile clean under /W4, and it is not ours to fix.
# Marking the fetched include directories as SYSTEM keeps third-party warnings out of our build log
# without lowering the warning level on engine code.
foreach(dependency spdlog TracyClient mimalloc-static VulkanMemoryAllocator spng_static sol2 glfw
                   imgui_static)
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
    glfw
    imgui_static
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
