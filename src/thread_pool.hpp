#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class ThreadPool {
    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
public:
    explicit ThreadPool(size_t count) {
        try {
            for (size_t i = 0; i < count; ++i) workers_.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                        if (stopping_ && tasks_.empty()) return;
                        task = std::move(tasks_.front()); tasks_.pop();
                    }
                    task();
                }
            });
        } catch (...) { shutdown(); throw; }
    }
    bool submit(std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || tasks_.size() >= 4096) return false;
        tasks_.push(std::move(task)); ready_.notify_one(); return true;
    }
    void shutdown() {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
        ready_.notify_all();
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
    }
    ~ThreadPool() { shutdown(); }
};
