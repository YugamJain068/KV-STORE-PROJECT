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

void WAL::truncateUpTo(int lastIncludedIndex)
{
    std::lock_guard<std::mutex> lock(wal_mtx);

    std::vector<LogEntry> remainingEntries;

    // Keep only entries after lastIncludedIndex
    for (const auto &entry : cachedEntries)
    {
        if (entry.index > lastIncludedIndex)
            remainingEntries.push_back(entry);
    }

    // Rewrite WAL file with remaining entries
    std::ofstream outFile(walBinaryFile, std::ios::binary | std::ios::trunc);
    if (!outFile.is_open())
        throw std::runtime_error("Failed to open WAL file for truncation");

    for (const auto &entry : remainingEntries)
    {
        std::string binaryData = entry.toBinary();
        outFile.write(binaryData.data(), binaryData.size());
    }

    outFile.flush();

    // Update cached entries and lastIndex
    cachedEntries = std::move(remainingEntries);
    lastIndex = cachedEntries.empty() ? 0 : cachedEntries.back().index;
}
