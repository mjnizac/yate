# Build-time shader compilation. There is no runtime shader compilation in this version
# (spec section 4): every `.comp` under assets/shaders/ becomes a `.spv` next to the executables.

set(ENGINE_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/shaders")

# Collects every shader source and emits one glslc command per file.
# Shared GLSL in assets/shaders/lib/ is reachable with `#include "lib/..."`.
function(engine_collect_shaders out_var)
    file(GLOB_RECURSE sources
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/*.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/*.vert"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/*.frag"
    )
    file(GLOB_RECURSE includes "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/lib/*.glsl")

    set(outputs "")
    foreach(source ${sources})
        file(RELATIVE_PATH relative "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders" "${source}")
        set(output "${ENGINE_SHADER_OUTPUT_DIR}/${relative}.spv")
        get_filename_component(output_dir "${output}" DIRECTORY)
        add_custom_command(
            OUTPUT "${output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${output_dir}"
            COMMAND Vulkan::glslc
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
