#ifndef RAFTNODE_H
#define RAFTNODE_H

#include<vector>
#include<string>
#include<random>
#include<time.h>
#include<chrono>

enum class NodeState {
    FOLLOWER,
    CANDIDATE,
    LEADER
};

struct RaftNode{
    int id;
    NodeState state = NodeState::FOLLOWER;
    int currentTerm=0;
    int votedFor=-1;
    std::vector<std::string>log;
    int commitIndex=0;
    int lastApplied=0;
    int electionTimeout;
    std::chrono::steady_clock::time_point lastHeartbeatTimePoint;
};

void raftAlgorithm();
void reset_timeout(RaftNode &node);
void send_heartbeats(RaftNode &leader, std::vector<RaftNode> &nodes);
void start_election(RaftNode &candidate, std::vector<RaftNode> &nodes);

#endif