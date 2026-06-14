# ---- Fetch cpp-httplib ----
# Used by HttpTransport (production HTTP client). Unit tests use FakeTransport.

include(FetchContent)

FetchContent_Declare(
  cpp-httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.16.3
  GIT_SHALLOW TRUE
)

set(HTTPLIB_COMPILE ON CACHE BOOL "Build httplib as a compiled library" FORCE)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE ON CACHE BOOL "Use OpenSSL for HTTPS" FORCE)
set(HTTPLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(HTTPLIB_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(cpp-httplib)
