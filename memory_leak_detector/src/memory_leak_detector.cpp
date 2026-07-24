#include "memory_leak_detector.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <unordered_map>

#if defined(_WIN32)
#include <malloc.h>
#else
#include <errno.h>
#endif

namespace {

#if defined(__STDCPP_DEFAULT_NEW_ALIGNMENT__)
constexpr std::size_t kDefaultNewAlignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
#else
constexpr std::size_t kDefaultNewAlignment = alignof(std::max_align_t);
#endif

thread_local int g_detector_depth = 0;

class DetectorInternalScope {
public:
    DetectorInternalScope() noexcept {
        ++g_detector_depth;
    }

    ~DetectorInternalScope() noexcept {
        --g_detector_depth;
    }

    DetectorInternalScope(const DetectorInternalScope&) = delete;
    DetectorInternalScope& operator=(const DetectorInternalScope&) = delete;
};

bool insideDetector() noexcept {
    return g_detector_depth > 0;
}

void* allocateRaw(std::size_t size, std::size_t alignment) {
    if (size == 0) {
        size = 1;
    }

    if (alignment > kDefaultNewAlignment) {
#if defined(_WIN32)
        void* ptr = _aligned_malloc(size, alignment);
        if (!ptr) {
            throw std::bad_alloc();
        }
        return ptr;
#else
        void* ptr = nullptr;
        if (posix_memalign(&ptr, alignment, size) != 0) {
            throw std::bad_alloc();
        }
        return ptr;
#endif
    }

    void* ptr = std::malloc(size);
    if (!ptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void freeRaw(void* ptr, std::size_t alignment) noexcept {
    if (!ptr) {
        return;
    }

    if (alignment > kDefaultNewAlignment) {
#if defined(_WIN32)
        _aligned_free(ptr);
#else
        std::free(ptr);
#endif
        return;
    }

    std::free(ptr);
}

void* allocateMemory(std::size_t size,
                     const char* file,
                     int line,
                     bool array_allocation,
                     std::size_t alignment) {
    void* ptr = allocateRaw(size, alignment);

    if (!insideDetector()) {
        foundation::MemoryLeakDetector::instance().recordAllocation(
            ptr,
            size,
            file,
            line,
            array_allocation,
            alignment);
    }

    return ptr;
}

void deallocateMemory(void* ptr,
                      bool array_delete,
                      std::size_t fallback_alignment) noexcept {
    if (!ptr) {
        return;
    }

    std::size_t alignment = fallback_alignment;
    if (!insideDetector()) {
        alignment = foundation::MemoryLeakDetector::instance().recordDeallocation(
            ptr,
            array_delete,
            fallback_alignment);
    }

    freeRaw(ptr, alignment);
}

std::string allocationKind(const foundation::MemoryAllocationRecord& record) {
    if (record.array_allocation && record.aligned_allocation) {
        return "new[] aligned";
    }
    if (record.array_allocation) {
        return "new[]";
    }
    if (record.aligned_allocation) {
        return "new aligned";
    }
    return "new";
}

} // namespace

namespace foundation {

struct MemoryLeakDetector::State {
    mutable std::mutex mutex;
    std::unordered_map<void*, MemoryAllocationRecord> live_allocations;
    MemoryLeakStats stats;
    std::uint64_t next_sequence = 1;
    std::atomic<bool> enabled{false};
};

MemoryLeakDetector& MemoryLeakDetector::instance() {
    static MemoryLeakDetector detector;
    return detector;
}

MemoryLeakDetector::MemoryLeakDetector() {
    static State* process_lifetime_state = [] {
        void* memory = std::malloc(sizeof(State));
        if (!memory) {
            std::abort();
        }

        return new (memory) State();
    }();

    state_ = process_lifetime_state;
}

void MemoryLeakDetector::setEnabled(bool enabled) noexcept {
    state_->enabled.store(enabled, std::memory_order_release);
}

bool MemoryLeakDetector::enabled() const noexcept {
    return state_->enabled.load(std::memory_order_acquire);
}

MemoryLeakStats MemoryLeakDetector::stats() const {
    DetectorInternalScope guard;
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->stats;
}

MemoryLeakSnapshot MemoryLeakDetector::snapshot() const {
    DetectorInternalScope guard;
    std::lock_guard<std::mutex> lock(state_->mutex);
    MemoryLeakSnapshot snapshot;
    snapshot.stats = state_->stats;
    snapshot.next_sequence = state_->next_sequence;
    return snapshot;
}

bool MemoryLeakDetector::hasLeaks() const {
    return stats().live_allocations > 0;
}

bool MemoryLeakDetector::hasLeaksSince(const MemoryLeakSnapshot& snapshot) const {
    DetectorInternalScope guard;
    std::lock_guard<std::mutex> lock(state_->mutex);
    return std::any_of(
        state_->live_allocations.begin(),
        state_->live_allocations.end(),
        [&snapshot](const auto& entry) {
            return entry.second.sequence >= snapshot.next_sequence;
        });
}

std::vector<MemoryAllocationRecord> MemoryLeakDetector::liveAllocations() const {
    DetectorInternalScope guard;
    std::vector<MemoryAllocationRecord> records;

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        records.reserve(state_->live_allocations.size());
        for (const auto& entry : state_->live_allocations) {
            records.push_back(entry.second);
        }
    }

    std::sort(records.begin(), records.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.sequence < rhs.sequence;
    });
    return records;
}

std::vector<MemoryAllocationRecord>
MemoryLeakDetector::leaksSince(const MemoryLeakSnapshot& snapshot) const {
    DetectorInternalScope guard;
    std::vector<MemoryAllocationRecord> records;

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        records.reserve(state_->live_allocations.size());
        for (const auto& entry : state_->live_allocations) {
            if (entry.second.sequence >= snapshot.next_sequence) {
                records.push_back(entry.second);
            }
        }
    }

