# ---- Library ----

add_library(kalshi_lib STATIC auth.cpp)

target_include_directories(
    kalshi_lib
    PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)

target_compile_features(kalshi_lib PUBLIC cxx_std_23)

target_link_libraries(kalshi_lib PUBLIC OpenSSL::Crypto)
