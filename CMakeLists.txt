cmake_minimum_required(VERSION 3.16)
project(tcbridge VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static linking for portability
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")

add_executable(tcbridge
        src/main.cpp
        src/init/initialization.cpp
        src/config/config.cpp
        src/dll/dll_wrapper.cpp
        src/net/tcp_server.cpp
        src/net/tcp_server_io.cpp
        src/net/tcp_server_broadcast.cpp
        src/net/ring_buffer.cpp
        src/log/logger.cpp
)

target_include_directories(tcbridge PRIVATE
        src
        src/config
        src/dll
        src/net
        src/log
        src/init
)
target_compile_definitions(tcbridge PRIVATE _CRT_SECURE_NO_WARNINGS)

# Link Winsock2 and Windows threading
target_link_libraries(tcbridge PRIVATE ws2_32 winmm)

# Tests (cross-compiled, run under Wine)
option(BUILD_TESTS "Build tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
