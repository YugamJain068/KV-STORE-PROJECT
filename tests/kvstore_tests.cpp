#include <gtest/gtest.h>
#include "../src/kvstore.h" // your main KV store header

TEST(KVStoreTest, PutAndGet) {
    KVStore store;
    store.put("key1", "value1");
    auto val = store.get("key1");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(val.value(), "value1");
}

TEST(KVStoreTest, DeleteKey) {
    KVStore store;
    store.put("key2", "value2");
    store.remove("key2");
    auto val = store.get("key2");
    EXPECT_FALSE(val.has_value());
}

TEST(KVStoreTest, WALPersistence) {
    {
        KVStore store; // this should create WAL file
        store.put("key3", "persisted");
    }
    KVStore recoveredStore; // should reload from WAL
    auto val = recoveredStore.get("key3");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(val.value(), "persisted");
}

TEST(KVStoreTest, ThreadSafety) {
    KVStore store;
    std::thread t1([&]() { for (int i=0;i<100;i++) store.put("k", "v1"); });
    std::thread t2([&]() { for (int i=0;i<100;i++) store.put("k", "v2"); });
    t1.join();
    t2.join();
    auto val = store.get("k");
    ASSERT_TRUE(val.has_value());
}
