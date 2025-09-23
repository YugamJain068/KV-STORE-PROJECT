#include "persist_functions.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include "raft_node.h"

void persistMetadata(std::shared_ptr<RaftNode> node)
{
    nlohmann::json j;
    j["currentTerm"] = node->currentTerm;
    j["votedFor"] = node->votedFor;
    j["logEntries"] = node->log;

    // Save clientLastRequest
    {
        std::lock_guard<std::mutex> lock(node->clientMutex);
        j["clientLastRequest"] = nlohmann::json::object();
        for (const auto &p : node->clientLastRequest)
        {
            j["clientLastRequest"][p.first] = p.second;
        }

        // Save clientResultCache
        j["clientResultCache"] = nlohmann::json::object();
        for (const auto &p : node->clientResultCache)
        {
            j["clientResultCache"][p.first] = p.second;
        }
    }

    std::ofstream out(node->metadataFile);
    out << j.dump(4);
}

void loadMetadata(std::shared_ptr<RaftNode> node)
{
    std::ifstream in(node->metadataFile);
    if (!in.is_open())
        return; // first run
    nlohmann::json j;
    in >> j;

    node->currentTerm = j.value("currentTerm", 0);
    node->votedFor = j.value("votedFor", -1);
    node->log = j.value("logEntries", std::vector<logEntry>{});

    // Restore clientLastRequest
    if (j.contains("clientLastRequest"))
    {
        for (auto &el : j["clientLastRequest"].items())
        {
            node->clientLastRequest[el.key()] = el.value();
        }
    }

    // Restore clientResultCache
    if (j.contains("clientResultCache"))
    {
        for (auto &el : j["clientResultCache"].items())
        {
            node->clientResultCache[el.key()] = el.value();
        }
    }
}