# KVStore – Distributed Key-Value Store with Raft Consensus

## Overview

**KVStore** is a **distributed, fault-tolerant key-value database** built in **C++**, implementing the **Raft consensus algorithm** to maintain strong consistency across multiple nodes.  
It provides **durable, highly available storage** with **write-ahead logging (WAL)**, **snapshotting**, and **multi-threaded concurrency** — enabling resilient state recovery even under failures.

---

## ✨ Key Features

- 🧠 **Raft Consensus Algorithm** – Leader-based consistency with majority commit rule  
- ⚡ **Automatic Leader Election** – Randomized election timeouts prevent split votes  
- 🔁 **Log Replication** – Replicates write commands across all nodes before commit  
- 💾 **Persistent State** – Term, votedFor, and logs persisted to disk  
- 📡 **TCP-based RPC** – Inter-node communication via JSON-based RPCs  
- 🔐 **Thread-Safe Operations** – Uses `std::mutex` for critical section protection  
- 🧱 **Write-Ahead Logging (WAL)** – Guarantees crash recovery and durability  
- 📷 **Snapshotting & Checkpointing** – Compact logs and store periodic state snapshots  
- 🧭 **CLI Admin Tool** – Cluster diagnostics, node term, and leader state introspection  
- 📊 **Metrics & Logging** – Leader term, node role, and replication statistics  
- 🧩 **Multi-node Deployment** – Seamless setup for 3+ nodes  
## 💻 Command Reference

| **Category** | **Command** | **Description** |
|---------------|-------------|-----------------|
| **🧩 Basic Commands** | `PUT <key> <value>` | Add or update a key-value pair |
|  | `GET <key>` | Retrieve the value for a key |
|  | `DELETE <key>` | Delete a key from the store |
| **⚙️ Advanced Formats** | `PUT --key <k> --value <v>` | Use named parameters for clarity |
|  | `PUT <key> "value with spaces"` | Support quoted values containing spaces |
| **🛠️ Admin Commands** | `STATUS` | Display current node and role information |
|  | `METRICS` | Show detailed performance and resource metrics |
|  | `LOGSIZE` | Show size and details of the Raft log |
|  | `SNAPSHOT` | Trigger a manual snapshot checkpoint |
|  | `LOGS [count]` | Display recent Raft log entries |
|  | `CLUSTER` | Show information about all cluster nodes |
|  | `WATCH [seconds]` | Auto-refresh and display metrics periodically |
| **🧰 Utility Commands** | `HELP` or `?` | Display this command reference |
|  | `HISTORY` | Show previously executed commands |
|  | `!!` | Repeat the last executed command |
|  | `!<n>` | Repeat the *nth* command from history |
|  | `STATS` | Show network and connection statistics |
|  | `CLEAR` | Clear the terminal screen |
|  | `EXIT` | Gracefully close the client connection |

---

## 🏗️ Architecture

### Raft Consensus Layer
- **Node Roles**: Follower, Candidate, Leader  
- **Leader Election**: Randomized election timers ensure a single leader  
- **Replication**: Leaders replicate log entries via AppendEntries RPCs  
- **Persistence**: Each node maintains `term`, `votedFor`, and full Raft logs  
- **Snapshotting**: Periodically creates compact snapshots to prevent log growth  
- **Recovery**: Restarted nodes load persisted state & snapshots automatically  

### KV Store Integration
- **Consensus-Driven Writes** – All modifications go through Raft commit  
- **Read Consistency** – Follows leader’s committed state  
- **Follower Redirection** – Non-leaders reject writes with redirect notice  
- **Crash Recovery** – WAL + Snapshot replay ensures state integrity  

---

## ⚙️ Build Instructions

```bash
# Clone repository
git clone https://github.com/YugamJain068/KV-STORE-PROJECT
cd kvstore_project

# Create build directory
mkdir build && cd build

# Configure with CMake (Debug mode)
cmake -DCMAKE_BUILD_TYPE=Debug ..

# Build project
make
```

---

## 🚀 Running the Distributed Cluster

### Option 1: Automated 3-Node Cluster
```bash
# Launch 3 Raft nodes on ports 5000, 5001, and 5002
./kvstore
```

### Option 2: Manual Startup
```bash
# Terminal 1 - Node 0
./kvstore --node-id 0 --port 5000 --peers 5001,5002

# Terminal 2 - Node 1
./kvstore --node-id 1 --port 5001 --peers 5000,5002

# Terminal 3 - Node 2
./kvstore --node-id 2 --port 5002 --peers 5000,5001
```

---

## 💬 Client Interaction

Clients can connect to any node using Netcat or telnet.  
Write operations succeed **only on the leader**.

```bash
nc localhost 5000
PUT key1 value1
GET key1
DELETE key1
EXIT
```

### Sample Output
```bash
# On leader node
PUT key1 value1  → key1 added successfully.

# On follower node
PUT key2 value2  → Error: Not leader. Current leader is Node 0

GET key1  → value of key1: value1
```

---

## 🔄 Raft Consensus Flow

1. Client sends command (PUT/DELETE)  
2. Node checks if it is the leader  
3. Leader appends command to log  
4. Leader sends AppendEntries RPCs to followers  
5. Once majority acknowledge → entry is committed  
6. Command applied to state machine (KV Store)  
7. Client receives success response  

---

