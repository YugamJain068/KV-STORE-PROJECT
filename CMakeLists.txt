cmake_minimum_required(VERSION 3.10)
project(DistributedKVStore)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(kvstore
    src/main.cpp
    src/kvstore.cpp
    src/raft_node.cpp
    src/wal.cpp
)

# Force console subsystem (important for MinGW)
set_target_properties(kvstore PROPERTIES
    WIN32_EXECUTABLE OFF
)
