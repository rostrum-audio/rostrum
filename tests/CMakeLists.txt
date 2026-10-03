find_package(Qt6 REQUIRED COMPONENTS Test)

function(rostrum_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE rostrum_core Qt6::Test)
    target_compile_definitions(${name} PRIVATE ROSTRUM_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

rostrum_add_test(tst_paths)
rostrum_add_test(tst_requirements)
