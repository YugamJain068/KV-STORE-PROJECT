#include "Kvstore.h"
#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>

KVStore::KVStore()
{
    loggerThread = std::thread(&KVStore::loggerWorker, this);
}

KVStore::~KVStore()
{
    stopLogging = true;
    log_cv.notify_all();
    if (loggerThread.joinable())
        loggerThread.join();
}

void KVStore::put(const std::string &key, const std::string &value)
{
    {
        std::lock_guard<std::mutex> lock(log_mtx);
        logQueue.push("PUT " + key + "=" + value);
    }
    log_cv.notify_one();
    {
        std::lock_guard<std::mutex> lock(mtx);
        mp[key] = value;
    }
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
    {
        std::lock_guard<std::mutex> lock(log_mtx);
        logQueue.push("DELETE " + key);
    }
    log_cv.notify_one();
    
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



void KVStore::loggerWorker()
{
    std::ofstream logFile("kvstore.log", std::ios::app);
    if (!logFile.is_open())
        return;

    while (true)
    {
        std::unique_lock<std::mutex> lock(log_mtx);
        log_cv.wait(lock, [this]
                    { return !logQueue.empty() || stopLogging; });

        while (!logQueue.empty())
        {
            std::string msg = logQueue.front();
            logQueue.pop();
            lock.unlock(); // allow producers to continue
            logFile << msg << std::endl;
            lock.lock();
        }

        if (stopLogging && logQueue.empty())
            break;
    }
}
