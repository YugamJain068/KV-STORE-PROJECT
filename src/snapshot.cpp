#include "snapshot.h"
#include "kvstore.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <iostream>

using json = nlohmann::json;

void saveSnapshot(const Snapshot &snapshot, int nodeId)
{
    json j;
    j["metadata"]["lastIncludedIndex"] = snapshot.lastIncludedIndex;
    j["metadata"]["lastIncludedTerm"] = snapshot.lastIncludedTerm;
    j["state"] = snapshot.kvState; // snapshot already holds state

    std::filesystem::create_directories("./snapshots");
    std::string filename = "./snapshots/node_" + std::to_string(nodeId) + ".snap";

    std::ofstream file(filename);
    file << j.dump(4); // pretty-print
    file.close();
}

void truncateLog(int lastIncludedIndex, std::vector<logEntry> &log)
{
    std::vector<logEntry> newLog;
    for (auto &entry : log)
    {
        if (entry.index > lastIncludedIndex)
        {
            newLog.push_back(entry);
        }
    }
    log = std::move(newLog);
}

Snapshot loadSnapshot(int nodeId)
{
    std::string filename = "./snapshots/node_" + std::to_string(nodeId) + ".snap";

    if (!std::filesystem::exists(filename))
    {
        throw std::runtime_error("No snapshot found for node " + std::to_string(nodeId));
    }

    std::ifstream file(filename);
    json j;
    file >> j;
    file.close();

    Snapshot snapshot;
    snapshot.lastIncludedIndex = j["metadata"]["lastIncludedIndex"];
    snapshot.lastIncludedTerm = j["metadata"]["lastIncludedTerm"];
    snapshot.kvState = j["state"].get<std::unordered_map<std::string, std::string>>();

    return snapshot;
}

void restoreFromSnapshot(int nodeId,
                         KVStore &store,
                         int &lastApplied,
                         int &commitIndex,
                         std::vector<logEntry> &log)
{
    try
    {
        Snapshot snapshot = loadSnapshot(nodeId);

        // Restore KVStore
        store.loadFromMap(snapshot.kvState);

        // Update Raft state
        lastApplied = snapshot.lastIncludedIndex;
        commitIndex = snapshot.lastIncludedIndex;

        // Reset logs after snapshot
        log.clear();

        std::cout << "[Node " << nodeId << "] Restored from snapshot (Index="
                  << snapshot.lastIncludedIndex
                  << ", Term=" << snapshot.lastIncludedTerm << ")\n";
    }
    catch (const std::exception &e)
    {
        std::cout << "[Node " << nodeId << "] No snapshot found, starting fresh.\n";
    }
}

