cmake_minimum_required(VERSION 3.14)

# ---- In-source guard ----

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not allowed. Please create a build directory.")
endif()

project(
    kalshi-market-maker
    VERSION 0.1.0
    DESCRIPTION "Kalshi prediction market maker"
    LANGUAGES CXX
)

# ---- Developer mode ----

option(kalshi_market_maker_DEVELOPER_MODE "Enable developer mode" ON)

# ---- Dependencies ----

find_package(OpenSSL REQUIRED)

include(cmake/json.cmake)
include(cmake/httplib.cmake)
include(cmake/ixwebsocket.cmake)

# ---- Build source ----

add_subdirectory(source)

# ---- Developer mode targets ----

if(kalshi_market_maker_DEVELOPER_MODE)
  include(cmake/dev-mode.cmake)
endif()
