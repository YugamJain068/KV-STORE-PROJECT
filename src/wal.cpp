#include "log_entry.h"
#include "wal.h"
#include <fstream>
#include <utility>

WAL::WAL(const std::string &filename) : walBinaryFile(std::move(filename)), lastIndex(0)
{
    std::ifstream file(walBinaryFile, std::ios::binary);
    if (!file.is_open())
    {
        return;
    }

    while (true)
    {
        LogEntry entry;
        if (!LogEntry::fromBinary(file, entry))
            break;
        cachedEntries.push_back(entry);   
        lastIndex = entry.index;
    }
}

void WAL::appendEntry(const std::string &command)
{
    std::lock_guard<std::mutex> lock(wal_mtx);

    LogEntry entry{1, ++lastIndex, command};
    std::string binaryData = entry.toBinary();

    std::ofstream walBinaryFileStream(walBinaryFile, std::ios::binary | std::ios::app);

    if (walBinaryFileStream.is_open())
    {
        walBinaryFileStream.write(binaryData.data(), binaryData.size());
        walBinaryFileStream.flush();
    }
    cachedEntries.push_back(entry);
}

std::vector<LogEntry> WAL::loadAllEntries() const
{
    std::lock_guard<std::mutex> lock(wal_mtx);
    return cachedEntries;
}