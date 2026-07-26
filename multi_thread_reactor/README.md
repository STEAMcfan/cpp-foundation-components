# Multi-Thread Reactor

这个目录包含一个 C++17 多线程 Reactor 学习组件。它是在单线程 `epoll reactor` 之上继续往工程形态推进的一版：主 Reactor 负责监听 socket 和 `accept`，多个 worker Reactor 各自运行在独立线程里，负责普通连接的读写事件。

这个组件适合用来学习：

- `epoll + nonblocking socket + eventfd` 的事件循环写法
- one loop per thread 的多线程 Reactor 模型
- 新连接如何用 round-robin 分发给 worker loop
- 为什么回调必须在 Reactor 内部锁之外执行
- 为什么 ET 模式要读写到 `EAGAIN`
- 为什么真正的网络服务要维护每个连接自己的输入/输出缓冲区

## Layout

```text
multi_thread_reactor/
+-- include/multi_thread_reactor.hpp
+-- src/multi_thread_reactor.cpp
+-- examples/demo.cpp
+-- examples/echo_server.cpp
+-- tests/multi_thread_reactor_test.cpp
+-- handwritten/multi_thread_reactor_handwritten.cpp
+-- CMakeLists.txt
+-- README.md
```

## Build And Run

```bash
cd cpp-foundation-components/multi_thread_reactor
cmake -S . -B build
cmake --build build
./build/multi_thread_reactor_demo
ctest --test-dir build --output-on-failure
```

完整 echo server 示例：

```bash
./build/multi_thread_reactor_echo_server 9090
nc 127.0.0.1 9090
```

这个组件依赖 Linux `epoll`。在非 Linux 平台上仍然可以编译测试，测试会走“不支持平台”的分支；真正运行 demo 和 echo server 需要 Linux。

## Basic Usage

```cpp
foundation::MultiThreadReactor::Config config;
config.worker_threads = 4;

foundation::MultiThreadReactor reactor(config);
reactor.start();

reactor.add(fd,
            foundation::ReactorEvents::Read |
            foundation::ReactorEvents::EdgeTriggered,
            [&](foundation::EventLoop& loop, const foundation::ReactorEvent& event) {
                if (event.readable()) {
                    // read until EAGAIN
                }
                if (event.writable()) {
                    // write pending output until EAGAIN
                }
                if (event.error() || event.hangUp() || event.peerClosed()) {
                    reactor.remove(event.fd);
                    close(event.fd);
                }
            });

reactor.stop();
```

## How It Works

完整版本分成两层：

1. `EventLoop`
   - 每个 `EventLoop` 内部有一个 `epoll_fd`。
   - 用 `epoll_ctl` 注册、修改、删除 fd。
   - 用 `epoll_wait` 等待就绪事件。
   - 用 `eventfd` 做自唤醒，让别的线程调用 `stop/add/modify/remove` 时可以唤醒正在阻塞的 loop。
   - fd 到 callback 的映射放在 `unordered_map<int, Watcher>` 里。
   - `runOnce()` 先复制要执行的 callback，再释放内部锁，最后执行用户回调。

2. `MultiThreadReactor`
   - 构造多个 `EventLoop`。
   - `start()` 时给每个 loop 创建一个线程。
   - `add()` 用 round-robin 选一个 worker loop，把 fd 注册进去。
   - 用 `owners` 表记录每个 fd 属于哪个 loop，这样 `modify/remove` 能找到正确线程。
   - `stop()` 会通知所有 loop 停止，并等待 worker 线程退出。

这就是常见的多线程 Reactor 主干：

```text
listenfd
   |
   v
main reactor thread: accept new connfd
   |
   +--> worker reactor 0: connfd read/write
   +--> worker reactor 1: connfd read/write
   +--> worker reactor 2: connfd read/write
   +--> worker reactor 3: connfd read/write
```

## Design Notes

- 一个 fd 只属于一个 worker loop，避免多个线程同时处理同一个连接。
- `epoll_ctl` 可以从其他线程调用，但 loop 可能正在 `epoll_wait`，所以组件用 `eventfd` 显式唤醒。
- 回调不能在内部锁里执行，否则业务代码里再调用 `modify/remove` 会很容易死锁。
- ET 模式下，`accept/recv/send` 都要循环直到 `EAGAIN` 或 `EWOULDBLOCK`。
- 写事件不要长期关注。只有输出缓冲区没写完时才注册 `Write`，写空后立刻取消 `Write`，否则会一直收到“可写”事件。
- Reactor 线程适合做短小的 IO 状态推进；CPU 密集型任务应该扔给业务线程池，完成后再把结果投递回对应连接。
- 多线程 Reactor 不等于“一个连接一个线程”。它是少量线程管理大量连接。

