#include "raft_node.h"
#include "persist_functions.h"
#include <nlohmann/json.hpp>
#include "rpc_server.h"
#include "kvstore.h"
#include <unistd.h>
#include <sys/socket.h>
#include "snapshot.h"
#include "kvstore_global.h"
#include <fstream>
#include "decode_encodebase64.h"
#include "logger.h"

using json = nlohmann::json;

using Clock = std::chrono::steady_clock;

std::mutex store_mutex;
std::atomic<bool> raftShutdownRequested(false);
const int SNAPSHOT_THRESHOLD = 3000;

void requestRaftShutdown()
{
    raftShutdownRequested.store(true);
}

bool isRaftShutdownRequested()
{
    return raftShutdownRequested.load();
}

RaftNode::RaftNode(int nodeId_, int port, const std::vector<int> &peers)
    : id(nodeId_), rpcPort(port), peerRpcPorts(peers), latestSnapshot(nullptr)
{
    metadataFile = "RaftNode" + std::to_string(id) + ".json";
    matchIndex.resize(peerRpcPorts.size(), -1);
    nextIndex.resize(peerRpcPorts.size(), 0);
    lastHeartbeatTimePoint = Clock::now();
}

void RaftNode::start()
{
    auto self = shared_from_this();
    rpcServerThread = std::thread([self]()
                                  { startRaftRPCServer(self->rpcPort, self); });

    std::string snapPath = "./snapshots/node_" + std::to_string(id) + ".snap";
    if (std::filesystem::exists(snapPath))
    {
        std::ifstream file(snapPath);
        nlohmann::json jSnap;
        file >> jSnap;
        file.close();

        auto snap = std::make_shared<Snapshot>();
        snap->lastIncludedIndex = jSnap["metadata"]["lastIncludedIndex"];
        snap->lastIncludedTerm = jSnap["metadata"]["lastIncludedTerm"];

        auto tempMap = jSnap["state"].get<std::map<std::string, std::string>>();
        snap->kvState = std::unordered_map<std::string, std::string>(
            tempMap.begin(), tempMap.end());

        latestSnapshot = snap;

        // Apply to state machine
        store.loadFromMap(snap->kvState);
        lastApplied = snap->lastIncludedIndex;
        commitIndex = snap->lastIncludedIndex;

        Logger::info(id, "Follower", currentTerm, "Restored snapshot: lastIncludedIndex=" + std::to_string(snap->lastIncludedIndex) + ", entries=" + std::to_string(snap->kvState.size()));
    }
}
RaftNode::~RaftNode()
{
    if (shutdownRequested.exchange(true))
    {
        Logger::info(id, "Follower", currentTerm, "Already shutting down, skipping");
        return;
    }
    else
    {
        shutdownNode();
    }
    Logger::info(id, "Follower", currentTerm, "Destroyed");
}
void RaftNode::updateLogMetrics()
{
    metrics.logSize = getLogSize();
    metrics.commitIndex = commitIndex;
    metrics.lastApplied = lastApplied;

    if (latestSnapshot)
    {
        metrics.lastSnapshotIndex = latestSnapshot->lastIncludedIndex;
        metrics.lastSnapshotTerm = latestSnapshot->lastIncludedTerm;
    }
}

nlohmann::json RaftNode::getStatus()
{
    updateLogMetrics();

    std::string roleStr = "Follower";
    if (state == NodeState::LEADER)
        roleStr = "Leader";
    else if (state == NodeState::CANDIDATE)
        roleStr = "Candidate";

    return nlohmann::json{
        {"node_id", id},
        {"term", currentTerm},
        {"role", roleStr},
        {"leader_id", leaderId},
        {"commit_index", commitIndex},
        {"last_applied", lastApplied},
        {"log_size", getLogSize()},
        {"voted_for", votedFor},
        {"snapshot_count", metrics.snapshotCount.load()},
        {"last_snapshot_index", metrics.lastSnapshotIndex.load()}};
}

bool RaftNode::triggerSnapshot()
{
    try
    {
        if ((int)log.size() == 0)
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(mtx);

        auto newSnapshot = std::make_shared<Snapshot>();
        newSnapshot->lastIncludedIndex = lastApplied;
        newSnapshot->lastIncludedTerm = getLogTerm(lastApplied);

        {
            std::lock_guard<std::mutex> storeLock(store_mutex);
            newSnapshot->kvState = store.dumpToMap();
        }

        saveSnapshot(*newSnapshot, id);
        truncateLogSafe(newSnapshot->lastIncludedIndex);
        latestSnapshot = newSnapshot;

        store.writeAheadLog_truncate(newSnapshot->lastIncludedIndex);

        for (size_t i = 0; i < nextIndex.size(); i++)
        {
            if (nextIndex[i] <= newSnapshot->lastIncludedIndex)
            {
                nextIndex[i] = newSnapshot->lastIncludedIndex + 1;
            }
        }

        persistMetadata(shared_from_this());
        metrics.recordSnapshot(newSnapshot->lastIncludedIndex, newSnapshot->lastIncludedTerm);
        Logger::info(id, getRoleString(), currentTerm, "Manual snapshot created successfully");

        return true;
    }
    catch (const std::exception &e)
    {
        Logger::error(id, getRoleString(), currentTerm, "Snapshot creation failed: " + std::string(e.what()));
        return false;
    }
    catch (...)
    {
        Logger::error(id, getRoleString(), currentTerm, "Snapshot creation failed: Unknown error");
        return false;
    }
}
int RaftNode::getLogTerm(int index) const

