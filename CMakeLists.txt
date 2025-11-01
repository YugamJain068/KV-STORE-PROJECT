cmake_minimum_required(VERSION 3.10)
project(DistributedKVStore)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ------------------- Main Executable -------------------
add_executable(kvstore
    src/main.cpp
    src/kvstore.cpp
    src/raft_node.cpp
    src/wal.cpp
    src/server.cpp
    src/rpc_server.cpp
    src/persist_functions.cpp
    src/snapshot.cpp
    src/decode_encodebase64.cpp
)

# Force console subsystem for MinGW (Windows)
set_target_properties(kvstore PROPERTIES WIN32_EXECUTABLE OFF)

# # ------------------- Include Directories -------------------
# include_directories(include)
# include_directories(${gtest_SOURCE_DIR}/include ${gtest_SOURCE_DIR})

# # ------------------- Enable Testing -------------------
# enable_testing()
# add_subdirectory(googletest)



# # ------------------- Test Executable -------------------
# add_executable(runTests
#     tests/kvstore_tests.cpp
#     tests/concurrency_tests.cpp
#     tests/wal_recovery_test.cpp
#     tests/tcp_server_tests.cpp
#     tests/raft_tests.cpp
#     src/kvstore.cpp
#     src/raft_node.cpp
#     src/wal.cpp
#     src/server.cpp
#     src/rpc_server.cpp
#     src/persist_functions.cpp
# )

# target_compile_definitions(runTests PRIVATE TESTING)


# # Link GoogleTest and pthread
# target_link_libraries(runTests gtest gtest_main pthread)

# # Register test with CTest
# add_test(NAME KVStoreTests COMMAND runTests)
