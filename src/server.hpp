#pragma once
#include "store.hpp"
#include <csignal>
#include <string>

struct Options {
    std::string bind = "127.0.0.1";
    int port = 6379;
    int threads = 4;
    std::string aof = "mini-redis.aof";
    bool sync = true;
};
void serve(const Options& options, Store& store, const volatile std::sig_atomic_t& stopped);
