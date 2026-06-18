# Fuzz targets — only built when KALSHI_FUZZ_TESTS=ON.
# Requires Clang and the fuzz CMake preset (-fsanitize=fuzzer,address).
#
# Usage:
#   cmake --preset=fuzz
#   cmake --build --preset=fuzz
#   ./build-fuzz/test/fuzz/parse_ws_message_fuzz test/fuzz/corpus/ \
#       -max_total_time=30

if(NOT KALSHI_FUZZ_TESTS)
    return()
endif()

function(kalshi_add_fuzz_target target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE kalshi_lib OpenSSL::Crypto)
    target_include_directories(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}/test/source"
        "${CMAKE_SOURCE_DIR}/source"
    )
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_compile_options(${target} PRIVATE -fsanitize=fuzzer,address)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer,address)
endfunction()

kalshi_add_fuzz_target(parse_ws_message_fuzz parse_ws_message_fuzz.cpp)
