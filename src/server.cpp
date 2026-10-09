#include "server.hpp"
#include "thread_pool.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unordered_map>
#include <unistd.h>

namespace {
struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) { if (fd < 0) throw std::runtime_error("cannot create file descriptor"); }
    ~Fd() { ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
struct Client {
    Fd fd;
    uint64_t id;
    std::string input, output;
    size_t sent = 0;
    bool busy = false, eof = false, closing = false;
    Client(int socket, uint64_t token) : fd(socket), id(token) {}
};
struct Completion { uint64_t id; std::string output; bool closing; };
constexpr size_t max_output = 32 * 1024 * 1024;
}
void serve(const Options& options, Store& store, const volatile std::sig_atomic_t& stopped) {
    Fd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    int yes = 1;
    ::setsockopt(listener.value, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(options.port));
    if (::inet_pton(AF_INET, options.bind.c_str(), &address.sin_addr) != 1) throw std::runtime_error("bind must be an IPv4 address");
    if (::bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) throw std::runtime_error("cannot bind port " + std::to_string(options.port));
    if (::listen(listener.value, 512) < 0) throw std::runtime_error("listen failed");
    Fd epoll(::epoll_create1(EPOLL_CLOEXEC));
    Fd wake(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    auto watch = [&](int operation, int fd, uint64_t id, uint32_t flags) {
        epoll_event event{}; event.events = flags; event.data.u64 = id;
        return ::epoll_ctl(epoll.value, operation, fd, &event) == 0;
    };
    if (!watch(EPOLL_CTL_ADD, listener.value, 0, EPOLLIN) || !watch(EPOLL_CTL_ADD, wake.value, 1, EPOLLIN)) throw std::runtime_error("epoll registration failed");
    std::unordered_map<uint64_t, std::unique_ptr<Client>> clients;
    std::mutex completed_mutex;
    std::deque<Completion> completed;
    // Pool is destroyed before completion queue, sockets and eventfd.
    ThreadPool pool(static_cast<size_t>(options.threads));
    uint64_t next_id = 2;
    auto remove = [&](uint64_t id) { clients.erase(id); };
    auto update = [&](Client& client) {
        uint32_t flags = 0;
        if (!client.eof && !client.closing) flags |= EPOLLIN | EPOLLRDHUP;
        if (client.sent < client.output.size()) flags |= EPOLLOUT;
        return watch(EPOLL_CTL_MOD, client.fd.value, client.id, flags);
    };
    auto dispatch = [&](Client& client) {
        if (client.busy || client.closing || client.output.size() - client.sent > 1024 * 1024) return;
        std::vector<resp::Command> batch;
        size_t consumed = 0;
        bool closing = false;
        std::string suffix;
        while (batch.size() < 128 && consumed < client.input.size()) {
            auto parsed = resp::parse(std::string_view(client.input).substr(consumed));
            if (parsed.status == resp::Status::incomplete && !client.eof) break;
            if (parsed.status != resp::Status::complete) {
                suffix = resp::error("protocol error"); closing = true; break;
            }
            consumed += parsed.consumed;
            bool quit = parsed.command.size() == 1 && (parsed.command[0] == "QUIT" || parsed.command[0] == "quit");
            // Commands are case insensitive, including mixed-case QUIT.
            if (parsed.command.size() == 1) {
                auto name = parsed.command[0];
                for (char& c : name) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
                quit = name == "QUIT";
            }
            batch.push_back(std::move(parsed.command));
            if (quit) { closing = true; break; }
        }
        client.input.erase(0, consumed);
        if (batch.empty() && suffix.empty()) return;
        client.busy = true;
        const auto id = client.id;
        if (!pool.submit([&, id, batch = std::move(batch), closing, suffix = std::move(suffix)]() mutable {
            std::string output;
            for (const auto& command : batch) {
                auto response = store.execute(command);
                if (response.size() > max_output - output.size()) { closing = true; break; }
                output += response;
            }
            output += suffix;
            { std::lock_guard<std::mutex> lock(completed_mutex); completed.push_back({id, std::move(output), closing}); }
            uint64_t one = 1;
            while (::write(wake.value, &one, sizeof(one)) < 0 && errno == EINTR) {}
        })) {
            client.busy = false; client.closing = true; client.output += resp::error("server is busy");
        }
    };
    auto flush = [&](Client& client) {
        while (client.sent < client.output.size()) {
            ssize_t n = ::send(client.fd.value, client.output.data() + client.sent, client.output.size() - client.sent, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n <= 0) return false;
            client.sent += static_cast<size_t>(n);
        }
        if (client.sent == client.output.size()) { client.output.clear(); client.sent = 0; }
        else if (client.sent > 1024 * 1024) { client.output.erase(0, client.sent); client.sent = 0; }
        return true;
    };
    auto service = [&](uint64_t id) {
        auto it = clients.find(id);
        if (it == clients.end()) return;
        auto& client = *it->second;
        if (!flush(client)) { remove(id); return; }
        dispatch(client);
        if (!client.busy && client.output.empty() && (client.closing || (client.eof && client.input.empty()))) { remove(id); return; }
        if (!update(client)) remove(id);
    };
    std::cout << "Mini Redis listening on " << options.bind << ':' << options.port
              << " | workers=" << options.threads << " | AOF=" << (options.aof.empty() ? "off" : options.aof)
              << " | fsync=" << (options.sync ? "always" : "no") << std::endl;
    auto next_expiration = std::chrono::steady_clock::now();
    while (!stopped) {
        epoll_event events[128];
        int count = ::epoll_wait(epoll.value, events, 128, 100);
        if (count < 0) { if (errno == EINTR) continue; throw std::runtime_error("epoll_wait failed"); }
        for (int i = 0; i < count; ++i) {
            const auto id = events[i].data.u64;
            if (id == 0) {
                // Bound each accept turn to keep existing clients responsive.
                for (int accepted = 0; accepted < 128; ++accepted) {
                    int fd = ::accept4(listener.value, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (fd < 0) { if (errno == EINTR) { --accepted; continue; } break; }
                    if (clients.size() >= 4096) { ::close(fd); continue; }
                    auto client = std::make_unique<Client>(fd, next_id++);
                    if (watch(EPOLL_CTL_ADD, fd, client->id, EPOLLIN | EPOLLRDHUP)) clients.emplace(client->id, std::move(client));
                }
            } else if (id == 1) {
                uint64_t n;
                while (::read(wake.value, &n, sizeof(n)) < 0 && errno == EINTR) {}
                std::deque<Completion> ready;
                { std::lock_guard<std::mutex> lock(completed_mutex); ready.swap(completed); }
                for (auto& completion : ready) {
                    auto it = clients.find(completion.id);
                    if (it == clients.end()) continue;
                    auto& client = *it->second;
                    client.busy = false;
                    if (completion.output.size() > max_output - (client.output.size() - client.sent)) { remove(completion.id); continue; }
                    client.output += completion.output;
                    client.closing = completion.closing;
                    service(completion.id);
                }
            } else {
                auto it = clients.find(id);
                if (it == clients.end()) continue;
                auto& client = *it->second;
                if (events[i].events & EPOLLERR) { remove(id); continue; }
                bool bad = false;
                if (events[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) {
                    char buffer[65536];
                    size_t read_this_turn = 0;
                    while (!client.eof && !client.closing && read_this_turn < 256 * 1024) {
                        ssize_t n = ::recv(client.fd.value, buffer, sizeof(buffer), 0);
                        if (n < 0 && errno == EINTR) continue;
                        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                        if (n < 0) { bad = true; break; }
                        if (n == 0) { client.eof = true; break; }
                        client.input.append(buffer, static_cast<size_t>(n));
                        read_this_turn += static_cast<size_t>(n);
                        if (client.input.size() > resp::max_buffer) { bad = true; break; }
                    }
                }
                if (bad) { remove(id); continue; }
                service(id);
            }
        }
        if (std::chrono::steady_clock::now() >= next_expiration) {
            pool.submit([&store] { store.expire(); });
            next_expiration = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
    }
    pool.shutdown();
    std::cout << "Mini Redis stopped" << std::endl;
}
