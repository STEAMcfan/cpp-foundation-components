# Memory Leak Detector

这个目录包含一个 C++17 内存泄漏检测组件，适合学习、运行和面试讲解。

它的核心思路是：在调试构建里接管 `new/delete`，分配时把地址、大小、文件、行号、线程号记录到表里，释放时从表里删除。程序结束或某个测试作用域结束时，表里还活着的记录就是疑似泄漏。

## Layout

```text
memory_leak_detector/
+-- include/memory_leak_detector.hpp
+-- src/memory_leak_detector.cpp
+-- examples/demo.cpp
+-- tests/memory_leak_detector_test.cpp
+-- CMakeLists.txt
+-- README.md
```

## Build And Run

```bash
cd memory_leak_detector
cmake -S . -B build
cmake --build build
./build/memory_leak_detector_demo
ctest --test-dir build --output-on-failure
```

On multi-config generators, the executable may be under `build/Debug/`.

## Basic Usage

```cpp
#include "memory_leak_detector.hpp"

void run_case() {
    foundation::ScopedMemoryLeakCheck check;

    int* ok = LEAK_DETECTOR_NEW int(1);
    delete ok;

    int* leaked = LEAK_DETECTOR_NEW int[8];

    if (check.hasLeaks()) {
        std::cout << check.report();
    }

    delete[] leaked;
}
```

也可以手动控制检测器：

```cpp
auto& detector = foundation::MemoryLeakDetector::instance();
detector.setEnabled(true);

auto baseline = detector.snapshot();
auto* p = LEAK_DETECTOR_NEW int(42);

std::cout << detector.reportSince(baseline);
delete p;
```

## Design Notes

- 全局 `operator new/delete` 负责接管分配和释放；`LEAK_DETECTOR_NEW` 会把 `__FILE__` 和 `__LINE__` 塞进分配记录。
- 检测器内部用 `unordered_map<void*, MemoryAllocationRecord>` 保存仍然存活的分配。
- `snapshot()` 记录一个序列号基线，`reportSince()` 只报告基线之后发生且仍未释放的分配，适合单元测试和局部排查。
- `ScopedMemoryLeakCheck` 构造时打开检测并记录基线，析构时恢复之前的开关状态。
- 实现里有线程局部递归保护，避免检测器自己的 `unordered_map`、`vector`、`string` 分配再次进入检测器造成死循环或误报。
- 组件支持普通 `new`、`new[]`、C++17 over-aligned `new`，并统计 `new/delete[]` 这类释放形式不匹配的问题。
- 默认不开启检测，实际工程里通常只在 debug/test 构建启用，避免影响 release 性能和全局分配行为。

## Key Points

内存泄漏检测组件的关键不是“最后扫一遍 map”，而是这几个边界：

1. 分配和释放必须成对进入同一套记录逻辑，否则会漏报或误报。
2. 记录表本身也会申请内存，所以必须有递归保护。
3. 删除时不一定知道原始分配大小，因此记录表要以地址为 key 保存元数据。
4. 文件行号不是 `delete` 时拿到的，而是通过 `new(__FILE__, __LINE__)` 在分配时记录的。
5. 作用域快照很重要，否则全局静态对象、测试框架、运行库分配很容易污染结果。
6. 生产环境更常用 ASan、LSan、Valgrind、CRT Debug Heap 等成熟工具；这种组件主要用于理解原理、做轻量级 debug 辅助和面试手撕。

## Interview Handwritten Version

面试手撕时不建议一上来写完整的全局 `new/delete` 全家桶。先写核心版本：一个记录表、一个宏、一个分配函数、一个释放函数、一个报告函数。

```cpp
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <unordered_map>

struct Rec {
    size_t size;
    const char* file;
    int line;
};

class LeakDetector {
    std::mutex mtx;
    std::unordered_map<void*, Rec> live;

public:
    static LeakDetector& inst() {
        static LeakDetector d;
        return d;
    }

    void* alloc(size_t n, const char* file, int line) {
        void* p = std::malloc(n);
        if (!p) throw std::bad_alloc();

        std::lock_guard<std::mutex> lock(mtx);
        live[p] = Rec{n, file, line};
        return p;
    }

    void free(void* p) {
        if (!p) return;

        {
            std::lock_guard<std::mutex> lock(mtx);
            live.erase(p);
        }

        std::free(p);
    }

    void report() {
        std::lock_guard<std::mutex> lock(mtx);
        for (auto& [p, r] : live) {
            std::cout << "leak " << p << " "
                      << r.size << " bytes at "
                      << r.file << ":" << r.line << "\n";
        }
    }
};

#define DEBUG_NEW new(__FILE__, __LINE__)

void* operator new(size_t n, const char* file, int line) {
    return LeakDetector::inst().alloc(n, file, line);
}

void operator delete(void* p) noexcept {
    LeakDetector::inst().free(p);
}
```

这个版本能讲清楚主线，但真实工程还要补：

- `operator new[] / delete[]`
- 构造函数抛异常时对应的 placement delete
- C++17 aligned new/delete
- sized delete
- 多线程安全
- 检测器内部递归保护
- 作用域快照和报告过滤

## Possible Interview Questions

- 内存泄漏检测的基本原理是什么？
- 为什么要重载 `new/delete`，只封装 `malloc/free` 行不行？
- 文件名和行号怎么拿到？
- 为什么 `delete` 的时候通常拿不到文件名和行号？
- 为什么检测器内部需要递归保护？
- `new[]` 和 `delete` 混用会有什么问题？能不能检测？
- 构造函数抛异常时，已经分配的内存怎么释放？
- 多线程环境下记录表怎么保证安全？
- 为什么实际项目里更推荐 ASan/LSan/Valgrind？
- 这种自研检测器适合放在 release 环境吗？

## Common Uses In Development

- 单元测试里包一层 `ScopedMemoryLeakCheck`，检查某个 case 是否留下未释放对象。
- Debug 构建中临时启用，定位小型模块的裸指针泄漏。
- 教学或面试中解释 `new/delete`、RAII、宏替换、全局重载和内存工具原理。
- 老 C/C++ 项目迁移 RAII 前，用它快速找出明显的 `new` 后忘记 `delete`。
- 真正线上服务通常使用智能指针、容器和 RAII 降低泄漏概率，再结合 ASan/LSan、Valgrind、平台 CRT 工具做系统级检测。
