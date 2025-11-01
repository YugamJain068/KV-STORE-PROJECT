#include "raft_node.h"
#include "persist_functions.h"
#include <iostream>
#include <thread>
#include <nlohmann/json.hpp>
#include "rpc_server.h"
#include "kvstore.h"
#include <unistd.h>
#include <sys/socket.h>
#include "snapshot.h"
#include "kvstore_global.h"
#include <fstream>
#include "decode_encodebase64.h"

using json = nlohmann::json;

using Clock = std::chrono::steady_clock;

std::mutex store_mutex;
std::atomic<bool> raftShutdownRequested(false);
const int SNAPSHOT_THRESHOLD = 100;

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

    // Load snapshot if it exists
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

        // Convert std::map to std::unordered_map
        auto tempMap = jSnap["state"].get<std::map<std::string, std::string>>();
        snap->kvState = std::unordered_map<std::string, std::string>(
            tempMap.begin(), tempMap.end());

        latestSnapshot = snap;

        // Apply to state machine
        store.loadFromMap(snap->kvState);
        lastApplied = snap->lastIncludedIndex;
        commitIndex = snap->lastIncludedIndex;

        std::cout << "[Node " << id << "] Restored snapshot: lastIncludedIndex="
                  << snap->lastIncludedIndex << ", entries=" << snap->kvState.size() << "\n";
    }
}
RaftNode::~RaftNode()
{
    if (shutdownRequested.exchange(true))
    {
        std::cout << "[Node " << id << "] Already shutting down, skipping\n";
        return;
    }
    else
    {
        shutdownNode();
    }
    std::cout << "[Node " << id << "] Destroyed\n";
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

        // Index is before snapshot - this shouldn't happen in normal operation

        return 0;
    }

    // Calculate position in actual log array

    int logIndex = index;

    if (latestSnapshot)

    {

        logIndex = index - latestSnapshot->lastIncludedIndex - 1;
    }

    // Check bounds

    if (logIndex < 0 || logIndex >= (int)log.size())

    {

        return 0; // Invalid index
    }

    return log[logIndex].term;
}

// Also update getLogSize() to account for snapshot:

int RaftNode::getLogSize() const

{

    int baseIndex = 0;

    if (latestSnapshot)

    {

        baseIndex = latestSnapshot->lastIncludedIndex + 1;
    }

    return baseIndex + log.size();
}

// And update getLogEntries to handle snapshot offset:

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

    // Clamp to valid range

    logStart = std::max(0, logStart);

    logEnd = std::min((int)log.size(), logEnd);

    if (logStart >= logEnd || logStart >= (int)log.size())

    {

        return entries; // Empty
    }

    entries.insert(entries.end(),

                   log.begin() + logStart,

                   log.begin() + logEnd);

    return entries;
}

// Update appendLogEntry to use global indexing:

void RaftNode::appendLogEntry(const logEntry &entry)

{

    std::lock_guard<std::mutex> lock(mtx);

    log.push_back(entry);
}

// Update truncateLogSafe to properly handle snapshot:

