#ifndef LOGGER_H
#define LOGGER_H

#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <mutex>

enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR
};

class Logger {
private:
    static std::mutex log_mutex;
    static LogLevel min_level;
    static std::ofstream log_file;
    static bool log_to_file;
    static bool log_to_console;
    
    static std::string getCurrentTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t_now), "%Y-%m-%d %H:%M:%S");
        ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
        return ss.str();
    }
    
    static std::string getLevelString(LogLevel level) {
        switch(level) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::ERROR: return "ERROR";
            default: return "UNKNOWN";
        }
    }
    
    static std::string getColorCode(LogLevel level) {
        switch(level) {
            case LogLevel::DEBUG: return "\033[36m"; // Cyan
            case LogLevel::INFO:  return "\033[32m"; // Green
            case LogLevel::WARN:  return "\033[33m"; // Yellow
            case LogLevel::ERROR: return "\033[31m"; // Red
            default: return "\033[0m";
        }
    }
    
    static void log(LogLevel level, const std::string& message, 
                   int nodeId = -1, const std::string& role = "", int term = -1) {
        if (level < min_level) return;
        
        std::lock_guard<std::mutex> lock(log_mutex);
        
        std::stringstream ss;
        ss << "[" << getCurrentTimestamp() << "]";
        ss << "[" << getLevelString(level) << "]";
        
        if (nodeId >= 0) {
            ss << "[Node " << nodeId << "]";
        }
        if (!role.empty()) {
            ss << "[" << role << "]";
        }
        if (term >= 0) {
            ss << "[Term " << term << "]";
        }
        
        ss << ": " << message;
        
        std::string log_line = ss.str();
        
        // Console output with colors
        if (log_to_console) {
            std::cout << getColorCode(level) << log_line << "\033[0m" << std::endl;
        }
        
        // File output without colors
        if (log_to_file && log_file.is_open()) {
            log_file << log_line << std::endl;
            log_file.flush();
        }
    }

public:
    // Initialize logger
    static void init(const std::string& filename = "", LogLevel level = LogLevel::INFO) {
        min_level = level;
        log_to_console = true;
        
        if (!filename.empty()) {
            log_file.open(filename, std::ios::app);
            log_to_file = log_file.is_open();
            if (!log_to_file) {
                std::cerr << "Failed to open log file: " << filename << std::endl;
            }
        } else {
            log_to_file = false;
        }
    }
    
    // Close logger
    static void close() {
        if (log_file.is_open()) {
            log_file.close();
        }
    }
    
    // Set minimum log level
    static void setLevel(LogLevel level) {
        min_level = level;
    }
    
    // Enable/disable console logging
    static void setConsoleLogging(bool enable) {
        log_to_console = enable;
    }
    
    // Logging methods without node context
    static void debug(const std::string& message) {
        log(LogLevel::DEBUG, message);
    }
    
    static void info(const std::string& message) {
        log(LogLevel::INFO, message);
    }
    
    static void warn(const std::string& message) {
        log(LogLevel::WARN, message);
    }
    
    static void error(const std::string& message) {
        log(LogLevel::ERROR, message);
    }
    
    // Logging methods with node context
    static void debug(int nodeId, const std::string& role, int term, const std::string& message) {
        log(LogLevel::DEBUG, message, nodeId, role, term);
    }
    
    static void info(int nodeId, const std::string& role, int term, const std::string& message) {
        log(LogLevel::INFO, message, nodeId, role, term);
    }
    
    static void warn(int nodeId, const std::string& role, int term, const std::string& message) {
        log(LogLevel::WARN, message, nodeId, role, term);
    }
    
    static void error(int nodeId, const std::string& role, int term, const std::string& message) {
        log(LogLevel::ERROR, message, nodeId, role, term);
    }
    
    // Convenience methods with just node ID
    static void info(int nodeId, const std::string& message) {
        log(LogLevel::INFO, message, nodeId);
    }
    
    static void warn(int nodeId, const std::string& message) {
        log(LogLevel::WARN, message, nodeId);
    }
    
    static void error(int nodeId, const std::string& message) {
        log(LogLevel::ERROR, message, nodeId);
    }
};


// Convenience macros for easier logging
#define LOG_DEBUG(msg) Logger::debug(msg)
#define LOG_INFO(msg) Logger::info(msg)
#define LOG_WARN(msg) Logger::warn(msg)
#define LOG_ERROR(msg) Logger::error(msg)

#define LOG_NODE_DEBUG(nodeId, role, term, msg) Logger::debug(nodeId, role, term, msg)
#define LOG_NODE_INFO(nodeId, role, term, msg) Logger::info(nodeId, role, term, msg)
#define LOG_NODE_WARN(nodeId, role, term, msg) Logger::warn(nodeId, role, term, msg)
#define LOG_NODE_ERROR(nodeId, role, term, msg) Logger::error(nodeId, role, term, msg)

#endif