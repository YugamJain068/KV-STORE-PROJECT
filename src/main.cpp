#include "kvstore.h"
#include "server.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <sstream>
#include "raft_node.h"

int main()
{
    // KVStore store;

    // std::cout << "running CLI" << std::endl;

    // while (true)
    // {
    //     std::cout << "> ";
    //     std::string inputCommand;
    //     std::getline(std::cin, inputCommand);

    //     std::istringstream iss(inputCommand);
    //     std::string command;
    //     iss >> command;

    //     if (command.empty())
    //         continue;

    //     if (command == "PUT")
    //     {
    //         std::string key, value;
    //         iss >> key;
    //         std::getline(iss, value);
    //         if (!value.empty() && value[0] == ' ')
    //         {
    //             value.erase(0, 1); // remove leading space
    //         }
    //         if (key.empty() || value.empty())
    //         {
    //             std::cout << "Usage: PUT <Key> <Value>\n";
    //         }
    //         else
    //         {
    //             store.put(key, value);
    //             std::cout << key << " added successfully.\n";
    //         }
    //     }
    //     else if (command == "GET")
    //     {
    //         std::string key;
    //         iss >> key;
    //         if (key.empty())
    //         {
    //             std::cout << "Usage: GET <Key>\n";
    //         }
    //         else
    //         {
    //             if (auto val = store.get(key); val.has_value())
    //             {
    //                 std::cout << "value of " << key << " " << *val << "\n";
    //             }
    //             else
    //             {
    //                 std::cout << key << " does not exist.\n";
    //             }
    //         }
    //     }
    //     else if (command == "DELETE")
    //     {
    //         std::string key;
    //         iss >> key;
    //         if (key.empty())
    //         {
    //             std::cout << "Usage: DELETE <Key>\n";
    //         }
    //         else
    //         {
    //             if (store.remove(key))
    //             {
    //                 std::cout << key << " deleted.\n";
    //             }
    //             else
    //             {
    //                 std::cout << key << " does not exist.\n";
    //             }
    //         }
    //     }
    //     else if (command == "EXIT")
    //     {
    //         std::cout << "Exiting...\n";
    //         break;
    //     }
    //     else
    //     {
    //         std::cout << "Unknown command. Available: PUT, GET, DELETE, EXIT.\n";
    //     }
    // }

    // start_server(8080);
    raftAlgorithm();
}
