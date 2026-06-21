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
kalshi_add_test(pricing_model_test source/pricing_model_test.cpp)
kalshi_add_test(fair_value_test source/fair_value_test.cpp)
kalshi_add_test(quoter_test source/quoter_test.cpp)
kalshi_add_test(main_loop_test source/main_loop_test.cpp)
kalshi_add_test(config_test source/config_test.cpp)
kalshi_add_test(paper_transport_test source/paper_transport_test.cpp)
kalshi_add_test(adverse_selection_test source/adverse_selection_test.cpp)
kalshi_add_test(theo_grid_test source/theo_grid_test.cpp)
kalshi_add_test(logger_test source/logger_test.cpp)

# ---- Replay tests ----

add_executable(replay_test replay/replay_test.cpp)
target_link_libraries(replay_test PRIVATE kalshi_lib GTest::gtest_main OpenSSL::Crypto)
target_include_directories(replay_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/source"
    "${PROJECT_SOURCE_DIR}/source"
)
target_compile_features(replay_test PRIVATE cxx_std_23)
target_compile_definitions(replay_test PRIVATE
    KALSHI_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
set_target_properties(replay_test PROPERTIES CXX_CLANG_TIDY "clang-tidy")
gtest_discover_tests(replay_test)

# ---- Fuzz targets (KALSHI_FUZZ_TESTS=ON, fuzz preset) ----

option(KALSHI_FUZZ_TESTS "Build libFuzzer fuzz targets (requires Clang -fsanitize=fuzzer)" OFF)
add_subdirectory(fuzz)
