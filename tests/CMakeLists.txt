include(FetchContent)

FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

set(SRC_INCLUDES
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/src/config
        ${PROJECT_SOURCE_DIR}/src/dll
        ${PROJECT_SOURCE_DIR}/src/net
        ${PROJECT_SOURCE_DIR}/src/log
        ${PROJECT_SOURCE_DIR}/src/init
)

# --- test_protocol ---
add_executable(test_protocol test_protocol.cpp)
target_include_directories(test_protocol PRIVATE ${SRC_INCLUDES})
target_link_libraries(test_protocol PRIVATE gtest gtest_main)
add_test(NAME test_protocol COMMAND test_protocol)

# --- test_tcp_server ---
add_executable(test_tcp_server
        test_tcp_server.cpp
        ${PROJECT_SOURCE_DIR}/src/net/tcp_server.cpp
        ${PROJECT_SOURCE_DIR}/src/net/tcp_server_io.cpp
        ${PROJECT_SOURCE_DIR}/src/net/tcp_server_broadcast.cpp
        ${PROJECT_SOURCE_DIR}/src/net/ring_buffer.cpp
        ${PROJECT_SOURCE_DIR}/src/log/logger.cpp
)
target_include_directories(test_tcp_server PRIVATE ${SRC_INCLUDES})
target_link_libraries(test_tcp_server PRIVATE gtest gtest_main ws2_32 winmm)
add_test(NAME test_tcp_server COMMAND test_tcp_server)

# --- test_logger ---
add_executable(test_logger
        test_logger.cpp
        ${PROJECT_SOURCE_DIR}/src/log/logger.cpp
)
target_include_directories(test_logger PRIVATE ${SRC_INCLUDES})
target_link_libraries(test_logger PRIVATE gtest gtest_main)
add_test(NAME test_logger COMMAND test_logger)

# --- test_config ---
add_executable(test_config
        test_config.cpp
        ${PROJECT_SOURCE_DIR}/src/config/config.cpp
)
target_include_directories(test_config PRIVATE ${SRC_INCLUDES})
target_link_libraries(test_config PRIVATE gtest gtest_main)
add_test(NAME test_config COMMAND test_config)

# --- test_ring_buffer ---
add_executable(test_ring_buffer
        test_ring_buffer.cpp
        ${PROJECT_SOURCE_DIR}/src/net/ring_buffer.cpp
)
target_include_directories(test_ring_buffer PRIVATE ${SRC_INCLUDES})
target_link_libraries(test_ring_buffer PRIVATE gtest gtest_main)
add_test(NAME test_ring_buffer COMMAND test_ring_buffer)