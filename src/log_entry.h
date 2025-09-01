#pragma once
#include <string>
#include <sstream>
#include <vector>
#include <cstring>
#include <cstdint>

struct LogEntry
{
    int term;
    uint64_t index;
    std::string command;

    std::string toBinary() const
    {
        std::ostringstream oss(std::ios::binary);
        oss.write(reinterpret_cast<const char *>(&term), sizeof(term));
        oss.write(reinterpret_cast<const char *>(&index), sizeof(index));

        uint64_t commandSize = command.size();
        oss.write(reinterpret_cast<const char *>(&commandSize), sizeof(commandSize));
        oss.write(command.data(), commandSize);

        return oss.str();
    }
    static bool fromBinary(std::istream &is, LogEntry &Entry)
    {

        if (!is.read(reinterpret_cast<char *>(&Entry.term), sizeof(Entry.term)))
            return false;
        if (!is.read(reinterpret_cast<char *>(&Entry.index), sizeof(Entry.index)))
            return false;

        uint64_t size;
        if (!is.read(reinterpret_cast<char *>(&size), sizeof(size)))
            return false;

        Entry.command.resize(size);
        if (!is.read(Entry.command.data(), size))
            return false;

        return true;
    }
};
