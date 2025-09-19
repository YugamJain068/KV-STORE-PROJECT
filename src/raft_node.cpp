#include "raft_node.h"
#include "persist_functions.h"
#include <iostream>
#include <thread>
#include <nlohmann/json.hpp>
#include "rpc_server.h"
#include "kvstore.h"
#include <unistd.h>
#include <sys/socket.h>

using json = nlohmann::json;

using Clock = std::chrono::steady_clock;

RaftNode::RaftNode(int nodeId_, int port, const std::vector<int> &peers)
    : id(nodeId_), rpcPort(port), peerRpcPorts(peers)
{
    metadataFile = "RaftNode" + std::to_string(id) + ".json";
    matchIndex.resize(peerRpcPorts.size(), -1);
    nextIndex.resize(peerRpcPorts.size(), log.size());
    lastHeartbeatTimePoint = Clock::now();
}

void RaftNode::start()
{
    auto self = shared_from_this(); // now safe, must be called on a shared_ptr-managed object
    rpcServerThread = std::thread([self]()
                                  { startRaftRPCServer(self->rpcPort, self); });
}

RaftNode::~RaftNode()
{
    if (shutdownRequested.exchange(true))
    {
        std::cout << "[Node " << id << "] Already shutting down, skipping\n";
        return; // Already shutting down
    }
    else
    {
        shutdownNode();
    }
    std::cout << "[Node " << id << "] Destroyed\n";
}

void RaftNode::shutdownNode()
{
    // Set shutdown flags FIRST
    {
        std::lock_guard<std::mutex> lock(mtx);

        stopTimer = true;
        runningHeartbeats = false;
        stopRPC = true;
    }

    std::cout << "[Node " << id << "] Starting shutdown...\n";

    // Wake up ALL waiting threads
    cv.notify_all();

    // Close socket to unblock RPC server
    if (serverSocket != -1)
    {
        shutdown(serverSocket, SHUT_RDWR);
        close(serverSocket);
        serverSocket = -1;
    }

    try
    {
        // 1. Stop heartbeat thread first
        if (heartbeatThread.joinable())
        {
            std::cout << "[Node " << id << "] Joining heartbeat thread...\n";
            heartbeatThread.join();
            std::cout << "[Node " << id << "] Heartbeat thread joined\n";
        }

        // 2. Stop RPC server thread
        if (rpcServerThread.joinable())
        {
            std::cout << "[Node " << id << "] Joining RPC server thread...\n";
            rpcServerThread.join();
            std::cout << "[Node " << id << "] RPC server thread joined\n";
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Node " << id << "] Exception during thread join: " << e.what() << "\n";
    }

    std::cout << "[Node " << id << "] Shutdown complete\n";
}
void RaftNode::becomeFollower(int newTerm)
{
    {
        std::lock_guard<std::mutex> lock(mtx);

        if (shutdownRequested.load())
        {
            return;
        }

        currentTerm = newTerm;
        state = NodeState::FOLLOWER;
        votedFor = -1;

        // stop heartbeats if running
        runningHeartbeats = false;
    }

    cv.notify_all();

    persistMetadata(shared_from_this());

    std::cout << "[Node " << id << "] Became FOLLOWER (term " << currentTerm << ")\n";
}

void reset_timeout(std::shared_ptr<RaftNode> node)
{
    std::lock_guard<std::mutex> lock(node->mtx);
    if (node->shutdownRequested.load())
    {
        return;
    }

    node->lastHeartbeatTimePoint = Clock::now();
    // **Generate new random timeout each time to prevent synchronized elections**
    node->electionTimeout = std::chrono::milliseconds(2000 + rand() % 2000);
    node->cv.notify_all();
    
    std::cout << "[Node " << node->id << "] Timeout reset, new timeout: " 
              << node->electionTimeout.count() << "ms\n";
}