    std::sort(records.begin(), records.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.sequence < rhs.sequence;
    });
    return records;
}

std::string MemoryLeakDetector::report(std::size_t max_records) const {
    DetectorInternalScope guard;
    const auto records = liveAllocations();
    const auto snapshot_stats = stats();

    std::ostringstream out;
    if (records.empty()) {
        out << "No memory leaks detected.\n";
        return out.str();
    }

    out << "Memory leak report\n"
        << "  live allocations: " << snapshot_stats.live_allocations << '\n'
        << "  live bytes:       " << snapshot_stats.live_bytes << '\n'
        << "  peak live bytes:  " << snapshot_stats.peak_live_bytes << '\n'
        << "  total allocated:  " << snapshot_stats.total_allocations << " blocks, "
        << snapshot_stats.total_bytes << " bytes\n"
        << "  total frees:      " << snapshot_stats.total_frees << '\n'
        << "  mismatched delete:" << snapshot_stats.mismatched_deletes << "\n\n";

    const auto shown = std::min(max_records, records.size());
    for (std::size_t i = 0; i < shown; ++i) {
        const auto& record = records[i];
        out << "  #" << record.sequence
            << " " << record.address
            << " " << record.size << " bytes"
            << " via " << allocationKind(record)
            << " at " << (record.file ? record.file : "unknown")
            << ':' << record.line
            << " thread " << record.thread_id;
        if (record.aligned_allocation) {
            out << " alignment " << record.alignment;
        }
        out << '\n';
    }

    if (shown < records.size()) {
        out << "  ... " << (records.size() - shown) << " more allocation(s)\n";
    }

    return out.str();
}

