cmake_minimum_required(VERSION 3.25)

project(terrain_engine VERSION 0.3.0 LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Everything lands next to the executables, so shaders and the pipeline cache resolve against
# the executable directory at runtime.
foreach(output RUNTIME LIBRARY ARCHIVE)
    set(CMAKE_${output}_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    foreach(config Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER ${config} config_upper)
        set(CMAKE_${output}_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/bin")
    endforeach()
endforeach()

# --- Options ---------------------------------------------------------------------------------

option(ENGINE_BUILD_SHARED "Link the engine objects as a shared library" ON)
option(ENGINE_TRACY "Enable the Tracy profiler client" ON)
option(ENGINE_TRACY_CALLSTACKS "Capture callstacks on allocations (expensive, Debug only)" OFF)
option(ENGINE_BUILD_TESTS "Build the ctest suite" ON)

set(_validation_default OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_validation_default ON)
endif()
option(ENGINE_VULKAN_VALIDATION "Enable Vulkan validation layers and the debug messenger"
       ${_validation_default})
option(ENGINE_VULKAN_VALIDATION_VERBOSE "Include Vulkan info and verbose messages" OFF)

include(engine_dependencies)
include(engine_shaders)

function(engine_set_warnings target)
    if(MSVC)
        # 4251/4275: exporting classes with standard-library members is intentional here; the
        # engine and its consumers are always built with the same toolchain and runtime.
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:preprocessor
                                                 /wd4251 /wd4275)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion
                                                 -Wno-unused-parameter)
    endif()
endfunction()

engine_collect_shaders(ENGINE_SHADER_OUTPUTS)
add_custom_target(engine_shaders ALL DEPENDS ${ENGINE_SHADER_OUTPUTS})

# --- Engine objects -------------------------------------------------------------------------

add_library(engine_objects OBJECT
    src/application.cpp
    src/engine.cpp
    src/log.cpp
    src/memory/arena.cpp
    src/memory/general.cpp
    src/memory/new_delete.cpp
    src/memory/pool.cpp
    src/platform/platform.cpp
    src/terrain/mapping.cpp
    src/vulkan/allocator.cpp
    src/vulkan/buffer_pool.cpp
    src/vulkan/context.cpp
    src/vulkan/device.cpp
    src/vulkan/pipeline.cpp
)

# The engine objects become either a DLL or a static library; either way only symbols marked
# with the generated export macro cross the boundary.
if(ENGINE_BUILD_SHARED)
    add_library(engine SHARED $<TARGET_OBJECTS:engine_objects>)
else()
    add_library(engine STATIC $<TARGET_OBJECTS:engine_objects>)
endif()

include(GenerateExportHeader)
generate_export_header(engine
    BASE_NAME ENGINE
    EXPORT_MACRO_NAME ENGINE_API
    EXPORT_FILE_NAME "${CMAKE_BINARY_DIR}/generated/include/engine/api.h"
    STATIC_DEFINE ENGINE_STATIC_DEFINE
)

# Usage requirements shared by the objects, the library and the white-box tests.
add_library(engine_interface INTERFACE)
target_include_directories(engine_interface INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_BINARY_DIR}/generated/include"
)
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE ENGINE_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT ENGINE_COMMIT_HASH)
    set(ENGINE_COMMIT_HASH "unknown")
endif()

target_compile_definitions(engine_interface INTERFACE
    ENGINE_VERSION_STRING="${PROJECT_VERSION}"
    ENGINE_COMMIT_HASH="${ENGINE_COMMIT_HASH}"
    # TRACE in Debug, INFO in Release: stripped at compile time by the logging macros.
    ENGINE_LOG_LEVEL=$<IF:$<CONFIG:Debug>,0,2>
    $<$<CONFIG:Debug>:ENGINE_DEBUG>
    $<$<NOT:$<BOOL:${ENGINE_BUILD_SHARED}>>:ENGINE_STATIC_DEFINE>
)
target_link_libraries(engine_interface INTERFACE engine_third_party)

# Internal declarations are visible only while compiling the engine itself.
target_compile_definitions(engine_objects PRIVATE IS_ENGINE engine_EXPORTS)
target_link_libraries(engine_objects PUBLIC engine_interface)

target_link_libraries(engine PUBLIC engine_interface)
add_dependencies(engine engine_shaders)

engine_set_warnings(engine_objects)

# --- Executables ------------------------------------------------------------------------------

# terrain_export runs in Headless mode and must work on a machine with no display.
add_executable(terrain_export
    src/terrain/export_main.cpp
    src/memory/new_delete.cpp   # Each module routes its own operator new (spec section 7.2).
)
target_link_libraries(terrain_export PRIVATE engine)
engine_set_warnings(terrain_export)

# --- Tests ------------------------------------------------------------------------------------

if(ENGINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# --- Summary ----------------------------------------------------------------------------------

message(STATUS "terrain_engine ${PROJECT_VERSION} (${CMAKE_BUILD_TYPE})")
message(STATUS "  shared engine ....... ${ENGINE_BUILD_SHARED}")
message(STATUS "  Tracy ............... ${ENGINE_TRACY} (callstacks: ${ENGINE_TRACY_CALLSTACKS})")
message(STATUS "  Vulkan validation ... ${ENGINE_VULKAN_VALIDATION} (verbose: ${ENGINE_VULKAN_VALIDATION_VERBOSE})")
message(STATUS "  tests ............... ${ENGINE_BUILD_TESTS}")
