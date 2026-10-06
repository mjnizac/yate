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
    GIT_TAG        v0.11.1
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

# --- Vulkan SDK ---------------------------------------------------------------------------------
# Looked up last so a missing SDK is the only thing a fresh configure can fail on, after the
# fetched dependencies are already in place.

find_package(Vulkan 1.4 REQUIRED COMPONENTS glslc)

# --- Aggregated usage requirements -------------------------------------------------------------

# VMA's single-header implementation does not compile clean under /W4, and it is not ours to fix.
# Marking the fetched include directories as SYSTEM keeps third-party warnings out of our build log
# without lowering the warning level on engine code.
foreach(dependency spdlog tracy mimalloc vma)
    if(TARGET ${dependency})
        get_target_property(_dirs ${dependency} INTERFACE_INCLUDE_DIRECTORIES)
        if(_dirs)
            target_include_directories(${dependency} SYSTEM INTERFACE ${_dirs})
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
    spdlog::spdlog
    mimalloc-static
    Tracy::TracyClient
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
