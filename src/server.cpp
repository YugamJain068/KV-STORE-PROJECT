#include "server.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <iostream>
#include <sstream>
#include <shared_mutex>
#include "kvstore.h"
#include "rpc_server.h"
#include <atomic>
#include <nlohmann/json.hpp>
#include "raft_node.h"
#include <chrono>
#include <algorithm>
#include <unordered_map>
#include <cctype>
#include <thread>
#include <fcntl.h>
#include "logger.h"

std::string trim(const std::string &str)
{
    size_t first = str.find_first_not_of(" \t\n\r");
    if (first == std::string::npos)
        return "";
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, (last - first + 1));
}


std::string toUpper(const std::string &str)
{
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::toupper);
    return result;
}


std::string parseValue(std::istringstream &iss)
{
    std::string value;
    iss >> std::ws;

    if (iss.peek() == '"')
    {
        iss.get();
        std::getline(iss, value, '"');
    }
    else if (iss.peek() == '\'')
    {
        iss.get();
        std::getline(iss, value, '\'');
    }
    else
    {
        std::getline(iss, value);
        value = trim(value);
    }
    return value;
}


bool parseKeyValueFormat(std::istringstream &iss, std::string &key, std::string &value)
{
    std::string token;
    while (iss >> token)
    {
        std::string upperToken = toUpper(token);
        if (upperToken == "--KEY" || upperToken == "-K")
        {
            iss >> std::ws;
            key = parseValue(iss);
        }
        else if (upperToken == "--VALUE" || upperToken == "-V")
        {
            iss >> std::ws;
            value = parseValue(iss);
        }
    }
    return !key.empty();
}


std::string resolveCommandAlias(const std::string &cmd)
{
    static std::unordered_map<std::string, std::string> aliases = {
        {"G", "GET"}, {"RETRIEVE", "GET"}, {"FETCH", "GET"}, {"READ", "GET"}, {"P", "PUT"}, {"SET", "PUT"}, {"ADD", "PUT"}, {"UPDATE", "PUT"}, {"WRITE", "PUT"}, {"D", "DELETE"}, {"DEL", "DELETE"}, {"REMOVE", "DELETE"}, {"RM", "DELETE"}, {"H", "HELP"}, {"?", "HELP"}, {"Q", "EXIT"}, {"QUIT", "EXIT"}, {"BYE", "EXIT"}, {"S", "STATUS"}, {"M", "METRICS"}};

    std::string upper = toUpper(cmd);
    auto it = aliases.find(upper);
    return (it != aliases.end()) ? it->second : upper;
}


std::string getHelpText()
{
    return R"(╔══════════════════════════════════════════════════════╗
║         RAFT KEY-VALUE STORE - COMMANDS              ║
╠══════════════════════════════════════════════════════╣
║ BASIC COMMANDS                                       ║
║   PUT <key> <value>     - Add/update key-value       ║
║   GET <key>             - Retrieve value             ║
║   DELETE <key>          - Delete key                 ║
║                                                      ║
║ ADVANCED FORMATS                                     ║
║   PUT --key <k> --value <v>  - Named parameters      ║
║   PUT <key> "value"     - Quoted values (spaces)     ║
║                                                      ║
║ ADMIN COMMANDS                                       ║
║   STATUS                - Show cluster status        ║
║   METRICS               - Show detailed metrics      ║
║   LOGSIZE               - Show log information       ║
║   SNAPSHOT              - Trigger manual snapshot    ║
║   LOGS [count]          - Show recent log entries    ║
║   CLUSTER               - Show cluster information   ║
║   WATCH [seconds]       - Auto-refresh metrics       ║
║                                                      ║
║ UTILITY COMMANDS                                     ║
║   HELP or ?             - Show this help             ║
║   HISTORY               - Show command history       ║
║   !!                    - Repeat last command        ║
║   !<n>                  - Repeat nth command         ║
║   STATS                 - Connection statistics      ║
║   CLEAR                 - Clear screen               ║
║   EXIT                  - Close connection           ║
║                                                      ║
║ COMMAND ALIASES                                      ║
║   GET: g, fetch, read   PUT: p, set, add             ║
║   DELETE: d, del, rm    STATUS: s                    ║
║   METRICS: m            HELP: h, ?                   ║
║                                                      ║
║ EXAMPLES                                             ║
║   put name "John Doe"   - Store value with spaces    ║
║   g name                - Get using alias            ║
║   status                - Check cluster state        ║
║   watch 5               - Auto-refresh every 5s      ║
║   logs 20               - View last 20 log entries   ║
╚══════════════════════════════════════════════════════╝
)";
}

