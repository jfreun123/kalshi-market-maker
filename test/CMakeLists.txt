# ---- Tests ----

include(GoogleTest)

function(kalshi_add_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE kalshi_lib GTest::gtest_main)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source")
    target_compile_features(${target} PRIVATE cxx_std_23)
    set_target_properties(${target} PROPERTIES
        CXX_CLANG_TIDY "clang-tidy"
        CXX_CPPCHECK "cppcheck;--inline-suppr"
    )
    gtest_discover_tests(${target})
endfunction()

kalshi_add_test(types_test source/types_test.cpp)
kalshi_add_test(auth_test source/auth_test.cpp)
kalshi_add_test(rest_client_test source/rest_client_test.cpp)
kalshi_add_test(orderbook_test source/orderbook_test.cpp)
kalshi_add_test(websocket_client_test source/websocket_client_test.cpp)
kalshi_add_test(order_manager_test source/order_manager_test.cpp)
kalshi_add_test(risk_manager_test source/risk_manager_test.cpp)
kalshi_add_test(fair_value_test source/fair_value_test.cpp)
