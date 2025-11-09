#include "logger.h"

std::mutex Logger::log_mutex;
LogLevel Logger::min_level = LogLevel::INFO;
std::ofstream Logger::log_file;
bool Logger::log_to_file = false;
bool Logger::log_to_console = true;