#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace resp {
using Command = std::vector<std::string>;
enum class Status { complete, incomplete, invalid };
struct Parsed { Status status; size_t consumed = 0; Command command; };
constexpr size_t max_buffer = 16 * 1024 * 1024;
Parsed parse(std::string_view input);
std::string simple(std::string_view s);
std::string error(std::string_view s);
std::string integer(long long n);
std::string bulk(std::string_view s);
std::string nil();
std::string array(const std::vector<std::string>& encoded);
std::string encode(const Command& command);
bool number(std::string_view text, long long& value);
}
