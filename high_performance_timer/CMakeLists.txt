cmake_minimum_required(VERSION 3.16)

project(high_performance_timer
    VERSION 0.1.0
    DESCRIPTION "A small educational high-performance timer"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(high_performance_timer
    src/high_performance_timer.cpp
)

target_include_directories(high_performance_timer
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

find_package(Threads REQUIRED)
target_link_libraries(high_performance_timer
    PUBLIC
        Threads::Threads
)

add_executable(high_performance_timer_demo
    examples/demo.cpp
)

target_link_libraries(high_performance_timer_demo
    PRIVATE
        high_performance_timer
)

enable_testing()

add_executable(high_performance_timer_tests
    tests/high_performance_timer_test.cpp
)

target_link_libraries(high_performance_timer_tests
    PRIVATE
        high_performance_timer
)

add_test(NAME high_performance_timer_tests COMMAND high_performance_timer_tests)
