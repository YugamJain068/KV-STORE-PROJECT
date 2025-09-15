#ifndef RPCSERVER_H
#define RPCSERVER_H

#pragma once

#include <vector>
#include <string>

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
    std::vector<std::string>entries;
    int leaderCommit;
};

struct RequestVoteResponse {
    int term;
    bool voteGranted;
};

struct AppendEntriesResponse {
    int term;
    bool success;
};

void startRaftRPCServer(int port, RaftNode* node);
void handle_node_client(int client_socket, RaftNode* node);
std::string sendRPC(const std::string &targetIp, int targetPort, const std::string &jsonPayload);

#endif
