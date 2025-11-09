#include "rpc_server.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <thread>
#include <nlohmann/json.hpp>
#include <fstream>
#include "snapshot.h"
#include "persist_functions.h"
#include "kvstore_global.h"
#include "decode_encodebase64.h"
#include "logger.h"

extern std::vector<std::shared_ptr<RaftNode>> nodes;

Snapshot deserializeSnapshot(const std::vector<char> &buffer)
{
    Snapshot snap;
    std::istringstream iss(std::string(buffer.begin(), buffer.end()), std::ios::binary);

    iss.read(reinterpret_cast<char *>(&snap.lastIncludedIndex), sizeof(snap.lastIncludedIndex));
    iss.read(reinterpret_cast<char *>(&snap.lastIncludedTerm), sizeof(snap.lastIncludedTerm));

    int size = 0;
    iss.read(reinterpret_cast<char *>(&size), sizeof(size));

    for (int i = 0; i < size; i++)
    {
        int klen = 0, vlen = 0;

        iss.read(reinterpret_cast<char *>(&klen), sizeof(klen));
        std::string key(klen, '\0');
        iss.read(&key[0], klen);

        iss.read(reinterpret_cast<char *>(&vlen), sizeof(vlen));
        std::string value(vlen, '\0');
        iss.read(&value[0], vlen);

        snap.kvState[key] = value;
    }

    return snap;
}

void to_json(nlohmann::json &j, const RequestVoteRPC &r)
{
    j = nlohmann::json{
        {"rpc", "RequestVote"},
        {"term", r.term},
        {"candidateId", r.candidateId},
        {"lastLogIndex", r.lastLogIndex},
        {"lastLogTerm", r.lastLogTerm}};
}
void to_json(nlohmann::json &j, const AppendEntriesRPC &r)
{
    j = nlohmann::json{
        {"rpc", "AppendEntries"},
        {"term", r.term},
        {"leaderId", r.leaderId},
        {"prevLogIndex", r.prevLogIndex},
        {"prevLogTerm", r.prevLogTerm},
        {"entries", r.entries},
        {"leaderCommit", r.leaderCommit}};
}

void from_json(const nlohmann::json &j, RequestVoteResponse &r)
{
    r.term = j.value("term", 0);
    r.voteGranted = j.value("voteGranted", false);
}
void from_json(const nlohmann::json &j, AppendEntriesResponse &r)
{
    r.term = j.value("term", 0);
    r.success = j.value("success", false);
}

void to_json(nlohmann::json &j, const InstallSnapshotRPC &p)
{
    j = nlohmann::json{
        {"term", p.term},
        {"leaderId", p.leaderId},
        {"lastIncludedIndex", p.lastIncludedIndex},
        {"lastIncludedTerm", p.lastIncludedTerm},
        {"offset", p.offset},
        {"data", base64Encode(p.data)},
        {"done", p.done}};
}

void from_json(const nlohmann::json &j, InstallSnapshotRPC &p)
{
    j.at("term").get_to(p.term);
    j.at("leaderId").get_to(p.leaderId);
    j.at("lastIncludedIndex").get_to(p.lastIncludedIndex);
    j.at("lastIncludedTerm").get_to(p.lastIncludedTerm);
    j.at("offset").get_to(p.offset);

    std::string chunkBase64;
    j.at("data").get_to(chunkBase64);
    p.data = base64Decode(chunkBase64);

    j.at("done").get_to(p.done);
}

#ifndef TESTING
std::string sendRPC(const std::string &targetIp, int targetPort, const std::string &jsonPayload)
{
    try
    {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
        {
            Logger::error("Socket creation failed");
            return "";
        }

        struct timeval tv{};
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));

        struct sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(targetPort);
        inet_pton(AF_INET, targetIp.c_str(), &server_addr.sin_addr);

        if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
        {
            Logger::error("Connect failed to " + targetIp + ":" + std::to_string(targetPort));
            close(sock);
            return "";
        }

        send(sock, jsonPayload.c_str(), jsonPayload.size(), 0);

        char buffer[64 * 1024];
        int bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0);
        std::string response;
        if (bytes_received > 0)
        {
            buffer[bytes_received] = '\0';
            response = buffer;
            Logger::info("[RPC Reply] " + response);
        }

        close(sock);
        return response;
    }
    catch (...)
    {
        Logger::error("Connect failed to " + targetIp + ":" + std::to_string(targetPort));
        return "{}";
    }
}
#endif