void RaftNode::truncateLogSafe(int lastIncludedIndex)
{

    // Get the OLD snapshot index (before updating)
    int oldSnapshotIndex = latestSnapshot ? latestSnapshot->lastIncludedIndex : -1;

    // Only skip if we're moving backward relative to OLD snapshot
    if (lastIncludedIndex <= oldSnapshotIndex)
    {
        std::cout << "[Node " << id << "] Skipping truncation at " << lastIncludedIndex
                  << " (current snapshot at " << oldSnapshotIndex << ")\n";
        return;
    }

    // Calculate how many entries to remove from the beginning
    int baseIndex = oldSnapshotIndex + 1;
    int truncateAtLogIndex = lastIncludedIndex - baseIndex + 1;

    std::cout << "[Node " << id << "] Truncating: lastIncludedIndex=" << lastIncludedIndex
              << ", oldSnapshotIndex=" << oldSnapshotIndex
              << ", baseIndex=" << baseIndex
              << ", truncateAtLogIndex=" << truncateAtLogIndex
              << ", log.size()=" << log.size() << "\n";

    if (truncateAtLogIndex > 0 && truncateAtLogIndex <= (int)log.size())
    {
        log.erase(log.begin(), log.begin() + truncateAtLogIndex);

        std::cout << "[Node " << id << "] Truncated log up to index "
                  << lastIncludedIndex << ", " << log.size()
                  << " entries remaining\n";
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

    std::cout << "[Node " << id << "] Starting shutdown...\n";

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
            std::cout << "[Node " << id << "] Joining heartbeat thread...\n";
            heartbeatThread.join();
            std::cout << "[Node " << id << "] Heartbeat thread joined\n";
        }

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
                    auto snapshot = leader->latestSnapshot;
                    int nextIdx = leader->nextIndex[peerIdx];

                    // FIX: Check if follower needs snapshot BEFORE trying AppendEntries
                    if (snapshot && nextIdx <= snapshot->lastIncludedIndex)
                    {
                        std::cout << "[Leader " << leader->id << "] Peer " << peerIdx
                                  << " needs snapshot (nextIdx=" << nextIdx
                                  << ", snapshot=" << snapshot->lastIncludedIndex << ")\n";

                        std::vector<char> snapBytes = serializeSnapshot(*snapshot);
                        int chunkSize = 512;

                        std::cout << "[Leader " << leader->id << "] Starting snapshot transfer to node " << peerIdx
                                  << ", total size: " << snapBytes.size()
                                  << " bytes, chunk size: " << chunkSize << "\n";

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
                                std::cerr << "[Leader " << leader->id << "] JSON too large: " << jsonStr.length()
                                          << " bytes, reducing chunk size\n";
                                chunkSize = chunkSize / 2;
                                chunkCount = (snapBytes.size() + chunkSize - 1) / chunkSize;
                                chunkNum = -1;
                                continue;
                            }

                            std::cout << "[Leader " << leader->id << "] Sending chunk " << (chunkNum + 1)
                                      << "/" << chunkCount << " (offset=" << offset
                                      << ", size=" << chunk.size() << ", JSON size=" << jsonStr.length() << ")\n";

                            std::string snapRespStr = sendRPC("127.0.0.1", peerPort, jsonStr);

                            if (snapRespStr.empty())
                            {
                                std::cerr << "[Leader " << leader->id << "] Empty response for snapshot chunk " << (chunkNum + 1) << "\n";
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
                                    std::cerr << "[Leader " << leader->id << "] Snapshot chunk " << (chunkNum + 1)
                                              << " rejected: " << error << "\n";
                                    snapshotSuccess = false;
                                    break;
                                }

                                if (end == snapBytes.size() && accepted)
                                {
                                    leader->nextIndex[peerIdx] = snapshot->lastIncludedIndex + 1;
                                    leader->matchIndex[peerIdx] = snapshot->lastIncludedIndex;
                                    std::cout << "[Leader " << leader->id << "] Successfully sent complete snapshot to follower "
                                              << peerIdx << " at index " << snapshot->lastIncludedIndex << "\n";
                                }
                            }
                            catch (const nlohmann::json::parse_error &e)
                            {
                                std::cerr << "[Leader " << leader->id << "] JSON parse error in snapshot response: " << e.what() << "\n";
                                snapshotSuccess = false;
                                break;
                            }
                        }

                        // Skip AppendEntries for this peer in this iteration
                        continue;
                    }

                    // Normal AppendEntries flow
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
                        // Decrement nextIndex and try again next iteration
                        if (leader->nextIndex[peerIdx] > 0)
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

        if (candidate->shutdownRequested.load())
        {
            std::cout << "[Candidate " << candidate->id << "] Aborting election - shutdown requested\n";
            return;
        }

        RequestVoteRPC req{
            candidate->currentTerm,
            candidate->id,
            candidate->getLogSize() - 1,                       // ✅ Use getLogSize() not log.size()
            candidate->getLogTerm(candidate->getLogSize() - 1) // ✅ Get term correctly
        };

        std::string responseStr = sendRPC("127.0.0.1", peerPort, nlohmann::json(req).dump());

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

    if (candidate->shutdownRequested.load())
    {
        std::cout << "[Candidate " << candidate->id << "] Not becoming leader - shutdown requested\n";
        return;
    }

    if (votes > nodes.size() / 2)
    {
        {
            std::lock_guard<std::mutex> lock(candidate->mtx);

            if (candidate->shutdownRequested.load())
            {
                std::cout << "[Candidate " << candidate->id << "] Not becoming leader - shutdown requested\n";
                return;
            }

            candidate->state = NodeState::LEADER;
            candidate->runningHeartbeats = true;
            candidate->leaderId = candidate->id;

            for (size_t i = 0; i < candidate->nextIndex.size(); i++)
            {
                candidate->nextIndex[i] = candidate->getLogSize();
                candidate->matchIndex[i] = -1;
            }
        }

        std::cout << "Leader elected: Node " << candidate->id
                  << " Term: " << candidate->currentTerm << "\n";
        reset_timeout(candidate);

        if (candidate->heartbeatThread.joinable())
        {
            candidate->runningHeartbeats = false;
            std::cout << "[Node " << candidate->id << "] Joining existing heartbeat thread...\n";
            candidate->heartbeatThread.join();
            std::cout << "[Node " << candidate->id << "] Existing heartbeat thread joined\n";
            candidate->runningHeartbeats = true;
        }

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

std::string RaftNode::applyToStateMachine(const std::string &command)
{
    if (shutdownRequested.load())
        return "error: node shutting down";

    std::cout << "[Node " << id << "] Applying command: " << command << "\n";

    std::istringstream iss(command);
    std::string cmd, key, value;
    iss >> cmd >> key;
    std::getline(iss, value);
    if (!value.empty() && value[0] == ' ')
        value.erase(0, 1); // remove leading space

    std::string result;

    if (cmd == "PUT")
    {
        {
            std::lock_guard<std::mutex> lock(store_mutex); // thread-safe
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
            deleted = store.remove(key); // your delete returns bool
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
            return clientResultCache[clientId]; // return cached result
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

                    int chunkSize = 512; // Conservative chunk size

                    std::cout << "[Leader " << id << "] Starting snapshot transfer to node " << i
                              << ", total size: " << snapBytes.size()
                              << " bytes, chunk size: " << chunkSize << "\n";

                    bool snapshotSuccess = true;
                    int chunkCount = (snapBytes.size() + chunkSize - 1) / chunkSize;

                    for (int chunkNum = 0; chunkNum < chunkCount && snapshotSuccess; chunkNum++)
                    {
                        int offset = chunkNum * chunkSize;
                        int end = std::min(offset + chunkSize, (int)snapBytes.size());
                        std::vector<char> chunk(snapBytes.begin() + offset, snapBytes.begin() + end);

                        std::string chunkBase64 = base64Encode(chunk);

                        // Validate the final JSON size before sending
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

                        // Safety check: ensure JSON fits in buffer
                        if (jsonStr.length() > 2040) // Leave small safety margin
                        {
                            std::cerr << "[Leader " << id << "] JSON too large: " << jsonStr.length()
                                      << " bytes, reducing chunk size\n";
                            chunkSize = chunkSize / 2;
                            chunkCount = (snapBytes.size() + chunkSize - 1) / chunkSize;
                            chunkNum = -1; // Restart loop
                            continue;
                        }

                        std::cout << "[Leader " << id << "] Sending chunk " << (chunkNum + 1)
                                  << "/" << chunkCount << " (offset=" << offset
                                  << ", size=" << chunk.size() << ", JSON size=" << jsonStr.length() << ")\n";

                        std::string snapRespStr = sendRPC("127.0.0.1", port, jsonStr);

                        if (snapRespStr.empty())
                        {
                            std::cerr << "[Leader " << id << "] Empty response for snapshot chunk " << (chunkNum + 1) << "\n";
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
                                std::cerr << "[Leader " << id << "] Snapshot chunk " << (chunkNum + 1)
                                          << " rejected: " << error << "\n";
                                snapshotSuccess = false;
                                break;
                            }

                            if (end == snapBytes.size() && accepted)
                            {
                                nextIndex[i] = snapshot->lastIncludedIndex + 1;
                                matchIndex[i] = snapshot->lastIncludedIndex;
                                std::cout << "[Leader " << id << "] Successfully sent complete snapshot to follower "
                                          << i << " at index " << snapshot->lastIncludedIndex << "\n";
                            }
                        }
                        catch (const nlohmann::json::parse_error &e)
                        {
                            std::cerr << "[Leader " << id << "] JSON parse error in snapshot response: " << e.what() << "\n";
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

            // Calculate actual log index
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

        std::cout << "[Node " << id << "] After applying: totalLogSize=" << totalLogSize
                  << ", log.size()=" << log.size()
                  << ", lastApplied=" << lastApplied << "\n";

        // FIX: Check getLogSize() not log.size()
        if ((int)log.size() >= SNAPSHOT_THRESHOLD)
        {
            std::cout << "[Node " << id << "] STARTING snapshot creation at totalLogSize="
                      << totalLogSize << ", lastApplied=" << lastApplied << "\n";

            auto newSnapshot = std::make_shared<Snapshot>();
            newSnapshot->lastIncludedIndex = lastApplied;
            newSnapshot->lastIncludedTerm = getLogTerm(lastApplied);

            std::cout << "[Node " << id << "] About to dump KV store...\n";
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

            std::cout << "[Node " << id << "] Created snapshot at index "
                      << newSnapshot->lastIncludedIndex
                      << " (term " << newSnapshot->lastIncludedTerm << ")\n";
        }
        else
        {
            std::cout << "[Node " << id << "] Snapshot not triggered: "
                      << totalLogSize << " < " << SNAPSHOT_THRESHOLD << "\n";
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

                std::cout << "Node " << node->id << " election timeout ("
                          << timeSinceHeartbeat.count() << "ms >= "
                          << node->electionTimeout.count() << "ms) → starting election\n";
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

    std::shared_ptr<RaftNode> leader = nullptr;

    // Wait up to some timeout
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

        // Optional timeout to avoid infinite loop
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() > 10)
        {
            std::cout << "No leader elected in 10 seconds!\n";
            return;
        }
    }

    std::cout << "Leader elected: Node " << leader->id << "\n";
    std::this_thread::sleep_for(std::chrono::seconds(3));

    

    while (!raftShutdownRequested.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    // Print final state
    std::cout << "\n=== FINAL STATE ===\n";
    for (auto &node : nodes)
    {
        std::cout << "Node " << node->id
                  << ": lastApplied=" << node->lastApplied
                  << ", commitIndex=" << node->commitIndex
                  << ", log.size()=" << node->log.size()
                  << ", snapshot=" << (node->latestSnapshot ? std::to_string(node->latestSnapshot->lastIncludedIndex) : "none")
                  << "\n";
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