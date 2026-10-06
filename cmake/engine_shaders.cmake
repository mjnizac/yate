# Build-time shader compilation. There is no runtime shader compilation in this version
# (spec section 4): every shader under assets/shaders/ becomes a `.spv` next to the executables.
#
# Naming convention, and why it matters to this file:
#   ops/<name>.comp.glsl   a translation unit: has main(), declares local_size, becomes one .spv
#   lib/<name>.lib.glsl    an include fragment: no main(), no stage, never compiled on its own
#
# Because the sources no longer end in a stage extension, glslc cannot infer the stage and is told
# explicitly with -fshader-stage. The upside is that the target glob is explicit per stage, so a
# library file can never be mistaken for something compilable, and an op can never be skipped in
# silence for having the wrong extension.

set(ENGINE_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/shaders")

# Maps the stage component of a file name to the glslc stage name.
function(engine_shader_stage file out_var)
    get_filename_component(without_glsl "${file}" NAME_WE)
    get_filename_component(stem "${file}" NAME)
    # `arith.comp.glsl` -> `comp`
    string(REGEX REPLACE "^.*\\.([^.]+)\\.glsl$" "\\1" stage "${stem}")
    if(stage STREQUAL "comp")
        set(${out_var} "compute" PARENT_SCOPE)
    elseif(stage STREQUAL "vert")
        set(${out_var} "vertex" PARENT_SCOPE)
    elseif(stage STREQUAL "frag")
        set(${out_var} "fragment" PARENT_SCOPE)
    else()
        message(FATAL_ERROR "shader ${file} has no recognised stage component")
    endif()
    set(engine_unused "${without_glsl}")
endfunction()

# Collects every shader source and emits one glslc command per file.
function(engine_collect_shaders out_var)
    file(GLOB_RECURSE sources
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/*.comp.glsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/*.vert.glsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/*.frag.glsl"
    )
    file(GLOB_RECURSE includes "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/lib/*.lib.glsl")

    set(outputs "")
    foreach(source ${sources})
        file(RELATIVE_PATH relative "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders" "${source}")
        engine_shader_stage("${source}" stage)
        # `ops/arith.comp.glsl` -> `ops/arith.comp.spv`, so the paths the op registry holds in C++
        # are unaffected by the source naming.
        string(REGEX REPLACE "\\.glsl$" ".spv" output_relative "${relative}")
        set(output "${ENGINE_SHADER_OUTPUT_DIR}/${output_relative}")
        get_filename_component(output_dir "${output}" DIRECTORY)
        add_custom_command(
            OUTPUT "${output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${output_dir}"
            COMMAND Vulkan::glslc
                    -fshader-stage=${stage}
                    --target-env=vulkan1.4
                    -I "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders"
                    $<IF:$<CONFIG:Debug>,-g,-O>
                    -Werror
                    -o "${output}" "${source}"
            DEPENDS "${source}" ${includes}
            COMMENT "glslc ${relative}"
            VERBATIM
        )
        list(APPEND outputs "${output}")
    endforeach()
    set(${out_var} "${outputs}" PARENT_SCOPE)
endfunction()
