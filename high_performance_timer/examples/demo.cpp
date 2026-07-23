#include "high_performance_timer.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>

namespace {

std::mutex cout_mutex;

void log(const std::string& message) {
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << message << '\n';
}

void printStats(const foundation::HighPerformanceTimer::Stats& stats) {
    std::cout << "\nTimer stats\n"
              << "  scheduled:         " << stats.scheduled << '\n'
              << "  cancelled:         " << stats.cancelled << '\n'
              << "  fired:             " << stats.fired << '\n'
              << "  active:            " << stats.active << '\n'
              << "  pending callbacks: " << stats.pending_callbacks << '\n'
              << "  callback threads:  " << stats.callback_threads << '\n';
}

} // namespace

int main() {
    using namespace std::chrono_literals;

    foundation::HighPerformanceTimer::Config config;
    config.callback_threads = 2;

    foundation::HighPerformanceTimer timer(config);

    std::mutex done_mutex;
    std::condition_variable done;
    std::atomic<int> finished{0};
    std::atomic<int> heartbeat_count{0};

    timer.scheduleAfter(100ms, [&] {
        log("[once] refresh cache after 100ms");
        if (finished.fetch_add(1) + 1 == 3) {
            done.notify_one();
        }
    });

    timer.scheduleAfter(250ms, [&] {
        log("[once] close idle connection after 250ms");
        if (finished.fetch_add(1) + 1 == 3) {
            done.notify_one();
        }
    });

    foundation::HighPerformanceTimer::TimerId heartbeat =
        foundation::HighPerformanceTimer::invalid_timer_id;

    heartbeat = timer.scheduleEvery(80ms, [&] {
        const int count = heartbeat_count.fetch_add(1) + 1;
        log("[repeat] heartbeat #" + std::to_string(count));

        if (count == 5) {
            timer.cancel(heartbeat);
            log("[repeat] heartbeat cancelled");

            if (finished.fetch_add(1) + 1 == 3) {
                done.notify_one();
            }
        }
    });

    {
        std::unique_lock<std::mutex> lock(done_mutex);
        done.wait_for(lock, 2s, [&] {
            return finished.load() == 3;
        });
    }

    printStats(timer.stats());
    timer.shutdown();

    return 0;
}