{

    // Check if index is in snapshot range
    if (latestSnapshot && index <= latestSnapshot->lastIncludedIndex)
    {
        if (index == latestSnapshot->lastIncludedIndex)

        {

            return latestSnapshot->lastIncludedTerm;
        }

        return 0;
    }

    int logIndex = index;

    if (latestSnapshot)
    {
        logIndex = index - latestSnapshot->lastIncludedIndex - 1;
    }

    if (logIndex < 0 || logIndex >= (int)log.size())
    {
        return 0;
    }

    return log[logIndex].term;
}


int RaftNode::getLogSize() const

{
    int baseIndex = 0;

    if (latestSnapshot)
    {
        baseIndex = latestSnapshot->lastIncludedIndex + 1;
    }

    return baseIndex + log.size();
}


std::vector<logEntry> RaftNode::getLogEntries(int startIndex, int endIndex) const

{
    std::vector<logEntry> entries;

    int baseIndex = 0;

    if (latestSnapshot)

    {
        baseIndex = latestSnapshot->lastIncludedIndex + 1;
    }

    // Convert global indices to log array indices

    int logStart = startIndex - baseIndex;
    int logEnd = endIndex - baseIndex;

    logStart = std::max(0, logStart);
    logEnd = std::min((int)log.size(), logEnd);

    if (logStart >= logEnd || logStart >= (int)log.size())
    {
        return entries; 
    }

    entries.insert(entries.end(),log.begin() + logStart,log.begin() + logEnd);

    return entries;
}

void RaftNode::appendLogEntry(const logEntry &entry)
{
    std::lock_guard<std::mutex> lock(mtx);

    log.push_back(entry);
}


void RaftNode::truncateLogSafe(int lastIncludedIndex)
{

    int oldSnapshotIndex = latestSnapshot ? latestSnapshot->lastIncludedIndex : -1;

    if (lastIncludedIndex <= oldSnapshotIndex)
    {
        Logger::info(id, getRoleString(), currentTerm, "Skipping truncation at " + std::to_string(lastIncludedIndex) + " (current snapshot at " + std::to_string(oldSnapshotIndex) + ")");
        return;
    }

    int baseIndex = oldSnapshotIndex + 1;
    int truncateAtLogIndex = lastIncludedIndex - baseIndex + 1;

    Logger::info(id, getRoleString(), currentTerm, "Truncating: lastIncludedIndex=" + std::to_string(lastIncludedIndex) + ", oldSnapshotIndex=" + std::to_string(oldSnapshotIndex) + ", baseIndex=" + std::to_string(baseIndex) + ", truncateAtLogIndex=" + std::to_string(truncateAtLogIndex) + ", log.size()=" + std::to_string(log.size()));

    if (truncateAtLogIndex > 0 && truncateAtLogIndex <= (int)log.size())
    {
        log.erase(log.begin(), log.begin() + truncateAtLogIndex);

        Logger::info(id, getRoleString(), currentTerm, "Truncated log up to index " + std::to_string(lastIncludedIndex) + ", " + std::to_string(log.size()) + " entries remaining");
    }
}

void RaftNode::shutdownNode()
{

    {
        std::lock_guard<std::mutex> lock(mtx);

        stopTimer = true;
        runningHeartbeats = false;
        stopRPC = true;
    }

    Logger::info(id, getRoleString(), currentTerm, "Starting shutdown...");
    cv.notify_all();

    if (serverSocket != -1)
    {
        shutdown(serverSocket, SHUT_RDWR);
        close(serverSocket);
        serverSocket = -1;
    }

    try
    {

        if (heartbeatThread.joinable())
        {
            Logger::info(id, getRoleString(), currentTerm, "Joining heartbeat thread...");
            heartbeatThread.join();
            Logger::info(id, getRoleString(), currentTerm, "Heartbeat thread joined");
        }

        if (rpcServerThread.joinable())
        {
            Logger::info(id, getRoleString(), currentTerm, "Joining RPC server thread...");
            rpcServerThread.join();
            Logger::info(id, getRoleString(), currentTerm, "RPC server thread joined");
        }
    }
    catch (const std::exception &e)
    {
        Logger::error(id, getRoleString(), currentTerm, "Exception during thread join: " + std::string(e.what()));
    }

    Logger::info(id, getRoleString(), currentTerm, "Shutdown complete");
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
        metrics.currentTerm = currentTerm;
        metrics.votedFor = votedFor;
        metrics.updateRole("Follower", id);
        votedFor = -1;

        runningHeartbeats = false;
    }

    cv.notify_all();

    persistMetadata(shared_from_this());

    Logger::info(id, "Follower", newTerm, "Became FOLLOWER (term " + std::to_string(currentTerm) + ")");
}

