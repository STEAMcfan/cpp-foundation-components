#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <thread>
#include <vector>

void* operator new(std::size_t size, const char* file, int line);
void* operator new[](std::size_t size, const char* file, int line);
void* operator new(std::size_t size,
                   std::align_val_t alignment,
                   const char* file,
                   int line);
void* operator new[](std::size_t size,
                     std::align_val_t alignment,
                     const char* file,
                     int line);
void operator delete(void* ptr, const char* file, int line) noexcept;
void operator delete[](void* ptr, const char* file, int line) noexcept;
void operator delete(void* ptr,
                     std::align_val_t alignment,
                     const char* file,
                     int line) noexcept;
void operator delete[](void* ptr,
                       std::align_val_t alignment,
                       const char* file,
                       int line) noexcept;

#ifndef FOUNDATION_LEAK_DETECTOR_NEW
#define FOUNDATION_LEAK_DETECTOR_NEW new(__FILE__, __LINE__)
#endif

#ifndef LEAK_DETECTOR_NEW
#define LEAK_DETECTOR_NEW FOUNDATION_LEAK_DETECTOR_NEW
#endif

namespace foundation {

struct MemoryAllocationRecord {
    void* address = nullptr;
    std::size_t size = 0;
    const char* file = "unknown";
    int line = 0;
    bool array_allocation = false;
    bool aligned_allocation = false;
    std::size_t alignment = 0;
    std::uint64_t sequence = 0;
    std::thread::id thread_id;
};

struct MemoryLeakStats {
    std::size_t live_allocations = 0;
    std::size_t live_bytes = 0;
    std::size_t total_allocations = 0;
    std::size_t total_bytes = 0;
    std::size_t total_frees = 0;
    std::size_t peak_live_bytes = 0;
    std::size_t mismatched_deletes = 0;
};

struct MemoryLeakSnapshot {
    MemoryLeakStats stats;
    std::uint64_t next_sequence = 1;
};

class MemoryLeakDetector {
public:
    static MemoryLeakDetector& instance();

    MemoryLeakDetector(const MemoryLeakDetector&) = delete;
    MemoryLeakDetector& operator=(const MemoryLeakDetector&) = delete;
    MemoryLeakDetector(MemoryLeakDetector&&) = delete;
    MemoryLeakDetector& operator=(MemoryLeakDetector&&) = delete;

    void setEnabled(bool enabled) noexcept;
    bool enabled() const noexcept;

    MemoryLeakStats stats() const;
    MemoryLeakSnapshot snapshot() const;

    bool hasLeaks() const;
    bool hasLeaksSince(const MemoryLeakSnapshot& snapshot) const;

    std::vector<MemoryAllocationRecord> liveAllocations() const;
    std::vector<MemoryAllocationRecord> leaksSince(const MemoryLeakSnapshot& snapshot) const;

    std::string report(std::size_t max_records = 32) const;
    std::string reportSince(const MemoryLeakSnapshot& snapshot,
                            std::size_t max_records = 32) const;

    // Intended for tests and short demos. Call it only after tracked allocations
    // have been released, otherwise later deletes become untracked.
    void reset() noexcept;

    // Internal hooks used by the global new/delete overloads.
    void recordAllocation(void* address,
                          std::size_t size,
                          const char* file,
                          int line,
                          bool array_allocation,
                          std::size_t alignment) noexcept;
    std::size_t recordDeallocation(void* address,
                                   bool array_delete,
                                   std::size_t fallback_alignment) noexcept;

private:
    MemoryLeakDetector();

    struct State;
    State* state_;
};

class ScopedMemoryLeakCheck {
public:
    explicit ScopedMemoryLeakCheck(bool enable_detector = true);
    ~ScopedMemoryLeakCheck();

    ScopedMemoryLeakCheck(const ScopedMemoryLeakCheck&) = delete;
    ScopedMemoryLeakCheck& operator=(const ScopedMemoryLeakCheck&) = delete;
    ScopedMemoryLeakCheck(ScopedMemoryLeakCheck&&) = delete;
    ScopedMemoryLeakCheck& operator=(ScopedMemoryLeakCheck&&) = delete;

    const MemoryLeakSnapshot& baseline() const noexcept;
    bool hasLeaks() const;
    std::string report(std::size_t max_records = 32) const;

private:
    bool previous_enabled_ = false;
    MemoryLeakSnapshot baseline_;
};

} // namespace foundation
