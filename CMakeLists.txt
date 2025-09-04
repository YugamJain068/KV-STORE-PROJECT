cmake_minimum_required(VERSION 3.10)
project(DistributedKVStore)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Main executable
add_executable(kvstore
    src/main.cpp
    src/kvstore.cpp
    src/raft_node.cpp
    src/wal.cpp
    src/server.cpp
)

# Force console subsystem for MinGW
set_target_properties(kvstore PROPERTIES
    WIN32_EXECUTABLE OFF
)

# Enable testing
enable_testing()
add_subdirectory(googletest)

# Include directories
include_directories(include)
include_directories(${gtest_SOURCE_DIR}/include ${gtest_SOURCE_DIR})

# Test executable
add_executable(runTests
    tests/kvstore_tests.cpp
    tests/concurrency_tests.cpp
    src/kvstore.cpp
    src/wal.cpp
    tests/wal_recovery_test.cpp
    tests/tcp_server_tests.cpp
    src/server.cpp
)

# Link with GoogleTest (pthread only if needed)
target_link_libraries(runTests gtest gtest_main)

add_test(NAME KVStoreTests COMMAND runTests)
