#include "memory_leak_detector.hpp"

#include <cassert>
#include <cstddef>
#include <string>

namespace {

struct alignas(64) OverAlignedValue {
    int value = 0;
};

foundation::MemoryLeakDetector& detector() {
    return foundation::MemoryLeakDetector::instance();
}

void resetDisabled() {
    detector().setEnabled(false);
    detector().reset();
}

void detectsLeakedScalarWithLocation() {
    resetDisabled();
    detector().setEnabled(true);
    const auto baseline = detector().snapshot();

    int* leaked = LEAK_DETECTOR_NEW int(42);

    assert(detector().hasLeaksSince(baseline));
    const auto leaks = detector().leaksSince(baseline);
    assert(leaks.size() == 1);
    assert(leaks.front().address == leaked);
    assert(leaks.front().size >= sizeof(int));
    assert(leaks.front().line > 0);

    const std::string report = detector().reportSince(baseline);
    assert(report.find("memory_leak_detector_test.cpp") != std::string::npos);

    delete leaked;
    assert(!detector().hasLeaksSince(baseline));
    resetDisabled();
}

void tracksArrayAllocations() {
    resetDisabled();
    detector().setEnabled(true);
    const auto baseline = detector().snapshot();

    int* values = LEAK_DETECTOR_NEW int[4]{1, 2, 3, 4};

    const auto leaks = detector().leaksSince(baseline);
    assert(leaks.size() == 1);
    assert(leaks.front().array_allocation);
    assert(leaks.front().size >= sizeof(int) * 4);

    delete[] values;
    assert(!detector().hasLeaksSince(baseline));
    resetDisabled();
}

void tracksAlignedAllocations() {
    resetDisabled();
    detector().setEnabled(true);
    const auto baseline = detector().snapshot();

    OverAlignedValue* value = LEAK_DETECTOR_NEW OverAlignedValue();
    value->value = 99;

    const auto leaks = detector().leaksSince(baseline);
    assert(leaks.size() == 1);
    assert(leaks.front().aligned_allocation);
    assert(leaks.front().alignment >= alignof(OverAlignedValue));

    delete value;
    assert(!detector().hasLeaksSince(baseline));
    resetDisabled();
}

void detectsMismatchedDeleteForms() {
    resetDisabled();
    detector().setEnabled(true);

    void* raw = ::operator new[](64, "manual-allocation", 123);
    assert(detector().stats().live_allocations == 1);

    ::operator delete(raw);

    const auto stats = detector().stats();
    assert(stats.live_allocations == 0);
    assert(stats.mismatched_deletes == 1);
    resetDisabled();
}

void scopedCheckUsesABaselineAndRestoresState() {
    resetDisabled();
    assert(!detector().enabled());

    {
        foundation::ScopedMemoryLeakCheck check;
        assert(detector().enabled());

        int* leaked = LEAK_DETECTOR_NEW int(5);
        assert(check.hasLeaks());

        delete leaked;
        assert(!check.hasLeaks());
    }

    assert(!detector().enabled());
    resetDisabled();
}

void ignoresAllocationsWhileDisabled() {
    resetDisabled();

    int* value = new int(1);
    assert(!detector().hasLeaks());
    delete value;

    resetDisabled();
}

} // namespace

int main() {
    detectsLeakedScalarWithLocation();
    tracksArrayAllocations();
    tracksAlignedAllocations();
    detectsMismatchedDeleteForms();
    scopedCheckUsesABaselineAndRestoresState();
    ignoresAllocationsWhileDisabled();
    return 0;
}
