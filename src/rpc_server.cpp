#include "rpc_server.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include <errno.h>
#include <iostream>
#include <thread>

std::string sendRPC(const std::string &targetIp, int targetPort, const std::string &jsonPayload){
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

void handle_node_client(int client_socket)
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
    std::string response = R"({ "status": "ok" })";
    send(client_socket, response.c_str(), response.size(), 0);
}

void startRaftRPCServer(int port)
{
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
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

    std::cout << "RPC Server started on port " << port << "\n";

    while (true)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_socket = accept(socket_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_socket < 0)
        {
            std::cerr << "Accept failed\n";
            continue;
        }
        std::string ip_address = inet_ntoa(client_addr.sin_addr);
        uint16_t client_port = ntohs(client_addr.sin_port);
        std::cout << "Client connected: " << ip_address << ":" << client_port << "\n";

        std::thread client_thread(handle_node_client, client_socket);
        client_thread.detach();
    }
    close(socket_fd);
}