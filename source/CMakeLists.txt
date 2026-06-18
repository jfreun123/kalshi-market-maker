# ---- Library ----

add_library(kalshi_lib STATIC auth.cpp config.cpp http_transport.cpp rest_client.cpp orderbook.cpp websocket_client.cpp order_manager.cpp risk_manager.cpp fair_value.cpp quoter.cpp)

add_executable(kalshi_mm main.cpp)
target_link_libraries(kalshi_mm PRIVATE kalshi_lib)
target_compile_features(kalshi_mm PRIVATE cxx_std_23)
set_target_properties(kalshi_mm PROPERTIES CXX_CLANG_TIDY "clang-tidy")

target_include_directories(
    kalshi_lib
    PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)

target_compile_features(kalshi_lib PUBLIC cxx_std_23)

target_link_libraries(
    kalshi_lib
    PUBLIC
    OpenSSL::Crypto
    OpenSSL::SSL
    httplib::httplib
    nlohmann_json::nlohmann_json
)
