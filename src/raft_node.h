#ifndef RAFTNODE_H
#define RAFTNODE_H

#include<vector>
#include<string>
#include<random>
#include<ctime>
#include<chrono>
#include<mutex>
#include<condition_variable>
#include<atomic>
#include<thread>
#include "rpc_server.h"
#include <nlohmann/json.hpp>
#include <memory>



using Clock = std::chrono::steady_clock;

enum class NodeState {
    FOLLOWER,
    CANDIDATE,
    LEADER
};

struct logEntry{
    int term;
    std::string command;
};

inline void to_json(nlohmann::json &j, const logEntry &e) {
    j = nlohmann::json{{"term", e.term}, {"command", e.command}};
}

inline void from_json(const nlohmann::json &j, logEntry &e) {
    j.at("term").get_to(e.term);
    j.at("command").get_to(e.command);
}


class RaftNode : public std::enable_shared_from_this<RaftNode>{
    public:
    int id;
    NodeState state = NodeState::FOLLOWER;
    int currentTerm=0;
    int votedFor=-1;
    std::vector<logEntry>log;
    int commitIndex=0;
    int lastApplied=0;
    std::chrono::milliseconds electionTimeout{150};
    std::chrono::time_point<Clock> lastHeartbeatTimePoint = Clock::now();
    int rpcPort;
    std::thread rpcServerThread;

    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> stopTimer{false};
    std::string metadataFile;

    std::vector<int> peerRpcPorts;

    RaftNode(int nodeId_, int port, std::vector<int> peers) : id(nodeId_), rpcPort(port), peerRpcPorts(peers) {
        metadataFile = "RaftNode" + std::to_string(id) + ".json";

        // Start RPC server thread
        rpcServerThread = std::thread([this] {
            startRaftRPCServer(this->rpcPort,this);
        });
    }
    ~RaftNode() {
        if (rpcServerThread.joinable()) {
            rpcServerThread.join();
        }
    }

    void handleClientCommand(const std::string commmand);
};



void raftAlgorithm();
void reset_timeout(std::shared_ptr<RaftNode> node);
void send_heartbeats(std::shared_ptr<RaftNode> leader, std::vector<std::shared_ptr<RaftNode>> &nodes);
void start_election(std::shared_ptr<RaftNode> candidate, std::vector<std::shared_ptr<RaftNode>> &nodes);
void election_timer(std::shared_ptr<RaftNode> node, std::vector<std::shared_ptr<RaftNode>> &nodes);


#endif