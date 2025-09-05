#include "raft_node.h"
#include <iostream>
#include <thread>

using Clock = std::chrono::steady_clock;

void reset_timeout(RaftNode &node)
{
    node.electionTimeout = 150 + rand() % 151;
    node.lastHeartbeatTimePoint = Clock::now();
}

void send_heartbeats(RaftNode &leader, std::vector<RaftNode> &nodes)
{
    for (auto &node : nodes)
    {
        if (node.id != leader.id)
        {
            node.lastHeartbeatTimePoint = Clock::now();
            node.state = NodeState::FOLLOWER;
        }
    }
}

void start_election(RaftNode &candidate, std::vector<RaftNode> &nodes)
{
    candidate.state = NodeState::CANDIDATE;
    candidate.currentTerm++;
    candidate.votedFor = candidate.id;
    int votes = 1;

    for (auto &node : nodes)
    {
        if (node.id != candidate.id)
        {
            if (node.votedFor == -1 || node.currentTerm < candidate.currentTerm)
            {
                node.votedFor = candidate.id;
                node.currentTerm = candidate.currentTerm;
                votes++;
            }
        }
    }

    if (votes >= (nodes.size() / 2 + 1))
    {
        candidate.state = NodeState::LEADER;
        std::cout << "Leader elected: Node " << candidate.id
                  << " Term: " << candidate.currentTerm << "\n";
        send_heartbeats(candidate, nodes);
        for (auto &node : nodes)
            reset_timeout(node);
    }
}

void raftAlgorithm()
{
    srand(time(NULL));
    std::vector<RaftNode> nodes;
    for (int i = 0; i < 3; i++)
    {
        RaftNode node({i});
        node.electionTimeout = 150 + rand() % 151;
        node.lastHeartbeatTimePoint = Clock::now();
        nodes.push_back(node);
    }

    int simulation_time = 0;
    while (simulation_time < 5000)
    {
        for (auto &node : nodes)
        {
            if (node.state != NodeState::LEADER)
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   Clock::now() - node.lastHeartbeatTimePoint)
                                   .count();
                if (elapsed > node.electionTimeout)
                {
                    std::cout << "Node " << node.id
                              << " election timeout → starting election\n";
                    start_election(node, nodes);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        simulation_time += 50;
    }
}
