# ---- Benchmark ----

add_executable(kalshi_bench source/benchmark.cpp)
target_link_libraries(kalshi_bench PRIVATE kalshi_lib benchmark::benchmark)
target_compile_features(kalshi_bench PRIVATE cxx_std_23)

target_compile_options(kalshi_bench PRIVATE -Werror)
