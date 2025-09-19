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
#include "persist_functions.h"

extern std::vector<std::shared_ptr<RaftNode>> nodes;

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

        char buffer[2048];
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

    char buffer[2048];

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

            if (term < node->currentTerm)
            {
                // Stale leader
                success = false;
                std::cout << "[Node " << node->id << "] Rejected stale AppendEntries from term "
                          << term << " (current: " << node->currentTerm << ")\n";
            }
            else
            {
                bool shouldResetTimeout = (term >= node->currentTerm);

                if (term > node->currentTerm || node->state == NodeState::CANDIDATE)
                {
                    std::cout << "[Node " << node->id << "] Stepping down due to AppendEntries from term "
                              << term << "\n";
                    node->becomeFollower(term);
                    shouldResetTimeout = true;
                }

                if (shouldResetTimeout)
                {
                    std::lock_guard<std::mutex> lock(node->mtx);
                    node->lastHeartbeatTimePoint = Clock::now();
                    node->electionTimeout = std::chrono::milliseconds(2000 + rand() % 2000);
                    node->cv.notify_all();

                    std::cout << "[Node " << node->id << "] Heartbeat received from Leader "
                              << leaderId << ", timeout reset to "
                              << node->electionTimeout.count() << "ms\n";
                }

                {
                    std::lock_guard<std::mutex> lock(node->mtx);

                    // Log consistency check
                    if (prevLogIndex >= 0)
                    {
                        if (prevLogIndex >= (int)node->log.size() ||
                            node->log[prevLogIndex].term != prevLogTerm)
                        {
                            success = false;
                            std::cout << "[Node " << node->id << "] Log inconsistency: prevLogIndex="
                                      << prevLogIndex << ", myLogSize=" << node->log.size() << "\n";
                        }
                        else
                        {
                            success = true;
                        }
                    }
                    else
                    {
                        success = true; // no prev entry to check
                    }

                    // Append new entries if consistent
                    if (success && !entries.empty())
                    {
                        int insertIndex = prevLogIndex + 1;

                        if (insertIndex < (int)node->log.size())
                        {
                            node->log.erase(node->log.begin() + insertIndex, node->log.end());
                            std::cout << "[Node " << node->id << "] Deleted conflicting entries from index "
                                      << insertIndex << "\n";
                        }

                        for (auto &entryJson : entries)
                        {
                            logEntry e = entryJson.get<logEntry>();
                            node->log.push_back(e);
                            std::cout << "[Node " << node->id << "] Appended entry: " << e.command << "\n";
                        }
                    }
                    
                    persistMetadata(node);

                    // Update commit index
                    if (success && leaderCommit > node->commitIndex)
                    {
                        int oldCommitIndex = node->commitIndex;
                        node->commitIndex = std::min(leaderCommit, (int)node->log.size() - 1);
                        std::cout << "[Node " << node->id << "] Commit index updated from "
                                  << oldCommitIndex << " to " << node->commitIndex << "\n";

                        while (node->lastApplied < node->commitIndex && !node->shutdownRequested.load())
                        {
                            node->lastApplied++;
                            node->applyToStateMachine(node->log[node->lastApplied].command);
                        }
                    }
                }
            }

            response = {
                {"term", node->currentTerm},
                {"success", success}};

            std::cout << "[Node " << node->id << "] AppendEntries response: success="
                      << success << ", term=" << node->currentTerm << "\n";
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