void send_heartbeats(std::shared_ptr<RaftNode> leader, std::vector<std::shared_ptr<RaftNode>> &nodes)
{
    try
    {
        while (leader->runningHeartbeats &&
               leader->state == NodeState::LEADER &&
               !leader->shutdownRequested.load())
        {
            for (size_t peerIdx = 0; peerIdx < leader->peerRpcPorts.size(); peerIdx++)
            {
                if (!leader->runningHeartbeats || leader->shutdownRequested.load())
                    break;

                int peerPort = leader->peerRpcPorts[peerIdx];

                try
                {
                    int nextIdx = leader->nextIndex[peerIdx];
                    int prevIndex = nextIdx - 1;
                    int prevTerm = (prevIndex >= 0 && prevIndex < (int)leader->log.size()) ? 
                                   leader->log[prevIndex].term : 0;

                    json entries = json::array();
                    if (nextIdx < (int)leader->log.size()) {
                        for (int i = nextIdx; i < (int)leader->log.size(); i++) {
                            entries.push_back(leader->log[i]);
                        }
                    }

                    AppendEntriesRPC heartbeat{
                        leader->currentTerm, 
                        leader->id, 
                        prevIndex, 
                        prevTerm, 
                        entries, 
                        leader->commitIndex
                    };

                    std::string responseStr = sendRPC("127.0.0.1", peerPort, nlohmann::json(heartbeat).dump());

                    if (!leader->runningHeartbeats || leader->shutdownRequested.load())
                        break;

                    if (responseStr.empty())
                        continue;

                    auto respJson = nlohmann::json::parse(responseStr);
                    AppendEntriesResponse resp = respJson.get<AppendEntriesResponse>();

                    if (resp.term > leader->currentTerm)
                    {
                        {
                            std::lock_guard<std::mutex> lock(leader->mtx);
                            if (leader->shutdownRequested.load())
                            {
                                return;
                            }
                            leader->currentTerm = resp.term;
                            leader->state = NodeState::FOLLOWER;
                            leader->votedFor = -1;
                            leader->runningHeartbeats = false;
                            
                            // **FIX: Reset timeout when stepping down**
                            leader->lastHeartbeatTimePoint = Clock::now();
                        }
                        leader->cv.notify_all(); // Wake up election timer
                        persistMetadata(leader);
                        return;
                    }
                    
                    if (resp.success && !entries.empty())
                    {
                        leader->nextIndex[peerIdx] = leader->log.size();
                        leader->matchIndex[peerIdx] = leader->log.size() - 1;
                    }
                    else if (!resp.success && leader->nextIndex[peerIdx] > 0)
                    {
                        leader->nextIndex[peerIdx]--;
                    }
                }
                catch (const std::exception &e)
                {
                    std::cerr << "[Heartbeat] Exception: " << e.what() << "\n";
                    if (leader->shutdownRequested.load())
                        break;
                }
            }

            if (leader->shutdownRequested.load())
            {
                break;
            }

            // **FIX: Increase heartbeat frequency to prevent timeouts**
            // Send heartbeats more frequently (every 200ms instead of 50ms for better reliability)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Heartbeat thread] Exception: " << e.what() << "\n";
    }

    std::cout << "[Heartbeat thread] Exiting for Node " << leader->id << "\n";
}
void start_election(std::shared_ptr<RaftNode> candidate, std::vector<std::shared_ptr<RaftNode>> &nodes)
{
    {
        std::lock_guard<std::mutex> lock(candidate->mtx);
        if (candidate->shutdownRequested.load())
        {
            std::cout << "[Candidate " << candidate->id << "] Skipping election - shutdown requested\n";
            return;
        }

        candidate->state = NodeState::CANDIDATE;
        candidate->currentTerm++;
        candidate->votedFor = candidate->id;
    }

    persistMetadata(candidate);
    int votes = 1;

    for (int peerPort : candidate->peerRpcPorts)
    {
        // Check shutdown before each vote request
        if (candidate->shutdownRequested.load())
        {
            std::cout << "[Candidate " << candidate->id << "] Aborting election - shutdown requested\n";
            return;
        }

        RequestVoteRPC req{
            candidate->currentTerm,
            candidate->id,
            candidate->log.empty() ? -1 : (int)candidate->log.size() - 1,
            candidate->log.empty() ? 0 : candidate->log.back().term};

        std::string responseStr = sendRPC("127.0.0.1", peerPort, nlohmann::json(req).dump());

        // Check shutdown after RPC
        if (candidate->shutdownRequested.load())
        {
            std::cout << "[Candidate " << candidate->id << "] Aborting election after RPC - shutdown requested\n";
            return;
        }

        if (responseStr.empty())
        {
            std::cerr << "[Candidate " << candidate->id << "] Empty vote response from peer " << peerPort << "\n";
            continue;
        }
        try
        {
            auto respJson = nlohmann::json::parse(responseStr);
            RequestVoteResponse resp = respJson.get<RequestVoteResponse>();
            if (resp.term > candidate->currentTerm)
            {
                std::lock_guard<std::mutex> lock(candidate->mtx);

                // Check shutdown before state change
                if (candidate->shutdownRequested.load())
                {
                    return;
                }

                candidate->currentTerm = resp.term;
                candidate->state = NodeState::FOLLOWER;
                candidate->votedFor = -1;
                persistMetadata(candidate);
                std::cout << "[Candidate " << candidate->id
                          << "] Step down to FOLLOWER (term " << resp.term << ")\n";
                return;
            }
            if (resp.voteGranted)
            {
                votes++;
                std::cout << "[Candidate " << candidate->id << "] Vote granted from peer "
                          << peerPort << "\n";
            }
        }
        catch (std::exception &e)
        {
            std::cerr << "[Candidate " << candidate->id
                      << "] Failed to parse vote response: " << e.what() << "\n";
        }
    }

    // Check shutdown before becoming leader
    if (candidate->shutdownRequested.load())
    {
        std::cout << "[Candidate " << candidate->id << "] Not becoming leader - shutdown requested\n";
        return;
    }

    if (votes > nodes.size() / 2)
    {
        {
            std::lock_guard<std::mutex> lock(candidate->mtx);

            // Final shutdown check before leadership
            if (candidate->shutdownRequested.load())
            {
                std::cout << "[Candidate " << candidate->id << "] Not becoming leader - shutdown requested\n";
                return;
            }

            candidate->state = NodeState::LEADER;
            candidate->runningHeartbeats = true;
            
            for (size_t i = 0; i < candidate->nextIndex.size(); i++)
            {
                candidate->nextIndex[i] = candidate->log.size();
                candidate->matchIndex[i] = -1;
            }
        }

        std::cout << "Leader elected: Node " << candidate->id
                  << " Term: " << candidate->currentTerm << "\n";
        reset_timeout(candidate);

        // More careful heartbeat thread management
        if (candidate->heartbeatThread.joinable())
        {
            candidate->runningHeartbeats = false;
            std::cout << "[Node " << candidate->id << "] Joining existing heartbeat thread...\n";
            candidate->heartbeatThread.join();
            std::cout << "[Node " << candidate->id << "] Existing heartbeat thread joined\n";
            candidate->runningHeartbeats = true; // Reset flag after join
        }

        // Check shutdown before starting heartbeat thread
        if (!candidate->shutdownRequested.load())
        {
            std::cout << "[Node " << candidate->id << "] Starting new heartbeat thread...\n";
            candidate->heartbeatThread = std::thread(send_heartbeats, candidate, std::ref(nodes));
        }
        else
        {
            std::cout << "[Node " << candidate->id << "] Not starting heartbeat thread - shutdown requested\n";
        }

        persistMetadata(candidate);
    }
}

void RaftNode::applyToStateMachine(const std::string &command)
{
    if (shutdownRequested.load())
    {
        return;
    }

    // parse command into key/value if KV store
    std::cout << "[Node " << id << "] Applying command: " << command << "\n";
    // KVStore store;
    // store.put();
}

void RaftNode::handleClientCommand(const std::string command)
{
    if (shutdownRequested.load()) return;
    if (state != NodeState::LEADER) return;

    {
        std::lock_guard<std::mutex> lock(mtx);
        logEntry entry{currentTerm, (int)log.size(), command}; // keep index = vector idx
        log.push_back(entry);
        std::cout << "[Leader " << id << "] Appended command to log: " << command << "\n";
    }

    int newIndex = (int)log.size() - 1;
    int successCount = 1; // self

    for (size_t i = 0; i < peerRpcPorts.size(); i++)
    {
        if (shutdownRequested.load()) return;

        int port = peerRpcPorts[i];
        int nextIdx = nextIndex[i];
        int prevIndex = nextIdx - 1;
        int prevTerm = (prevIndex >= 0 && prevIndex < (int)log.size()) ? log[prevIndex].term : 0;

        json entries = json::array();
        for (int j = nextIdx; j <= newIndex; j++) {
            if (j < (int)log.size()) {
                entries.push_back(log[j]);
            }
        }

        AppendEntriesRPC msg{currentTerm, id, prevIndex, prevTerm, entries, commitIndex};
        std::string responseStr = sendRPC("127.0.0.1", port, nlohmann::json(msg).dump());

        if (responseStr.empty()) continue;

        try {
            auto respJson = nlohmann::json::parse(responseStr);
            AppendEntriesResponse resp = respJson.get<AppendEntriesResponse>();

            if (resp.term > currentTerm) {
                std::lock_guard<std::mutex> lock(mtx);
                currentTerm = resp.term;
                state = NodeState::FOLLOWER;
                votedFor = -1;
                persistMetadata(shared_from_this());
                return;
            }

            if (resp.success) {
                nextIndex[i] = newIndex + 1;
                matchIndex[i] = newIndex;
                successCount++;
            } else {
                if (nextIndex[i] > 0) nextIndex[i]--;
            }
        } catch (...) {}
    }

    if (shutdownRequested.load()) return;

    if (log[newIndex].term == currentTerm && successCount > (peerRpcPorts.size() + 1) / 2)
    {
        std::lock_guard<std::mutex> lock(mtx);
        commitIndex = newIndex;
        while (lastApplied < commitIndex && !shutdownRequested.load()) {
            lastApplied++;
            applyToStateMachine(log[lastApplied].command);
        }
    }

    persistMetadata(shared_from_this());
}


void election_timer(std::shared_ptr<RaftNode> node,
                    std::vector<std::shared_ptr<RaftNode>> &nodes)
{
    try
    {
        while (!node->stopTimer && !node->shutdownRequested.load())
        {
            std::unique_lock<std::mutex> lock(node->mtx);

            // **FIX 1: Check if we're a leader - leaders don't need election timeouts**
            if (node->state == NodeState::LEADER) {
                // Leaders should never timeout for elections
                node->cv.wait(lock, [&] { 
                    return node->state != NodeState::LEADER || 
                           node->stopTimer || 
                           node->shutdownRequested.load(); 
                });
                continue;
            }

            // **FIX 2: Calculate time since last valid heartbeat/vote**
            auto now = Clock::now();
            auto timeSinceHeartbeat = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - node->lastHeartbeatTimePoint);

            // **FIX 3: Only start election if actually timed out**
            if (timeSinceHeartbeat >= node->electionTimeout) {
                // Timeout occurred - start election
                lock.unlock(); // Release lock before expensive operation
                
                // Double-check shutdown
                if (node->stopTimer || node->shutdownRequested.load()) {
                    break;
                }
                
                std::cout << "Node " << node->id << " election timeout (" 
                          << timeSinceHeartbeat.count() << "ms >= " 
                          << node->electionTimeout.count() << "ms) → starting election\n";
                start_election(node, nodes);
            } else {
                // **FIX 4: Wait for remaining timeout period**
                auto remainingTime = node->electionTimeout - timeSinceHeartbeat;
                bool woken_up = node->cv.wait_for(lock, remainingTime, [&] { 
                    return node->state == NodeState::LEADER || 
                           node->stopTimer || 
                           node->shutdownRequested.load(); 
                });

                // If shutdown or became leader, continue loop
                if (node->stopTimer || node->shutdownRequested.load() || woken_up) {
                    continue;
                }
                // If we reach here, the wait timed out naturally - check again in next iteration
            }
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Election timer] Exception: " << e.what() << "\n";
    }

    std::cout << "Election timer for Node " << node->id << " stopped\n";
}



