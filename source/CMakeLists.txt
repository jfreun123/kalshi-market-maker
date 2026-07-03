# ---- Library ----

add_library(kalshi_lib STATIC auth.cpp cli.cpp rate_limiter.cpp config.cpp http_transport.cpp paper_transport.cpp rest_client.cpp orderbook.cpp websocket_client.cpp order_manager.cpp risk_manager.cpp adverse_selection.cpp theo_grid.cpp pricing_model.cpp fair_value.cpp quoter.cpp logger.cpp ensure.cpp ticker_scanner.cpp scan_output.cpp portfolio.cpp trading_session.cpp capture.cpp flow_imbalance.cpp)

add_executable(kalshi_mm main.cpp)
target_link_libraries(kalshi_mm PRIVATE kalshi_lib)
target_compile_features(kalshi_mm PRIVATE cxx_std_23)
target_compile_options(kalshi_mm PRIVATE -Werror)
target_compile_options(kalshi_lib PRIVATE -Werror)

target_include_directories(
    kalshi_lib
    PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
    ${OPENSSL_INCLUDE_DIR}
)

target_compile_features(kalshi_lib PUBLIC cxx_std_23)

target_link_libraries(
    kalshi_lib
    PUBLIC
    OpenSSL::Crypto
    OpenSSL::SSL
    httplib::httplib
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    PRIVATE
    ixwebsocket
)
