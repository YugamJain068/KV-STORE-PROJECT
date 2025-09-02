#include "kvstore.h"
#include "server.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <sstream>

int main()
{
    start_server(8080);
    return 0;
}
