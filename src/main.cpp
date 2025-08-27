#include "kvstore.h"
#include <iostream>
#include <thread>
#include <chrono>

KVStore globalStore;

void heartbeat(bool &running)
{
    while (running)
    {
        std::cout << "heartbeat" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

void kvfunct()
{
    globalStore.put("user1", "Alice");
    globalStore.put("user2", "Bob");
    globalStore.put("lang", "C++");

    if (auto val = globalStore.get("user1"))
    {
        std::cout << "user1: " << *val << std::endl;
    }

    if (globalStore.remove("user2"))
    {
        std::cout << "user2 removed" << std::endl;
    }

    globalStore.saveToFile("data.txt");

    KVStore store2;
    store2.loadFromFile("data.txt");

    store2.filterAndPrint([](const auto &key, const auto &value)
                          { return key.find("user") != std::string::npos; });
}

int main()
{
    bool running = true;
    std::thread t1(heartbeat, std::ref(running));
    std::thread t2(kvfunct);
    t2.join();

    running = false;
    t1.join();

    std::cout << "main thread finished" << std::endl;
}