std::string MemoryLeakDetector::reportSince(const MemoryLeakSnapshot& snapshot,
                                            std::size_t max_records) const {
    DetectorInternalScope guard;
    const auto records = leaksSince(snapshot);
    std::size_t live_bytes = 0;
    for (const auto& record : records) {
        live_bytes += record.size;
    }

    std::ostringstream out;
    if (records.empty()) {
        out << "No memory leaks detected since snapshot.\n";
        return out.str();
    }

    out << "Memory leak report since snapshot\n"
        << "  leaked allocations: " << records.size() << '\n'
        << "  leaked bytes:       " << live_bytes << "\n\n";

    const auto shown = std::min(max_records, records.size());
    for (std::size_t i = 0; i < shown; ++i) {
        const auto& record = records[i];
        out << "  #" << record.sequence
            << " " << record.address
            << " " << record.size << " bytes"
            << " via " << allocationKind(record)
            << " at " << (record.file ? record.file : "unknown")
            << ':' << record.line
            << " thread " << record.thread_id;
        if (record.aligned_allocation) {
            out << " alignment " << record.alignment;
        }
        out << '\n';
    }

    if (shown < records.size()) {
        out << "  ... " << (records.size() - shown) << " more allocation(s)\n";
    }

    return out.str();
}

void MemoryLeakDetector::reset() noexcept {
    DetectorInternalScope guard;
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->live_allocations.clear();
    state_->stats = MemoryLeakStats{};
    state_->next_sequence = 1;
}

void MemoryLeakDetector::recordAllocation(void* address,
                                          std::size_t size,
                                          const char* file,
                                          int line,
                                          bool array_allocation,
                                          std::size_t alignment) noexcept {
    if (!address || !enabled()) {
        return;
    }

    DetectorInternalScope guard;

    try {
        std::lock_guard<std::mutex> lock(state_->mutex);

        MemoryAllocationRecord record;
        record.address = address;
        record.size = size;
        record.file = file ? file : "unknown";
        record.line = line;
        record.array_allocation = array_allocation;
        record.aligned_allocation = alignment > kDefaultNewAlignment;
        record.alignment = alignment;
        record.sequence = state_->next_sequence++;
        record.thread_id = std::this_thread::get_id();

        auto it = state_->live_allocations.find(address);
        if (it != state_->live_allocations.end()) {
            if (state_->stats.live_bytes >= it->second.size) {
                state_->stats.live_bytes -= it->second.size;
            } else {
                state_->stats.live_bytes = 0;
            }
            it->second = record;
        } else {
            state_->live_allocations.emplace(address, record);
            ++state_->stats.live_allocations;
        }

        state_->stats.live_bytes += size;
        ++state_->stats.total_allocations;
        state_->stats.total_bytes += size;
        state_->stats.peak_live_bytes =
            std::max(state_->stats.peak_live_bytes, state_->stats.live_bytes);
    } catch (...) {
    }
}

std::size_t MemoryLeakDetector::recordDeallocation(void* address,
                                                   bool array_delete,
                                                   std::size_t fallback_alignment) noexcept {
    if (!address) {
        return fallback_alignment;
    }

    DetectorInternalScope guard;

    try {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto it = state_->live_allocations.find(address);
        if (it == state_->live_allocations.end()) {
            return fallback_alignment;
        }

        const auto record = it->second;
        if (record.array_allocation != array_delete) {
            ++state_->stats.mismatched_deletes;
        }

        if (state_->stats.live_bytes >= record.size) {
            state_->stats.live_bytes -= record.size;
        } else {
            state_->stats.live_bytes = 0;
        }

        state_->live_allocations.erase(it);
        state_->stats.live_allocations = state_->live_allocations.size();
        ++state_->stats.total_frees;

        return record.alignment != 0 ? record.alignment : fallback_alignment;
    } catch (...) {
        return fallback_alignment;
    }
}

ScopedMemoryLeakCheck::ScopedMemoryLeakCheck(bool enable_detector) {
    auto& detector = MemoryLeakDetector::instance();
    previous_enabled_ = detector.enabled();
    if (enable_detector) {
        detector.setEnabled(true);
    }
    baseline_ = detector.snapshot();
}

