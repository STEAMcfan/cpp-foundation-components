#include "connection_pool.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace foundation {
namespace {

using Clock = std::chrono::steady_clock;

bool isExpired(Clock::time_point start,
               Clock::time_point now,
               std::chrono::milliseconds limit) {
    return limit.count() >= 0 && now - start >= limit;
}

} // namespace

struct ConnectionPool::State {
    struct Entry {
        std::unique_ptr<IDbConnection> connection;
        Clock::time_point created_at;
        Clock::time_point last_used_at;
    };

    explicit State(Factory connection_factory, ConnectionPoolConfig pool_config)
        : factory(std::move(connection_factory)),
          config(pool_config) {}

    ~State() {
        shutdown();
    }

    Factory factory;
    ConnectionPoolConfig config;

    mutable std::mutex mutex;
    std::condition_variable available;
    std::condition_variable shutdown_done;
    std::condition_variable cleaner_wakeup;
    std::deque<std::unique_ptr<Entry>> idle;

    std::thread cleaner;
    bool shutting_down = false;

    std::size_t total = 0;
    std::size_t active = 0;
    std::size_t waiting = 0;
    std::size_t created = 0;
    std::size_t destroyed = 0;
    std::size_t acquired = 0;
    std::size_t timeouts = 0;

    void startCleaner() {
        cleaner = std::thread([this] {
            cleanerLoop();
        });
    }

    void shutdown() {
        std::deque<std::unique_ptr<Entry>> idle_to_close;

        {
            std::unique_lock<std::mutex> lock(mutex);
            shutting_down = true;
            available.notify_all();
            cleaner_wakeup.notify_all();
        }

        if (cleaner.joinable() && cleaner.get_id() != std::this_thread::get_id()) {
            cleaner.join();
        }

        {
            std::unique_lock<std::mutex> lock(mutex);
            shutdown_done.wait(lock, [this] {
                return active == 0;
            });

            idle_to_close.swap(idle);
            destroyed += idle_to_close.size();
            total -= idle_to_close.size();
        }

        closeEntries(std::move(idle_to_close));
    }

    std::unique_ptr<Entry> createEntry() {
        try {
            auto connection = factory();
            if (!connection) {
                return nullptr;
            }

            const auto now = Clock::now();
            return std::make_unique<Entry>(Entry{
                std::move(connection),
                now,
                now,
            });
        } catch (...) {
            return nullptr;
        }
    }

    std::size_t warmUp() {
        std::size_t warmed = 0;

        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (shutting_down || idle.size() >= config.min_idle || total >= config.max_size) {
                    break;
                }
                ++total;
            }

            auto entry = createEntry();
            if (!entry) {
                std::unique_lock<std::mutex> lock(mutex);
                --total;
                available.notify_one();
                break;
            }

            std::unique_ptr<Entry> close_now;
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (shutting_down) {
                    --total;
                    close_now = std::move(entry);
                } else {
                    ++created;
                    ++warmed;
                    idle.push_back(std::move(entry));
                    available.notify_one();
                }
            }

            closeEntry(std::move(close_now));
        }

        return warmed;
    }

    void release(std::unique_ptr<Entry> entry) {
        if (!entry) {
            return;
        }

        entry->last_used_at = Clock::now();
        const bool reached_lifetime =
            isExpired(entry->created_at, entry->last_used_at, config.max_lifetime);

        std::unique_ptr<Entry> close_now;
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (shutting_down || reached_lifetime) {
                --active;
                --total;
                ++destroyed;
                close_now = std::move(entry);
            } else {
                --active;
                idle.push_back(std::move(entry));
            }

            available.notify_one();
            if (active == 0) {
                shutdown_done.notify_all();
            }
        }

        closeEntry(std::move(close_now));
    }

    bool shouldRetire(const Entry& entry, Clock::time_point now) const {
        return isExpired(entry.created_at, now, config.max_lifetime) ||
               isExpired(entry.last_used_at, now, config.idle_timeout);
    }

    bool isHealthy(Entry& entry) const {
        try {
            return entry.connection && entry.connection->ping();
        } catch (...) {
            return false;
        }
    }

    void retireActive(std::unique_ptr<Entry> entry) {
        closeEntry(std::move(entry));

        {
            std::unique_lock<std::mutex> lock(mutex);
            --active;
            --total;
            ++destroyed;
            available.notify_one();
            if (active == 0) {
                shutdown_done.notify_all();
            }
        }
    }

    void closeEntry(std::unique_ptr<Entry> entry) {
        if (!entry || !entry->connection) {
            return;
        }

        try {
            entry->connection->close();
        } catch (...) {
        }
    }

    void closeEntries(std::deque<std::unique_ptr<Entry>> entries) {
        for (auto& entry : entries) {
            closeEntry(std::move(entry));
        }
    }

    void cleanupIdle() {
        std::deque<std::unique_ptr<Entry>> stale;
        const auto now = Clock::now();

        {
            std::unique_lock<std::mutex> lock(mutex);
            auto it = idle.begin();
            while (it != idle.end()) {
                if (shouldRetire(**it, now) && total > config.min_idle) {
                    stale.push_back(std::move(*it));
                    it = idle.erase(it);
                    --total;
                    ++destroyed;
                } else {
                    ++it;
                }
            }

            if (!stale.empty()) {
                available.notify_all();
            }
        }

        closeEntries(std::move(stale));
    }

    void cleanerLoop() {
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (cleaner_wakeup.wait_for(lock, config.cleaner_interval, [this] {
                        return shutting_down;
                    })) {
                    return;
                }
            }

            cleanupIdle();
            warmUp();
        }
    }
};

