#include "store.hpp"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/file.h>
#include <unistd.h>

namespace {
long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
}
Store::Store(const std::string& path, bool sync) : sync_(sync) {
    if (path.empty()) return;
    aof_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (aof_ < 0) throw std::runtime_error("cannot open AOF: " + path);
    try {
        if (::flock(aof_, LOCK_EX | LOCK_NB) < 0) throw std::runtime_error("AOF is already in use: " + path);
        std::string buffer;
        char chunk[65536];
        off_t valid = 0;
        for (;;) {
            ssize_t n = ::read(aof_, chunk, sizeof(chunk));
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) throw std::runtime_error("cannot read AOF");
            if (n == 0) break;
            buffer.append(chunk, static_cast<size_t>(n));
            size_t consumed = 0;
            for (;;) {
                auto parsed = resp::parse(std::string_view(buffer).substr(consumed));
                if (parsed.status == resp::Status::invalid) throw std::runtime_error("corrupt AOF at byte " + std::to_string(valid));
                if (parsed.status == resp::Status::incomplete) break;
                const auto& command = parsed.command;
                if (command[0] != "SET" && command[0] != "DEL" && command[0] != "PEXPIREAT") throw std::runtime_error("unsupported AOF record");
                auto response = execute_locked(command, true);
                if (!response.empty() && response[0] == '-') throw std::runtime_error("invalid AOF record: " + response);
                consumed += parsed.consumed; valid += static_cast<off_t>(parsed.consumed);
            }
            buffer.erase(0, consumed);
            if (buffer.size() > resp::max_buffer) throw std::runtime_error("oversized AOF record");
        }
        // A crash can leave a partial final record; truncate only this incomplete tail.
        if (!buffer.empty()) {
            if (::ftruncate(aof_, valid) < 0 || ::fsync(aof_) < 0) throw std::runtime_error("cannot repair AOF tail");
        }
        purge();
    } catch (...) { ::close(aof_); aof_ = -1; throw; }
}
Store::~Store() { if (aof_ >= 0) { ::fsync(aof_); ::close(aof_); } }
void Store::append(const resp::Command& command) {
    if (failed_) throw std::runtime_error("AOF is unavailable; restart after fixing storage");
    if (aof_ < 0) return;
    auto record = resp::encode(command);
    size_t offset = 0;
    while (offset < record.size()) {
        ssize_t n = ::write(aof_, record.data() + offset, record.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { failed_ = true; throw std::runtime_error("AOF write failed"); }
        offset += static_cast<size_t>(n);
    }
    if (sync_ && ::fsync(aof_) < 0) { failed_ = true; throw std::runtime_error("AOF fsync failed"); }
}
void Store::purge() {
    const auto now = now_ms();
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.expires && it->second.expires <= now) it = entries_.erase(it);
        else ++it;
    }
}
void Store::expire() { std::lock_guard<std::mutex> lock(mutex_); purge(); }
std::string Store::execute(const resp::Command& command) {
    std::lock_guard<std::mutex> lock(mutex_);
    try { return execute_locked(command, false); }
    catch (const std::exception& e) { return resp::error(e.what()); }
}
std::string Store::execute_locked(const resp::Command& args, bool replay) {
    if (args.empty()) return resp::error("empty command");
    const auto cmd = upper(args[0]);
    auto arity = [&] { return resp::error("wrong number of arguments for '" + cmd + "'"); };
    auto find = [&](const std::string& key) {
        auto it = entries_.find(key);
        if (!replay && it != entries_.end() && it->second.expires && it->second.expires <= now_ms()) {
            entries_.erase(it); return entries_.end();
        }
        return it;
    };
    if (cmd == "PING") {
        if (args.size() > 2) return arity();
        return args.size() == 1 ? resp::simple("PONG") : resp::bulk(args[1]);
    }
    if (cmd == "ECHO") return args.size() == 2 ? resp::bulk(args[1]) : arity();
    if (cmd == "SET") {
        if (args.size() != 3 && args.size() != 5) return arity();
        long long expires = 0;
        if (args.size() == 5) {
            long long n;
            auto option = upper(args[3]);
            if (!resp::number(args[4], n) || n <= 0) return resp::error("invalid expire time");
            if (option == "PXAT") expires = n;
            else if (option == "EX" || option == "PX") {
                const auto factor = option == "EX" ? 1000 : 1;
                const auto now = now_ms();
                if (n > (std::numeric_limits<long long>::max() - now) / factor) return resp::error("invalid expire time");
                expires = now + n * factor;
            } else return resp::error("syntax error");
        }
        resp::Command record{"SET", args[1], args[2]};
        if (expires) { record.push_back("PXAT"); record.push_back(std::to_string(expires)); }
        if (!replay) append(record);
        entries_[args[1]] = {args[2], expires};
        return resp::simple("OK");
    }
    if (cmd == "GET") {
        if (args.size() != 2) return arity();
        auto it = find(args[1]);
        return it == entries_.end() ? resp::nil() : resp::bulk(it->second.value);
    }
    if (cmd == "DEL" || cmd == "EXISTS") {
        if (args.size() < 2) return arity();
        if (cmd == "DEL" && !replay) { auto record = args; record[0] = "DEL"; append(record); }
        long long count = 0;
        for (size_t i = 1; i < args.size(); ++i) {
            auto it = find(args[i]);
            if (it != entries_.end()) { ++count; if (cmd == "DEL") entries_.erase(it); }
        }
        return resp::integer(count);
    }
    if (cmd == "INCR" || cmd == "DECR") {
        if (args.size() != 2) return arity();
        auto it = find(args[1]);
        long long value = 0, expires = 0;
        if (it != entries_.end()) {
            if (!resp::number(it->second.value, value)) return resp::error("value is not an integer or out of range");
            expires = it->second.expires;
        }
        if ((cmd == "INCR" && value == std::numeric_limits<long long>::max()) ||
            (cmd == "DECR" && value == std::numeric_limits<long long>::min())) return resp::error("increment or decrement would overflow");
        value += cmd == "INCR" ? 1 : -1;
        resp::Command record{"SET", args[1], std::to_string(value)};
        if (expires) { record.push_back("PXAT"); record.push_back(std::to_string(expires)); }
        if (!replay) append(record);
        entries_[args[1]] = {std::to_string(value), expires};
        return resp::integer(value);
    }
    if (cmd == "EXPIRE" || cmd == "PEXPIREAT") {
        if (args.size() != 3) return arity();
        long long n;
        if (!resp::number(args[2], n)) return resp::error("value is not an integer or out of range");
        auto it = find(args[1]);
        if (it == entries_.end()) return resp::integer(0);
        long long expires = n;
        if (cmd == "EXPIRE") {
            const auto now = now_ms();
            if (n > (std::numeric_limits<long long>::max() - now) / 1000) return resp::error("invalid expire time");
            expires = n <= 0 ? now : now + n * 1000;
        }
        if (!replay) append({"PEXPIREAT", args[1], std::to_string(expires)});
        // Replay must preserve records in order even if already expired today.
        if (!replay && expires <= now_ms()) entries_.erase(it);
        else it->second.expires = expires <= 0 ? 1 : expires;
        return resp::integer(1);
    }
    if (cmd == "TTL" || cmd == "PTTL") {
        if (args.size() != 2) return arity();
        auto it = find(args[1]);
        if (it == entries_.end()) return resp::integer(-2);
        if (!it->second.expires) return resp::integer(-1);
        auto remaining = std::max(0LL, it->second.expires - now_ms());
        return resp::integer(cmd == "TTL" ? remaining / 1000 : remaining);
    }
    if (cmd == "MGET") {
        if (args.size() < 2) return arity();
        std::vector<std::string> values;
        for (size_t i = 1; i < args.size(); ++i) {
            auto it = find(args[i]); values.push_back(it == entries_.end() ? resp::nil() : resp::bulk(it->second.value));
        }
        return resp::array(values);
    }
    if (cmd == "DBSIZE") { if (args.size() != 1) return arity(); purge(); return resp::integer(static_cast<long long>(entries_.size())); }
    if (cmd == "COMMAND") { if (args.size() != 1) return arity(); return resp::array({}); }
    if (cmd == "QUIT") return args.size() == 1 ? resp::simple("OK") : arity();
    return resp::error("unknown command");
}
