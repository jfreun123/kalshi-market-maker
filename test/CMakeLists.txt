# ---- Tests ----

include(GoogleTest)

function(kalshi_add_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE cpp-starter_lib GTest::gtest_main)
    target_compile_features(${target} PRIVATE cxx_std_23)
    set_target_properties(${target} PROPERTIES
        CXX_CLANG_TIDY "clang-tidy"
        CXX_CPPCHECK "cppcheck;--inline-suppr"
    )
    gtest_discover_tests(${target})
endfunction()

kalshi_add_test(types_test source/types_test.cpp)
kalshi_add_test(auth_test source/auth_test.cpp)
