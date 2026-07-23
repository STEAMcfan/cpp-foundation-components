#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace foundation {

class HighPerformanceTimer {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Callback = std::function<void()>;
    using TimerId = std::uint64_t;

    static constexpr TimerId invalid_timer_id = 0;

    struct Config {
        std::size_t callback_threads = 1;
    };

    struct Stats {
        std::size_t scheduled = 0;
        std::size_t cancelled = 0;
        std::size_t fired = 0;
        std::size_t active = 0;
        std::size_t pending_callbacks = 0;
        std::size_t callback_threads = 0;
    };

    explicit HighPerformanceTimer(Config config = {});
    ~HighPerformanceTimer();

    HighPerformanceTimer(const HighPerformanceTimer&) = delete;
    HighPerformanceTimer& operator=(const HighPerformanceTimer&) = delete;
    HighPerformanceTimer(HighPerformanceTimer&&) = delete;
    HighPerformanceTimer& operator=(HighPerformanceTimer&&) = delete;

    TimerId scheduleAt(TimePoint deadline, Callback callback);
    TimerId scheduleAfter(std::chrono::milliseconds delay, Callback callback);
    TimerId scheduleEvery(std::chrono::milliseconds interval, Callback callback);

    bool cancel(TimerId id);
    std::size_t cancelAll();
    void shutdown();
    Stats stats() const;

private:
    struct State;

    std::shared_ptr<State> state_;
};

} // namespace foundation
