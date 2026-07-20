#include "connection_pool.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::mutex cout_mutex;

void log(const std::string& message) {
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << message << '\n';
}

class MockDbConnection : public foundation::IDbConnection {
public:
    explicit MockDbConnection(int id)
        : id_(id) {
        log("[connect] open connection #" + std::to_string(id_));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    ~MockDbConnection() override {
        close();
    }

    bool ping() override {
        return !closed_;
    }

    bool execute(const std::string& sql) override {
        if (closed_) {
            return false;
        }

        log("[query] connection #" + std::to_string(id_) + " executes: " + sql);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        return true;
    }

    void close() override {
        if (!closed_) {
            closed_ = true;
            log("[close] connection #" + std::to_string(id_));
        }
    }

private:
    int id_;
    bool closed_ = false;
};

void printStats(const foundation::ConnectionPoolStats& stats) {
    std::cout << "\nPool stats\n"
              << "  total:     " << stats.total << '\n'
              << "  idle:      " << stats.idle << '\n'
              << "  active:    " << stats.active << '\n'
              << "  acquired:  " << stats.acquired << '\n'
              << "  created:   " << stats.created << '\n'
              << "  destroyed: " << stats.destroyed << '\n'
              << "  timeouts:  " << stats.timeouts << '\n';
}

} // namespace

int main() {
    using namespace std::chrono_literals;

    foundation::ConnectionPoolConfig config;
    config.min_idle = 1;
    config.max_size = 3;
    config.acquire_timeout = 500ms;
    config.idle_timeout = 2s;
    config.max_lifetime = 10s;
    config.cleaner_interval = 1s;

    std::atomic<int> next_id{1};
    foundation::ConnectionPool pool(
        [&next_id] {
            return std::make_unique<MockDbConnection>(next_id.fetch_add(1));
        },
        config
    );

    pool.warmUp();

    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) {
        workers.emplace_back([&pool, i] {
            auto conn = pool.acquire();
            if (!conn) {
                log("[timeout] worker " + std::to_string(i) + " did not get a connection");
                return;
            }

            conn->execute("select * from orders where worker_id = " + std::to_string(i));
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    printStats(pool.stats());
    pool.shutdown();

    return 0;
}
