#include "kvstore.h"
#include "wal.h"
#include <fstream>

KVStore store;

KVStore::KVStore() : writeAheadLog("walfile.wal")
{
    std::vector<LogEntry> Entries = writeAheadLog.loadAllEntries();
    for (const auto &entry : Entries)
    {
        const std::string &commandStr = entry.command;

        auto spacePos = commandStr.find(' ');
        if (spacePos == std::string::npos)
            continue;

        std::string command = commandStr.substr(0, spacePos);

        if (command == "PUT")
        {
            auto eqPos = commandStr.find('=', spacePos + 1);
            if (eqPos == std::string::npos)
                continue;

            std::string key = commandStr.substr(spacePos + 1, eqPos - (spacePos + 1));
            std::string value = commandStr.substr(eqPos + 1);

            if (key.empty())
                continue;

            std::unique_lock<std::shared_mutex> lock(mtx);
            mp[key] = value;
        }

        else if (command == "DELETE")
        {
            std::string key = commandStr.substr(spacePos + 1);
            if (key.empty())
                continue;

            std::unique_lock<std::shared_mutex> lock(mtx);
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
    std::unique_lock<std::shared_mutex> lock(mtx);
    mp[key] = value;
}

std::optional<std::string> KVStore::get(const std::string &key)
{
    std::shared_lock<std::shared_mutex> lock(mtx);
    auto it = mp.find(key);
    if (it != mp.end())
        return it->second;
    return std::nullopt;
}

bool KVStore::remove(const std::string &key)
{
    writeAheadLog.appendEntry("DELETE " + key);
    std::unique_lock<std::shared_mutex> lock(mtx);
    return mp.erase(key) > 0;
}


void KVStore::writeAheadLog_truncate(int lastIncludedIndex){
    writeAheadLog.truncateUpTo(lastIncludedIndex);
}
