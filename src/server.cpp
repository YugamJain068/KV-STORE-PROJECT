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

KVStore store;
std::mutex store_mutex;

void handle_client(int client_socket)
{
    while (true)
    {
        std::string start="> ";
        send(client_socket,start.c_str(), start.size(), 0);
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

        if (command == "PUT")
        {
            std::string key, value;
            iss >> key;
            std::getline(iss, value);
            if (!value.empty() && value[0] == ' ')
            {
                value.erase(0, 1); // remove leading space
            }
            if (key.empty() || value.empty())
            {
                response = "Usage: PUT <Key> <Value>\n";
            }
            else
            {
                std::lock_guard<std::mutex> lock(store_mutex);
                store.put(key, value);
                response = " added successfully.\n";
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
                std::lock_guard<std::mutex> lock(store_mutex);
                if (auto val = store.get(key); val.has_value())
                {
                    response = "value of " + key + " " + *val + "\n";
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
                std::lock_guard<std::mutex> lock(store_mutex);
                if (store.remove(key))
                {
                    response = key + " deleted.\n";
                }
                else
                {
                    response = key + " does not exist.\n";
                }
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

    std::cout << "Server started on port " << port << "\n";

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
        
        std::thread client_thread(handle_client,client_socket);
        client_thread.detach();
    }
    close(socket_fd);
}