void reset_timeout(std::shared_ptr<RaftNode> node)
{
    std::lock_guard<std::mutex> lock(node->mtx);
    if (node->shutdownRequested.load())
    {
        return;
    }

    node->lastHeartbeatTimePoint = Clock::now();

    node->electionTimeout = std::chrono::milliseconds(2000 + rand() % 2000);
    node->cv.notify_all();

    Logger::info(node->id, node->getRoleString(), node->currentTerm,
                 "Timeout reset, new timeout: " + std::to_string(node->electionTimeout.count()) + "ms");
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
                    auto snapshot = leader->latestSnapshot;
                    int nextIdx = leader->nextIndex[peerIdx];
                    
                    if (snapshot && nextIdx <= snapshot->lastIncludedIndex)
                    {
                        Logger::info(leader->id, "Leader", leader->currentTerm,
                                     "Peer " + std::to_string(peerIdx) + " needs snapshot (nextIdx=" + std::to_string(nextIdx) +
                                         ", snapshot=" + std::to_string(snapshot->lastIncludedIndex) + ")");

                        std::vector<char> snapBytes = serializeSnapshot(*snapshot);
                        int chunkSize = 512;

                        Logger::info(leader->id, "Leader", leader->currentTerm,
                                     "Starting snapshot transfer to node " + std::to_string(peerIdx) +
                                         ", total size: " + std::to_string(snapBytes.size()) +
                                         " bytes, chunk size: " + std::to_string(chunkSize));
                        bool snapshotSuccess = true;
                        int chunkCount = (snapBytes.size() + chunkSize - 1) / chunkSize;

                        for (int chunkNum = 0; chunkNum < chunkCount && snapshotSuccess; chunkNum++)
                        {
                            int offset = chunkNum * chunkSize;
                            int end = std::min(offset + chunkSize, (int)snapBytes.size());
                            std::vector<char> chunk(snapBytes.begin() + offset, snapBytes.begin() + end);

                            std::string chunkBase64 = base64Encode(chunk);

                            nlohmann::json j;
                            j["rpc"] = "InstallSnapshot";
                            j["term"] = leader->currentTerm;
                            j["leaderId"] = leader->id;
                            j["lastIncludedIndex"] = snapshot->lastIncludedIndex;
                            j["lastIncludedTerm"] = snapshot->lastIncludedTerm;
                            j["offset"] = offset;
                            j["data"] = chunkBase64;
                            j["done"] = (end == snapBytes.size());
                            j["chunkNum"] = chunkNum;
                            j["totalChunks"] = chunkCount;

                            std::string jsonStr = j.dump();

                            if (jsonStr.length() > 2040)
                            {
                                Logger::error(leader->id, "Leader", leader->currentTerm,
                                              "JSON too large: " + std::to_string(jsonStr.length()) + " bytes, reducing chunk size");
                                chunkSize = chunkSize / 2;
                                chunkCount = (snapBytes.size() + chunkSize - 1) / chunkSize;
                                chunkNum = -1;
                                continue;
                            }

                            Logger::info(leader->id, "Leader", leader->currentTerm,
                                         "Sending chunk " + std::to_string(chunkNum + 1) + "/" + std::to_string(chunkCount) +
                                             " (offset=" + std::to_string(offset) + ", size=" + std::to_string(chunk.size()) +
                                             ", JSON size=" + std::to_string(jsonStr.length()) + ")");

                            std::string snapRespStr = sendRPC("127.0.0.1", peerPort, jsonStr);
                            leader->metrics.installSnapshotsSent++;

                            if (snapRespStr.empty())
                            {
                                Logger::error(leader->id, "Leader", leader->currentTerm,
                                              "Empty response for snapshot chunk " + std::to_string(chunkNum + 1));
                                snapshotSuccess = false;
                                break;
                            }

                            try
                            {
                                auto snapRespJson = nlohmann::json::parse(snapRespStr);
                                bool accepted = snapRespJson.value("success", false);

                                if (!accepted)
                                {
                                    std::string error = snapRespJson.value("error", "unknown");
                                    Logger::error(leader->id, "Leader", leader->currentTerm,
                                                  "Snapshot chunk " + std::to_string(chunkNum + 1) + " rejected: " + error);
                                    snapshotSuccess = false;
                                    break;
                                }

                                if (end == snapBytes.size() && accepted)
                                {
                                    leader->nextIndex[peerIdx] = snapshot->lastIncludedIndex + 1;
                                    leader->matchIndex[peerIdx] = snapshot->lastIncludedIndex;
                                    Logger::info(leader->id, "Leader", leader->currentTerm,
                                                 "Successfully sent complete snapshot to follower " + std::to_string(peerIdx) +
                                                     " at index " + std::to_string(snapshot->lastIncludedIndex));
                                }
                            }
                            catch (const nlohmann::json::parse_error &e)
                            {
                                Logger::error(leader->id, "Leader", leader->currentTerm,
                                              "JSON parse error in snapshot response: " + std::string(e.what()));
                                snapshotSuccess = false;
                                break;
                            }
                        }

                        continue;
                    }

                    int prevIndex = nextIdx - 1;
                    int prevTerm = leader->getLogTerm(prevIndex);

                    int currentLogSize = leader->getLogSize();
                    json entries = json::array();

                    if (nextIdx < currentLogSize)
                    {
                        const int MAX_ENTRIES_PER_BATCH = 20;
                        int endIndex = std::min(nextIdx + MAX_ENTRIES_PER_BATCH, currentLogSize);
                        auto logEntries = leader->getLogEntries(nextIdx, endIndex);
                        for (const auto &entry : logEntries)
                        {
                            entries.push_back(entry);
                        }
                    }

                    AppendEntriesRPC heartbeat{
                        leader->currentTerm,
                        leader->id,
                        prevIndex,
                        prevTerm,
                        entries,
                        leader->commitIndex};

                    std::string responseStr = sendRPC("127.0.0.1", peerPort, nlohmann::json(heartbeat).dump());
                    leader->metrics.appendEntriesSent++;

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
                            leader->metrics.currentTerm = leader->currentTerm;
                            leader->metrics.votedFor = leader->votedFor;
                            leader->metrics.updateRole("Follower", leader->id);
                            leader->votedFor = -1;
                            leader->runningHeartbeats = false;

                            leader->lastHeartbeatTimePoint = Clock::now();
                        }
                        leader->cv.notify_all();
                        persistMetadata(leader);
                        return;
                    }

                    if (resp.success && !entries.empty())
                    {
                        leader->nextIndex[peerIdx] = nextIdx + entries.size();
                        leader->matchIndex[peerIdx] = nextIdx + entries.size() - 1;
                    }
                    else if (!resp.success)
                    {
                        if (leader->nextIndex[peerIdx] > 0)
                            leader->nextIndex[peerIdx]--;
                    }
                }
                catch (const std::exception &e)
                {
                    Logger::error(leader->id, leader->getRoleString(), leader->currentTerm,
                                  "Heartbeat Exception: " + std::string(e.what()));
                    if (leader->shutdownRequested.load())
                        break;
                }
            }

            if (leader->shutdownRequested.load())
            {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    catch (const std::exception &e)
    {
        Logger::error(leader->id, leader->getRoleString(), leader->currentTerm, "Heartbeat Exception: " + std::string(e.what()));
    }

    Logger::info(leader->id, leader->getRoleString(), leader->currentTerm, "Heartbeat thread Exiting for Node " + std::to_string(leader->id));
}
void start_election(std::shared_ptr<RaftNode> candidate, std::vector<std::shared_ptr<RaftNode>> &nodes)
{
    {
        std::lock_guard<std::mutex> lock(candidate->mtx);
        if (candidate->shutdownRequested.load())
        {
            Logger::info(candidate->id, "Candidate", candidate->currentTerm, "Skipping election - shutdown requested");
            return;
        }

        candidate->state = NodeState::CANDIDATE;
        candidate->metrics.currentTerm = candidate->currentTerm;
        candidate->metrics.votedFor = candidate->votedFor;
        candidate->metrics.electionCount++;
        candidate->metrics.updateRole("Candidate", candidate->id);
        candidate->currentTerm++;
        candidate->votedFor = candidate->id;
    }

    persistMetadata(candidate);
    int votes = 1;

    for (int peerPort : candidate->peerRpcPorts)
    {

        if (candidate->shutdownRequested.load())
        {
            Logger::info(candidate->id, "Candidate", candidate->currentTerm, "Aborting election - shutdown requested");
            return;
        }

        RequestVoteRPC req{
            candidate->currentTerm,
            candidate->id,
            candidate->getLogSize() - 1,                      
            candidate->getLogTerm(candidate->getLogSize() - 1)
        };

        std::string responseStr = sendRPC("127.0.0.1", peerPort, nlohmann::json(req).dump());
        candidate->metrics.requestVotesSent++;

        if (candidate->shutdownRequested.load())
        {
            Logger::info(candidate->id, "Candidate", candidate->currentTerm, "Aborting election after RPC - shutdown requested");
            return;
        }

        if (responseStr.empty())
        {
            Logger::error(candidate->id, "Candidate", candidate->currentTerm,
                          "Empty vote response from peer " + std::to_string(peerPort));
            continue;
        }
        try
        {
            auto respJson = nlohmann::json::parse(responseStr);
            RequestVoteResponse resp = respJson.get<RequestVoteResponse>();
            if (resp.term > candidate->currentTerm)
            {
                std::lock_guard<std::mutex> lock(candidate->mtx);

                if (candidate->shutdownRequested.load())
                {
                    return;
                }

                candidate->currentTerm = resp.term;
                candidate->state = NodeState::FOLLOWER;
                candidate->metrics.currentTerm = candidate->currentTerm;
                candidate->metrics.votedFor = candidate->votedFor;
                candidate->metrics.updateRole("Follower", candidate->id);
                candidate->votedFor = -1;
                persistMetadata(candidate);
                Logger::info(candidate->id, "Candidate", candidate->currentTerm,
                             "Step down to FOLLOWER (term " + std::to_string(resp.term) + ")");
                return;
            }
            if (resp.voteGranted)
            {
                votes++;
                Logger::info(candidate->id, "Candidate", candidate->currentTerm,
                             "Vote granted from peer " + std::to_string(peerPort));
            }
        }
        catch (std::exception &e)
        {
            Logger::error(candidate->id, "Candidate", candidate->currentTerm,
                          "Failed to parse vote response: " + std::string(e.what()));
        }
    }

    if (candidate->shutdownRequested.load())
    {
        Logger::info(candidate->id, "Candidate", candidate->currentTerm, "Not becoming leader - shutdown requested");
        return;
    }

    if (votes > nodes.size() / 2)
    {
        {
            std::lock_guard<std::mutex> lock(candidate->mtx);

            if (candidate->shutdownRequested.load())
            {
                Logger::info(candidate->id, "Candidate", candidate->currentTerm, "Not becoming leader - shutdown requested");
                return;
                return;
            }

            candidate->state = NodeState::LEADER;
            candidate->metrics.leaderId = candidate->id;
            candidate->metrics.updateRole("Leader", candidate->id);
            candidate->runningHeartbeats = true;
            candidate->leaderId = candidate->id;

            for (size_t i = 0; i < candidate->nextIndex.size(); i++)
            {
                candidate->nextIndex[i] = candidate->getLogSize();
                candidate->matchIndex[i] = -1;
            }
        }

        Logger::info(candidate->id, "Leader", candidate->currentTerm,
                     "Leader elected: Node " + std::to_string(candidate->id) + " Term: " + std::to_string(candidate->currentTerm));
        reset_timeout(candidate);

        if (candidate->heartbeatThread.joinable())
        {
            candidate->runningHeartbeats = false;
            Logger::info(candidate->id, "Leader", candidate->currentTerm, "Joining existing heartbeat thread...");
            candidate->heartbeatThread.join();
            Logger::info(candidate->id, "Leader", candidate->currentTerm, "Existing heartbeat thread joined");
            candidate->runningHeartbeats = true;
        }

        if (!candidate->shutdownRequested.load())
        {
            Logger::info(candidate->id, "Leader", candidate->currentTerm, "Starting new heartbeat thread...");
            candidate->heartbeatThread = std::thread(send_heartbeats, candidate, std::ref(nodes));
        }
        else
        {
            Logger::info(candidate->id, "Leader", candidate->currentTerm, "Not starting heartbeat thread - shutdown requested");
        }

        persistMetadata(candidate);
    }
}

