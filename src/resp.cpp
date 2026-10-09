#include "resp.hpp"
#include <charconv>

namespace resp {
bool number(std::string_view text, long long& value) {
    if (text.empty()) return false;
    auto r = std::from_chars(text.data(), text.data() + text.size(), value);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}
Parsed parse(std::string_view input) {
    if (input.empty()) return {Status::incomplete, 0, {}};
    if (input[0] != '*') {
        // redis-benchmark includes an inline PING workload alongside RESP arrays.
        constexpr std::string_view ping = "PING\r\n";
        for (size_t i = 0; i < input.size() && i < ping.size(); ++i) {
            char c = input[i];
            if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
            if (c != ping[i]) return {Status::invalid, 0, {}};
        }
        if (input.size() < ping.size()) return {Status::incomplete, 0, {}};
        return {Status::complete, ping.size(), {"PING"}};
    }
    size_t pos = 1;
    auto line = [&](long long& n) -> Status {
        size_t end = input.find("\r\n", pos);
        if (end == std::string_view::npos) return input.size() - pos > 32 ? Status::invalid : Status::incomplete;
        if (!number(input.substr(pos, end - pos), n)) return Status::invalid;
        pos = end + 2;
        return Status::complete;
    };
    long long count = 0;
    auto status = line(count);
    if (status != Status::complete) return {status, 0, {}};
    if (count < 1 || count > 1024) return {Status::invalid, 0, {}};
    Command cmd;
    cmd.reserve(static_cast<size_t>(count));
    for (long long i = 0; i < count; ++i) {
        if (pos == input.size()) return {Status::incomplete, 0, {}};
        if (input[pos++] != '$') return {Status::invalid, 0, {}};
        long long length = 0;
        status = line(length);
        if (status != Status::complete) return {status, 0, {}};
        if (length < 0 || length > 8 * 1024 * 1024) return {Status::invalid, 0, {}};
        size_t n = static_cast<size_t>(length);
        if (input.size() - pos < n + 2) return {Status::incomplete, 0, {}};
        if (input.substr(pos + n, 2) != "\r\n") return {Status::invalid, 0, {}};
        cmd.emplace_back(input.substr(pos, n));
        pos += n + 2;
        if (pos > max_buffer) return {Status::invalid, 0, {}};
    }
    return {Status::complete, pos, std::move(cmd)};
}
std::string simple(std::string_view s) { return "+" + std::string(s) + "\r\n"; }
std::string error(std::string_view s) { return "-ERR " + std::string(s) + "\r\n"; }
std::string integer(long long n) { return ":" + std::to_string(n) + "\r\n"; }
std::string bulk(std::string_view s) { return "$" + std::to_string(s.size()) + "\r\n" + std::string(s) + "\r\n"; }
std::string nil() { return "$-1\r\n"; }
std::string array(const std::vector<std::string>& items) {
    std::string result = "*" + std::to_string(items.size()) + "\r\n";
    for (const auto& item : items) result += item;
    return result;
}
std::string encode(const Command& command) {
    std::vector<std::string> items;
    for (const auto& arg : command) items.push_back(bulk(arg));
    return array(items);
}
}