void raftAlgorithm()
{
    srand(time(NULL));
    std::vector<std::shared_ptr<RaftNode>> nodes;
    int basePort = 5000;
    int numNodes = 3;

    for (int i = 0; i < numNodes; i++)
    {
        std::vector<int> peers;
        for (int j = 0; j < numNodes; j++)
        {
            if (j != i)
                peers.push_back(basePort + j);
        }

        auto node = std::make_shared<RaftNode>(i, basePort + i, peers);
        loadMetadata(node);

        node->electionTimeout = std::chrono::milliseconds(2000 + rand() % 2000); 
        node->start();
        nodes.push_back(node);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::vector<std::thread> timers;
    for (auto &node : nodes)
    {
        timers.emplace_back([&nodes, node]
                            { election_timer(node, nodes); });
    }

    std::this_thread::sleep_for(std::chrono::seconds(5));

    // Client commands...
    std::shared_ptr<RaftNode> leader;
    for (auto &node : nodes)
    {
        if (node->state == NodeState::LEADER)
        {
            leader = node;
            break;
        }
    }

    if (leader)
    {
        leader->handleClientCommand("PUT key=value100");
        leader->handleClientCommand("DELETE key");
    }

    std::cout << "Shutting down Raft cluster...\n";

    for (auto &node : nodes)
    {
        std::lock_guard<std::mutex> lock(node->mtx);
        node->shutdownRequested.store(true);
        node->stopTimer = true;
        node->runningHeartbeats = false;
        node->stopRPC = true;
    }

    for (auto &node : nodes)
    {
        std::lock_guard<std::mutex> lock(node->mtx);
        node->cv.notify_all();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::cout << "Joining timer threads...\n";
    try
    {
        for (size_t i = 0; i < timers.size(); ++i)
        {
            if (timers[i].joinable())
            {
                std::cout << "Joining timer thread " << i << "...\n";
                timers[i].join();
                std::cout << "Timer thread " << i << " joined\n";
            }
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Exception joining timer threads: " << e.what() << "\n";
    }
    
    std::cout << "Shutting down individual nodes...\n";
    try
    {
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            std::cout << "Shutting down node " << i << "...\n";
            nodes[i]->shutdownNode();
            std::cout << "Node " << i << " shutdown complete\n";
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Exception during node shutdown: " << e.what() << "\n";
    }

    
    std::cout << "Clearing node references...\n";
    nodes.clear();

    std::cout << "Raft cluster shutdown complete.\n";
}