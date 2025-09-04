#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include "../src/kvstore.h"
#include "../src/server.h"

TEST(TcpServerTest, BasicCRUD)
{
    std::thread server_thread([]()
                              { start_server(8080); });

    std::this_thread::sleep_for(std::chrono::seconds(1));

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);
    ASSERT_GE(connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)), 0);

    auto send_and_recv = [&](const std::string &cmd) -> std::string
    {
        send(sock, cmd.c_str(), cmd.size(), 0);
        char buffer[1024];
        int bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (bytes > 0)
            buffer[bytes] = '\0';
        else
            buffer[0] = '\0';
        return std::string(buffer);
    };

    EXPECT_NE(send_and_recv("PUT key1 value1").find("key1 added successfully.\n"), std::string::npos);
    EXPECT_NE(send_and_recv("PUT key2 value2").find("key2 added successfully.\n"), std::string::npos);
    EXPECT_NE(send_and_recv("GET key1").find("value of key1 value1\n"), std::string::npos);
    EXPECT_NE(send_and_recv("DELETE key2").find("key2 deleted.\n"), std::string::npos);
    EXPECT_NE(send_and_recv("GET key2").find("key2 does not exist.\n"), std::string::npos);

    send_and_recv("EXIT");

    close(sock);

    if (server_thread.joinable())
        server_thread.detach();
}
