#ifndef RPCSERVER_H
#define RPCSERVER_H

#pragma once

#include <vector>
#include <string>
#include <nlohmann/json.hpp>
#include "raft_node.h"

struct RaftNode;

struct RequestVoteRPC{
    int term;
    int candidateId;
    int lastLogIndex;
    int lastLogTerm;
};

struct AppendEntriesRPC{
    int term;
    int leaderId;
    int prevLogIndex;
    int prevLogTerm;
    std::vector<logEntry> entries;
    int leaderCommit;
};

void to_json(nlohmann::json &j, const RequestVoteRPC &r) ;
void to_json(nlohmann::json &j, const AppendEntriesRPC &r) ;

struct RequestVoteResponse {
    int term;
    bool voteGranted;
};
struct AppendEntriesResponse {
    int term;
    bool success;
};

void from_json(const nlohmann::json &j, RequestVoteResponse &r);
void from_json(const nlohmann::json &j, AppendEntriesResponse &r) ;


void startRaftRPCServer(int port, std::shared_ptr<RaftNode> node);
void handle_node_client(int client_socket, std::shared_ptr<RaftNode> node);
std::string sendRPC(const std::string &targetIp, int targetPort, const std::string &jsonPayload);

#endif