std::string RaftNode::applyToStateMachine(const std::string &command)
{
    if (shutdownRequested.load())
        return "error: node shutting down";

    Logger::info(id, getRoleString(), currentTerm, "Applying command: " + command);

    std::istringstream iss(command);
    std::string cmd, key, value;
    iss >> cmd >> key;
    std::getline(iss, value);
    if (!value.empty() && value[0] == ' ')
        value.erase(0, 1);

    std::string result;

    if (cmd == "PUT")
    {
        {
            std::lock_guard<std::mutex> lock(store_mutex);
            store.put(key, value);
        }
        result = "OK";
    }
    else if (cmd == "GET")
    {
        std::optional<std::string> val;
        {
            std::lock_guard<std::mutex> lock(store_mutex);
            val = store.get(key);
        }
        if (val.has_value())
            result = val.value();
        else
            result = "key not found";
    }
    else if (cmd == "DELETE")
    {
        bool deleted;
        {
            std::lock_guard<std::mutex> lock(store_mutex);
            deleted = store.remove(key);
        }
        result = deleted ? "OK" : "key not found";
    }
    else
    {
        result = "error: unknown command";
    }

    return result;
}
std::vector<char> serializeSnapshot(const Snapshot &snap)
{
    std::ostringstream oss(std::ios::binary);

    // Metadata
    oss.write((char *)&snap.lastIncludedIndex, sizeof(snap.lastIncludedIndex));
    oss.write((char *)&snap.lastIncludedTerm, sizeof(snap.lastIncludedTerm));

    // State map
    int size = snap.kvState.size();
    oss.write((char *)&size, sizeof(size));

    for (auto &[k, v] : snap.kvState)
    {
        int klen = k.size();
        int vlen = v.size();
        oss.write((char *)&klen, sizeof(klen));
        oss.write(k.data(), klen);
        oss.write((char *)&vlen, sizeof(vlen));
        oss.write(v.data(), vlen);
    }

    std::string str = oss.str();
    return std::vector<char>(str.begin(), str.end());
}

