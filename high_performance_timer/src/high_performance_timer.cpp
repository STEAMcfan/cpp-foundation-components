#include "high_performance_timer.hpp"

#include <condition_variable>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace foundation {
namespace {

using Clock = HighPerformanceTimer::Clock;
using TimePoint = HighPerformanceTimer::TimePoint;
using TimerId = HighPerformanceTimer::TimerId;

} // namespace

struct HighPerformanceTimer::State {
    struct TimerEntry {
        TimerId id = invalid_timer_id;
        TimePoint deadline;
        std::chrono::milliseconds interval{0};
        bool repeating = false;
        std::uint64_t generation = 0;
        Callback callback;
    };

    struct HeapItem {
        TimePoint deadline;
        TimerId id = invalid_timer_id;
        std::uint64_t generation = 0;

        bool operator>(const HeapItem& other) const {
            if (deadline != other.deadline) {
                return deadline > other.deadline;
            }
            return id > other.id;
        }
    };

    explicit State(Config timer_config)
        : config(timer_config) {}

    ~State() {
        shutdown();
    }

    Config config;

    mutable std::mutex mutex;
    std::condition_variable timer_changed;
    std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<HeapItem>> heap;
    std::unordered_map<TimerId, TimerEntry> timers;

    std::thread scheduler;
    bool shutting_down = false;
    TimerId next_id = 1;

    mutable std::mutex callback_mutex;
    std::condition_variable callback_available;
    std::queue<Callback> ready_callbacks;
    std::vector<std::thread> callback_workers;
    bool callback_stopping = false;

    std::size_t scheduled = 0;
    std::size_t cancelled = 0;
    std::size_t fired = 0;

    void start() {
        scheduler = std::thread([this] {
            schedulerLoop();
        });

        callback_workers.reserve(config.callback_threads);
        for (std::size_t i = 0; i < config.callback_threads; ++i) {
            callback_workers.emplace_back([this] {
                callbackLoop();
            });
        }
    }

    TimerId addTimer(TimePoint deadline,
                     std::chrono::milliseconds interval,
                     bool repeating,
                     Callback callback) {
        if (!callback) {
            throw std::invalid_argument("HighPerformanceTimer callback cannot be empty");
        }

        if (repeating && interval.count() <= 0) {
            throw std::invalid_argument("HighPerformanceTimer interval must be positive");
        }

        const auto now = Clock::now();
        if (deadline < now) {
            deadline = now;
        }

        std::unique_lock<std::mutex> lock(mutex);
        if (shutting_down) {
            return invalid_timer_id;
        }

        const TimerId id = next_id++;
        if (next_id == invalid_timer_id) {
            ++next_id;
        }

        TimerEntry entry;
        entry.id = id;
        entry.deadline = deadline;
        entry.interval = interval;
        entry.repeating = repeating;
        entry.generation = 1;
        entry.callback = std::move(callback);

        timers.emplace(id, std::move(entry));
        heap.push(HeapItem{deadline, id, 1});
        ++scheduled;

        lock.unlock();
        timer_changed.notify_one();
        return id;
    }

    bool cancel(TimerId id) {
        if (id == invalid_timer_id) {
            return false;
        }

        std::unique_lock<std::mutex> lock(mutex);
        const auto erased = timers.erase(id);
        if (erased == 0) {
            return false;
        }

        ++cancelled;
        lock.unlock();
        timer_changed.notify_one();
        return true;
    }

    std::size_t cancelAll() {
        std::unique_lock<std::mutex> lock(mutex);
        const auto count = timers.size();
        timers.clear();
        cancelled += count;

        lock.unlock();
        timer_changed.notify_all();
        return count;
    }

