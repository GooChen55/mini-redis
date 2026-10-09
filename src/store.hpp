#pragma once
#include "resp.hpp"
#include <mutex>
#include <unordered_map>

class Store {
    struct Entry { std::string value; long long expires = 0; };
    std::unordered_map<std::string, Entry> entries_;
    std::mutex mutex_;
    int aof_ = -1;
    bool sync_;
    bool failed_ = false;
    std::string execute_locked(const resp::Command& command, bool replay);
    void append(const resp::Command& command);
    void purge();
public:
    Store(const std::string& path, bool sync);
    ~Store();
    std::string execute(const resp::Command& command);
    void expire();
};
