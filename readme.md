KVStore – Distributed Key-Value Store with Raft Consensus
=========================================================

Overview
--------

KVStore is a **distributed, consensus-based key-value store** written in C++ that uses the **Raft consensus algorithm** for data consistency across multiple nodes. It supports basic CRUD operations (PUT, GET, DELETE) with **Write-Ahead Logging (WAL)** for durability, **multi-threaded client handling** for concurrency, and **distributed consensus** for fault tolerance.

Features
--------

*   **Raft Consensus Algorithm**: Ensures strong consistency across distributed nodes
    
*   **Leader Election**: Automatic leader election with randomized timeouts
    
*   **Log Replication**: Commands are replicated across majority of nodes before commit
    
*   **Persistent State**: Term, votedFor, and log entries persist across restarts
    
*   **TCP-based RPC**: Inter-node communication via JSON-based RPCs
    
*   **Thread-Safe Operations**: Uses std::mutex to protect shared state
    
*   **WAL Persistence**: Ensures data recovery after crashes or restarts
    
*   **Multi-node Deployment**: Run multiple nodes on different ports
    
*   **Basic Commands**:
    
    *   PUT – Insert or update a key-value pair (leader only)
        
    *   GET – Retrieve the value for a key
        
    *   DELETE – Remove a key-value pair (leader only)
        
    *   EXIT – Close the client connection
        

Architecture
------------

### Raft Implementation

*   **RaftNode Structure**: Contains node state, log entries, and consensus metadata
    
*   **Leader Election**: Nodes use randomized election timeouts to avoid split votes
    
*   **Log Replication**: Leaders replicate log entries to followers before committing
    
*   **Persistent Storage**: Critical Raft state persists to JSON files
    
*   **RPC Communication**: RequestVote and AppendEntries RPCs over TCP
    

### Integration with KV Store

*   **Consensus-Driven**: All write operations go through Raft consensus
    
*   **Leader-Only Writes**: Only the leader accepts PUT/DELETE commands
    
*   **Committed Execution**: Commands apply to KV store only after majority replication
    
*   **Follower Redirection**: Non-leaders reject write operations
    

Build Instructions
------------------

``` bash   
# Clone repository  
git clone https://github.com/YugamJain068/KV-STORE-PROJECT  cd kvstore_project

# Create build directory  
mkdir build && cd build  

# Configure with CMake (Debug mode)  
cmake -DCMAKE_BUILD_TYPE=Debug ..  

# Build  
make   
````

Running the Distributed Cluster
-------------------------------

### Option 1: Automated 3-Node Cluster

```bash
# Starts 3 nodes on ports 5000, 5001, 5002  
./kvstore
```
### Option 2: Manual Node Startup

```bash
# Terminal 1 - Node 0
./kvstore --node-id 0 --port 5000 --peers 5001,5002  

# Terminal 2 - Node 1
./kvstore --node-id 1 --port 5001 --peers 5000,5002  

# Terminal 3 - Node 2
./kvstore --node-id 2 --port 5002 --peers 5000,5001   
```
Client Interaction
------------------

Connect to any node, but write operations only work on the leader:

```bash
nc localhost 5000  
PUT key1 value1  
GET key1  
DELETE key1  
EXIT   
```

Sample Output
-------------

``` bash

# On leader node  
PUT key1 value1  → key1 added successfully.  

# On follower node    
PUT key2 value2  → Error: Not leader. Current leader is Node 0  

