cmake_minimum_required(VERSION 3.16)

project(lock_free_queue
    VERSION 0.1.0
    DESCRIPTION "A small educational lock-free queue"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(lock_free_queue INTERFACE)

target_include_directories(lock_free_queue
    INTERFACE
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(lock_free_queue
    INTERFACE
        cxx_std_17
)

find_package(Threads REQUIRED)
target_link_libraries(lock_free_queue
    INTERFACE
        Threads::Threads
)

add_executable(lock_free_queue_demo
    examples/demo.cpp
)

target_link_libraries(lock_free_queue_demo
    PRIVATE
        lock_free_queue
)

enable_testing()

add_executable(lock_free_queue_tests
    tests/lock_free_queue_test.cpp
)

target_link_libraries(lock_free_queue_tests
    PRIVATE
        lock_free_queue
)

add_test(NAME lock_free_queue_tests COMMAND lock_free_queue_tests)
