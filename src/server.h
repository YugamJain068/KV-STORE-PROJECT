#ifndef SERVER_H
#define SERVER_H

#include<string>
#include<vector>


struct NodeInfo {
    std::string host;
    int port;
};

extern std::vector<NodeInfo> raft_nodes;



void start_server(int port);

#endif