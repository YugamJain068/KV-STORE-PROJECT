#ifndef KVSTORE_H
#define KVSTORE_H

#include <unordered_map>
#include <string>
#include <optional>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <iostream>
#include <shared_mutex>
#include "wal.h"


class KVStore
{
public:
    KVStore();
    ~KVStore();
    void put(const std::string &key, const std::string &value);
    std::optional<std::string> get(const std::string &key);
    bool remove(const std::string &key);
    void saveToFile(const std::string &filename);
    void loadFromFile(const std::string &filename);
    void writeAheadLog_truncate(int lastIncludedIndex);

    template <typename Predicate>
    void filterAndPrint(Predicate pred) const
    {
        std::shared_lock lock(mtx);
        for (const auto &[k, v] : mp)
        {
            if (pred(k, v))
            {
                std::cout << k << " -> " << v << "\n";
            }
        }
    }
    std::unordered_map<std::string, std::string> dumpToMap() const
    {
        std::shared_lock lock(mtx);
        return mp; // copy the state
    }

    void loadFromMap(const std::unordered_map<std::string, std::string> &state)
    {
        std::unique_lock lock(mtx);
        mp = state; // replace with snapshot state
    }

private:
    std::unordered_map<std::string, std::string> mp;
    mutable std::shared_mutex mtx;
    WAL writeAheadLog;
};



#endif
