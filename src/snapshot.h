#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <unordered_map>
#include <string>
#include <vector>
#include "raft_node.h"
#include "kvstore.h"

struct logEntry;

struct Snapshot
{
    int lastIncludedIndex;
    int lastIncludedTerm;
    std::unordered_map<std::string, std::string> kvState;
};
struct SnapshotChunk
{
    int term;                  // Leader’s term
    int leaderId;              // Leader ID
    int lastIncludedIndex;     // Index covered by the snapshot
    int lastIncludedTerm;      // Term of lastIncludedIndex

    int offset;                // Where this chunk belongs in snapshot
    std::vector<char> data;    // Raw bytes of snapshot chunk
    bool done;                 // True if this is the last chunk
};

// APIs
void saveSnapshot(const Snapshot &snapshot, int nodeId);
void truncateLog(int lastIncludedIndex, std::vector<logEntry> &log);
Snapshot loadSnapshot(int nodeId);
void restoreFromSnapshot(int nodeId,
                         KVStore &store,
                         int &lastApplied,
                         int &commitIndex,
                         std::vector<logEntry> &log);

#endif