ScopedMemoryLeakCheck::~ScopedMemoryLeakCheck() {
    MemoryLeakDetector::instance().setEnabled(previous_enabled_);
}

const MemoryLeakSnapshot& ScopedMemoryLeakCheck::baseline() const noexcept {
    return baseline_;
}

bool ScopedMemoryLeakCheck::hasLeaks() const {
    return MemoryLeakDetector::instance().hasLeaksSince(baseline_);
}

std::string ScopedMemoryLeakCheck::report(std::size_t max_records) const {
    return MemoryLeakDetector::instance().reportSince(baseline_, max_records);
}

} // namespace foundation

void* operator new(std::size_t size) {
    return allocateMemory(size, nullptr, 0, false, 0);
}

void* operator new[](std::size_t size) {
    return allocateMemory(size, nullptr, 0, true, 0);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocateMemory(size, nullptr, 0, false, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocateMemory(size, nullptr, 0, true, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return operator new(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return operator new[](size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new(std::size_t size,
                   std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
    try {
        return operator new(size, alignment);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size,
                     std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    try {
        return operator new[](size, alignment);
    } catch (...) {
        return nullptr;
    }
}

void* operator new(std::size_t size, const char* file, int line) {
    return allocateMemory(size, file, line, false, 0);
}

void* operator new[](std::size_t size, const char* file, int line) {
    return allocateMemory(size, file, line, true, 0);
}

void* operator new(std::size_t size,
                   std::align_val_t alignment,
                   const char* file,
                   int line) {
    return allocateMemory(size, file, line, false, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size,
                     std::align_val_t alignment,
                     const char* file,
                     int line) {
    return allocateMemory(size, file, line, true, static_cast<std::size_t>(alignment));
}

void operator delete(void* ptr) noexcept {
    deallocateMemory(ptr, false, 0);
}

void operator delete[](void* ptr) noexcept {
    deallocateMemory(ptr, true, 0);
}

void operator delete(void* ptr, std::size_t) noexcept {
    deallocateMemory(ptr, false, 0);
}

void operator delete[](void* ptr, std::size_t) noexcept {
    deallocateMemory(ptr, true, 0);
}

void operator delete(void* ptr, std::align_val_t alignment) noexcept {
    deallocateMemory(ptr, false, static_cast<std::size_t>(alignment));
}

void operator delete[](void* ptr, std::align_val_t alignment) noexcept {
    deallocateMemory(ptr, true, static_cast<std::size_t>(alignment));
}

void operator delete(void* ptr, std::size_t, std::align_val_t alignment) noexcept {
    deallocateMemory(ptr, false, static_cast<std::size_t>(alignment));
}

void operator delete[](void* ptr, std::size_t, std::align_val_t alignment) noexcept {
    deallocateMemory(ptr, true, static_cast<std::size_t>(alignment));
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    deallocateMemory(ptr, false, 0);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    deallocateMemory(ptr, true, 0);
}

void operator delete(void* ptr,
                     std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    deallocateMemory(ptr, false, static_cast<std::size_t>(alignment));
}

void operator delete[](void* ptr,
                       std::align_val_t alignment,
                       const std::nothrow_t&) noexcept {
    deallocateMemory(ptr, true, static_cast<std::size_t>(alignment));
}

void operator delete(void* ptr, const char*, int) noexcept {
    deallocateMemory(ptr, false, 0);
}

void operator delete[](void* ptr, const char*, int) noexcept {
    deallocateMemory(ptr, true, 0);
}

void operator delete(void* ptr,
                     std::align_val_t alignment,
                     const char*,
                     int) noexcept {
    deallocateMemory(ptr, false, static_cast<std::size_t>(alignment));
}

void operator delete[](void* ptr,
                       std::align_val_t alignment,
                       const char*,
                       int) noexcept {
    deallocateMemory(ptr, true, static_cast<std::size_t>(alignment));
}
