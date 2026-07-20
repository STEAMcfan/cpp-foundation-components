# Database Connection Pool

This directory contains a small C++17 database connection pool implementation for learning and interview discussion.

The pool does not depend on a real MySQL/PostgreSQL client library. Instead, it uses a tiny `IDbConnection` interface and a factory callback, so the pool logic is easy to study:

- reuse idle connections instead of creating one per query
- cap concurrent database connections with `max_size`
- block callers when the pool is exhausted
- return `nullptr` when acquiring a connection times out
- use RAII so borrowed connections are returned automatically
- remove idle or long-lived connections during periodic cleanup

## Layout

```text
connection_pool/
+-- include/connection_pool.hpp
+-- src/connection_pool.cpp
+-- examples/demo.cpp
+-- tests/connection_pool_test.cpp
+-- CMakeLists.txt
+-- README.md
```

## Build And Run

```bash
cd connection_pool
cmake -S . -B build
cmake --build build
./build/connection_pool_demo
ctest --test-dir build --output-on-failure
```

On multi-config generators, the executable may be under `build/Debug/`.

## Basic Usage

```cpp
foundation::ConnectionPoolConfig config;
config.min_idle = 1;
config.max_size = 4;
config.acquire_timeout = std::chrono::milliseconds(500);

foundation::ConnectionPool pool(
    [] {
        return std::make_unique<MyDbConnection>();
    },
    config
);

pool.warmUp();

auto conn = pool.acquire();
if (!conn) {
    // The pool was exhausted and acquire_timeout elapsed.
    return;
}

conn->execute("select * from users");
// No manual release is needed. The shared_ptr returns it to the pool.
```

## Design Notes

`ConnectionPool::acquire()` returns a `std::shared_ptr<IDbConnection>` with a custom owner. When the last copy of the pointer is destroyed, the connection is automatically returned to the pool. This prevents the most common connection-pool bug: forgetting to call `release()`.

The pool stores metadata next to every connection:

- creation time, used by `max_lifetime`
- last-used time, used by `idle_timeout`
- current pool counters, used by `stats()`

The implementation keeps slow work such as connecting, pinging, and closing outside the main mutex whenever possible. That keeps other threads from being blocked by database I/O while they are only trying to borrow or return a connection.

`shutdown()` is graceful: it stops new borrows, wakes waiting callers, and waits until active borrowed connections have been returned before closing idle connections.