GET key1  → value of key1: value1   
```

Raft Consensus Flow
-------------------

1.  **Client Command**: Client sends PUT/DELETE to any node
    
2.  **Leader Check**: Only leader accepts write commands
    
3.  **Log Append**: Leader appends command to its log
    
4.  **Replication**: Leader sends AppendEntries RPCs to followers
    
5.  **Majority Commit**: Once majority of nodes replicate entry, it's committed
    
6.  **State Machine**: Command applies to KV store after commit
    
7.  **Client Response**: Leader responds to client with success/failure
    

Thread Safety & Concurrency
---------------------------

*   **Per-Node Protection**: Each RaftNode has its own mutex for state protection
    
*   **Multi-threaded RPC**: Each node runs TCP server handling multiple concurrent RPCs
    
*   **Election Timers**: Separate threads handle election timeouts
    
*   **Client Handlers**: Separate threads for each client connection
    
*   **Lock Ordering**: Prevents deadlocks in multi-node operations
    

Testing
-------

The project includes comprehensive GoogleTest-based tests:

### Raft Tests

*   **Leader Election**: Verifies single leader election
    
*   **Log Replication**: Tests command replication across nodes
    
*   **Persistence**: Validates state recovery after restarts
    
*   **Network Partitions**: Simulates split-brain scenarios
    

### Integration Tests

*   **End-to-End**: Full client-server-consensus flow
    
*   **Concurrent Operations**: Multiple clients, multiple nodes
    
*   **Failure Recovery**: Node failures and rejoin scenarios
    

### Legacy KV Store Tests

*   **CRUD Operations**: Basic key-value operations
    
*   **WAL Persistence**: Write-ahead logging functionality
    
*   **Concurrency Stress**: High-load concurrent access
    

```bash
# Run all tests  
./runTests  

# Run only Raft tests  
./runTests --gtest_filter="RaftClusterTest.*"  
```

Persistent Storage
------------------

### Raft Metadata

Each node maintains persistent files:

*   RaftNode0.json - Node 0's term, votedFor, and log
    
*   RaftNode1.json - Node 1's term, votedFor, and log
    
*   RaftNode2.json - Node 2's term, votedFor, and log
    

### WAL Recovery

*   Write-ahead log for KV operations
    
*   Automatic replay on node restart
    
*   Consistent state restoration across cluster
    

Fault Tolerance
---------------

### Leader Failure

*   **Detection**: Followers detect leader failure via heartbeat timeout
    
*   **Election**: New leader election with incremented term
    
*   **Recovery**: New leader continues from last committed index
    

### Network Partitions

*   **Split Brain Prevention**: Requires majority for leader election
    
*   **Minority Partition**: Minority nodes become followers, reject writes
    
*   **Partition Healing**: Nodes sync logs when partition resolves
    

### Node Recovery

*   **State Restoration**: Recovering nodes load persistent state
    
*   **Log Catchup**: Leaders help recovering nodes catch up
    
*   **Automatic Integration**: No manual intervention required
    

Development Status
------------------

### ✅ Completed (Week 3 Deliverables)

*   **RaftNode Structure**: Complete node state management
    
*   **Leader Election**: Randomized timeouts, vote requests, majority consensus
    
*   **Persistent State**: Term, votedFor, and log entries survive restarts
    
*   **AppendEntries Implementation**: Heartbeats and log replication
    
*   **TCP-based RPC Server**: Multi-port JSON RPC communication
    
*   **Consensus-Driven KV Store**: Commands apply only after Raft commit
    
*   **Unit Tests**: Leader election and log replication test coverage
    

### 🚧 Future Enhancements

*   **Log Compaction**: Snapshot mechanism to prevent unbounded log growth
    
*   **Dynamic Membership**: Add/remove nodes from cluster
    
*   **Client Redirection**: Automatic redirect to leader node
    
*   **Monitoring**: Metrics and health check endpoints
    
*   **Configuration Management**: External config files for cluster setup
    

Repository Structure
--------------------

  kvstore_project  
  ├── src/  
  │   ├── raft_node.h/cpp     # Raft consensus implementation   
  │   ├── rpc_server.h/cpp    # TCP RPC communication  
  │   ├── persist_functions.h # Persistent storage utilities  
  │   ├── kvstore.h/cpp       # Key-value store logic  
  │   └── main.cpp            # Application entry point  
  ├── tests/  
  │   ├── raft_tests.cpp      # Raft algorithm tests  
  │   ├── kvstore_tests.cpp   # KV store functionality tests  
  │   └── integration_tests.cpp # End-to-end tests  
  ├── CMakeLists.txt          # Build configuration  
  └── README.md               # This file   