## Interview Handwritten Version

面试手撕时不要一上来写完整工程版。建议写三个核心块：`EventLoop`、`ReactorPool`、`accept 后分发连接`。

最核心代码已经放在：

```text
handwritten/multi_thread_reactor_handwritten.cpp
```

手撕主干可以这样讲：

```cpp
class EventLoop {
    int epfd;
    int wakefd;
    unordered_map<int, function<void(int, uint32_t)>> callbacks;

public:
    EventLoop() {
        epfd = epoll_create1(0);
        wakefd = eventfd(0, EFD_NONBLOCK);
        add wakefd to epoll;
    }

    void add(int fd, uint32_t events, Callback cb) {
        callbacks[fd] = cb;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
        wake();
    }

    void loop() {
        while (!stop) {
            int n = epoll_wait(epfd, events, 1024, -1);
            for each ready event:
                if fd == wakefd: drain wakefd
                else: callbacks[fd](fd, events)
        }
    }
};

class ReactorPool {
    vector<EventLoop> loops;
    vector<thread> threads;
    atomic<int> next;

public:
    void start() {
        for each loop:
            threads.emplace_back([&] { loop.loop(); });
    }

    EventLoop& nextLoop() {
        return loops[next++ % loops.size()];
    }
};
```

然后补上网络流程：

```text
1. main loop 监听 listenfd。
2. listenfd 可读表示有新连接，循环 accept 到 EAGAIN。
3. 每个 connfd 设置非阻塞。
4. 用 pool.nextLoop() 选一个 worker。
5. 把 connfd 注册到 worker 的 epoll。
6. worker 收到 EPOLLIN 后循环 recv 到 EAGAIN。
7. 如果 send 没写完，把剩余数据放入 connection output buffer，并关注 EPOLLOUT。
8. EPOLLOUT 触发后继续写，写空后取消 EPOLLOUT。
9. EPOLLERR/EPOLLHUP/EPOLLRDHUP 时从 epoll 删除 fd 并 close。
```

## Possible Interview Questions

- Reactor 模式是什么？和阻塞 IO、线程池模型有什么区别？
- 单线程 Reactor 和多线程 Reactor 的区别是什么？
- 为什么主 Reactor 通常只负责 `accept`，worker Reactor 负责连接读写？
- 一个 fd 能不能被多个 worker loop 同时处理？为什么通常不这么做？
- `epoll_create1`、`epoll_ctl`、`epoll_wait` 分别做什么？
- LT 和 ET 有什么区别？ET 为什么必须配合非阻塞 IO？
- `accept/recv/send` 为什么要循环到 `EAGAIN`？
- `eventfd` 在 Reactor 里解决什么问题？
- 为什么 callback 要在 Reactor 内部锁之外执行？
- 为什么不能一直监听 `EPOLLOUT`？
- `EPOLLONESHOT` 在多线程网络库里有什么用？
- 如果业务回调很慢，会对 Reactor 造成什么影响？应该怎么处理？
- 如何做连接的读缓冲、写缓冲和半包处理？
- 多线程 Reactor 如何做负载均衡？round-robin 有什么问题？
- Reactor 模型和 Proactor、io_uring、IOCP 有什么区别？

## Common Uses

- 高并发 TCP server，例如网关、RPC server、IM server、代理服务器。
- 需要用少量线程管理大量长连接的服务。
- 连接数很高、单个连接 IO 事件很碎的场景。
- IO 轻、业务处理可异步投递到线程池的后端服务。
- 学习 Redis、Nginx、Netty、muduo 这类事件驱动网络框架的底层模型。

不适合的场景：

- 连接数很少、逻辑很简单，阻塞 IO 加线程已经足够。
- 每个请求都是长时间 CPU 计算，而且没有业务线程池承接。
- Windows 生产网络服务更适合研究 IOCP；Linux 新项目也可以关注 io_uring。
