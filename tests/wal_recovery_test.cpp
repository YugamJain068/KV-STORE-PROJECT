#include <gtest/gtest.h>
#include "../src/kvstore.h"

TEST(walRecoveryTest, recovery_test)
{
    {
        KVStore store;
        store.put("key1", "value1");
        store.put("key2", "value2");
        store.remove("key1");
    }

    KVStore store;
    auto val1 = store.get("key1");
    EXPECT_FALSE(val1.has_value());
    auto val2 = store.get("key2");
    ASSERT_TRUE(val2.has_value());
    EXPECT_EQ(val2.value(), "value2");
}