#include "Kvstore.h"
#include "wal.h"
#include <iostream>
#include <fstream>

KVStore::KVStore() : writeAheadLog("walfile.log")
{
    std::vector<std::string> Entries = writeAheadLog.loadAllEntries();
    for (auto &raw : Entries)
    {
        std::string entry = raw;
        if (!entry.empty() && entry.back() == '\r')
            entry.pop_back();
        if (entry.empty())
            continue;

        auto spacePos = entry.find(' ');
        if (spacePos == std::string::npos)
            continue;

        std::string command = entry.substr(0, spacePos);

        if (command == "PUT")
        {
            auto eqPos = entry.find('=', spacePos + 1);
            if (eqPos == std::string::npos)
                continue;

            std::string key = entry.substr(spacePos + 1, eqPos - (spacePos + 1));
            std::string value = entry.substr(eqPos + 1);

            if (key.empty())
                continue;

            std::lock_guard<std::mutex> lock(mtx);
            mp[key] = value;
        }

        else if (command == "DELETE")
        {
            std::string key = entry.substr(spacePos + 1);
            if (key.empty())
                continue;

            std::lock_guard<std::mutex> lock(mtx);
            mp.erase(key);
        }
        else{
            continue;
        }
    }
}

KVStore::~KVStore()
{
}

void KVStore::put(const std::string &key, const std::string &value)
{
    writeAheadLog.appendEntry("PUT " + key + "=" + value);
    std::lock_guard<std::mutex> lock(mtx);
    mp[key] = value;
}

std::optional<std::string> KVStore::get(const std::string &key)
{
    std::lock_guard<std::mutex> lock(mtx);
    auto it = mp.find(key);
    if (it != mp.end())
        return it->second;
    return std::nullopt;
}

bool KVStore::remove(const std::string &key)
{
    writeAheadLog.appendEntry("DELETE " + key);
    std::lock_guard<std::mutex> lock(mtx);
    return mp.erase(key) > 0;
}

void KVStore::saveToFile(const std::string &filename)
{
    std::lock_guard<std::mutex> lock(mtx);
    std::ofstream out(filename);
    for (const auto &[key, value] : mp)
    {
        out << key << "=" << value << std::endl;
    }
}

void KVStore::loadFromFile(const std::string &filename)
{
    std::ifstream in(filename);
    std::string line;
    std::lock_guard<std::mutex> lock(mtx);
    while (std::getline(in, line))
    {
        auto pos = line.find('=');
        if (pos != std::string::npos)
        {
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            mp[key] = std::move(value); // i can call put function here but it will cause self-deadlock because lock is acquired in loadFromFile function and then put function will also try to lock the same mutex resulting in deadlock
        }
    }
}