    void shutdown() {
        {
            std::unique_lock<std::mutex> lock(mutex);
            shutting_down = true;
            timers.clear();
        }
        timer_changed.notify_all();

        if (scheduler.joinable() && scheduler.get_id() != std::this_thread::get_id()) {
            scheduler.join();
        }

        {
            std::unique_lock<std::mutex> lock(callback_mutex);
            callback_stopping = true;
        }
        callback_available.notify_all();

        for (auto& worker : callback_workers) {
            if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
                worker.join();
            }
        }
    }

    Stats stats() const {
        Stats snapshot;

        {
            std::unique_lock<std::mutex> lock(mutex);
            snapshot.scheduled = scheduled;
            snapshot.cancelled = cancelled;
            snapshot.fired = fired;
            snapshot.active = timers.size();
            snapshot.callback_threads = config.callback_threads;
        }

        {
            std::unique_lock<std::mutex> lock(callback_mutex);
            snapshot.pending_callbacks = ready_callbacks.size();
        }

        return snapshot;
    }

    bool isCurrent(const HeapItem& item) const {
        const auto it = timers.find(item.id);
        return it != timers.end() &&
               it->second.generation == item.generation &&
               it->second.deadline == item.deadline;
    }

    void discardStaleHeapItems() {
        while (!heap.empty() && !isCurrent(heap.top())) {
            heap.pop();
        }
    }

    void schedulerLoop() {
        while (true) {
            std::vector<Callback> due_callbacks;

            {
                std::unique_lock<std::mutex> lock(mutex);

                while (true) {
                    if (shutting_down) {
                        return;
                    }

                    discardStaleHeapItems();
                    if (heap.empty()) {
                        timer_changed.wait(lock);
                        continue;
                    }

                    const auto next_deadline = heap.top().deadline;
                    const auto now = Clock::now();
                    if (next_deadline > now) {
                        timer_changed.wait_until(lock, next_deadline);
                        continue;
                    }

                    break;
                }

                const auto now = Clock::now();
                while (!heap.empty()) {
                    discardStaleHeapItems();
                    if (heap.empty() || heap.top().deadline > now) {
                        break;
                    }

                    const HeapItem item = heap.top();
                    heap.pop();

                    auto it = timers.find(item.id);
                    if (it == timers.end() ||
                        it->second.generation != item.generation ||
                        it->second.deadline != item.deadline) {
                        continue;
                    }

                    Callback callback = it->second.callback;

                    if (it->second.repeating) {
                        auto next_deadline = it->second.deadline + it->second.interval;
                        while (next_deadline <= now) {
                            next_deadline += it->second.interval;
                        }

                        it->second.deadline = next_deadline;
                        ++it->second.generation;
                        heap.push(HeapItem{
                            it->second.deadline,
                            it->second.id,
                            it->second.generation,
                        });
                    } else {
                        timers.erase(it);
                    }

                    ++fired;
                    due_callbacks.push_back(std::move(callback));
                }
            }

            enqueueCallbacks(std::move(due_callbacks));
        }
    }

    void enqueueCallbacks(std::vector<Callback> callbacks) {
        if (callbacks.empty()) {
            return;
        }

        {
            std::unique_lock<std::mutex> lock(callback_mutex);
            for (auto& callback : callbacks) {
                ready_callbacks.push(std::move(callback));
            }
        }

        callback_available.notify_all();
    }

    void callbackLoop() {
        while (true) {
            Callback callback;

            {
                std::unique_lock<std::mutex> lock(callback_mutex);
                callback_available.wait(lock, [this] {
                    return callback_stopping || !ready_callbacks.empty();
                });

                if (ready_callbacks.empty() && callback_stopping) {
                    return;
                }

                callback = std::move(ready_callbacks.front());
                ready_callbacks.pop();
            }

            try {
                callback();
            } catch (...) {
            }
        }
    }
};

HighPerformanceTimer::HighPerformanceTimer(Config config)
    : state_(std::make_shared<State>(config)) {
    if (state_->config.callback_threads == 0) {
        throw std::invalid_argument("HighPerformanceTimer callback_threads must be positive");
    }

    state_->start();
}

HighPerformanceTimer::~HighPerformanceTimer() {
    shutdown();
}

HighPerformanceTimer::TimerId
HighPerformanceTimer::scheduleAt(TimePoint deadline, Callback callback) {
    return state_->addTimer(deadline, std::chrono::milliseconds{0}, false, std::move(callback));
}

HighPerformanceTimer::TimerId
HighPerformanceTimer::scheduleAfter(std::chrono::milliseconds delay, Callback callback) {
    if (delay.count() < 0) {
        delay = std::chrono::milliseconds{0};
    }

    return scheduleAt(Clock::now() + delay, std::move(callback));
}

HighPerformanceTimer::TimerId
HighPerformanceTimer::scheduleEvery(std::chrono::milliseconds interval, Callback callback) {
    return state_->addTimer(Clock::now() + interval, interval, true, std::move(callback));
}

bool HighPerformanceTimer::cancel(TimerId id) {
    return state_->cancel(id);
}

std::size_t HighPerformanceTimer::cancelAll() {
    return state_->cancelAll();
}

void HighPerformanceTimer::shutdown() {
    if (state_) {
        state_->shutdown();
    }
}

HighPerformanceTimer::Stats HighPerformanceTimer::stats() const {
    return state_->stats();
}

} // namespace foundation