ConnectionPool::ConnectionPool(Factory factory, ConnectionPoolConfig config)
    : state_(std::make_shared<State>(std::move(factory), config)) {
    if (!state_->factory) {
        throw std::invalid_argument("ConnectionPool requires a connection factory");
    }

    if (state_->config.max_size == 0) {
        throw std::invalid_argument("ConnectionPool max_size must be greater than 0");
    }

    if (state_->config.min_idle > state_->config.max_size) {
        throw std::invalid_argument("ConnectionPool min_idle cannot exceed max_size");
    }

    if (state_->config.cleaner_interval.count() <= 0) {
        throw std::invalid_argument("ConnectionPool cleaner_interval must be positive");
    }

    state_->startCleaner();
}

ConnectionPool::~ConnectionPool() {
    shutdown();
}

std::shared_ptr<IDbConnection> ConnectionPool::acquire() {
    return tryAcquireFor(state_->config.acquire_timeout);
}

std::shared_ptr<IDbConnection> ConnectionPool::tryAcquireFor(std::chrono::milliseconds timeout) {
    if (timeout.count() < 0) {
        timeout = std::chrono::milliseconds{0};
    }

    const auto deadline = Clock::now() + timeout;
    auto state = state_;

    while (true) {
        std::unique_ptr<State::Entry> entry;
        bool create_new = false;

        {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (state->shutting_down) {
                return {};
            }

            if (!state->idle.empty()) {
                entry = std::move(state->idle.front());
                state->idle.pop_front();
                ++state->active;
            } else if (state->total < state->config.max_size) {
                ++state->total;
                ++state->active;
                create_new = true;
            } else {
                ++state->waiting;
                const bool ready = state->available.wait_until(lock, deadline, [&state] {
                    return state->shutting_down || !state->idle.empty() ||
                           state->total < state->config.max_size;
                });
                --state->waiting;

                if (!ready) {
                    ++state->timeouts;
                    return {};
                }
                continue;
            }
        }

        if (create_new) {
            entry = state->createEntry();
            if (!entry) {
                std::unique_lock<std::mutex> lock(state->mutex);
                --state->active;
                --state->total;
                state->available.notify_one();
                if (state->active == 0) {
                    state->shutdown_done.notify_all();
                }
                return {};
            }

            std::unique_lock<std::mutex> lock(state->mutex);
            ++state->created;
        } else {
            const auto now = Clock::now();
            if (state->shouldRetire(*entry, now) || !state->isHealthy(*entry)) {
                state->retireActive(std::move(entry));
                continue;
            }
        }

        {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (state->shutting_down) {
                lock.unlock();
                state->retireActive(std::move(entry));
                return {};
            }
            ++state->acquired;
        }

        auto holder = std::shared_ptr<State::Entry>(
            entry.release(),
            [state](State::Entry* raw) {
                state->release(std::unique_ptr<State::Entry>(raw));
            });

        return std::shared_ptr<IDbConnection>(holder, holder->connection.get());
    }
}

std::size_t ConnectionPool::warmUp() {
    return state_->warmUp();
}

void ConnectionPool::shutdown() {
    if (state_) {
        state_->shutdown();
    }
}

ConnectionPoolStats ConnectionPool::stats() const {
    std::unique_lock<std::mutex> lock(state_->mutex);
    return ConnectionPoolStats{
        state_->total,
        state_->idle.size(),
        state_->active,
        state_->waiting,
        state_->created,
        state_->destroyed,
        state_->acquired,
        state_->timeouts,
    };
}

} // namespace foundation
