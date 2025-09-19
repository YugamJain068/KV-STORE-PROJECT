#ifndef RAFTNODE_H
#define RAFTNODE_H

#include <vector>
#include <string>
#include <random>
#include <ctime>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <nlohmann/json.hpp>
#include <memory>


using Clock = std::chrono::steady_clock;

enum class NodeState
{
    FOLLOWER,
    CANDIDATE,
    LEADER
};

struct logEntry {
    int term;
    int index;
    std::string command;
};

inline void to_json(nlohmann::json &j, const logEntry &e)
{
    j = nlohmann::json{{"term", e.term},{"index", e.index}, {"command", e.command}};
}

inline void from_json(const nlohmann::json &j, logEntry &e)
{
    j.at("term").get_to(e.term);
    j.at("index").get_to(e.index);
    j.at("command").get_to(e.command);
}

class RaftNode : public std::enable_shared_from_this<RaftNode>
{   
public:
    std::atomic<bool> shutdownRequested{false};
    RaftNode(int nodeId_, int port, const std::vector<int>& peers);
    void start();
    ~RaftNode();
    void becomeFollower(int newTerm);
    int id;
    void shutdownNode();
    NodeState state = NodeState::FOLLOWER;
    int currentTerm = 0;
    int votedFor = -1;
    std::vector<logEntry> log;
    int commitIndex = 0;
    int lastApplied = 0;
    std::chrono::milliseconds electionTimeout;
    std::chrono::time_point<Clock> lastHeartbeatTimePoint = Clock::now();
    int rpcPort;
    std::thread rpcServerThread;

    std::mutex mtx;
    std::condition_variable cv;

    
    std::atomic<bool> stopTimer{false};
    std::string metadataFile;

    std::vector<int> peerRpcPorts;
    std::vector<int> matchIndex;
    std::vector<int> nextIndex;

    std::atomic<bool> stopRPC{false};
    int serverSocket = -1;

    std::atomic<bool> runningHeartbeats{false};
    std::thread heartbeatThread;

    void handleClientCommand(const std::string commmand);
    void applyToStateMachine(const std::string &command);
};

void raftAlgorithm();
void reset_timeout(std::shared_ptr<RaftNode> node);
void send_heartbeats(std::shared_ptr<RaftNode> leader, std::vector<std::shared_ptr<RaftNode>> &nodes);
void start_election(std::shared_ptr<RaftNode> candidate, std::vector<std::shared_ptr<RaftNode>> &nodes);
void election_timer(std::shared_ptr<RaftNode> node, std::vector<std::shared_ptr<RaftNode>> &nodes);

#endif