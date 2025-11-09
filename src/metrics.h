#ifndef METRICS_H
#define METRICS_H

#include <string>
#include <chrono>
#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>

using Clock = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<Clock>;

struct Metrics {
    
    std::atomic<int> currentTerm{0};
    std::atomic<int> votedFor{-1};
    std::atomic<int> leaderId{-1};
    std::string role = "Follower";
    
    
    std::atomic<int> logSize{0};
    std::atomic<int> commitIndex{0};
    std::atomic<int> lastApplied{0};
    
    
    std::atomic<int> snapshotCount{0};
    std::atomic<int> lastSnapshotIndex{0};
    std::atomic<int> lastSnapshotTerm{0};
    
    
    std::atomic<int> electionCount{0};
    std::atomic<int> leaderChanges{0};
    std::atomic<int> votesReceived{0};
    
    
    std::atomic<int> appendEntriesSent{0};
    std::atomic<int> appendEntriesReceived{0};
    std::atomic<int> requestVotesSent{0};
    std::atomic<int> requestVotesReceived{0};
    std::atomic<int> installSnapshotsSent{0};
    std::atomic<int> installSnapshotsReceived{0};
    
    
    std::atomic<int> clientRequestsReceived{0};
    std::atomic<int> clientRequestsSucceeded{0};
    std::atomic<int> clientRequestsFailed{0};
    std::atomic<int> clientRequestsRedirected{0};
    
    
    std::atomic<long long> totalProcessingTimeMs{0};
    std::atomic<long long> avgResponseTimeMs{0};
    
    
    TimePoint startTime;
    
    
    mutable std::mutex metrics_mutex;
    
    Metrics() {
        startTime = Clock::now();
    }
    
    
    long long getUptimeSeconds() const {
        auto now = Clock::now();
        return std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();
    }
    
    
    nlohmann::json toJson() const {
        std::lock_guard<std::mutex> lock(metrics_mutex);
        
        return nlohmann::json{
            {"raft_state", {
                {"term", currentTerm.load()},
                {"role", role},
                {"voted_for", votedFor.load()},
                {"leader_id", leaderId.load()}
            }},
            {"log_state", {
                {"log_size", logSize.load()},
                {"commit_index", commitIndex.load()},
                {"last_applied", lastApplied.load()}
            }},
            {"snapshots", {
                {"count", snapshotCount.load()},
                {"last_index", lastSnapshotIndex.load()},
                {"last_term", lastSnapshotTerm.load()}
            }},
            {"elections", {
                {"election_count", electionCount.load()},
                {"leader_changes", leaderChanges.load()},
                {"votes_received", votesReceived.load()}
            }},
            {"rpcs", {
                {"append_entries_sent", appendEntriesSent.load()},
                {"append_entries_received", appendEntriesReceived.load()},
                {"request_votes_sent", requestVotesSent.load()},
                {"request_votes_received", requestVotesReceived.load()},
                {"install_snapshots_sent", installSnapshotsSent.load()},
                {"install_snapshots_received", installSnapshotsReceived.load()}
            }},
            {"client_requests", {
                {"received", clientRequestsReceived.load()},
                {"succeeded", clientRequestsSucceeded.load()},
                {"failed", clientRequestsFailed.load()},
                {"redirected", clientRequestsRedirected.load()}
            }},
            {"performance", {
                {"total_processing_time_ms", totalProcessingTimeMs.load()},
                {"avg_response_time_ms", avgResponseTimeMs.load()}
            }},
            {"uptime_seconds", getUptimeSeconds()}
        };
    }
    
