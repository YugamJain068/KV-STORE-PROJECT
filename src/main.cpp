#include "kvstore.h"
#include "server.h"
#include <thread>
#include <chrono>
#include <sstream>
#include "raft_node.h"
#include "rpc_server.h"
#include "logger.h"

int main()
{
    Logger::init("raft_node.log", LogLevel::INFO);
    std::thread kvThread(start_server, 4000);
    std::thread raftAlgoThread(raftAlgorithm);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    kvThread.join();
    raftAlgoThread.join();
    Logger::close();
}
