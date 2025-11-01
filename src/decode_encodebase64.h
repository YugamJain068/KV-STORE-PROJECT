#ifndef DECODEENCODE_H
#define DECODEENCODE_H

#pragma once
#include <string>
#include <vector>

std::string base64Encode(const std::vector<char>& data);
std::vector<char> base64Decode(const std::string& s);

#endif
