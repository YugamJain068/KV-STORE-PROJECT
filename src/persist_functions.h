#ifndef PERSISTFUNCTION_H
#define PERSISTFUNCTION_H
#include <nlohmann/json.hpp>
#include <fstream>
#include "raft_node.h"



void persistMetadata(std::shared_ptr<RaftNode> node) {
    nlohmann::json j;
    j["currentTerm"] = node->currentTerm;
    j["votedFor"] = node->votedFor;
    j["logEntries"] = node->log;

    std::ofstream out(node->metadataFile);
    out << j.dump(4);
}

void loadMetadata(std::shared_ptr<RaftNode> node) {
    std::ifstream in(node->metadataFile);
    if (!in.is_open()) return; // first run
    nlohmann::json j;
    in >> j;

    node->currentTerm = j.value("currentTerm", 0);
    node->votedFor = j.value("votedFor", -1);
    node->log = j.value("logEntries", std::vector<logEntry>{});
}

#endif
