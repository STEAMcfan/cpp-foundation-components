# Epoll Reactor

这个目录包含一个 C++17 epoll 组件，用来学习 Linux 高性能网络 IO 的核心写法：

- 用 `epoll_create1()` 创建事件集合。
- 用 `epoll_ctl()` 注册、修改、删除 fd。
- 用 `epoll_wait()` 等待就绪事件。
- 所有被 epoll 管理的 socket / pipe 都要设置成非阻塞。
- 边缘触发 `EPOLLET` 模式下，必须循环读写直到 `EAGAIN` / `EWOULDBLOCK`。

`EpollReactor` 是一个轻量 reactor 封装。它不是业务框架，而是把 epoll 的样板代码整理成一个能编译、能测试、能讲清楚的学习组件。

## Layout

```text
epoll/
+-- include/epoll_reactor.hpp
+-- src/epoll_reactor.cpp
+-- examples/demo.cpp
+-- examples/echo_server.cpp
+-- tests/epoll_reactor_test.cpp
+-- CMakeLists.txt
+-- README.md
```

## Build And Run

```bash
cd epoll
cmake -S . -B build
cmake --build build
./build/epoll_demo
ctest --test-dir build --output-on-failure
```

完整 echo server 示例：

```bash
./build/epoll_echo_server 9090
nc 127.0.0.1 9090
```

epoll 是 Linux 专有接口。这个组件在非 Linux 系统上仍可编译，但运行 demo / test 时会跳过或验证“不支持”分支。

## Basic Usage

```cpp
foundation::EpollReactor reactor;

reactor.add(fd,
            foundation::EpollEvents::Read |
            foundation::EpollEvents::EdgeTriggered,
            [&](const foundation::EpollEvent& event) {
                while (true) {
                    char buf[4096];
                    ssize_t n = read(event.fd, buf, sizeof(buf));
                    if (n > 0) {
                        // handle bytes
                        continue;
                    }
                    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    }
                    reactor.remove(event.fd);
                    close(event.fd);
                    break;
                }
            });

reactor.run();
```

## Design Notes

`epoll` 本身不负责“把数据读完”，它只告诉你“某个 fd 现在可能可以读 / 写”。真正的工程重点是：

- fd 必须是非阻塞，否则一个 fd 的 `read()` / `write()` 就可能卡住整个事件循环。
- `EPOLLET` 只在状态变化时通知一次，所以收到事件后要一直读到 `EAGAIN`。
- `EPOLLIN` 表示可读，`EPOLLOUT` 表示可写，`EPOLLERR` / `EPOLLHUP` 表示异常或关闭。
- 监听 socket 收到 `EPOLLIN` 后，要循环 `accept()` 到 `EAGAIN`。
- 普通连接收到 `EPOLLIN` 后，要循环 `recv()` 到 `EAGAIN`。
- 写缓冲区没有清空时才关注 `EPOLLOUT`，否则会不断收到“可写”事件导致空转。
- 回调执行时不能长期持有 reactor 内部锁，否则业务代码会阻塞事件注册和删除。
- `stop()` 需要唤醒正在 `epoll_wait()` 的线程；工程版通常用 `eventfd` 做自唤醒。

## Key Points

epoll 的关键不是“会调用三个系统调用”，而是理解它的模型：

1. epoll 管的是 fd 集合，不管 fd 的业务状态。
2. epoll 返回的是“就绪事件”，不是完整数据。
3. 非阻塞 IO 和 epoll 必须配套使用。
4. LT 水平触发适合入门，ET 边缘触发性能更好但更容易写错。
5. ET 模式必须读写到 `EAGAIN`，否则剩余数据可能不会再次触发事件。
6. 高并发服务器通常是 `epoll + nonblocking socket + per-connection buffer + state machine`。

## Common Uses

- 高并发 TCP server，比如网关、代理、RPC server、IM server。
- 单线程或少量线程管理大量连接。
- 事件驱动程序，例如网络连接、timerfd、eventfd、signalfd 混合在一个 loop 里处理。
- 替代 `select` / `poll`，避免每轮扫描所有 fd。
- 和线程池配合：IO 线程只做收发和状态推进，CPU 重任务扔给 worker。

## Interview Handwritten Version

面试手撕 epoll 时，先把三板斧写清楚，再把它放进 echo server。这样面试官能看到你理解的是“事件通知模型”，不是只背了一段网络代码。

最小骨架：

```cpp
// 1. 创建 epoll 实例。epfd 是一个“事件中心”的 fd。
int epfd = epoll_create1(0);

// 2. 用 epoll_ctl 把要监听的 fd 加进去。
epoll_event ev{};
ev.events = EPOLLIN | EPOLLET;
ev.data.fd = listenfd;
epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev);

// 3. 用 epoll_wait 等待事件发生。
epoll_event events[1024];
while (true) {
    int n = epoll_wait(epfd, events, 1024, -1);
    for (int i = 0; i < n; ++i) {
        int fd = events[i].data.fd;
        // fd == listenfd: accept 新连接
        // fd != listenfd: read/write 普通客户端连接
    }
}
```