std::string RaftNode::handleClientCommand(const std::string &clientId, int requestId, const std::string command, const std::string key, const std::string value)
{
    if (shutdownRequested.load())
        return "error: shutdown";
    if (state != NodeState::LEADER)
        return "error: not leader";

    {
        std::lock_guard<std::mutex> lock(clientMutex);
        if (clientLastRequest.count(clientId) && clientLastRequest[clientId] == requestId)
        {
            return clientResultCache[clientId];
        }
    }

    std::string sendCommand = command + " " + key + " " + value;

    int newIndex;
    {
        std::lock_guard<std::mutex> lock(mtx);
        newIndex = getLogSize(); // This will be the index of the new entry
        logEntry entry{currentTerm, newIndex, sendCommand};
        log.push_back(entry);
    }
    int successCount = 1;

    for (size_t i = 0; i < peerRpcPorts.size(); i++)
    {
        if (shutdownRequested.load())
            return "error: shutdown";

        int port = peerRpcPorts[i];
        int nextIdx = nextIndex[i];
        int prevIndex = nextIdx - 1;
        int prevTerm = getLogTerm(prevIndex);

        json entries = json::array();

        auto logEntries = getLogEntries(nextIdx, newIndex + 1);
        for (const auto &logEntry : logEntries)
        {
            entries.push_back(logEntry);
        }

        AppendEntriesRPC msg{currentTerm, id, prevIndex, prevTerm, entries, commitIndex};
        std::string responseStr = sendRPC("127.0.0.1", port, nlohmann::json(msg).dump());
        metrics.appendEntriesSent++;

        if (responseStr.empty())
            continue;

        try
        {
            auto respJson = nlohmann::json::parse(responseStr);
            AppendEntriesResponse resp = respJson.get<AppendEntriesResponse>();

            if (resp.term > currentTerm)
            {
                std::lock_guard<std::mutex> lock(mtx);
                currentTerm = resp.term;
                state = NodeState::FOLLOWER;
                metrics.currentTerm = currentTerm;
                metrics.votedFor = votedFor;
                metrics.updateRole("Follower", id);
                votedFor = -1;
                persistMetadata(shared_from_this());
                return "leader step down";
            }

            if (resp.success)
            {
                nextIndex[i] = newIndex + 1;
                matchIndex[i] = newIndex;
                successCount++;
            }
            else
            {
                if (nextIndex[i] > 0)
                    nextIndex[i]--;

                auto snapshot = latestSnapshot;

                if (snapshot && nextIndex[i] <= snapshot->lastIncludedIndex)
                {
                    std::vector<char> snapBytes = serializeSnapshot(*snapshot);

                    int chunkSize = 512;

                    Logger::info(id, "Leader", currentTerm,
                                 "Starting snapshot transfer to node " + std::to_string(i) +
                                     ", total size: " + std::to_string(snapBytes.size()) +
                                     " bytes, chunk size: " + std::to_string(chunkSize));

                    bool snapshotSuccess = true;
                    int chunkCount = (snapBytes.size() + chunkSize - 1) / chunkSize;

                    for (int chunkNum = 0; chunkNum < chunkCount && snapshotSuccess; chunkNum++)
                    {
                        int offset = chunkNum * chunkSize;
                        int end = std::min(offset + chunkSize, (int)snapBytes.size());
                        std::vector<char> chunk(snapBytes.begin() + offset, snapBytes.begin() + end);

                        std::string chunkBase64 = base64Encode(chunk);

                        nlohmann::json j;
                        j["rpc"] = "InstallSnapshot";
                        j["term"] = currentTerm;
                        j["leaderId"] = id;
                        j["lastIncludedIndex"] = snapshot->lastIncludedIndex;
                        j["lastIncludedTerm"] = snapshot->lastIncludedTerm;
                        j["offset"] = offset;
                        j["data"] = chunkBase64;
                        j["done"] = (end == snapBytes.size());
                        j["chunkNum"] = chunkNum;
                        j["totalChunks"] = chunkCount;

                        std::string jsonStr = j.dump();

                        if (jsonStr.length() > 2040) 
                        {
                            Logger::error(id, "Leader", currentTerm,
                                          "JSON too large: " + std::to_string(jsonStr.length()) + " bytes, reducing chunk size");

                            chunkSize = chunkSize / 2;
                            chunkCount = (snapBytes.size() + chunkSize - 1) / chunkSize;
                            chunkNum = -1;
                            continue;
                        }

                        Logger::info(id, "Leader", currentTerm,
                                     "Sending chunk " + std::to_string(chunkNum + 1) + "/" + std::to_string(chunkCount) +
                                         " (offset=" + std::to_string(offset) + ", size=" + std::to_string(chunk.size()) +
                                         ", JSON size=" + std::to_string(jsonStr.length()) + ")");

                        std::string snapRespStr = sendRPC("127.0.0.1", port, jsonStr);
                        metrics.installSnapshotsSent++;

                        if (snapRespStr.empty())
                        {
                            Logger::error(id, "Leader", currentTerm,
                                          "Empty response for snapshot chunk " + std::to_string(chunkNum + 1));
                            snapshotSuccess = false;
                            break;
                        }

                        try
                        {
                            auto snapRespJson = nlohmann::json::parse(snapRespStr);
                            bool accepted = snapRespJson.value("success", false);

                            if (!accepted)
                            {
                                std::string error = snapRespJson.value("error", "unknown");
                                Logger::error(id, "Leader", currentTerm,
                                              "Snapshot chunk " + std::to_string(chunkNum + 1) + " rejected: " + error);
                                snapshotSuccess = false;
                                break;
                            }

                            if (end == snapBytes.size() && accepted)
                            {
                                nextIndex[i] = snapshot->lastIncludedIndex + 1;
                                matchIndex[i] = snapshot->lastIncludedIndex;
                                Logger::info(id, "Leader", currentTerm,
                                             "Successfully sent complete snapshot to follower " + std::to_string(i) +
                                                 " at index " + std::to_string(snapshot->lastIncludedIndex));
                            }
                        }
                        catch (const nlohmann::json::parse_error &e)
                        {
                            Logger::error(id, "Leader", currentTerm,
                                          "JSON parse error in snapshot response: " + std::string(e.what()));
                            snapshotSuccess = false;
                            break;
                        }
                    }
                }
            }
        }
        catch (...)
        {
        }
    }

    if (shutdownRequested.load())
        return "error: shutdown";

    std::string applyResult = "";
    if (getLogTerm(newIndex) == currentTerm &&
        successCount > (peerRpcPorts.size() + 1) / 2)
    {
        std::lock_guard<std::mutex> lock(mtx);
        commitIndex = newIndex;
        while (lastApplied < commitIndex && !shutdownRequested.load())
        {
            lastApplied++;

            int logIdx = lastApplied;
            if (latestSnapshot)
            {
                logIdx = lastApplied - latestSnapshot->lastIncludedIndex - 1;
            }

            if (logIdx >= 0 && logIdx < (int)log.size())
            {
                applyResult = applyToStateMachine(log[logIdx].command);
            }
        }
        int totalLogSize = getLogSize();

        Logger::info(id, "Leader", currentTerm,
                     "After applying: totalLogSize=" + std::to_string(totalLogSize) +
                         ", log.size()=" + std::to_string(log.size()) +
                         ", lastApplied=" + std::to_string(lastApplied));

        if ((int)log.size() >= SNAPSHOT_THRESHOLD)
        {
            Logger::info(id, "Leader", currentTerm,
                         "STARTING snapshot creation at totalLogSize=" + std::to_string(totalLogSize) +
                             ", lastApplied=" + std::to_string(lastApplied));

            auto newSnapshot = std::make_shared<Snapshot>();
            newSnapshot->lastIncludedIndex = lastApplied;
            newSnapshot->lastIncludedTerm = getLogTerm(lastApplied);

            Logger::info(id, "Leader", currentTerm, "About to dump KV store...");
            {
                std::lock_guard<std::mutex> storeLock(store_mutex);
                newSnapshot->kvState = store.dumpToMap();
            }

            saveSnapshot(*newSnapshot, id);

            truncateLogSafe(newSnapshot->lastIncludedIndex);

            latestSnapshot = newSnapshot;

            store.writeAheadLog_truncate(newSnapshot->lastIncludedIndex);

            for (size_t i = 0; i < nextIndex.size(); i++)
            {
                if (nextIndex[i] <= newSnapshot->lastIncludedIndex)
                {
                    nextIndex[i] = newSnapshot->lastIncludedIndex + 1;
                }
            }

            persistMetadata(shared_from_this());

            Logger::info(id, "Leader", currentTerm,
                         "Created snapshot at index " + std::to_string(newSnapshot->lastIncludedIndex) +
                             " (term " + std::to_string(newSnapshot->lastIncludedTerm) + ")");
        }
        else
        {
            Logger::info(id, "Leader", currentTerm,
                         "Snapshot not triggered: " + std::to_string(totalLogSize) + " < " + std::to_string(SNAPSHOT_THRESHOLD));
        }

        {
            std::lock_guard<std::mutex> lock(clientMutex);
            clientLastRequest[clientId] = requestId;
            clientResultCache[clientId] = applyResult;
        }
    }
    else
    {
        return "error: not committed";
    }

    persistMetadata(shared_from_this());
    return applyResult;
}