## 🧵 Thread Safety & Concurrency

- **Mutex Protection** – Each Raft node’s shared state guarded by `std::mutex`  
- **Threaded RPC** – Each RPC runs in a separate thread  
- **Client Threads** – Each connection handled concurrently  
- **Election Timers** – Independent timer threads per node  
- **Snapshot Threads** – Background compaction and checkpoint handling  

---

## 🧪 Testing Suite

### Raft Tests
- ✅ Leader election stability  
- ✅ Log replication across nodes  
- ✅ State persistence & recovery  
- ✅ Network partitions and rejoining  

### Integration Tests
- ✅ Full client-server-consensus flow  
- ✅ Concurrent clients  
- ✅ Failure recovery (leader crash + restore)  

### KV Store Tests
- ✅ CRUD operations  
- ✅ WAL persistence & replay  
- ✅ Snapshot loading  
- ✅ Concurrency stress tests  

```bash
# Run all tests
./runTests

# Run only Raft tests
./runTests --gtest_filter="RaftClusterTest.*"
```

---

## 💽 Persistent Storage

### Raft Metadata
Each node stores persistent JSON files like:
```
RaftNode0.json
RaftNode1.json
RaftNode2.json
```
Containing:
- Current term  
- VotedFor  
- Log entries  

### Write-Ahead Log (WAL)
- Append-only file for each node  
- Replayed at startup for state recovery  

### Snapshots & Checkpointing
- Periodic state snapshots reduce log size  
- Checkpoints capture full KV state and last included index  
- Enables quick restart without full log replay  

---

## 🧰 CLI & Metrics

### CLI Admin Tool
```bash
STATUS
METRICS
LOGSIZE
SNAPSHOT
LOGS [count]
CLUSTER
WATCH [seconds]
```

### Metrics Logging
- Current term, leader ID, commit index  
- AppendEntries success/failure rates  
- Snapshot interval events  
- Client request counts  

---

## 🔒 Fault Tolerance

### Leader Failure
- Followers detect missing heartbeats  
- Trigger new election automatically  
- State restored via persistent log  

### Network Partition
- Majority partition elects leader  
- Minority stays follower  
- When healed, logs synchronize automatically  

### Node Recovery
- Restores persisted term, log, and snapshot  
- Automatically catches up with current leader  
- No manual repair required  

---

## 🧭 Development Progress

### ✅ Completed (All Phases)
| Phase | Deliverables |
|:--|:--|
| **Phase 1: Foundation (Week 1–2)** | Single-node KV store, WAL persistence, thread-safety, TCP server |
| **Phase 2: Raft Core (Week 3–6)** | Leader election, heartbeat, log replication, fault tolerance |
| **Phase 3: Advanced & Polish (Week 7–9)** | Snapshotting, checkpointing, CLI tool, metrics/logging, final refactor |

---

## 🧱 Repository Structure

```
kvstore_project/
├── src/
│   ├── decode_encodebase64.h/cpp   # Base64 encoding/decoding utilities for snapshots/logs
│   ├── kvstore_global.h            # Global constants, enums, and utility definitions
│   ├── kvstore.h/cpp               # Core key-value store logic (PUT, GET, DELETE)
│   ├── log_entry.h                 # Raft log entry structure (term, index, command)
│   ├── logger.h/cpp                # Centralized logging utilities
│   ├── main.cpp                    # Application entry point
│   ├── metrics.h                   # Metrics definitions and monitoring utilities
│   ├── persist_functions.h/cpp     # Persistence helpers for Raft metadata and KV state
│   ├── raft_node.h/cpp             # Raft consensus algorithm implementation
│   ├── rpc_server.h/cpp            # TCP/JSON-based RPC communication between nodes
│   ├── server.h/cpp                # Client-facing server for handling CRUD commands
│   ├── snapshot.h/cpp              # Snapshot and checkpointing mechanism
│   └── wal.h/cpp                   # Write-Ahead Logging (WAL) for durability
├── tests/
│   ├── raft_tests.cpp              # Unit tests for Raft election and replication
│   ├── kvstore_tests.cpp           # Tests for KV operations and WAL recovery
│   ├── concurrency_tests.cpp       # KVStoreConcurrencyTest for multiple threads
│   ├── wal_recovery_tests.cpp      # tests for wal persistance and recovery
│   └── tcp_server_tests.cpp        # tests for basic CRUD in tcp server
│
├── CMakeLists.txt             # Build configuration
└── README.md                  # Project documentation
```

---

## 🚧 Future Enhancements

- 🔄 **Dynamic Membership** – Add/remove nodes without restart  
- 🌐 **HTTP API Layer** – RESTful interface for modern clients   
- 🔍 **Dashboard UI** – Real-time cluster monitoring  
- 🪶 **Compression & Encryption** – Secure & efficient storage  

---

## 🎥 Video Demo
[![View Demo on Google Drive](assets/Screenshot%202025-11-10%20222652.png)](https://drive.google.com/drive/u/0/folders/1KWkJYHPY7rt5WFnoz_0oOhH-K5Sm6Qbc)

---

**Author:** [Yugam Jain](https://github.com/YugamJain068)  
**Language:** C++17  
**Build System:** CMake  
**Testing Framework:** GoogleTest  
**Networking:** TCP (JSON RPC)  
**Consensus Algorithm:** Raft  
