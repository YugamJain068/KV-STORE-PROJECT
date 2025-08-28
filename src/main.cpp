#include "kvstore.h"
#include <iostream>
#include <thread>
#include <chrono>

int main()
{
    KVStore store;

    auto writer = [&store](int id)
    {
        for (int i = 0; i < 100; ++i)
        {
            store.put("key" + std::to_string(id), "value" + std::to_string(i));
        }
    };

    auto reader = [&store]()
    {
        for (int i = 0; i < 100; ++i)
        {
            if (auto val = store.get("key1"); val.has_value())
            {
                std::cout << "key1: " << *val << std::endl;
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 3; ++i)
        threads.emplace_back(writer, i);
    for (int i = 0; i < 2; ++i)
        threads.emplace_back(reader);

    for (auto &t : threads)
        t.join();

    if (auto val = store.get("key1"); val.has_value())
    {
        std::cout << "Final value of key1: " << *val << "\n";
    }
    else
    {
        std::cout << "key1 does not exist.\n";
    }
}