完整手撕版本推荐写成“非阻塞 echo server 核心循环”：

```cpp
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void add_fd(int epfd, int fd, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

void del_fd(int epfd, int fd) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
}

int main() {
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    set_nonblock(listenfd);

    int on = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(8080);
    bind(listenfd, (sockaddr*)&addr, sizeof(addr));
    listen(listenfd, SOMAXCONN);

    // epoll_create1: 创建 epoll 实例，epfd 用来管理一组 fd 的事件。
    int epfd = epoll_create1(0);

    // epoll_ctl ADD: 把监听 socket 加进 epoll。
    // listenfd 可读，表示有新客户端连接可以 accept。
    add_fd(epfd, listenfd, EPOLLIN | EPOLLET);

    epoll_event events[1024];
    char buf[4096];

    while (true) {
        // epoll_wait: 阻塞等待就绪事件，返回本轮有事件的 fd 数量。
        int n = epoll_wait(epfd, events, 1024, -1);

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == listenfd) {
                // listenfd 的 EPOLLIN 表示“有新连接”，要 accept。
                // ET 模式下必须一直 accept 到 EAGAIN。
                while (true) {
                    int connfd = accept(listenfd, nullptr, nullptr);
                    if (connfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        continue;
                    }

                    set_nonblock(connfd);

                    // epoll_ctl ADD: 新客户端 fd 也交给 epoll 监听。
                    // connfd 可读，表示客户端发来了数据。
                    add_fd(epfd, connfd, EPOLLIN | EPOLLET | EPOLLRDHUP);
                }
                continue;
            }

            if (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                // epoll_ctl DEL: 连接异常或对端关闭时，从 epoll 中删除 fd。
                del_fd(epfd, fd);
                continue;
            }

            if (ev & EPOLLIN) {
                // 普通客户端 fd 的 EPOLLIN 表示“有数据可读”。
                // ET 模式下必须一直 read 到 EAGAIN。
                while (true) {
                    int m = read(fd, buf, sizeof(buf));
                    if (m > 0) {
                        write(fd, buf, m); // demo: echo back
                    } else if (m == 0) {
                        del_fd(epfd, fd);
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        del_fd(epfd, fd);
                        break;
                    }
                }
            }
        }
    }
}
```

手写讲解顺序：

1. `epoll_create1()` 创建 epoll 实例，返回 `epfd`，它代表一个事件中心。
2. `epoll_ctl(..., EPOLL_CTL_ADD, ...)` 把 `listenfd` 注册进去，关注 `EPOLLIN`。
3. `epoll_wait()` 阻塞等待事件，返回本轮就绪的 fd。
4. 如果就绪的是 `listenfd`，说明有新连接，要循环 `accept()` 到 `EAGAIN`。
5. 如果就绪的是普通客户端 fd，说明有数据或断开，要 `read()` / `close()`。
6. 每个新 `connfd` 也要用 `epoll_ctl ADD` 加入 epoll，否则后续客户端发数据你收不到通知。
7. 连接关闭时用 `epoll_ctl DEL` 删除 fd，然后 `close(fd)`。
8. ET 模式通知次数少，但必须配合非阻塞 fd，并且一次读写到 `EAGAIN`。
9. 真正生产级 echo 不能直接裸 `write()`，应该维护每个连接的输出缓冲，写不完就注册 `EPOLLOUT`。

## Possible Interview Questions

- epoll 相比 select / poll 的优势是什么？
- epoll 的三个核心系统调用分别做什么？
- LT 和 ET 的区别是什么？ET 为什么必须配合非阻塞 IO？
- 为什么 ET 模式下 `read()` 要循环读到 `EAGAIN`？
- `EPOLLIN`、`EPOLLOUT`、`EPOLLERR`、`EPOLLHUP`、`EPOLLRDHUP` 分别表示什么？
- `EPOLLONESHOT` 有什么用？为什么多线程处理连接时常用它？
- 惊群问题是什么？Linux 里怎么缓解？
- epoll 为什么不是“异步 IO”？它和 IOCP / io_uring 的区别是什么？
- 为什么不要一直监听 `EPOLLOUT`？
- fd 被关闭后还在 epoll 里会怎样？为什么要先 `EPOLL_CTL_DEL` 再 `close()`？
- 高并发服务器为什么要保存每个连接的读缓冲、写缓冲和状态机？
- epoll 可以监听普通文件吗？为什么网络 fd 更适合 epoll？
