#include "high_performance_timer.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace {

foundation::HighPerformanceTimer makeTimer(std::size_t callback_threads = 2) {
    foundation::HighPerformanceTimer::Config config;
    config.callback_threads = callback_threads;
    return foundation::HighPerformanceTimer(config);
}

void firesOneShotTimer() {
    using namespace std::chrono_literals;

    auto timer = makeTimer();

    std::mutex mutex;
    std::condition_variable done;
    bool fired = false;

    const auto id = timer.scheduleAfter(20ms, [&] {
        std::lock_guard<std::mutex> lock(mutex);
        fired = true;
        done.notify_one();
    });

    assert(id != foundation::HighPerformanceTimer::invalid_timer_id);

    std::unique_lock<std::mutex> lock(mutex);
    assert(done.wait_for(lock, 1s, [&] {
        return fired;
    }));

    auto stats = timer.stats();
    assert(stats.scheduled == 1);
    assert(stats.fired == 1);
    assert(stats.active == 0);

    timer.shutdown();
}

void cancelPreventsOneShotTimer() {
    using namespace std::chrono_literals;

    auto timer = makeTimer();
    std::atomic<int> fired{0};

    const auto id = timer.scheduleAfter(100ms, [&] {
        ++fired;
    });

    assert(timer.cancel(id));
    std::this_thread::sleep_for(150ms);

    assert(fired == 0);
    assert(timer.stats().cancelled == 1);

    timer.shutdown();
}

void repeatingTimerCanBeCancelled() {
    using namespace std::chrono_literals;

    auto timer = makeTimer(1);

    std::mutex mutex;
    std::condition_variable done;
    std::atomic<int> fired{0};

    const auto id = timer.scheduleEvery(10ms, [&] {
        const int count = fired.fetch_add(1) + 1;
        if (count >= 4) {
            done.notify_one();
        }
    });

    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(done.wait_for(lock, 1s, [&] {
            return fired.load() >= 4;
        }));
    }

    const int before_cancel = fired.load();
    assert(timer.cancel(id));

    std::this_thread::sleep_for(50ms);
    assert(fired.load() <= before_cancel + 1);
    assert(timer.stats().active == 0);

    timer.shutdown();
}

void handlesManyTimersFromManyThreads() {
    using namespace std::chrono_literals;

    auto timer = makeTimer(4);

    constexpr int thread_count = 4;
    constexpr int timers_per_thread = 250;
    constexpr int total = thread_count * timers_per_thread;

    std::mutex mutex;
    std::condition_variable done;
    std::atomic<int> fired{0};
    std::vector<std::thread> schedulers;

    for (int t = 0; t < thread_count; ++t) {
        schedulers.emplace_back([&timer, &done, &fired, t, timers_per_thread, total] {
            for (int i = 0; i < timers_per_thread; ++i) {
                const auto delay = std::chrono::milliseconds((t + i) % 5);
                timer.scheduleAfter(delay, [&done, &fired, total] {
                    const int count = fired.fetch_add(1) + 1;
                    if (count == total) {
                        done.notify_one();
                    }
                });
            }
        });
    }

    for (auto& scheduler : schedulers) {
        scheduler.join();
    }

    std::unique_lock<std::mutex> lock(mutex);
    assert(done.wait_for(lock, 2s, [&] {
        return fired.load() == total;
    }));

    auto stats = timer.stats();
    assert(stats.scheduled == total);
    assert(stats.fired == total);
    assert(stats.active == 0);

    timer.shutdown();
}

void cancelAllClearsActiveTimers() {
    using namespace std::chrono_literals;

    auto timer = makeTimer();
    std::atomic<int> fired{0};

    for (int i = 0; i < 10; ++i) {
        timer.scheduleAfter(200ms, [&] {
            ++fired;
        });
    }

    assert(timer.cancelAll() == 10);
    std::this_thread::sleep_for(250ms);

    assert(fired == 0);
    assert(timer.stats().active == 0);
    assert(timer.stats().cancelled == 10);

    timer.shutdown();
}

} // namespace

int main() {
    firesOneShotTimer();
    cancelPreventsOneShotTimer();
    repeatingTimerCanBeCancelled();
    handlesManyTimersFromManyThreads();
    cancelAllClearsActiveTimers();
    return 0;
}
