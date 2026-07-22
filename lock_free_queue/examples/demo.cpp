#include "lock_free_queue.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
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

} // namespace

int main() {
    using namespace std::chrono_literals;

    foundation::LockFreeQueue<std::string> queue(8);
    std::atomic<int> consumed{0};

    std::vector<std::thread> consumers;
    for (int i = 0; i < 2; ++i) {
        consumers.emplace_back([&queue, &consumed, i] {
            while (consumed.load(std::memory_order_acquire) < 12) {
                auto item = queue.tryPop();
                if (!item) {
                    std::this_thread::sleep_for(1ms);
                    continue;
                }

                log("[consumer " + std::to_string(i) + "] " + *item);
                consumed.fetch_add(1, std::memory_order_release);
            }
        });
    }

    std::vector<std::thread> producers;
    for (int p = 0; p < 3; ++p) {
        producers.emplace_back([&queue, p] {
            for (int i = 0; i < 4; ++i) {
                auto message = "producer " + std::to_string(p) +
                               " item " + std::to_string(i);
                while (!queue.tryPush(std::move(message))) {
                    std::this_thread::sleep_for(1ms);
                }
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }

    for (auto& consumer : consumers) {
        consumer.join();
    }

    queue.close();

    const auto stats = queue.stats();
    {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "\nQueue stats\n"
                  << "  pushes:        " << stats.pushes << '\n'
                  << "  pops:          " << stats.pops << '\n'
                  << "  failed pushes: " << stats.failed_pushes << '\n'
                  << "  failed pops:   " << stats.failed_pops << '\n'
                  << "  closed pushes: " << stats.closed_pushes << '\n';
    }

    return 0;
}
