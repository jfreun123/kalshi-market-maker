# ---- Tests ----

include(CTest)
if(BUILD_TESTING)
  include(cmake/gtest.cmake)
  add_subdirectory(test)
endif()

# ---- Benchmark ----

include(cmake/benchmark.cmake)
add_subdirectory(benchmark)

# ---- Linting (clang-format) ----

include(cmake/lint-targets.cmake)

# ---- Documentation (Doxygen) ----

include(cmake/docs.cmake)
