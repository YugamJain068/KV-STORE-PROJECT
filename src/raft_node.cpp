#include "raft_node.h"
#include "persist_functions.h"
#include <iostream>
#include <thread>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

using Clock = std::chrono::steady_clock;

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
        nlohmann::json req = {
            {"rpc", "RequestVote"},
            {"term", candidate->currentTerm},
            {"candidateId", candidate->id},
            {"lastLogIndex", candidate->log.empty() ? -1 : (int)candidate->log.size() - 1},
            {"lastLogTerm", candidate->log.empty() ? 0 : candidate->log.back().term}};

        std::string response = sendRPC("127.0.0.1", peerPort, req.dump());
        // For now just log it
        std::cout << "[Candidate " << candidate->id << "] got response from peer "
                  << peerPort << ": " << response << "\n";

        // For Day 17, consider any response as a vote granted
        votes++;
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
            json heartbeat = {
                {"rpc", "AppendEntries"},
                {"term", candidate->currentTerm},
                {"leaderId", candidate->id},
                {"prevLogIndex", candidate->log.size() - 1},
                {"prevLogTerm", candidate->log.empty() ? 0 : candidate->log.back().term},
                {"entries", json::array()}, // empty = heartbeat
                {"leaderCommit", candidate->commitIndex}};
            sendRPC("127.0.0.1", peerPort, heartbeat.dump());
        }
        persistMetadata(candidate);
    }
}

void RaftNode::handleClientCommand(const std::string command) {
    if (state != NodeState::LEADER) {
        std::cout << "[Node " << id << "] Rejecting client command (not leader)\n";
        return;
    }

    logEntry entry{currentTerm, command};
    log.push_back(entry);
    std::cout << "[Leader " << id << "] Appended command to log: " << command << "\n";

    for (auto port : peerRpcPorts) {
        nlohmann::json msg = {
            {"rpc", "AppendEntries"},
            {"term", currentTerm},
            {"leaderId", id},
            {"prevLogIndex", (int)log.size() - 2},   // previous entry index
            {"prevLogTerm", log.size() > 1 ? log[log.size() - 2].term : 0},
            {"entries", {{{"term", currentTerm}, {"command", command}}}},
            {"leaderCommit", commitIndex}
        };
        std::string resp = sendRPC("127.0.0.1", port, msg.dump());
        std::cout << "[Leader " << id << "] got AppendEntries reply from peer "
                  << port << ": " << resp << "\n";
    }

    persistMetadata(shared_from_this());
}


void election_timer(std::shared_ptr<RaftNode> node, std::vector<std::shared_ptr<RaftNode>> &nodes)
{
    while (!node->stopTimer)
    {
        std::unique_lock<std::mutex> lock(node->mtx);
        auto now = Clock::now();
        if (node->cv.wait_until(lock, now + node->electionTimeout, [&]
                                { return node->state == NodeState::LEADER || node->stopTimer; }))
        {
            continue; // Woken up by heartbeat or shutdown
        }
        if (node->state != NodeState::LEADER)
        {
            std::cout << "Node " << node->id << " election timeout → starting election\n";
            start_election(node, nodes);
        }
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
        nodes.push_back(node);
    }

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
        leader->handleClientCommand("PUT key=value");
        leader->handleClientCommand("DELETE key");
    }

    for (auto &node : nodes)
        node->stopTimer = true;

    for (auto &t : timers)
        t.join();
}
