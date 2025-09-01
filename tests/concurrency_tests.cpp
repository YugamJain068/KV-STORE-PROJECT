#include "../src/kvstore.h"
#include <gtest/gtest.h>
#include <thread>
#include <vector>

TEST(KVStoreConcurrencyTest, MultipleThreadsPutAndGet)
{
    KVStore store;

    const int numThreads = 8;
    const int operationsPerThread = 1000;

    auto writer = [&store, operationsPerThread](int threadId)
    {
        for (int i = 0; i < operationsPerThread; ++i)
        {
            store.put("key_" + std::to_string(threadId) + "_" + std::to_string(i),
                      "value_" + std::to_string(threadId));
        }
    };

    auto reader = [&store, operationsPerThread](int threadId)
    {
        for (int i = 0; i < operationsPerThread; ++i)
        {
            auto value = store.get("key_" + std::to_string(threadId) + "_" + std::to_string(i));
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t)
    {
        threads.emplace_back(writer, t);
        threads.emplace_back(reader, t);
    }

    for (auto &th : threads)
    {
        th.join();
    }

    for (int t = 0; t < numThreads; ++t)
    {
        for (int i = 0; i < operationsPerThread; ++i)
        {
            auto value = store.get("key_" + std::to_string(t) + "_" + std::to_string(i));
            EXPECT_TRUE(value.has_value()); // Should exist after all threads joined
        }
    }
}
