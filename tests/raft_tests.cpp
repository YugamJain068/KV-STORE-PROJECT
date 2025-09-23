#define TESTING
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>
#include <chrono>
#include "../src/raft_node.h"

// ------------------- Mock RPC -------------------

std::string sendRPC(const std::string &host, int port, const std::string &msg) {
    // Mock responses based on RPC type
    if (msg.find("RequestVote") != std::string::npos) {
        // Always grant vote in tests
        return R"({"term":1,"voteGranted":true})";
    }
    if (msg.find("AppendEntries") != std::string::npos) {
        // Always succeed replication
        return R"({"term":1,"success":true})";
    }
    return "{}";
}

// ------------------- Helper Functions -------------------

std::vector<std::shared_ptr<RaftNode>> createTestCluster(int numNodes = 3) {
    int basePort = 5000;
    std::vector<std::shared_ptr<RaftNode>> nodes;

    for (int i = 0; i < numNodes; i++) {
        std::vector<int> peers;
        for (int j = 0; j < numNodes; j++)
            if (j != i) peers.push_back(basePort + j);

        auto node = std::make_shared<RaftNode>(i, basePort + i, peers);
        // For tests, do NOT call node->start() (no real RPC server)
        node->electionTimeout = std::chrono::milliseconds(10 + (i * 10)); // short timeout
        nodes.push_back(node);
    }

    return nodes;
}

void startElectionTimers(std::vector<std::shared_ptr<RaftNode>>& nodes, std::vector<std::thread>& timers) {
    for (auto& node : nodes) {
        timers.emplace_back([&nodes, node] {
            election_timer(node, nodes);
        });
    }
}

void shutdownCluster(std::vector<std::shared_ptr<RaftNode>>& nodes, std::vector<std::thread>& timers) {
    // Stop all election timers
    for (auto& node : nodes) {
        {
            std::lock_guard<std::mutex> lock(node->mtx);
            node->stopTimer = true;
        }
        node->cv.notify_all();
    }

    // Join all timer threads
    for (auto& t : timers) {
        if (t.joinable()) {
            t.join();
        }
    }
    
    timers.clear();
    nodes.clear();
}

std::shared_ptr<RaftNode> getLeader(const std::vector<std::shared_ptr<RaftNode>>& nodes) {
    for (auto& node : nodes)
        if (node->state == NodeState::LEADER)
            return node;
    return nullptr;
}

// ------------------- Tests -------------------

TEST(RaftClusterTest, LeaderElection) {
    auto nodes = createTestCluster();
    std::vector<std::thread> timers;
    startElectionTimers(nodes, timers);

    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // allow election

    auto leader = getLeader(nodes);
    ASSERT_NE(leader, nullptr) << "No leader was elected";

    int leaderCount = 0;
    for (auto& node : nodes)
        if (node->state == NodeState::LEADER) leaderCount++;
    EXPECT_EQ(leaderCount, 1);

    // Properly shutdown the cluster
    shutdownCluster(nodes, timers);
}

TEST(RaftClusterTest, LogReplicationAndCommit) {
    auto nodes = createTestCluster();
    std::vector<std::thread> timers;
    startElectionTimers(nodes, timers);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto leader = getLeader(nodes);
    ASSERT_NE(leader, nullptr);

    // Append commands
    leader->handleClientCommand("client-1",1,"PUT ","key","value100");
    leader->handleClientCommand("client-1",2,"DELETE", "key", "");

    // Wait for commit
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    EXPECT_EQ(leader->commitIndex, 1);
    EXPECT_EQ(leader->lastApplied, 1);

    // Properly shutdown the cluster
    shutdownCluster(nodes, timers);
}

TEST(RaftClusterTest, ApplyToStateMachine) {
    auto nodes = createTestCluster();
    std::vector<std::thread> timers;
    startElectionTimers(nodes, timers);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto leader = getLeader(nodes);
    ASSERT_NE(leader, nullptr);

    leader->handleClientCommand("client-1",1,"PUT ","key","value100");
    leader->handleClientCommand("client-1",2,"DELETE", "key", "");

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    EXPECT_EQ(leader->lastApplied, 1);

    // Properly shutdown the cluster
    shutdownCluster(nodes, timers);
}