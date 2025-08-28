#include "wal.h"
#include<fstream>
#include<utility>

WAL::WAL(const std::string &filename) :walFile(std::move(filename)){}

void WAL::appendEntry(const std::string &command){
    std::lock_guard<std::mutex> lock(wal_mtx);
    std::ofstream walFileStream(walFile, std::ios::app);
    if (!walFileStream.is_open())
        return;
    walFileStream << command << std::endl;
    walFileStream.flush();
}


std::vector<std::string> WAL::loadAllEntries() const{
    std::lock_guard<std::mutex> lock(wal_mtx);
    std::ifstream walFileStream(walFile);
    if (!walFileStream.is_open())
        return {};
    std::vector<std::string>commands;
    std::string line;
    while(std::getline(walFileStream,line)){
        commands.push_back(line);
    }
    return commands;
}