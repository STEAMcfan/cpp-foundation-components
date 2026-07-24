#include "memory_leak_detector.hpp"

#include <iostream>

int main() {
    auto& detector = foundation::MemoryLeakDetector::instance();
    detector.reset();

    foundation::ScopedMemoryLeakCheck leak_check;

    int* released = LEAK_DETECTOR_NEW int(7);
    delete released;

    int* leaked_array = LEAK_DETECTOR_NEW int[3]{1, 2, 3};

    if (leak_check.hasLeaks()) {
        std::cout << leak_check.report();
    }

    delete[] leaked_array;

    std::cout << "\nAfter cleanup\n"
              << leak_check.report();

    detector.setEnabled(false);
    detector.reset();
    return 0;
}
