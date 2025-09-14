#include "raft_node.h"
#include "persist_functions.h"
#include <iostream>
#include <thread>

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
        std::string rpcJson = "{ \"rpc\": \"RequestVote\", \"term\": " +
                              std::to_string(candidate->currentTerm) +
                              ", \"candidateId\": " + std::to_string(candidate->id) + " }";

        std::string response = sendRPC("127.0.0.1", peerPort, rpcJson);
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
            std::string heartbeat = "{ \"rpc\": \"AppendEntries\", \"term\": " +
                                    std::to_string(candidate->currentTerm) + " }";
            sendRPC("127.0.0.1", peerPort, heartbeat);
        }
    }
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

    for (auto &node : nodes)
        node->stopTimer = true;

    for (auto &t : timers)
        t.join();
}
