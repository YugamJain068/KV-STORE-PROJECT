#include "rpc_server.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
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
                    node->currentTerm = term;
                    node->state = NodeState::FOLLOWER;
                    node->votedFor = -1;
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
            // Extract fields
            int term = j.value("term", 0); // use .value(key, default) to be safe
            int leaderId = j.value("leaderId", -1);
            int prevLogIndex = j.value("prevLogIndex", -1);
            int prevLogTerm = j.value("prevLogTerm", 0);
            auto entries = j.value("entries", nlohmann::json::array()); // default empty array
            int leaderCommit = j.value("leaderCommit", 0);

            // TODO: Apply Raft log consistency checks here
            bool success = true; // dummy for now

            response = {
                {"term", term},
                {"success", success}};
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
    if (socket_fd < 0) {
        std::cerr << "Failed to create socket\n";
        return;
    }

    node->serverSocket = socket_fd;

    // Reuse address
    int opt = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Bind failed\n";
        close(socket_fd);
        return;
    }

    if (listen(socket_fd, 5) < 0) {
        std::cerr << "Listen failed\n";
        close(socket_fd);
        return;
    }

    std::cout << "RPC Server started on port " << port << "\n";

    // Set accept timeout (1 second)
    struct timeval tv{};
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    while (!node->stopRPC)
    {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_socket = accept(socket_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR)
                continue;  // timeout or interrupted → check stopRPC
            std::cerr << "Accept failed\n";
            continue;
        }

        std::string ip_address = inet_ntoa(client_addr.sin_addr);
        uint16_t client_port = ntohs(client_addr.sin_port);
        std::cout << "Client connected: " << ip_address << ":" << client_port << "\n";

        // Launch client handler in detached thread
        std::thread([client_socket, node]() {
            handle_node_client(client_socket, node);
            close(client_socket); // close client socket after handling
        }).detach();
    }

    close(socket_fd); // clean up listening socket
    node->serverSocket = -1;
    std::cout << "RPC Server on port " << port << " stopped.\n";
}
