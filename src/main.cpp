#include "server.hpp"
#include <iostream>
#include <stdexcept>

namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
int positive(const std::string& value, int maximum) {
    long long n;
    if (!resp::number(value, n) || n < 1 || n > maximum) throw std::runtime_error("invalid numeric option: " + value);
    return static_cast<int>(n);
}
}
int main(int argc, char** argv) {
    try {
        Options options;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "mini_redis [--bind IPv4] [--port 6379] [--threads 4]\n"
                             "           [--aof PATH | --no-aof] [--fsync always|no]\n";
                return 0;
            }
            if (arg == "--no-aof") { options.aof.clear(); continue; }
            if (i + 1 == argc) throw std::runtime_error("missing value for " + arg);
            std::string value = argv[++i];
            if (arg == "--bind") options.bind = value;
            else if (arg == "--port") options.port = positive(value, 65535);
            else if (arg == "--threads") options.threads = positive(value, 128);
            else if (arg == "--aof") { if (value.empty()) throw std::runtime_error("empty AOF path"); options.aof = value; }
            else if (arg == "--fsync") {
                if (value != "always" && value != "no") throw std::runtime_error("fsync must be always or no");
                options.sync = value == "always";
            } else throw std::runtime_error("unknown option: " + arg);
        }
        std::signal(SIGINT, stop); std::signal(SIGTERM, stop); std::signal(SIGPIPE, SIG_IGN);
        Store store(options.aof, options.sync);
        serve(options, store, stopped);
    } catch (const std::exception& e) { std::cerr << "mini_redis: " << e.what() << '\n'; return 1; }
}