void election_timer(std::shared_ptr<RaftNode> node,
                    std::vector<std::shared_ptr<RaftNode>> &nodes)
{
    try
    {
        while (!node->stopTimer && !node->shutdownRequested.load())
        {
            std::unique_lock<std::mutex> lock(node->mtx);

            if (node->state == NodeState::LEADER)
            {

                node->cv.wait(lock, [&]
                              { return node->state != NodeState::LEADER ||
                                       node->stopTimer ||
                                       node->shutdownRequested.load(); });
                continue;
            }

            auto now = Clock::now();
            auto timeSinceHeartbeat = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - node->lastHeartbeatTimePoint);

            if (timeSinceHeartbeat >= node->electionTimeout)
            {

                lock.unlock();

                if (node->stopTimer || node->shutdownRequested.load())
                {
                    break;
                }

                Logger::info(node->id, node->getRoleString(), node->currentTerm,
                             "Election timeout (" + std::to_string(timeSinceHeartbeat.count()) + "ms >= " +
                                 std::to_string(node->electionTimeout.count()) + "ms) → starting election");
                start_election(node, nodes);
            }
            else
            {

                auto remainingTime = node->electionTimeout - timeSinceHeartbeat;
                bool woken_up = node->cv.wait_for(lock, remainingTime, [&]
                                                  { return node->state == NodeState::LEADER ||
                                                           node->stopTimer ||
                                                           node->shutdownRequested.load(); });

                if (node->stopTimer || node->shutdownRequested.load() || woken_up)
                {
                    continue;
                }
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::error(node->id, node->getRoleString(), node->currentTerm, "Election timer Exception: " + std::string(e.what()));
    }

    Logger::info("Election timer for Node " + std::to_string(node->id) + " stopped");
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

    std::shared_ptr<RaftNode> leader = nullptr;

    auto start = std::chrono::steady_clock::now();
    while (!leader)
    {
        for (auto &node : nodes)
        {
            if (node->state == NodeState::LEADER)
            {
                leader = node;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() > 10)
        {
            Logger::info("No leader elected in 10 seconds!");
            return;
        }
    }

    Logger::info("Leader elected: Node " + std::to_string(leader->id));
    std::this_thread::sleep_for(std::chrono::seconds(3));

    while (!raftShutdownRequested.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    Logger::info("=== FINAL STATE ===");
    for (auto &node : nodes)
    {
        Logger::info(node->id, node->getRoleString(), node->currentTerm,
                     "lastApplied=" + std::to_string(node->lastApplied) +
                         ", commitIndex=" + std::to_string(node->commitIndex) +
                         ", log.size()=" + std::to_string(node->log.size()) +
                         ", snapshot=" + (node->latestSnapshot ? std::to_string(node->latestSnapshot->lastIncludedIndex) : "none"));
    }

    Logger::info("Shutting down Raft cluster...");

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

    Logger::info("Joining timer threads...");
    try
    {
        for (size_t i = 0; i < timers.size(); ++i)
        {
            if (timers[i].joinable())
            {
                Logger::info("Joining timer thread " + std::to_string(i) + "...");
                timers[i].join();
                Logger::info("Timer thread " + std::to_string(i) + " joined");
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("Exception joining timer threads: " + std::string(e.what()));
    }

    Logger::info("Shutting down individual nodes...");
    try
    {
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            Logger::info("Shutting down node " + std::to_string(i) + "...");
            nodes[i]->shutdownNode();
            Logger::info("Node " + std::to_string(i) + " shutdown complete");
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("Exception during node shutdown: " + std::string(e.what()));
    }

    Logger::info("Clearing node references...");
    nodes.clear();

    Logger::info("Raft cluster shutdown complete.");
}