# ---- Library ----

add_library(cpp-starter_lib STATIC auth.cpp)

target_include_directories(
    cpp-starter_lib
    PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)

target_compile_features(cpp-starter_lib PUBLIC cxx_std_23)

target_link_libraries(cpp-starter_lib PUBLIC OpenSSL::Crypto)
