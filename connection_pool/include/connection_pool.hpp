#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace foundation {

class IDbConnection {
public:
    virtual ~IDbConnection() = default;

    virtual bool ping() = 0;
    virtual bool execute(const std::string& sql) = 0;
    virtual void close() = 0;
};

struct ConnectionPoolConfig {
    std::size_t min_idle = 1;
    std::size_t max_size = 8;
    std::chrono::milliseconds acquire_timeout{1000};
    std::chrono::milliseconds idle_timeout{30000};
    std::chrono::milliseconds max_lifetime{300000};
    std::chrono::milliseconds cleaner_interval{1000};
};

struct ConnectionPoolStats {
    std::size_t total = 0;
    std::size_t idle = 0;
    std::size_t active = 0;
    std::size_t waiting = 0;
    std::size_t created = 0;
    std::size_t destroyed = 0;
    std::size_t acquired = 0;
    std::size_t timeouts = 0;
};

class ConnectionPool {
public:
    using Factory = std::function<std::unique_ptr<IDbConnection>()>;

    explicit ConnectionPool(Factory factory, ConnectionPoolConfig config = {});
    ~ConnectionPool();

    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;
    ConnectionPool(ConnectionPool&&) = delete;
    ConnectionPool& operator=(ConnectionPool&&) = delete;

    std::shared_ptr<IDbConnection> acquire();
    std::shared_ptr<IDbConnection> tryAcquireFor(std::chrono::milliseconds timeout);

    std::size_t warmUp();
    void shutdown();
    ConnectionPoolStats stats() const;

private:
    struct State;

    std::shared_ptr<State> state_;
};

} // namespace foundation
