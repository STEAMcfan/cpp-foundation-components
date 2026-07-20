#include "connection_pool.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace {

class TestConnection : public foundation::IDbConnection {
public:
    explicit TestConnection(std::atomic<int>& alive_count)
        : alive_count_(alive_count) {
        ++alive_count_;
    }

    ~TestConnection() override {
        close();
    }

    bool ping() override {
        return !closed_;
    }

    bool execute(const std::string&) override {
        if (closed_) {
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return true;
    }

    void close() override {
        if (!closed_) {
            closed_ = true;
            --alive_count_;
        }
    }

private:
    std::atomic<int>& alive_count_;
    bool closed_ = false;
};

foundation::ConnectionPoolConfig smallConfig() {
    foundation::ConnectionPoolConfig config;
    config.min_idle = 0;
    config.max_size = 2;
    config.acquire_timeout = std::chrono::milliseconds(200);
    config.idle_timeout = std::chrono::seconds(30);
    config.max_lifetime = std::chrono::minutes(5);
    config.cleaner_interval = std::chrono::milliseconds(50);
    return config;
}

void returnsConnectionAutomatically() {
    std::atomic<int> alive{0};
    foundation::ConnectionPool pool(
        [&alive] {
            return std::make_unique<TestConnection>(alive);
        },
        smallConfig()
    );

    {
        auto conn = pool.acquire();
        assert(conn);
        assert(pool.stats().active == 1);
    }

    assert(pool.stats().active == 0);
    assert(pool.stats().idle == 1);
    assert(alive == 1);

    pool.shutdown();
    assert(alive == 0);
}

void limitsMaxConnectionsAndTimesOut() {
    std::atomic<int> alive{0};
    auto config = smallConfig();
    config.max_size = 1;

    foundation::ConnectionPool pool(
        [&alive] {
            return std::make_unique<TestConnection>(alive);
        },
        config
    );

    auto first = pool.acquire();
    assert(first);

    auto second = pool.tryAcquireFor(std::chrono::milliseconds(30));
    assert(!second);
    assert(pool.stats().timeouts == 1);

    first.reset();

    auto third = pool.tryAcquireFor(std::chrono::milliseconds(100));
    assert(third);
    third.reset();

    pool.shutdown();
}

void reusesConnectionsUnderConcurrency() {
    std::atomic<int> alive{0};
    std::atomic<int> succeeded{0};
    auto config = smallConfig();
    config.max_size = 2;

    foundation::ConnectionPool pool(
        [&alive] {
            return std::make_unique<TestConnection>(alive);
        },
        config
    );

    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) {
        workers.emplace_back([&pool, &succeeded] {
            auto conn = pool.acquire();
            assert(conn);
            assert(conn->execute("select 1"));
            ++succeeded;
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    auto stats = pool.stats();
    assert(succeeded == 8);
    assert(stats.created <= 2);
    assert(stats.total <= 2);
    assert(stats.active == 0);

    pool.shutdown();
    assert(alive == 0);
}

} // namespace

int main() {
    returnsConnectionAutomatically();
    limitsMaxConnectionsAndTimesOut();
    reusesConnectionsUnderConcurrency();
    return 0;
}
