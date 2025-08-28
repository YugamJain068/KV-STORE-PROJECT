#ifndef WAL_H
#define WAL_H

#include <string>
#include <vector>
#include <mutex>

class WAL
{
public:
    explicit WAL(const std::string &filename);
    ~WAL() = default;

    void appendEntry(const std::string &command);
    std::vector<std::string> loadAllEntries() const;

private:
    std::string walFile;
    mutable std::mutex wal_mtx;
};

#endif