void handle_node_client(int client_socket, std::shared_ptr<RaftNode> node)
{
    if (node->stopRPC)
    {
        close(client_socket);
        return;
    }

    char buffer[8192];

    int bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received <= 0)
    {
        close(client_socket);
        return;
    }
    buffer[bytes_received] = '\0';
    std::string request(buffer);

    Logger::info("[RPC Received] " + request);

    nlohmann::json response;

    try
    {
        auto j = nlohmann::json::parse(request);
        std::string rpcType = j["rpc"];

        if (rpcType == "RequestVote")
        {

            int term = j.value("term", 0);
            int candidateId = j.value("candidateId", -1);
            int lastLogIndex = j.value("lastLogIndex", -1);
            int lastLogTerm = j.value("lastLogTerm", 0);

            bool voteGranted = false;

            node->metrics.requestVotesReceived++;

            if (term < node->currentTerm)
            {
                voteGranted = false;
            }
            else
            {
                if (term > node->currentTerm)
                {
                    node->becomeFollower(term);
                }
                bool notVotedYet = (node->votedFor == -1 || node->votedFor == candidateId);
                int myLastIndex = static_cast<int>(node->log.size()) - 1;
                int myLastTerm = (myLastIndex >= 0) ? node->log[myLastIndex].term : 0;
                bool candidateUpToDate =
                    (lastLogTerm > myLastTerm) ||
                    (lastLogTerm == myLastTerm && lastLogIndex >= myLastIndex);

                if (notVotedYet && candidateUpToDate)
                {
                    voteGranted = true;
                    node->votedFor = candidateId;
                    persistMetadata(node);
                    node->metrics.votesReceived++;
                }
            }

            response = {
                {"term", node->currentTerm},
                {"voteGranted", voteGranted}};
        }
        else if (rpcType == "AppendEntries")
        {
            int term = j.value("term", 0);
            int leaderId = j.value("leaderId", -1);
            int prevLogIndex = j.value("prevLogIndex", -1);
            int prevLogTerm = j.value("prevLogTerm", 0);
            auto entries = j.value("entries", nlohmann::json::array());
            int leaderCommit = j.value("leaderCommit", 0);

            bool success = false;

            node->metrics.appendEntriesReceived++;

            std::lock_guard<std::mutex> lock(node->mtx);

            if (node->receivingSnapshot.load() && node->snapshotLeaderId == leaderId)
            {
                Logger::info(node->id, node->getRoleString(), node->currentTerm,
                             "Rejecting AppendEntries - snapshot transfer in progress");

                std::string respStr = response.dump();
                send(client_socket, respStr.c_str(), respStr.size(), 0);
                Logger::info("[RPC Reply] " + respStr);
                close(client_socket);
                return;
            }

            if (term < node->currentTerm)
            {
                success = false;
                Logger::info(node->id, node->getRoleString(), node->currentTerm,
                             "Rejected stale AppendEntries from term " + std::to_string(term) +
                                 " (current: " + std::to_string(node->currentTerm) + ")");
            }
            else
            {
                
                if (term > node->currentTerm || node->state == NodeState::CANDIDATE)
                {
                    Logger::info(node->id, node->getRoleString(), node->currentTerm,
                                 "Stepping down due to AppendEntries from term " + std::to_string(term));
                    node->becomeFollower(term);
                }

                
                node->lastHeartbeatTimePoint = Clock::now();
                node->electionTimeout = std::chrono::milliseconds(2000 + rand() % 2000);
                node->leaderId = leaderId;
                node->cv.notify_all();
                Logger::info(node->id, node->getRoleString(), node->currentTerm,
                             "Heartbeat received from Leader " + std::to_string(leaderId) +
                                 ", timeout reset to " + std::to_string(node->electionTimeout.count()) + "ms");

                
                if (prevLogIndex >= 0)
                {
                    
                    if (node->latestSnapshot && prevLogIndex < node->latestSnapshot->lastIncludedIndex)
                    {
                        
                        success = false;
                        Logger::info(node->id, node->getRoleString(), node->currentTerm,
                                     "prevLogIndex " + std::to_string(prevLogIndex) +
                                         " is behind our snapshot at " + std::to_string(node->latestSnapshot->lastIncludedIndex));
                    }
                    else if (node->latestSnapshot && prevLogIndex == node->latestSnapshot->lastIncludedIndex)
                    {
                        
                        success = (prevLogTerm == node->latestSnapshot->lastIncludedTerm);
                        Logger::info(node->id, node->getRoleString(), node->currentTerm,
                                     "prevLogIndex matches snapshot boundary, success=" + std::to_string(success));
                    }
                    else
                    {
                        
                        int logIndex = prevLogIndex;
                        if (node->latestSnapshot)
                        {
                            logIndex = prevLogIndex - node->latestSnapshot->lastIncludedIndex - 1;
                        }

                        Logger::info(node->id, node->getRoleString(), node->currentTerm,
                                     "Consistency check: prevLogIndex=" + std::to_string(prevLogIndex) +
                                         ", prevLogTerm=" + std::to_string(prevLogTerm) +
                                         ", logIndex=" + std::to_string(logIndex) +
                                         ", log.size()=" + std::to_string(node->log.size()) +
                                         ", snapshot=" + std::to_string(node->latestSnapshot ? node->latestSnapshot->lastIncludedIndex : -1));

                        
                        if (logIndex < 0 || logIndex >= (int)node->log.size())
                        {
                            
                            success = false;
                            Logger::info(node->id, node->getRoleString(), node->currentTerm,
                                         "Log inconsistency: prevLogIndex=" + std::to_string(prevLogIndex) +
                                             ", calculated logIndex=" + std::to_string(logIndex) +
                                             ", log.size()=" + std::to_string(node->log.size()));
                        }
                        else if (node->log[logIndex].term != prevLogTerm)
                        {
                            
                            success = false;
                            Logger::info(node->id, node->getRoleString(), node->currentTerm,
                                         "Term mismatch at prevLogIndex=" + std::to_string(prevLogIndex) +
                                             " (expected term=" + std::to_string(prevLogTerm) +
                                             ", actual term=" + std::to_string(node->log[logIndex].term) + ")");
                        }
                        else
                        {
                            
                            success = true;
                        }
                    }
                }
                else
                {
                    
                    success = true;
                }

                
                if (success && !entries.empty())
                {
                    int insertIndex = prevLogIndex + 1;
                    int logInsertIndex = insertIndex;

                    if (node->latestSnapshot)
                    {
                        logInsertIndex = insertIndex - node->latestSnapshot->lastIncludedIndex - 1;
                    }

                    
                    if (logInsertIndex >= 0 && logInsertIndex < (int)node->log.size())
                    {
                        
                        logEntry firstNewEntry = entries[0].get<logEntry>();
                        if (node->log[logInsertIndex].term != firstNewEntry.term)
                        {
                            node->log.erase(node->log.begin() + logInsertIndex, node->log.end());
                        }
                        else
                        {
                            
                            Logger::info(node->id, node->getRoleString(), node->currentTerm,
                                         "Entry at index " + std::to_string(insertIndex) + " already exists and matches");
                            success = true;
                            goto skip_append;
                        }
                    }

                    for (auto &entryJson : entries)
                    {
                        logEntry e = entryJson.get<logEntry>();
                        int expectedLogIdx = e.index;
                        if (node->latestSnapshot)
                        {
                            expectedLogIdx = e.index - node->latestSnapshot->lastIncludedIndex - 1;
                        }

                        
                        if (expectedLogIdx >= (int)node->log.size())
                        {
                            node->log.push_back(e);
                            Logger::info(node->id, node->getRoleString(), node->currentTerm,
                                         "Appended entry: " + e.command);
                        }
                    }
                }
            skip_append:
                persistMetadata(node);

                
                if (success && leaderCommit > node->commitIndex)
                {
                    int newCommitIndex = std::min(leaderCommit, node->getLogSize() - 1);
                    node->commitIndex = newCommitIndex;

                    while (node->lastApplied < node->commitIndex)
                    {
                        node->lastApplied++;

                        int logIdx = node->lastApplied;
                        if (node->latestSnapshot)
                        {
                            logIdx = node->lastApplied - node->latestSnapshot->lastIncludedIndex - 1;
                        }

                        if (logIdx >= 0 && logIdx < (int)node->log.size())
                        {
                            node->applyToStateMachine(node->log[logIdx].command);
                        }
                    }

                    
                    int totalLogSize = node->getLogSize();

                    Logger::info(node->id, node->getRoleString(), node->currentTerm,
                                 "After applying: totalLogSize=" + std::to_string(totalLogSize) +
                                     ", log.size()=" + std::to_string(node->log.size()) +
                                     ", lastApplied=" + std::to_string(node->lastApplied));

                    if ((int)node->log.size() >= SNAPSHOT_THRESHOLD)
                    {
                        Logger::info(node->id, node->getRoleString(), node->currentTerm,
                                     "STARTING snapshot creation at totalLogSize=" + std::to_string(totalLogSize) +
                                         ", lastApplied=" + std::to_string(node->lastApplied));

                        auto newSnapshot = std::make_shared<Snapshot>();
                        newSnapshot->lastIncludedIndex = node->lastApplied;
                        newSnapshot->lastIncludedTerm = node->getLogTerm(node->lastApplied);

                        Logger::info(node->id, node->getRoleString(), node->currentTerm,
                                     "About to dump KV store...");
                        {
                            std::lock_guard<std::mutex> storeLock(store_mutex);
                            newSnapshot->kvState = store.dumpToMap();
                        }

                        saveSnapshot(*newSnapshot, node->id);

                        node->truncateLogSafe(newSnapshot->lastIncludedIndex);

                        node->latestSnapshot = newSnapshot;

                        store.writeAheadLog_truncate(newSnapshot->lastIncludedIndex);

                        for (size_t i = 0; i < node->nextIndex.size(); i++)
                        {
                            if (node->nextIndex[i] <= newSnapshot->lastIncludedIndex)
                            {
                                node->nextIndex[i] = newSnapshot->lastIncludedIndex + 1;
                            }
                        }

                        persistMetadata(node);

                        Logger::info(node->id, node->getRoleString(), node->currentTerm, 
    "Created snapshot at index " + std::to_string(newSnapshot->lastIncludedIndex) +
    " (term " + std::to_string(newSnapshot->lastIncludedTerm) + ")");
                    }
                }
            }

            response = {
                {"term", node->currentTerm},
                {"success", success}};

            Logger::info(node->id, node->getRoleString(), node->currentTerm, 
    "AppendEntries response: success=" + std::to_string(success) + 
    ", term=" + std::to_string(node->currentTerm));
        }

        else if (rpcType == "ClientRequest")
        {
            std::string command = j.value("command", "");
            std::string key = j.value("key", "");
            std::string value = j.value("value", "");
            std::string clientId = j.value("clientId", "");
            int requestId = j.value("requestId", 0);

            node->metrics.clientRequestsReceived++;

            if (node->state != NodeState::LEADER)
            {
                response = {
                    {"status", "redirect"},
                    {"leaderId", node->leaderId} 
                };
                node->metrics.clientRequestsRedirected++;
            }
            else
            {
                
                std::string result = node->handleClientCommand(clientId, requestId, command, key, value);

                if (result == "OK")
                {
                    response = {{"status", "OK"}};
                    node->metrics.clientRequestsSucceeded++;
                }
                else if (result == "key not found")
                {
                    response = {{"status", "error"}, {"msg", result}};
                    node->metrics.clientRequestsFailed++;
                }
                else
                {
                    
                    if (command == "GET")
                    {
                        response = {{"status", "OK"}, {"value", result}};
                        node->metrics.clientRequestsSucceeded++;
                    }
                    else
                    {
                        response = {{"status", "error"}, {"msg", result}};
                        node->metrics.clientRequestsFailed++;
                    }
                }
            }
        }
        else if (rpcType == "STATUS")
        {
            
            response = node->getStatus();
            response["status"] = "OK";
        }
        else if (rpcType == "METRICS")
        {
            
            node->updateLogMetrics();
            response = node->metrics.toJson();
            response["status"] = "OK";
        }
        else if (rpcType == "LOGSIZE")
        {
            
            int totalLogSize = node->getLogSize();
            int physicalLogSize = node->log.size();
            int snapshotIndex = node->latestSnapshot ? node->latestSnapshot->lastIncludedIndex : 0;

            response = {
                {"status", "OK"},
                {"total_log_size", totalLogSize},
                {"physical_log_size", physicalLogSize},
                {"snapshot_index", snapshotIndex},
                {"commit_index", node->commitIndex},
                {"last_applied", node->lastApplied}};
        }
        else if (rpcType == "SNAPSHOT_NOW")
        {
            
            bool success = node->triggerSnapshot();

            if (success)
            {
                response = {
                    {"status", "OK"},
                    {"message", "Snapshot created successfully"},
                    {"snapshot_index", node->latestSnapshot->lastIncludedIndex},
                    {"snapshot_term", node->latestSnapshot->lastIncludedTerm}};
            }
            else
            {
                response = {
                    {"status", "error"},
                    {"message", "Failed to create snapshot (log may be empty)"}};
            }
        }
        else if (rpcType == "GET_LOGS")
        {
            
            int count = j.value("count", 10); 
            int start = std::max(0, (int)node->log.size() - count);

            nlohmann::json logs = nlohmann::json::array();
            for (int i = start; i < (int)node->log.size(); i++)
            {
                logs.push_back({{"index", node->log[i].index},
                                {"term", node->log[i].term},
                                {"command", node->log[i].command}});
            }

            response = {
                {"status", "OK"},
                {"logs", logs},
                {"total_count", node->log.size()},
                {"returned_count", logs.size()}};
        }
        else if (rpcType == "CLUSTER_INFO")
        {
            
            nlohmann::json nodes = nlohmann::json::array();

            for (size_t i = 0; i < node->peerRpcPorts.size(); i++)
            {
                int nextIdx = (i < node->nextIndex.size()) ? node->nextIndex[i] : -1;
                int matchIdx = (i < node->matchIndex.size()) ? node->matchIndex[i] : -1;

                nodes.push_back({{"node_id", i},
                                 {"port", node->peerRpcPorts[i]},
                                 {"next_index", nextIdx},
                                 {"match_index", matchIdx}});
            }

            response = {
                {"status", "OK"},
                {"current_node_id", node->id},
                {"cluster_size", node->peerRpcPorts.size()},
                {"nodes", nodes},
                {"leader_id", node->leaderId}};
        }

        else if (rpcType == "InstallSnapshot")
        {
            try
            {
                int term = j.value("term", 0);
                int leaderId = j.value("leaderId", -1);
                int lastIncludedIndex = j.value("lastIncludedIndex", -1);
                int lastIncludedTerm = j.value("lastIncludedTerm", 0);
                int offset = j.value("offset", 0);
                std::string chunkBase64 = j.value("data", "");
                bool done = j.value("done", false);

                
                int chunkNum = j.value("chunkNum", -1);
                int totalChunks = j.value("totalChunks", -1);

                node->metrics.installSnapshotsReceived++;

                if (chunkNum >= 0)
                {
                   Logger::info(node->id, node->getRoleString(), node->currentTerm, 
    "Receiving chunk " + std::to_string(chunkNum + 1) + "/" + std::to_string(totalChunks));
                }

                if (term < node->currentTerm)
                {
                    response = {{"term", node->currentTerm}, {"success", false}, {"error", "stale_term"}};
                }
                else
                {
                    if (term > node->currentTerm)
                        node->becomeFollower(term);

                    
                    if (offset == 0)
                    {
                        node->snapshotBuffer.clear();
                        node->snapshotBuffer.reserve(1024 * 1024); 
Logger::info(node->id, node->getRoleString(), node->currentTerm, 
    "Starting snapshot reception, buffer cleared");
                    }

                    
                    std::vector<char> chunk;
                    try
                    {
                        if (!chunkBase64.empty())
                        {
                            chunk = base64Decode(chunkBase64);
                        }

                        
                        if (node->snapshotBuffer.size() < offset + chunk.size())
                        {
                            node->snapshotBuffer.resize(offset + chunk.size());
                        }

                        
                        if (!chunk.empty())
                        {
                            std::copy(chunk.begin(), chunk.end(), node->snapshotBuffer.begin() + offset);
                        }

                        Logger::info(node->id, node->getRoleString(), node->currentTerm, 
    "Chunk written at offset " + std::to_string(offset) +
    ", size " + std::to_string(chunk.size()) + ", buffer size now " +
    std::to_string(node->snapshotBuffer.size()));

                        if (done)
                        {
                            
                            Snapshot snap = deserializeSnapshot(node->snapshotBuffer);

                            
                            saveSnapshot(snap, node->id);

                            
                            {
                                std::lock_guard<std::mutex> lock(node->mtx);
                                store.loadFromMap(snap.kvState);
                                node->lastApplied = snap.lastIncludedIndex;
                                node->commitIndex = snap.lastIncludedIndex;
                                node->log.clear(); 

                                
                                auto snapshotPtr = std::make_shared<Snapshot>(snap);
                                node->latestSnapshot = snapshotPtr;
                            }

                            persistMetadata(node);
                            node->snapshotBuffer.clear();
                        }

                        response = {{"term", node->currentTerm}, {"success", true}};
                    }
                    catch (const std::exception &e)
                    {
Logger::error(node->id, node->getRoleString(), node->currentTerm, 
    "Chunk processing error: " + std::string(e.what()));                        response = {{"term", node->currentTerm}, {"success", false}, {"error", "processing_failed"}};
                    }
                }
            }
            catch (const std::exception &e)
            {
Logger::error(node->id, node->getRoleString(), node->currentTerm, 
    "InstallSnapshot handler error: " + std::string(e.what()));                response = {{"term", node->currentTerm}, {"success", false}, {"error", "handler_failed"}};
            }
        }
        else
        {
            response = {{"error", "Unknown RPC"}};
        }
    }
    catch (std::exception &e)
    {
Logger::error("JSON parse error: " + std::string(e.what()));
        response = {{"error", "Invalid JSON"}};
    }

    
    std::string respStr = response.dump();
    send(client_socket, respStr.c_str(), respStr.size(), 0);
    Logger::info("[RPC Reply] " + respStr);

    close(client_socket);
}

void startRaftRPCServer(int port, std::shared_ptr<RaftNode> node)
{
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
    {
Logger::error("Socket creation failed");
        return;
    }

    node->serverSocket = socket_fd;

    int opt = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    
    int flags = fcntl(socket_fd, F_GETFL, 0);
    fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
Logger::error("Bind failed");
        close(socket_fd);
        return;
    }

    if (listen(socket_fd, 5) < 0)
    {
Logger::error("Listen failed");
        close(socket_fd);
        return;
    }

Logger::info("RPC Server started on port " + std::to_string(port));

    while (!node->stopRPC)
    {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_socket = accept(socket_fd, (struct sockaddr *)&client_addr, &client_len);

        if (client_socket < 0)
        {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
Logger::error("Accept failed");
            continue;
        }

        std::thread(handle_node_client, client_socket, node).detach();
    }

    close(socket_fd);
    node->serverSocket = -1;
Logger::info("RPC Server on port " + std::to_string(port) + " stopped.");
}