    std::string toString() const {
        std::lock_guard<std::mutex> lock(metrics_mutex);
        
        std::stringstream ss;
        ss << "╔══════════════════════════════════════════════════════╗\n";
        ss << "║              RAFT NODE METRICS                       ║\n";
        ss << "╠══════════════════════════════════════════════════════╣\n";
        ss << "║ RAFT STATE                                           ║\n";
        ss << "║   Term:            " << std::setw(30) << std::left << currentTerm.load() << "║\n";
        ss << "║   Role:            " << std::setw(30) << std::left << role << "║\n";
        ss << "║   Leader ID:       " << std::setw(30) << std::left << leaderId.load() << "║\n";
        ss << "║   Voted For:       " << std::setw(30) << std::left << votedFor.load() << "║\n";
        ss << "╠══════════════════════════════════════════════════════╣\n";
        ss << "║ LOG STATE                                            ║\n";
        ss << "║   Log Size:        " << std::setw(30) << std::left << logSize.load() << "║\n";
        ss << "║   Commit Index:    " << std::setw(30) << std::left << commitIndex.load() << "║\n";
        ss << "║   Last Applied:    " << std::setw(30) << std::left << lastApplied.load() << "║\n";
        ss << "╠══════════════════════════════════════════════════════╣\n";
        ss << "║ SNAPSHOTS                                            ║\n";
        ss << "║   Count:           " << std::setw(30) << std::left << snapshotCount.load() << "║\n";
        ss << "║   Last Index:      " << std::setw(30) << std::left << lastSnapshotIndex.load() << "║\n";
        ss << "║   Last Term:       " << std::setw(30) << std::left << lastSnapshotTerm.load() << "║\n";
        ss << "╠══════════════════════════════════════════════════════╣\n";
        ss << "║ ELECTIONS                                            ║\n";
        ss << "║   Elections:       " << std::setw(30) << std::left << electionCount.load() << "║\n";
        ss << "║   Leader Changes:  " << std::setw(30) << std::left << leaderChanges.load() << "║\n";
        ss << "║   Votes Received:  " << std::setw(30) << std::left << votesReceived.load() << "║\n";
        ss << "╠══════════════════════════════════════════════════════╣\n";
        ss << "║ RPC STATISTICS                                       ║\n";
        ss << "║   AppendEntries:   " << std::setw(10) << appendEntriesSent.load() 
           << " sent / " << std::setw(10) << appendEntriesReceived.load() << " recv   ║\n";
        ss << "║   RequestVote:     " << std::setw(10) << requestVotesSent.load() 
           << " sent / " << std::setw(10) << requestVotesReceived.load() << " recv   ║\n";
        ss << "║   InstallSnapshot: " << std::setw(10) << installSnapshotsSent.load() 
           << " sent / " << std::setw(10) << installSnapshotsReceived.load() << " recv   ║\n";
        ss << "╠══════════════════════════════════════════════════════╣\n";
        ss << "║ CLIENT REQUESTS                                      ║\n";
        ss << "║   Received:        " << std::setw(30) << std::left << clientRequestsReceived.load() << "║\n";
        ss << "║   Succeeded:       " << std::setw(30) << std::left << clientRequestsSucceeded.load() << "║\n";
        ss << "║   Failed:          " << std::setw(30) << std::left << clientRequestsFailed.load() << "║\n";
        ss << "║   Redirected:      " << std::setw(30) << std::left << clientRequestsRedirected.load() << "║\n";
        ss << "╠══════════════════════════════════════════════════════╣\n";
        ss << "║ PERFORMANCE                                          ║\n";
        ss << "║   Avg Response:    " << std::setw(30) << std::left 
           << (std::to_string(avgResponseTimeMs.load()) + " ms") << "║\n";
        ss << "║   Uptime:          " << std::setw(30) << std::left 
           << (std::to_string(getUptimeSeconds()) + " seconds") << "║\n";
        ss << "╚══════════════════════════════════════════════════════╝\n";
        
        return ss.str();
    }
    
    
    void updateRole(const std::string& newRole, int nodeId) {
        std::lock_guard<std::mutex> lock(metrics_mutex);
        if (role != newRole) {
            if (newRole == "Leader") {
                leaderChanges++;
            }
            role = newRole;
        }
    }
    
    
    void recordSnapshot(int index, int term) {
        snapshotCount++;
        lastSnapshotIndex = index;
        lastSnapshotTerm = term;
    }
    
    
    void recordClientRequest(const std::string& result) {
        clientRequestsReceived++;
        if (result == "OK") {
            clientRequestsSucceeded++;
        } else if (result == "redirect") {
            clientRequestsRedirected++;
        } else {
            clientRequestsFailed++;
        }
    }
};

#endif 