std::atomic<bool> shutdownServer{false};

std::atomic<bool> watchMode{false};
std::atomic<int> watchInterval{2};

int socket_fd;

std::vector<NodeInfo> raft_nodes = {
    {"127.0.0.1", 5000},
    {"127.0.0.1", 5001},
    {"127.0.0.1", 5002}};

std::string sendClientRequest(nlohmann::json msg)
{
    int attempts = 0;
    int current_node = rand() % raft_nodes.size();

    while (attempts < raft_nodes.size())
    {
        try
        {
            std::string responseStr = sendRPC(
                raft_nodes[current_node].host,
                raft_nodes[current_node].port,
                msg.dump());

            auto respJson = nlohmann::json::parse(responseStr);

            if (respJson["status"] == "OK")
                return responseStr;

            if (respJson["status"] == "redirect")
            {
                int leaderId = respJson.value("leaderId", -1);
                if (leaderId == -1)
                {
                    current_node = rand() % raft_nodes.size();
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                else
                    current_node = leaderId;
                continue;
            }

            current_node = rand() % raft_nodes.size();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        catch (...)
        {
            current_node = (current_node + 1) % raft_nodes.size();
        }
        attempts++;
    }
    return R"({"status": "error", "msg": "All nodes unreachable"})";
}

void handle_client(int client_socket)
{
    
    std::vector<std::string> commandHistory;
    int commandCount = 0;
    auto sessionStart = std::chrono::steady_clock::now();

    static std::unordered_map<int, int> clientRequestCounter;
    std::string clientId = "client-" + std::to_string(client_socket);

    
    std::string welcome =
        "╔════════════════════════════════════════════════════╗\n"
        "║   Connected to RAFT Key-Value Store               ║\n"
        "║   Type 'HELP' for available commands              ║\n"
        "╚════════════════════════════════════════════════════╝\n\n";
    send(client_socket, welcome.c_str(), welcome.size(), 0);

    while (true)
    {
        char buffer[4096];
        int bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received <= 0)
        {
            close(client_socket);
            break;
        }
        buffer[bytes_received] = '\0';
        std::string command_line = trim(std::string(buffer));

        if (command_line.empty())
            continue;

        commandHistory.push_back(command_line);
        commandCount++;

        std::istringstream iss(command_line);
        std::string command;
        iss >> command;

        if (command.empty())
            continue;

        std::string response;
        int requestId = ++clientRequestCounter[client_socket];

        
        if (command == "!!")
        {
            if (commandHistory.size() >= 2)
            {
                command_line = commandHistory[commandHistory.size() - 2];
                response = "Repeating: " + command_line + "\n";
                send(client_socket, response.c_str(), response.size(), 0);
                iss = std::istringstream(command_line);
                iss >> command;
            }
            else
            {
                response = "✗ No previous command in history.\n";
                send(client_socket, response.c_str(), response.size(), 0);
                continue;
            }
        }
        else if (command[0] == '!' && command.length() > 1 && std::isdigit(command[1]))
        {
            int histIdx = std::stoi(command.substr(1)) - 1;
            if (histIdx >= 0 && histIdx < (int)commandHistory.size())
            {
                command_line = commandHistory[histIdx];
                response = "Repeating: " + command_line + "\n";
                send(client_socket, response.c_str(), response.size(), 0);
                iss = std::istringstream(command_line);
                iss >> command;
            }
            else
            {
                response = "✗ Invalid history index.\n";
                send(client_socket, response.c_str(), response.size(), 0);
                continue;
            }
        }

        command = resolveCommandAlias(command);

        
        if (command == "PUT")
        {
            std::string key, value;

            std::string peek;
            iss >> std::ws;
            std::streampos pos = iss.tellg();
            iss >> peek;
            iss.seekg(pos);

            if (toUpper(peek) == "--KEY" || toUpper(peek) == "-K")
            {
                if (!parseKeyValueFormat(iss, key, value))
                {
                    response = "✗ Usage: PUT --key <key> --value <value>\n";
                    send(client_socket, response.c_str(), response.size(), 0);
                    continue;
                }
            }
            else
            {
                iss >> key;
                iss >> std::ws;
                value = parseValue(iss);
            }

            if (key.empty() || value.empty())
            {
                response = "✗ Usage: PUT <key> <value>\n";
            }
            else
            {
                nlohmann::json msg = {
                    {"rpc", "ClientRequest"},
                    {"command", "PUT"},
                    {"key", key},
                    {"value", value},
                    {"clientId", clientId},
                    {"requestId", requestId}};

                std::string responseStr = sendClientRequest(msg);
                auto respJson = nlohmann::json::parse(responseStr);
                if (respJson["status"] == "OK")
                {
                    response = "✓ Key '" + key + "' added successfully.\n";
                }
                else if (respJson["status"] == "redirect")
                {
                    int leaderId = respJson["leaderId"].get<int>();
                    response = "→ Redirected to leader node " + std::to_string(leaderId) + "\n";
                }
                else
                {
                    response = "✗ Key '" + key + "' cannot be added.\n";
                }
            }
        }
        else if (command == "GET")
        {
            std::string key;
            iss >> key;
            if (key.empty())
            {
                response = "✗ Usage: GET <key>\n";
            }
            else
            {
                nlohmann::json msg = {
                    {"rpc", "ClientRequest"},
                    {"command", "GET"},
                    {"key", key},
                    {"value", ""},
                    {"clientId", clientId},
                    {"requestId", requestId}};

                std::string responseStr = sendClientRequest(msg);
                auto respJson = nlohmann::json::parse(responseStr);
                if (respJson["status"] == "OK")
                {
                    response = "┌─ Key: " + key + "\n";
                    response += "└─ Value: " + respJson["value"].get<std::string>() + "\n";
                }
                else if (respJson["status"] == "redirect")
                {
                    int leaderId = respJson["leaderId"].get<int>();
                    response = "→ Redirected to leader node " + std::to_string(leaderId) + "\n";
                }
                else
                {
                    response = "✗ Key '" + key + "' does not exist.\n";
                }
            }
        }
        else if (command == "DELETE")
        {
            std::string key;
            iss >> key;
            if (key.empty())
            {
                response = "✗ Usage: DELETE <key>\n";
            }
            else
            {
                nlohmann::json msg = {
                    {"rpc", "ClientRequest"},
                    {"command", "DELETE"},
                    {"key", key},
                    {"value", ""},
                    {"clientId", clientId},
                    {"requestId", requestId}};

                std::string responseStr = sendClientRequest(msg);
                auto respJson = nlohmann::json::parse(responseStr);
                if (respJson["status"] == "OK")
                {
                    response = "✓ Key '" + key + "' deleted successfully.\n";
                }
                else if (respJson["status"] == "redirect")
                {
                    int leaderId = respJson["leaderId"].get<int>();
                    response = "→ Redirected to leader node " + std::to_string(leaderId) + "\n";
                }
                else
                {
                    response = "✗ Key '" + key + "' does not exist.\n";
                }
            }
        }
        else if (command == "STATUS")
        {
            nlohmann::json msg = {
                {"rpc", "STATUS"}};

            std::string responseStr = sendClientRequest(msg);
            auto respJson = nlohmann::json::parse(responseStr);

            if (respJson["status"] == "OK")
            {
                response = "╔══════════════════════════════════════════════════════╗\n";
                response += "║              CLUSTER STATUS                          ║\n";
                response += "╠══════════════════════════════════════════════════════╣\n";
                response += "║ Node ID:         " + std::to_string(respJson["node_id"].get<int>()) + "\n";
                response += "║ Term:            " + std::to_string(respJson["term"].get<int>()) + "\n";
                response += "║ Role:            " + respJson["role"].get<std::string>() + "\n";
                response += "║ Leader ID:       " + std::to_string(respJson["leader_id"].get<int>()) + "\n";
                response += "║ Commit Index:    " + std::to_string(respJson["commit_index"].get<int>()) + "\n";
                response += "║ Last Applied:    " + std::to_string(respJson["last_applied"].get<int>()) + "\n";
                response += "║ Log Size:        " + std::to_string(respJson["log_size"].get<int>()) + "\n";
                response += "╚══════════════════════════════════════════════════════╝\n";
            }
            else
            {
                response = "✗ Failed to get status\n";
            }
        }
        else if (command == "METRICS")
        {
            nlohmann::json msg = {
                {"rpc", "METRICS"}};

            std::string responseStr = sendClientRequest(msg);
            auto respJson = nlohmann::json::parse(responseStr);

            if (respJson["status"] == "OK")
            {
                
                response = "╔══════════════════════════════════════════════════════╗\n";
                response += "║              RAFT NODE METRICS                       ║\n";
                response += "╠══════════════════════════════════════════════════════╣\n";
                response += "║ RAFT STATE                                           ║\n";
                response += "║   Term:            " + std::to_string(respJson["raft_state"]["term"].get<int>()) + "\n";
                response += "║   Role:            " + respJson["raft_state"]["role"].get<std::string>() + "\n";
                response += "║   Leader ID:       " + std::to_string(respJson["raft_state"]["leader_id"].get<int>()) + "\n";
                response += "║   Voted For:       " + std::to_string(respJson["raft_state"]["voted_for"].get<int>()) + "\n";
                response += "╠══════════════════════════════════════════════════════╣\n";
                response += "║ LOG STATE                                            ║\n";
                response += "║   Log Size:        " + std::to_string(respJson["log_state"]["log_size"].get<int>()) + "\n";
                response += "║   Commit Index:    " + std::to_string(respJson["log_state"]["commit_index"].get<int>()) + "\n";
                response += "║   Last Applied:    " + std::to_string(respJson["log_state"]["last_applied"].get<int>()) + "\n";
                response += "╠══════════════════════════════════════════════════════╣\n";
                response += "║ SNAPSHOTS                                            ║\n";
                response += "║   Count:           " + std::to_string(respJson["snapshots"]["count"].get<int>()) + "\n";
                response += "║   Last Index:      " + std::to_string(respJson["snapshots"]["last_index"].get<int>()) + "\n";
                response += "║   Last Term:       " + std::to_string(respJson["snapshots"]["last_term"].get<int>()) + "\n";
                response += "╠══════════════════════════════════════════════════════╣\n";
                response += "║ ELECTIONS                                            ║\n";
                response += "║   Elections:       " + std::to_string(respJson["elections"]["election_count"].get<int>()) + "\n";
                response += "║   Leader Changes:  " + std::to_string(respJson["elections"]["leader_changes"].get<int>()) + "\n";
                response += "║   Votes Received:  " + std::to_string(respJson["elections"]["votes_received"].get<int>()) + "\n";
                response += "╠══════════════════════════════════════════════════════╣\n";
                response += "║ RPC STATISTICS                                       ║\n";
                response += "║   AppendEntries:   " + std::to_string(respJson["rpcs"]["append_entries_sent"].get<int>()) +
                            " sent / " + std::to_string(respJson["rpcs"]["append_entries_received"].get<int>()) + " recv\n";
                response += "║   RequestVote:     " + std::to_string(respJson["rpcs"]["request_votes_sent"].get<int>()) +
                            " sent / " + std::to_string(respJson["rpcs"]["request_votes_received"].get<int>()) + " recv\n";
                response += "║   InstallSnapshot: " + std::to_string(respJson["rpcs"]["install_snapshots_sent"].get<int>()) +
                            " sent / " + std::to_string(respJson["rpcs"]["install_snapshots_received"].get<int>()) + " recv\n";
                response += "╠══════════════════════════════════════════════════════╣\n";
                response += "║ CLIENT REQUESTS                                      ║\n";
                response += "║   Received:        " + std::to_string(respJson["client_requests"]["received"].get<int>()) + "\n";
                response += "║   Succeeded:       " + std::to_string(respJson["client_requests"]["succeeded"].get<int>()) + "\n";
                response += "║   Failed:          " + std::to_string(respJson["client_requests"]["failed"].get<int>()) + "\n";
                response += "║   Redirected:      " + std::to_string(respJson["client_requests"]["redirected"].get<int>()) + "\n";
                response += "╠══════════════════════════════════════════════════════╣\n";
                response += "║ PERFORMANCE                                          ║\n";
                response += "║   Avg Response:    " + std::to_string(respJson["performance"]["avg_response_time_ms"].get<long long>()) + " ms\n";
                response += "║   Uptime:          " + std::to_string(respJson["uptime_seconds"].get<long long>()) + " seconds\n";
                response += "╚══════════════════════════════════════════════════════╝\n";
            }
            else
            {
                response = "✗ Failed to get metrics\n";
            }
        }
        else if (command == "LOGSIZE")
        {
            nlohmann::json msg = {
                {"rpc", "LOGSIZE"}};

            std::string responseStr = sendClientRequest(msg);
            auto respJson = nlohmann::json::parse(responseStr);

            if (respJson["status"] == "OK")
            {
                response = "╔══════════════════════════════════════════════════════╗\n";
                response += "║              LOG INFORMATION                         ║\n";
                response += "╠══════════════════════════════════════════════════════╣\n";
                response += "║ Total Log Size:      " + std::to_string(respJson["total_log_size"].get<int>()) + "\n";
                response += "║ Physical Log Size:   " + std::to_string(respJson["physical_log_size"].get<int>()) + "\n";
                response += "║ Snapshot Index:      " + std::to_string(respJson["snapshot_index"].get<int>()) + "\n";
                response += "║ Commit Index:        " + std::to_string(respJson["commit_index"].get<int>()) + "\n";
                response += "║ Last Applied:        " + std::to_string(respJson["last_applied"].get<int>()) + "\n";
                response += "╚══════════════════════════════════════════════════════╝\n";
            }
            else
            {
                response = "✗ Failed to get log size\n";
            }
        }
        else if (command == "SNAPSHOT")
        {
            nlohmann::json msg = {
                {"rpc", "SNAPSHOT_NOW"}};

            std::string responseStr = sendClientRequest(msg);
            auto respJson = nlohmann::json::parse(responseStr);

            if (respJson["status"] == "OK")
            {
                response = "✓ " + respJson["message"].get<std::string>() + "\n";
                response += "  Snapshot Index: " + std::to_string(respJson["snapshot_index"].get<int>()) + "\n";
                response += "  Snapshot Term:  " + std::to_string(respJson["snapshot_term"].get<int>()) + "\n";
            }
            else
            {
                response = "✗ " + respJson["message"].get<std::string>() + "\n";
            }
        }
        else if (command == "LOGS")
        {
            int count = 10;
            std::string countStr;
            iss >> countStr; 

            
            std::string digits;
            for (char c : countStr)
            {
                if (std::isdigit(c))
                {
                    digits += c;
                }
            }

            
            if (!digits.empty())
            {
                count = std::stoi(digits);
            }

            if (count <= 0)
                count = 10;

            nlohmann::json msg = {
                {"rpc", "GET_LOGS"},
                {"count", count}};

            std::string responseStr = sendClientRequest(msg);
            auto respJson = nlohmann::json::parse(responseStr);

            if (respJson["status"] == "OK")
            {
                response = "╔══════════════════════════════════════════════════════╗\n";
                response += "║         RECENT LOG ENTRIES                           ║\n";
                response += "╠══════════════════════════════════════════════════════╣\n";

                for (const auto &entry : respJson["logs"])
                {
                    response += "║ Index: " + std::to_string(entry["index"].get<int>()) +
                                " | Term: " + std::to_string(entry["term"].get<int>()) + "\n";
                    response += "║   Command: " + entry["command"].get<std::string>().substr(0, 40) + "\n";
                }

                response += "╠══════════════════════════════════════════════════════╣\n";
                response += "║ Total: " + std::to_string(respJson["total_count"].get<int>()) +
                            " | Showing: " + std::to_string(respJson["returned_count"].get<int>()) + "\n";
                response += "╚══════════════════════════════════════════════════════╝\n";
            }
            else
            {
                response = "✗ Failed to get logs\n";
            }
        }
        else if (command == "CLUSTER")
        {
            nlohmann::json msg = {
                {"rpc", "CLUSTER_INFO"}};

            std::string responseStr = sendClientRequest(msg);
            auto respJson = nlohmann::json::parse(responseStr);

            if (respJson["status"] == "OK")
            {
                response = "╔══════════════════════════════════════════════════════╗\n";
                response += "║         CLUSTER INFORMATION                          ║\n";
                response += "╠══════════════════════════════════════════════════════╣\n";
                response += "║ Cluster Size: " + std::to_string(respJson["cluster_size"].get<int>()) + "\n";
                response += "║ Current Leader: Node " + std::to_string(respJson["leader_id"].get<int>()) + "\n";
                response += "╠══════════════════════════════════════════════════════╣\n";

                for (const auto &node : respJson["nodes"])
                {
                    response += std::string("║ Node ") +
                                std::to_string(node["node_id"].get<int>()) +
                                ": Port " + std::to_string(node["port"].get<int>()) +
                                " | NextIdx=" + std::to_string(node["next_index"].get<int>()) +
                                " | MatchIdx=" + std::to_string(node["match_index"].get<int>()) + "\n";
                }

                response += "╚══════════════════════════════════════════════════════╝\n";
            }
            else
            {
                response = "✗ Failed to get cluster info\n";
            }
        }

        else if (command == "WATCH")
        {
            
            int interval = 2; 
            std::string intervalStr;
            iss >> intervalStr;

            if (!intervalStr.empty())
            {
                try
                {
                    interval = std::stoi(intervalStr);
                    if (interval < 1)
                        interval = 2;
                    if (interval > 60)
                        interval = 60; 
                }
                catch (...)
                {
                    interval = 2;
                }
            }

            
            response = "╔════════════════════════════════════════════════════╗\n";
            response += "║          WATCH MODE ACTIVATED                      ║\n";
            response += "╠════════════════════════════════════════════════════╣\n";
            response += "║ Refreshing every " + std::to_string(interval) + " seconds\n";
            response += "║ Max 60 refreshes (auto-stop)\n";
            response += "║\n";
            response += "║ To stop watch mode, type:\n";
            response += "║   • STOP\n";
            response += "║   • EXIT\n";
            response += "║   • Q\n";
            response += "╚════════════════════════════════════════════════════╝\n\n";
            send(client_socket, response.c_str(), response.size(), 0);

            
            int original_flags = fcntl(client_socket, F_GETFL, 0);

            
            fcntl(client_socket, F_SETFL, original_flags | O_NONBLOCK);

            bool stopWatch = false;
            int refreshCount = 0;
            const int maxRefreshes = 60;

            while (refreshCount < maxRefreshes && !stopWatch)
            {
                
                nlohmann::json metricsMsg = {{"rpc", "METRICS"}};
                std::string metricsResponse = sendClientRequest(metricsMsg);
                auto metricsJson = nlohmann::json::parse(metricsResponse);

                if (metricsJson["status"] == "OK")
                {
                    
                    std::string output;

                    
                    output = "\033[3J\033[H\033[2J";

                    
                    output += "╔══════════════════════════════════════════════════════╗\n";
                    output += "║           RAFT NODE METRICS (LIVE WATCH)             ║\n";
                    output += "╠══════════════════════════════════════════════════════╣\n";

                    
                    output += "║ RAFT STATE                                           ║\n";
                    output += "║   Term:            ";
                    output += std::to_string(metricsJson["raft_state"]["term"].get<int>());
                    output += "\n║   Role:            ";
                    output += metricsJson["raft_state"]["role"].get<std::string>();
                    output += "\n║   Leader ID:       ";
                    output += std::to_string(metricsJson["raft_state"]["leader_id"].get<int>());
                    output += "\n";

                    output += "╠══════════════════════════════════════════════════════╣\n";

                    
                    output += "║ LOG STATE                                            ║\n";
                    output += "║   Log Size:        ";
                    output += std::to_string(metricsJson["log_state"]["log_size"].get<int>());
                    output += "\n║   Commit Index:    ";
                    output += std::to_string(metricsJson["log_state"]["commit_index"].get<int>());
                    output += "\n║   Last Applied:    ";
                    output += std::to_string(metricsJson["log_state"]["last_applied"].get<int>());
                    output += "\n";

                    output += "╠══════════════════════════════════════════════════════╣\n";

                    
                    output += "║ SNAPSHOTS                                            ║\n";
                    output += "║   Count:           ";
                    output += std::to_string(metricsJson["snapshots"]["count"].get<int>());
                    output += "\n║   Last Index:      ";
                    output += std::to_string(metricsJson["snapshots"]["last_index"].get<int>());
                    output += "\n";

                    output += "╠══════════════════════════════════════════════════════╣\n";

                    
                    output += "║ ELECTIONS                                            ║\n";
                    output += "║   Count:           ";
                    output += std::to_string(metricsJson["elections"]["election_count"].get<int>());
                    output += "\n║   Leader Changes:  ";
                    output += std::to_string(metricsJson["elections"]["leader_changes"].get<int>());
                    output += "\n";

                    output += "╠══════════════════════════════════════════════════════╣\n";

                    
                    output += "║ RPC STATISTICS                                       ║\n";
                    output += "║   AppendEntries:   ";
                    output += std::to_string(metricsJson["rpcs"]["append_entries_sent"].get<int>());
                    output += " sent / ";
                    output += std::to_string(metricsJson["rpcs"]["append_entries_received"].get<int>());
                    output += " recv\n";
                    output += "║   RequestVote:     ";
                    output += std::to_string(metricsJson["rpcs"]["request_votes_sent"].get<int>());
                    output += " sent / ";
                    output += std::to_string(metricsJson["rpcs"]["request_votes_received"].get<int>());
                    output += " recv\n";

                    output += "╠══════════════════════════════════════════════════════╣\n";

                    
                    output += "║ CLIENT REQUESTS                                      ║\n";
                    output += "║   Total:           ";
                    output += std::to_string(metricsJson["client_requests"]["received"].get<int>());
                    output += "\n║   Succeeded:       ";
                    output += std::to_string(metricsJson["client_requests"]["succeeded"].get<int>());
                    output += "\n║   Failed:          ";
                    output += std::to_string(metricsJson["client_requests"]["failed"].get<int>());
                    output += "\n";

                    output += "╠══════════════════════════════════════════════════════╣\n";

                    
                    output += "║ PERFORMANCE                                          ║\n";
                    output += "║   Uptime:          ";
                    output += std::to_string(metricsJson["uptime_seconds"].get<long long>());
                    output += " seconds\n";

                    output += "╚══════════════════════════════════════════════════════╝\n\n";

                    
                    refreshCount++;
                    output += "┌────────────────────────────────────────────────────┐\n";
                    output += "│ Refresh: " + std::to_string(refreshCount) + "/" + std::to_string(maxRefreshes);
                    output += " | Interval: " + std::to_string(interval) + "s";
                    output += " | Next in " + std::to_string(interval) + "s";
                    output += "\n│ Type STOP, EXIT, or Q to quit watch mode           │\n";
                    output += "└────────────────────────────────────────────────────┘\n";

                    
                    send(client_socket, output.c_str(), output.size(), 0);
                }

                
                int sleepChunks = interval * 10; 
                for (int chunk = 0; chunk < sleepChunks && !stopWatch; chunk++)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));

                    
                    char inputBuffer[256];
                    int bytesRead = recv(client_socket, inputBuffer, sizeof(inputBuffer) - 1, 0);

                    if (bytesRead > 0)
                    {
                        inputBuffer[bytesRead] = '\0';
                        std::string userInput = trim(std::string(inputBuffer));
                        std::string upperInput = toUpper(userInput);

                        
                        if (upperInput == "STOP" || upperInput == "EXIT" ||
                            upperInput == "Q" || upperInput == "QUIT")
                        {
                            stopWatch = true;
                            break;
                        }
                    }
                    else if (bytesRead < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                    {
                        
                        stopWatch = true;
                        break;
                    }
                }
            }

            
            fcntl(client_socket, F_SETFL, original_flags);

            
            std::string exitMsg;
            if (stopWatch && refreshCount < maxRefreshes)
            {
                exitMsg = "\n╔════════════════════════════════════════════════════╗\n";
                exitMsg += "║ ⚡ Watch mode stopped by user                      ║\n";
                exitMsg += "║ Total refreshes: " + std::to_string(refreshCount) + "\n";
                exitMsg += "╚════════════════════════════════════════════════════╝\n\n";
            }
            else
            {
                exitMsg = "\n╔════════════════════════════════════════════════════╗\n";
                exitMsg += "║ ⚡ Watch mode auto-stopped (max refreshes)         ║\n";
                exitMsg += "║ Total refreshes: " + std::to_string(refreshCount) + "\n";
                exitMsg += "╚════════════════════════════════════════════════════╝\n\n";
            }

            send(client_socket, exitMsg.c_str(), exitMsg.size(), 0);

            
            continue; 
        }
        else if (command == "HELP")
        {
            response = getHelpText();
        }
        else if (command == "HISTORY")
        {
            response = "╔══════════════════════════════════════════════════════╗\n";
            response += "║              COMMAND HISTORY                         ║\n";
            response += "╠══════════════════════════════════════════════════════╣\n";

            int start = std::max(0, (int)commandHistory.size() - 20);
            for (size_t i = start; i < commandHistory.size(); i++)
            {
                response += "║ " + std::to_string(i + 1) + ". " + commandHistory[i] + "\n";
            }
            response += "╚══════════════════════════════════════════════════════╝\n";
        }
        else if (command == "STATS")
        {
            auto now = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - sessionStart);

            response = "╔══════════════════════════════════════════════════════╗\n";
            response += "║           CONNECTION STATISTICS                      ║\n";
            response += "╠══════════════════════════════════════════════════════╣\n";
            response += "║ Client ID:       " + clientId + "\n";
            response += "║ Commands run:    " + std::to_string(commandCount) + "\n";
            response += "║ History size:    " + std::to_string(commandHistory.size()) + "\n";
            response += "║ Session time:    " + std::to_string(duration.count()) + " seconds\n";
            response += "╚══════════════════════════════════════════════════════╝\n";
        }
        else if (command == "CLEAR")
        {
            response = "\033[3J\033[H\033[2J";
            response +=
                "╔════════════════════════════════════════════════════╗\n"
                "║   RAFT Key-Value Store - Screen Cleared            ║\n"
                "╚════════════════════════════════════════════════════╝\n\n";
        }
        else if (command == "EXIT-RAFT")
        {
            std::string password;
            iss >> password;

            if (password == "admin123")
            {
                requestRaftShutdown();
                response = "⚠ Raft shutdown signal sent.\n";
                send(client_socket, response.c_str(), response.size(), 0);
                continue;
            }
            else
            {
                response = "✗ Incorrect password.\n";
            }
        }
        else if (command == "EXIT-SERVER")
        {
            std::string password;
            iss >> password;

            if (password == "admin123")
            {
                response = "⚠ Shutting down CLI server...\n";
                send(client_socket, response.c_str(), response.size(), 0);
                shutdownServer.store(true);
                close(client_socket);
                break;
            }
            else
            {
                response = "✗ Invalid password.\n";
            }
        }
        else if (command == "EXIT")
        {
            response =
                "╔════════════════════════════════════════════════════╗\n"
                "║   Goodbye! Connection closing...                   ║\n"
                "║   Session: " +
                std::to_string(commandCount) + " commands executed\n"
                                               "╚════════════════════════════════════════════════════╝\n";
            send(client_socket, response.c_str(), response.size(), 0);
            close(client_socket);
            break;
        }
        else
        {
            response = "✗ Unknown command: '" + command + "'\n";
            response += "   Type 'HELP' for available commands.\n";
        }

        send(client_socket, response.c_str(), response.size(), 0);
    }
}

