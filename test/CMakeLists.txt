# ---- Tests ----

include(GoogleTest)

function(kalshi_add_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE kalshi_lib GTest::gtest_main)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source")
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_compile_options(${target} PRIVATE -Werror)
    set_target_properties(${target} PROPERTIES
        CXX_CPPCHECK "cppcheck;--inline-suppr;--error-exitcode=1;--suppress=*:${CMAKE_BINARY_DIR}/_deps/*"
    )
    gtest_discover_tests(${target})
endfunction()

kalshi_add_test(types_test source/types_test.cpp)
kalshi_add_test(cli_test source/cli_test.cpp)
kalshi_add_test(quantity_test source/quantity_test.cpp)
kalshi_add_test(ensure_test source/ensure_test.cpp)
kalshi_add_test(auth_test source/auth_test.cpp)
kalshi_add_test(rate_limiter_test source/rate_limiter_test.cpp)
kalshi_add_test(analytics_test source/analytics_test.cpp)
kalshi_add_test(app_modes_test source/app_modes_test.cpp)
kalshi_add_test(rest_client_test source/rest_client_test.cpp)
kalshi_add_test(orderbook_test source/orderbook_test.cpp)
kalshi_add_test(websocket_client_test source/websocket_client_test.cpp)
kalshi_add_test(order_manager_test source/order_manager_test.cpp)
kalshi_add_test(risk_manager_test source/risk_manager_test.cpp)
kalshi_add_test(pricing_model_test source/pricing_model_test.cpp)
kalshi_add_test(fair_value_test source/fair_value_test.cpp)
kalshi_add_test(quoter_test source/quoter_test.cpp)
kalshi_add_test(quoter_reprice_test source/quoter_reprice_test.cpp)
kalshi_add_test(main_loop_test source/main_loop_test.cpp)
kalshi_add_test(config_test source/config_test.cpp)
kalshi_add_test(paper_transport_test source/paper_transport_test.cpp)
kalshi_add_test(theo_grid_test source/theo_grid_test.cpp)
kalshi_add_test(logger_test source/logger_test.cpp)
kalshi_add_test(ticker_scanner_test source/ticker_scanner_test.cpp)
kalshi_add_test(scan_output_test source/scan_output_test.cpp)
kalshi_add_test(portfolio_test source/portfolio_test.cpp)
kalshi_add_test(trading_session_test source/trading_session_test.cpp)
kalshi_add_test(capture_test source/capture_test.cpp)
kalshi_add_test(flow_imbalance_test source/flow_imbalance_test.cpp)
kalshi_add_test(view_based_model_test source/view_based_model_test.cpp)

# ---- Replay tests ----

add_executable(replay_test replay/replay_test.cpp)
target_link_libraries(replay_test PRIVATE kalshi_lib GTest::gtest_main OpenSSL::Crypto)
target_include_directories(replay_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/source"
    "${PROJECT_SOURCE_DIR}/source"
)
target_compile_features(replay_test PRIVATE cxx_std_23)
target_compile_options(replay_test PRIVATE -Werror)
target_compile_definitions(replay_test PRIVATE
    KALSHI_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
gtest_discover_tests(replay_test)

# ---- Integration tests ----
#
# Replay full captured/synthetic exchange sessions through the production
# TradingSession stack. Hermetic (checked-in fixtures, no network), so on by
# default; networked capture-validation tests would live behind a separate
# off-by-default option.
option(KALSHI_INTEGRATION_TESTS "Build full-stack replay integration tests" ON)
if(KALSHI_INTEGRATION_TESTS)
    add_executable(replay_session_test integration/replay_session_test.cpp)
    target_link_libraries(replay_session_test PRIVATE kalshi_lib GTest::gtest_main OpenSSL::Crypto)
    target_include_directories(replay_session_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/source"
        "${PROJECT_SOURCE_DIR}/source"
    )
    target_compile_features(replay_session_test PRIVATE cxx_std_23)
    target_compile_options(replay_session_test PRIVATE -Werror)
    target_compile_definitions(replay_session_test PRIVATE
        KALSHI_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    )
    gtest_discover_tests(replay_session_test)
endif()

# Conformance smoke tests against the REAL Kalshi demo environment: send each
# request / subscribe each channel and assert the response parses into our
# types. Needs demo credentials + network, so OFF by default. Each test skips
# cleanly if KALSHI_DEMO_CONFIG (or repo config-demo.json) is unavailable or not
# a demo endpoint, so enabling the option never breaks a credential-less build.
option(KALSHI_DEMO_TESTS "Build demo-environment conformance smoke tests" OFF)
if(KALSHI_DEMO_TESTS)
    add_executable(demo_conformance_test integration/demo_conformance_test.cpp)
    target_link_libraries(demo_conformance_test PRIVATE kalshi_lib GTest::gtest_main OpenSSL::Crypto)
    target_include_directories(demo_conformance_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/source"
        "${PROJECT_SOURCE_DIR}/source"
    )
    target_compile_features(demo_conformance_test PRIVATE cxx_std_23)
    target_compile_options(demo_conformance_test PRIVATE -Werror)
    target_compile_definitions(demo_conformance_test PRIVATE
        KALSHI_DEMO_CONFIG_DEFAULT="${PROJECT_SOURCE_DIR}/config-demo.json"
    )
    gtest_discover_tests(demo_conformance_test)
endif()

# ---- Fuzz targets (KALSHI_FUZZ_TESTS=ON, fuzz preset) ----

option(KALSHI_FUZZ_TESTS "Build libFuzzer fuzz targets (requires Clang -fsanitize=fuzzer)" OFF)
add_subdirectory(fuzz)
