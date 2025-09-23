#include "server.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include <errno.h>
#include <iostream>
#include <sstream>
#include <shared_mutex>
#include "kvstore.h"
#include "rpc_server.h"
#include <atomic>
#include <nlohmann/json.hpp>
#include "raft_node.h"

std::atomic<bool> shutdownServer{false};

int socket_fd;

std::vector<NodeInfo> raft_nodes = {
    {"127.0.0.1", 5000},
    {"127.0.0.1", 5001},
    {"127.0.0.1", 5002}};

std::string sendClientRequest(nlohmann::json msg)
{
    int attempts = 0;
    int current_node = rand() % raft_nodes.size();

    while (attempts < raft_nodes.size())
    {
        try
        {
            std::string responseStr = sendRPC(
                raft_nodes[current_node].host,
                raft_nodes[current_node].port,
                msg.dump());

            auto respJson = nlohmann::json::parse(responseStr);

            if (respJson["status"] == "OK")
                return responseStr;

            if (respJson["status"] == "redirect")
            {
                int leaderId = respJson.value("leaderId", -1);
                if (leaderId == -1)
                {
                    current_node = rand() % raft_nodes.size();
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                else
                    current_node = leaderId;
                continue;
            }

            current_node = rand() % raft_nodes.size();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        catch (...)
        {
            current_node = (current_node + 1) % raft_nodes.size();
        }
        attempts++;
    }
    return R"({"status": "error", "msg": "All nodes unreachable"})";
}

void handle_client(int client_socket)
{
    while (true)
    {
        char buffer[1024];
        int bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received <= 0)
        {
            close(client_socket);
            break;
        }
        buffer[bytes_received] = '\0';
        std::string command_line(buffer);
        std::istringstream iss(command_line);
        std::string command;
        iss >> command;

        if (command.empty())
            continue;

        std::string response;

        int current_node = rand() % raft_nodes.size();

        static std::unordered_map<int, int> clientRequestCounter;
        std::string clientId = "client-" + std::to_string(client_socket);
        int requestId = ++clientRequestCounter[client_socket];

        if (command == "PUT")
        {
            std::string key, value;
            iss >> key;
            std::getline(iss, value);
            if (!value.empty() && value[0] == ' ')
            {
                value.erase(0, 1);
            }
            if (key.empty() || value.empty())
            {
                response = "Usage: PUT <Key> <Value>\n";
            }
            else
            {
                nlohmann::json msg = nlohmann::json{
                    {"rpc", "ClientRequest"},
                    {"command", "PUT"},
                    {"key", key},
                    {"value", value},
                    {"clientId", clientId},
                    {"requestId", requestId}};

                std::string responseStr = sendClientRequest(msg);
                auto respJson = nlohmann::json::parse(responseStr);
                if (respJson["status"] == "OK")
                {
                    response = key + " added successfully.\n";
                }
                else if (respJson["status"] == "redirect")
                {
                    int leaderId = respJson["leaderId"].get<int>();
                    response = "Redirected to leader node " + std::to_string(leaderId) + "\n";
                }
                else
                {
                    response = key + " cannot be added/modified.\n";
                }
            }
        }

        else if (command == "GET")
        {
            std::string key;
            iss >> key;
            if (key.empty())
            {
                response = "Usage: GET <Key>\n";
            }
            else
            {
                nlohmann::json msg = nlohmann::json{
                    {"rpc", "ClientRequest"},
                    {"command", "GET"},
                    {"key", key},
                    {"value", ""},
                    {"clientId", clientId},
                    {"requestId", requestId}};

                std::string responseStr = sendClientRequest(msg);
                auto respJson = nlohmann::json::parse(responseStr);
                if (respJson["status"] == "OK")
                {
                    response = "value of " + key + " = " + respJson["value"].get<std::string>() + "\n";
                }
                else if (respJson["status"] == "redirect")
                {
                    int leaderId = respJson["leaderId"].get<int>();
                    response = "Redirected to leader node " + std::to_string(leaderId) + "\n";
                }
                else
                {
                    response = key + " does not exist.\n";
                }
            }
        }
        else if (command == "DELETE")
        {
            std::string key;
            iss >> key;
            if (key.empty())
            {
                response = "Usage: DELETE <Key>\n";
            }
            else
            {
                nlohmann::json msg = nlohmann::json{
                    {"rpc", "ClientRequest"},
                    {"command", "DELETE"},
                    {"key", key},
                    {"value", ""},
                    {"clientId", clientId},
                    {"requestId", requestId}};

                std::string responseStr = sendClientRequest(msg);
                auto respJson = nlohmann::json::parse(responseStr);
                if (respJson["status"] == "OK")
                {
                    response = response = key + " got deleted\n";
                }
                else if (respJson["status"] == "redirect")
                {
                    int leaderId = respJson["leaderId"].get<int>();
                    response = "Redirected to leader node " + std::to_string(leaderId) + "\n";
                }
                else
                {
                    response = key + " does not exist.\n";
                }
            }
        }
        else if (command == "EXIT-RAFT")
        {
            std::string password;
            iss >> password;

            if (password == "admin123") // or your real admin password
            {
                requestRaftShutdown();
                response = "Raft shutdown signal sent. Nodes will terminate shortly.\n";
                send(client_socket, response.c_str(), response.size(), 0);
                continue;
            }
            else
            {
                response = "Incorrect password.\n";
            }
        }
        else if (command == "EXIT-SERVER")
        {
            std::string password;
            iss >> password;

            if (password == "admin123") // replace with your real password
            {
                response = "Shutting down CLI server...\n";
                send(client_socket, response.c_str(), response.size(), 0);

                shutdownServer.store(true); // trigger accept loop shutdown
                close(client_socket);
                break;
            }
            else
            {
                response = "Invalid admin password.\n";
                send(client_socket, response.c_str(), response.size(), 0);
            }
        }
        else if (command == "EXIT")
        {
            response = "Exiting...\n";
            send(client_socket, response.c_str(), response.size(), 0);
            close(client_socket);
            break;
        }
        else
        {
            response = "Unknown command. Available: PUT, GET, DELETE, EXIT.\n";
        }
        send(client_socket, response.c_str(), response.size(), 0);
    }
}

void start_server(int port)
{
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
    {
        std::cerr << "failed to create socket\n";
        return;
    }

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

    std::cout << "Server started on port " << port << "\n";

    while (!shutdownServer.load())
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_socket = accept(socket_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_socket < 0)
        {
            if (shutdownServer.load())
                break;
            if (errno == EINTR)
                continue;
            std::cerr << "Accept failed\n";
            continue;
        }

        std::string ip_address = inet_ntoa(client_addr.sin_addr);
        uint16_t client_port = ntohs(client_addr.sin_port);
        std::cout << "Client connected: " << ip_address << ":" << client_port << "\n";

        // Handle client in detached thread
        std::thread client_thread([client_socket]()
                                  {
            handle_client(client_socket);

            // If shutdown triggered inside handle_client, unblock accept
            if (shutdownServer.load())
            {
                int dummy_sock = socket(AF_INET, SOCK_STREAM, 0);
                if (dummy_sock >= 0)
                {
                    struct sockaddr_in dummy_addr{};
                    dummy_addr.sin_family = AF_INET;
                    dummy_addr.sin_port = htons(4000); // server port
                    inet_pton(AF_INET, "127.0.0.1", &dummy_addr.sin_addr);
                    connect(dummy_sock, (struct sockaddr*)&dummy_addr, sizeof(dummy_addr));
                    close(dummy_sock);
                }
            } });
        client_thread.detach();
    }

    close(socket_fd);
    std::cout << "Server socket closed. Server exited cleanly.\n";
}
