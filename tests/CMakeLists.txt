# Unit tests are white-box: they link the engine objects directly and define IS_ENGINE, so they
# can exercise internal allocators, the Vulkan context and the section pool.
#
# No test framework is pulled in: tests/unit/test_support.hpp is enough, and the dependency list
# stays exactly what the spec pins.

#
# Unlike the executables, a test must NOT add src/memory/new_delete.cpp to its own sources: the
# engine objects it links already carry that translation unit, and a second copy makes every
# operator new a duplicate symbol. The test and the engine objects are one module here.

function(engine_add_unit_test name)
    add_executable(${name} unit/${name}.cpp)
    target_compile_definitions(${name} PRIVATE IS_ENGINE engine_EXPORTS)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/unit")
    target_link_libraries(${name} PRIVATE engine_objects)
    engine_set_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

engine_add_unit_test(test_allocators)
engine_add_unit_test(test_mapping)
engine_add_unit_test(test_errors)

# Needs a Vulkan 1.4 device and the compiled shaders. Acceptance check for milestone 3.
engine_add_unit_test(test_compute_roundtrip)
engine_add_unit_test(test_noise)
add_dependencies(test_compute_roundtrip engine_shaders)
add_dependencies(test_noise engine_shaders)
