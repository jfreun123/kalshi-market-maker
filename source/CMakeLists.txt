# ---- Layered libraries (dependency DAG enforced at link time) ----

function(kalshi_add_layer target)
    add_library(${target} STATIC ${ARGN})
    target_compile_features(${target} PUBLIC cxx_std_23)
    target_compile_options(${target} PRIVATE -Werror)
    target_include_directories(
        ${target}
        PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
    )
endfunction()

kalshi_add_layer(kalshi_core
    core/logger.cpp core/ensure.cpp core/cli.cpp core/rate_limiter.cpp)
target_link_libraries(kalshi_core PUBLIC spdlog::spdlog)

kalshi_add_layer(kalshi_net
    net/auth.cpp net/http_transport.cpp net/paper_transport.cpp
    net/websocket_client.cpp net/capture.cpp)
target_include_directories(kalshi_net PUBLIC ${OPENSSL_INCLUDE_DIR})
target_link_libraries(
    kalshi_net
    PUBLIC
    kalshi_core
    OpenSSL::Crypto
    OpenSSL::SSL
    httplib::httplib
    nlohmann_json::nlohmann_json
    PRIVATE
    ixwebsocket
)

kalshi_add_layer(kalshi_engine
    engine/orderbook.cpp engine/trade_tape.cpp engine/flow_imbalance.cpp
    engine/pricing_model.cpp engine/fair_value.cpp engine/theo_grid.cpp
    engine/analytics.cpp engine/portfolio.cpp engine/risk_manager.cpp
    engine/trading_session.cpp engine/event_pump.cpp
    engine/drift_estimator.cpp)
target_link_libraries(kalshi_engine PUBLIC kalshi_core
                                            nlohmann_json::nlohmann_json)

kalshi_add_layer(kalshi_exchange
    exchange/rest_client.cpp exchange/order_manager.cpp
    exchange/ticker_scanner.cpp exchange/scan_output.cpp)
target_link_libraries(kalshi_exchange PUBLIC kalshi_core kalshi_net
                                              kalshi_engine)

kalshi_add_layer(kalshi_strategy strategy/quoter.cpp)
target_link_libraries(kalshi_strategy PUBLIC kalshi_core kalshi_engine)

kalshi_add_layer(kalshi_tooling tooling/fv_backtest.cpp)
target_link_libraries(kalshi_tooling PUBLIC kalshi_engine)

kalshi_add_layer(kalshi_app app/config.cpp app/app_modes.cpp)
target_link_libraries(kalshi_app PUBLIC kalshi_core kalshi_net kalshi_engine
                                         kalshi_exchange kalshi_strategy
                                         kalshi_tooling)

# ---- Aggregator (tests, replay, fuzz, and benchmarks link this) ----

add_library(kalshi_lib INTERFACE)
target_link_libraries(kalshi_lib INTERFACE kalshi_app)

# ---- Executable ----

add_executable(kalshi_mm app/main.cpp)
target_link_libraries(kalshi_mm PRIVATE kalshi_lib)
target_compile_features(kalshi_mm PRIVATE cxx_std_23)
target_compile_options(kalshi_mm PRIVATE -Werror)
