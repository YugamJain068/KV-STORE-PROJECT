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
}
void RaftNode::start()
{
    auto self = shared_from_this(); // now safe, must be called on a shared_ptr-managed object
    rpcServerThread = std::thread([self]()
                                  { startRaftRPCServer(self->rpcPort, self); });
}

RaftNode::~RaftNode()
{
    stopTimer = true;

    stopRPC = true; // tell RPC thread to exit
    if (serverSocket != -1)
    {
        shutdown(serverSocket, SHUT_RDWR);
        close(serverSocket);
    } // unblock accept()

    if (rpcServerThread.joinable())
        rpcServerThread.join();
}

void reset_timeout(std::shared_ptr<RaftNode> node)
{
    std::lock_guard<std::mutex> lock(node->mtx);
    node->lastHeartbeatTimePoint = Clock::now();
    node->electionTimeout = std::chrono::milliseconds(150 + rand() % 151);
    node->cv.notify_all();
}

void send_heartbeats(std::shared_ptr<RaftNode> leader, std::vector<std::shared_ptr<RaftNode>> &nodes)
{
    for (auto &node : nodes)
    {
        if (node->id != leader->id)
        {
            node->lastHeartbeatTimePoint = Clock::now();
            node->state = NodeState::FOLLOWER;
        }
    }
}

void start_election(std::shared_ptr<RaftNode> candidate, std::vector<std::shared_ptr<RaftNode>> &nodes)
{
    candidate->state = NodeState::CANDIDATE;
    candidate->currentTerm++;
    candidate->votedFor = candidate->id;
    persistMetadata(candidate);
    int votes = 1;

    for (int peerPort : candidate->peerRpcPorts)
    {
        RequestVoteRPC req{
            candidate->currentTerm,
            candidate->id,
            candidate->log.empty() ? -1 : (int)candidate->log.size() - 1,
            candidate->log.empty() ? 0 : candidate->log.back().term};

        std::string responseStr = sendRPC("127.0.0.1", peerPort, nlohmann::json(req).dump());
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

    if (votes >= (nodes.size() / 2 + 1))
    {
        candidate->state = NodeState::LEADER;
        std::cout << "Leader elected: Node " << candidate->id
                  << " Term: " << candidate->currentTerm << "\n";
        send_heartbeats(candidate, nodes);
        for (auto &node : nodes)
            reset_timeout(node);
        for (int peerPort : candidate->peerRpcPorts)
        {
            AppendEntriesRPC heartbeat = {candidate->currentTerm, candidate->id, candidate->log.empty() ? -1 : static_cast<int>(candidate->log.size() - 1), candidate->log.empty() ? 0 : candidate->log.back().term, json::array(), candidate->commitIndex};

            sendRPC("127.0.0.1", peerPort, nlohmann::json(heartbeat).dump());
        }
        persistMetadata(candidate);
    }
}

void RaftNode::applyToStateMachine(const std::string &command)
{
    // parse command into key/value if KV store
    std::cout << "[Node " << id << "] Applying command: " << command << "\n";
    // KVStore store;
    // store.put();
}

void RaftNode::handleClientCommand(const std::string command)
{
    if (state != NodeState::LEADER)
    {
        std::cout << "[Node " << id << "] Rejecting client command (not leader)\n";
        return;
    }

    logEntry entry{currentTerm, command};
    log.push_back(entry);
    std::cout << "[Leader " << id << "] Appended command to log: " << command << "\n";

    int newIndex = log.size() - 1;

    for (size_t i = 0; i < peerRpcPorts.size(); i++)
    {

        int port = peerRpcPorts[i];
        int prevIndex = newIndex - 1;
        int prevTerm = (prevIndex >= 0) ? log[prevIndex].term : 0;

        AppendEntriesRPC msg = {
            currentTerm,
            id,
            prevIndex,
            prevTerm,
            {log[newIndex]},
            commitIndex};

        std::string responseStr = sendRPC("127.0.0.1", port, nlohmann::json(msg).dump());
        auto respJson = nlohmann::json::parse(responseStr);
        AppendEntriesResponse resp = respJson.get<AppendEntriesResponse>();
        if (resp.term > currentTerm)
        {
            currentTerm = resp.term;
            state = NodeState::FOLLOWER;
            votedFor = -1;
            persistMetadata(shared_from_this());
            std::cout << "[Leader " << id
                      << "] Step down to FOLLOWER (term " << resp.term << ")\n";
            return;
        }
        if (resp.success)
        {
            matchIndex[i] = newIndex;
            std::cout << "[Leader " << id << "] Replicated log index " << newIndex
                      << " on peer " << port << "\n";
        }
    }
    int replicatedCount = 1;
    for (int m : matchIndex)
    {
        if (m >= newIndex)
            replicatedCount++;
    }
    if (replicatedCount > (peerRpcPorts.size() + 1) / 2)
    {
        commitIndex = newIndex;
        std::cout << "[Leader " << id << "] Entry " << newIndex
                  << " is committed (replicated on majority)\n";
        while (lastApplied < commitIndex)
        {
            lastApplied++;
            applyToStateMachine(log[lastApplied].command);
        }
    }
    persistMetadata(shared_from_this());
}

void election_timer(std::shared_ptr<RaftNode> node, std::vector<std::shared_ptr<RaftNode>> &nodes)
{
    while (!node->stopTimer)
    {
        std::unique_lock<std::mutex> lock(node->mtx);
        if (node->stopTimer)
            break;
        auto now = Clock::now();
        if (node->cv.wait_until(lock, now + node->electionTimeout, [&]
                                { return node->state == NodeState::LEADER || node->stopTimer; }))
        {
            if (node->stopTimer)
                break;
            continue; // Woken up by heartbeat or shutdown
        }
        if (node->stopTimer)
            break;
        if (node->state != NodeState::LEADER)
        {
            std::cout << "Node " << node->id << " election timeout → starting election\n";
            lock.unlock();

            // Check one more time before the expensive operation
            if (node->stopTimer)
                break;
            start_election(node, nodes);
        }
        std::cout << "Election timer for Node " << node->id << " stopped\n";
    }
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

        node->electionTimeout = std::chrono::milliseconds(150 + rand() % 151);
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

    // Shutdown sequence
    std::cout << "Shutting down Raft cluster...\n";

    // Stop election timers first
    for (auto &node : nodes)
    {
        std::lock_guard<std::mutex> lock(node->mtx);
        node->stopTimer = true;
    }
    for (auto &node : nodes)
    {
        node->cv.notify_all();
    }
    std::cout << "Waiting for timer threads to finish...\n";
    for (size_t i = 0; i < timers.size(); ++i)
    {
        if (timers[i].joinable())
        {
            timers[i].join();
            std::cout << "Timer thread " << i << " joined\n";
        }
    }
    
    
    std::cout << "All timer threads joined\n";

    // Clear nodes vector - this should trigger destructors which handle RPC cleanup
    std::cout << "Destroying nodes...\n";
    nodes.clear();
    
    std::cout << "Raft cluster shutdown complete.\n";
}
