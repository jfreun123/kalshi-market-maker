include(FetchContent)

set(USE_TLS ON CACHE BOOL "IXWebSocket TLS support" FORCE)
set(USE_OPEN_SSL ON CACHE BOOL "IXWebSocket OpenSSL TLS backend" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG        v11.4.5
)
FetchContent_MakeAvailable(ixwebsocket)