void start_server(int port)
{
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
    {
        Logger::error("Socket creation failed");
        return;
    }

    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        Logger::error("Bind failed");
        close(socket_fd);
        return;
    }

    if (listen(socket_fd, 5) < 0)
    {
        Logger::error("Listen failed");
        close(socket_fd);
        return;
    }

    Logger::info("Server started on port " + std::to_string(port));

    while (!shutdownServer.load())
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_socket = accept(socket_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_socket < 0)
        {
            if (shutdownServer.load())
                break;
            if (errno == EINTR)
                continue;
            Logger::error("Accept failed");
            continue;
        }

        std::string ip_address = inet_ntoa(client_addr.sin_addr);
        uint16_t client_port = ntohs(client_addr.sin_port);
        Logger::info("Client connected: " + std::string(ip_address) + ":" + std::to_string(client_port));

        
        std::thread client_thread([client_socket]()
                                  {
            handle_client(client_socket);

            
            if (shutdownServer.load())
            {
                int dummy_sock = socket(AF_INET, SOCK_STREAM, 0);
                if (dummy_sock >= 0)
                {
                    struct sockaddr_in dummy_addr{};
                    dummy_addr.sin_family = AF_INET;
                    dummy_addr.sin_port = htons(4000); 
                    inet_pton(AF_INET, "127.0.0.1", &dummy_addr.sin_addr);
                    connect(dummy_sock, (struct sockaddr*)&dummy_addr, sizeof(dummy_addr));
                    close(dummy_sock);
                }
            } });
        client_thread.detach();
    }

    close(socket_fd);
    Logger::info("Server socket closed. Server exited cleanly.");
}
