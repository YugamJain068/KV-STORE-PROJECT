#ifndef WAL_H
#define WAL_H

#include "log_entry.h"
#include <string>
#include <vector>
#include <mutex>

class WAL
{
public:
    explicit WAL(const std::string &filename);
    ~WAL() = default;

    void appendEntry(const std::string &command);
    std::vector<LogEntry> loadAllEntries() const;

private:
    std::string walBinaryFile;
    std::vector<LogEntry> cachedEntries;
    mutable std::mutex wal_mtx;
    uint64_t lastIndex;
};

#endif