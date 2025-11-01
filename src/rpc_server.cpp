#include "rpc_server.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string>
#include <errno.h>
#include <iostream>
#include <thread>
#include <nlohmann/json.hpp>
#include <fstream>
#include "snapshot.h"
#include "persist_functions.h"
#include "kvstore_global.h"
#include "decode_encodebase64.h"

extern std::vector<std::shared_ptr<RaftNode>> nodes;

Snapshot deserializeSnapshot(const std::vector<char> &buffer)
{
    Snapshot snap;
    std::istringstream iss(std::string(buffer.begin(), buffer.end()), std::ios::binary);

    // Metadata
    iss.read(reinterpret_cast<char *>(&snap.lastIncludedIndex), sizeof(snap.lastIncludedIndex));
    iss.read(reinterpret_cast<char *>(&snap.lastIncludedTerm), sizeof(snap.lastIncludedTerm));

    // State map size
    int size = 0;
    iss.read(reinterpret_cast<char *>(&size), sizeof(size));

    for (int i = 0; i < size; i++)
    {
        int klen = 0, vlen = 0;

        // Read key
        iss.read(reinterpret_cast<char *>(&klen), sizeof(klen));
        std::string key(klen, '\0');
        iss.read(&key[0], klen);

        // Read value
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

// Convert InstallSnapshotRPC → JSON
void to_json(nlohmann::json &j, const InstallSnapshotRPC &p)
{
    j = nlohmann::json{
        {"term", p.term},
        {"leaderId", p.leaderId},
        {"lastIncludedIndex", p.lastIncludedIndex},
        {"lastIncludedTerm", p.lastIncludedTerm},
        {"offset", p.offset},
        {"data", base64Encode(p.data)}, // encode raw bytes
        {"done", p.done}};
}

// Convert JSON → InstallSnapshotRPC
void from_json(const nlohmann::json &j, InstallSnapshotRPC &p)
{
    j.at("term").get_to(p.term);
    j.at("leaderId").get_to(p.leaderId);
    j.at("lastIncludedIndex").get_to(p.lastIncludedIndex);
    j.at("lastIncludedTerm").get_to(p.lastIncludedTerm);
    j.at("offset").get_to(p.offset);

    std::string chunkBase64;
    j.at("data").get_to(chunkBase64);
    p.data = base64Decode(chunkBase64); // decode back to raw bytes

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
            std::cerr << "Socket creation failed\n";
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
            std::cerr << "Connect failed to " << targetIp << ":" << targetPort << "\n";
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
            std::cout << "[RPC Reply] " << response << "\n";
        }

        close(sock);
        return response;
    }
    catch (...)
    {
        std::cerr << "Connect failed to " << targetIp << ":" << targetPort << "\n";
        return "{}"; // return empty JSON object instead of empty string
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

    std::cout << "[RPC Received] " << request << "\n";

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

            std::lock_guard<std::mutex> lock(node->mtx);

            if (node->receivingSnapshot.load() && node->snapshotLeaderId == leaderId)
            {
                std::cout << "[Node " << node->id << "] Rejecting AppendEntries - snapshot transfer in progress\n";
                response = {{"term", node->currentTerm}, {"success", false}};

                std::string respStr = response.dump();
                send(client_socket, respStr.c_str(), respStr.size(), 0);
                std::cout << "[RPC Reply] " << respStr << "\n";
                close(client_socket);
                return;
            }

            if (term < node->currentTerm)
            {
                // Stale leader
                success = false;
                std::cout << "[Node " << node->id << "] Rejected stale AppendEntries from term "
                          << term << " (current: " << node->currentTerm << ")\n";
            }
            else
            {
                // Step down if term is higher
                if (term > node->currentTerm || node->state == NodeState::CANDIDATE)
                {
                    std::cout << "[Node " << node->id << "] Stepping down due to AppendEntries from term "
                              << term << "\n";
                    node->becomeFollower(term);
                }

                // Reset heartbeat / timeout
                node->lastHeartbeatTimePoint = Clock::now();
                node->electionTimeout = std::chrono::milliseconds(2000 + rand() % 2000);
                node->leaderId = leaderId;
                node->cv.notify_all();
                std::cout << "[Node " << node->id << "] Heartbeat received from Leader "
                          << leaderId << ", timeout reset to "
                          << node->electionTimeout.count() << "ms\n";

                // --- Log consistency check ---
                if (prevLogIndex >= 0)
                {
                    // Check if prevLogIndex is covered by snapshot
                    if (node->latestSnapshot && prevLogIndex < node->latestSnapshot->lastIncludedIndex)
                    {
                        // Leader is behind our snapshot - shouldn't happen in normal operation
                        success = false;
                        std::cout << "[Node " << node->id << "] prevLogIndex " << prevLogIndex
                                  << " is behind our snapshot at " << node->latestSnapshot->lastIncludedIndex << "\n";
                    }
                    else if (node->latestSnapshot && prevLogIndex == node->latestSnapshot->lastIncludedIndex)
                    {
                        // prevLogIndex matches our snapshot boundary - this is valid
                        success = (prevLogTerm == node->latestSnapshot->lastIncludedTerm);
                        std::cout << "[Node " << node->id << "] prevLogIndex matches snapshot boundary, success="
                                  << success << "\n";
                    }
                    else
                    {
                        // prevLogIndex is beyond snapshot, check actual log
                        int logIndex = prevLogIndex;
                        if (node->latestSnapshot)
                        {
                            logIndex = prevLogIndex - node->latestSnapshot->lastIncludedIndex - 1;
                        }

                        std::cout << "[Node " << node->id << "] Consistency check: prevLogIndex=" << prevLogIndex
                                  << ", prevLogTerm=" << prevLogTerm
                                  << ", logIndex=" << logIndex
                                  << ", log.size()=" << node->log.size()
                                  << ", snapshot=" << (node->latestSnapshot ? node->latestSnapshot->lastIncludedIndex : -1);

                        // Check bounds and term match
                        if (logIndex < 0 || logIndex >= (int)node->log.size())
                        {
                            // Out of bounds
                            success = false;
                            std::cout << "[Node " << node->id << "] Log inconsistency: prevLogIndex="
                                      << prevLogIndex << ", calculated logIndex=" << logIndex
                                      << ", log.size()=" << node->log.size() << "\n";
                        }
                        else if (node->log[logIndex].term != prevLogTerm)
                        {
                            // Term mismatch
                            success = false;
                            std::cout << "[Node " << node->id << "] Term mismatch at prevLogIndex="
                                      << prevLogIndex << " (expected term=" << prevLogTerm
                                      << ", actual term=" << node->log[logIndex].term << ")\n";
                        }
                        else
                        {
                            // Everything matches
                            success = true;
                        }
                    }
                }
                else
                {
                    // prevLogIndex = -1 means empty log, always valid
                    success = true;
                }

                // --- Append entries if consistent ---
                if (success && !entries.empty())
                {
                    int insertIndex = prevLogIndex + 1;
                    int logInsertIndex = insertIndex;

                    if (node->latestSnapshot)
                    {
                        logInsertIndex = insertIndex - node->latestSnapshot->lastIncludedIndex - 1;
                    }

                    // Remove conflicting entries
                    if (logInsertIndex >= 0 && logInsertIndex < (int)node->log.size())
                    {
                        // Check if existing entry conflicts
                        logEntry firstNewEntry = entries[0].get<logEntry>();
                        if (node->log[logInsertIndex].term != firstNewEntry.term)
                        {
                            node->log.erase(node->log.begin() + logInsertIndex, node->log.end());
                        }
                        else
                        {
                            // Entry matches, skip appending duplicates
                            std::cout << "[Node " << node->id << "] Entry at index "
                                      << insertIndex << " already exists and matches\n";
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

                        // Only append if we don't have this entry
                        if (expectedLogIdx >= (int)node->log.size())
                        {
                            node->log.push_back(e);
                            std::cout << "[Node " << node->id << "] Appended entry: " << e.command << "\n";
                        }
                    }
                }
            skip_append:
                persistMetadata(node);

                // --- Update commit index safely ---
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

                    // Create snapshot if needed
                    int totalLogSize = node->getLogSize();

                    std::cout << "[Node " << node->id << "] After applying: totalLogSize="
                              << totalLogSize << ", log.size()=" << node->log.size()
                              << ", lastApplied=" << node->lastApplied << "\n";
                    if ((int)node->log.size() >= SNAPSHOT_THRESHOLD)
                    {
                        std::cout << "[Node " << node->id << "] STARTING snapshot creation at totalLogSize="
                                  << totalLogSize << ", lastApplied=" << node->lastApplied << "\n";

                        auto newSnapshot = std::make_shared<Snapshot>();
                        newSnapshot->lastIncludedIndex = node->lastApplied;
                        newSnapshot->lastIncludedTerm = node->getLogTerm(node->lastApplied);

                        std::cout << "[Node " << node->id << "] About to dump KV store...\n";
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

                        std::cout << "[Node " << node->id << "] Created snapshot at index "
                                  << newSnapshot->lastIncludedIndex
                                  << " (term " << newSnapshot->lastIncludedTerm << ")\n";
                    }
                }
            }

            response = {
                {"term", node->currentTerm},
                {"success", success}};

            std::cout << "[Node " << node->id << "] AppendEntries response: success="
                      << success << ", term=" << node->currentTerm << "\n";
        }

        else if (rpcType == "ClientRequest")
        {
            std::string command = j.value("command", "");
            std::string key = j.value("key", "");
            std::string value = j.value("value", "");
            std::string clientId = j.value("clientId", "");
            int requestId = j.value("requestId", 0);

            if (node->state != NodeState::LEADER)
            {
                response = {
                    {"status", "redirect"},
                    {"leaderId", node->leaderId} // send current leader ID for client
                };
            }
            else
            {
                // handleClientCommand now returns a string result from KV store
                std::string result = node->handleClientCommand(clientId, requestId, command, key, value);

                if (result == "OK")
                {
                    response = {{"status", "OK"}};
                }
                else if (result == "key not found")
                {
                    response = {{"status", "error"}, {"msg", result}};
                }
                else
                {
                    // for GET, return the value directly
                    if (command == "GET")
                        response = {{"status", "OK"}, {"value", result}};
                    else
                        response = {{"status", "error"}, {"msg", result}};
                }
            }
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

                // Optional: sequence tracking for debugging
                int chunkNum = j.value("chunkNum", -1);
                int totalChunks = j.value("totalChunks", -1);

                if (chunkNum >= 0)
                {
                    std::cout << "[Node " << node->id << "] Receiving chunk " << (chunkNum + 1)
                              << "/" << totalChunks << "\n";
                }

                if (term < node->currentTerm)
                {
                    response = {{"term", node->currentTerm}, {"success", false}, {"error", "stale_term"}};
                }
                else
                {
                    if (term > node->currentTerm)
                        node->becomeFollower(term);

                    // Reset buffer on first chunk
                    if (offset == 0)
                    {
                        node->snapshotBuffer.clear();
                        node->snapshotBuffer.reserve(1024 * 1024); // Pre-allocate 1MB
                        std::cout << "[Node " << node->id << "] Starting snapshot reception, buffer cleared\n";
                    }

                    // Decode and validate chunk
                    std::vector<char> chunk;
                    try
                    {
                        if (!chunkBase64.empty())
                        {
                            chunk = base64Decode(chunkBase64);
                        }

                        // Expand buffer if needed
                        if (node->snapshotBuffer.size() < offset + chunk.size())
                        {
                            node->snapshotBuffer.resize(offset + chunk.size());
                        }

                        // Copy chunk data
                        if (!chunk.empty())
                        {
                            std::copy(chunk.begin(), chunk.end(), node->snapshotBuffer.begin() + offset);
                        }

                        std::cout << "[Node " << node->id << "] Chunk written at offset " << offset
                                  << ", size " << chunk.size() << ", buffer size now "
                                  << node->snapshotBuffer.size() << "\n";

                        if (done)
                        {
                            // Deserialize snapshot
                            Snapshot snap = deserializeSnapshot(node->snapshotBuffer);

                            // Save to disk
                            saveSnapshot(snap, node->id);

                            // Update state machine and clear log
                            {
                                std::lock_guard<std::mutex> lock(node->mtx);
                                store.loadFromMap(snap.kvState);
                                node->lastApplied = snap.lastIncludedIndex;
                                node->commitIndex = snap.lastIncludedIndex;
                                node->log.clear(); // ✅ Clear ALL log entries

                                // Set snapshot
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
                        std::cerr << "[Node " << node->id << "] Chunk processing error: " << e.what() << "\n";
                        response = {{"term", node->currentTerm}, {"success", false}, {"error", "processing_failed"}};
                    }
                }
            }
            catch (const std::exception &e)
            {
                std::cerr << "[Node " << node->id << "] InstallSnapshot handler error: " << e.what() << "\n";
                response = {{"term", node->currentTerm}, {"success", false}, {"error", "handler_failed"}};
            }
        }
        else
        {
            response = {{"error", "Unknown RPC"}};
        }
    }
    catch (std::exception &e)
    {
        std::cerr << "JSON parse error: " << e.what() << "\n";
        response = {{"error", "Invalid JSON"}};
    }

    // Send JSON back to client
    std::string respStr = response.dump();
    send(client_socket, respStr.c_str(), respStr.size(), 0);
    std::cout << "[RPC Reply] " << respStr << "\n";

    close(client_socket);
}

void startRaftRPCServer(int port, std::shared_ptr<RaftNode> node)
{
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
    {
        std::cerr << "Socket creation failed\n";
        return;
    }

    node->serverSocket = socket_fd;

    int opt = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Non-blocking mode
    int flags = fcntl(socket_fd, F_GETFL, 0);
    fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        std::cerr << "Bind failed\n";
        close(socket_fd);
        return;
    }

    if (listen(socket_fd, 5) < 0)
    {
        std::cerr << "Listen failed\n";
        close(socket_fd);
        return;
    }

    std::cout << "RPC Server started on port " << port << "\n";

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
            std::cerr << "Accept failed\n";
            continue;
        }

        std::thread(handle_node_client, client_socket, node).detach();
    }

    close(socket_fd);
    node->serverSocket = -1;
    std::cout << "RPC Server on port " << port << " stopped.\n";
}