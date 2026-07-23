#include "ring_buffer.hpp"

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

    foundation::RingBuffer<std::string> buffer(4);

    std::vector<std::thread> consumers;
    for (int i = 0; i < 2; ++i) {
        consumers.emplace_back([&buffer, i] {
            while (true) {
                auto item = buffer.waitPop();
                if (!item) {
                    break;
                }

                log("[consumer " + std::to_string(i) + "] " + *item);
                std::this_thread::sleep_for(10ms);
            }
        });
    }

    std::vector<std::thread> producers;
    for (int p = 0; p < 3; ++p) {
        producers.emplace_back([&buffer, p] {
            for (int i = 0; i < 4; ++i) {
                auto message = "producer " + std::to_string(p) +
                               " item " + std::to_string(i);
                buffer.waitPush(std::move(message));
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }

    buffer.close();

    for (auto& consumer : consumers) {
        consumer.join();
    }

    const auto stats = buffer.stats();
    std::cout << "\nRingBuffer stats\n"
              << "  pushes:        " << stats.pushes << '\n'
              << "  pops:          " << stats.pops << '\n'
              << "  failed pushes: " << stats.failed_pushes << '\n'
              << "  failed pops:   " << stats.failed_pops << '\n'
              << "  overwrites:    " << stats.overwrites << '\n'
              << "  closed pushes: " << stats.closed_pushes << '\n';

    return 0;
}
