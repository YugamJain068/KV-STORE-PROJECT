# KVStore – Thread-Safe TCP Key-Value Store

## Overview
KVStore is a **thread-safe, TCP-based key-value store** written in C++.  
It supports basic CRUD operations (`PUT`, `GET`, `DELETE`) with **Write-Ahead Logging (WAL)** for durability and **multi-threaded client handling** for concurrency.

---

## Features
- **TCP Server**: Handles multiple clients via threads.
- **Thread Safety**: Uses `std::mutex` to protect shared state.
- **WAL Persistence**: Ensures data recovery after crashes or restarts.
- **Basic Commands**:
  - `PUT <key> <value>` – Insert or update a key-value pair.
  - `GET <key>` – Retrieve the value for a key.
  - `DELETE <key>` – Remove a key-value pair.
  - `EXIT` – Close the client connection.

---

## Build Instructions
```bash
# Clone repository
git clone <your-repo-url>
cd kvstore_project

# Create build directory
mkdir build && cd build

# Configure with CMake (Debug mode)
cmake -DCMAKE_BUILD_TYPE=Debug ..

# Build
make
```

## Starting the Server
By default, the server starts on port 8080.
```bash
./kvstore
```
## Example Client Commands
```bash
nc localhost 8080
PUT key1 value1
GET key1
DELETE key1
EXIT
```

## Sample Output
```bash
key1 added successfully.
value of key1 value1
key1 deleted.
```
## Thread Safety

- Global KVStore instance is shared among threads.

- Each client connection runs in a separate thread.

- std::mutex protects all put, get, and delete operations.

- This prevents race conditions and ensures consistent reads/writes.

## Testing

- The project includes GoogleTest-based tests:

- CRUD Operations

- WAL Persistence

- Concurrency Stress Tests

- End-to-End TCP Server Tests

- Run tests:
    ```bash
    ./runTests
    ```
## WAL Recovery
- Every operation (PUT/DELETE) is logged before applying.

- On restart, the server replays the WAL to restore the latest state.

- Verified using automated WAL recovery tests.


