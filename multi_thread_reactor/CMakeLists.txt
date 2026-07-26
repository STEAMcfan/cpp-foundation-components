cmake_minimum_required(VERSION 3.16)

project(multi_thread_reactor
    VERSION 0.1.0
    DESCRIPTION "A small educational multi-thread reactor"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(multi_thread_reactor
    src/multi_thread_reactor.cpp
)

target_include_directories(multi_thread_reactor
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(multi_thread_reactor
    PUBLIC
        cxx_std_17
)

find_package(Threads REQUIRED)
target_link_libraries(multi_thread_reactor
    PUBLIC
        Threads::Threads
)

add_executable(multi_thread_reactor_demo
    examples/demo.cpp
)

target_link_libraries(multi_thread_reactor_demo
    PRIVATE
        multi_thread_reactor
)

add_executable(multi_thread_reactor_echo_server
    examples/echo_server.cpp
)

target_link_libraries(multi_thread_reactor_echo_server
    PRIVATE
        multi_thread_reactor
)

enable_testing()

add_executable(multi_thread_reactor_tests
    tests/multi_thread_reactor_test.cpp
)

target_link_libraries(multi_thread_reactor_tests
    PRIVATE
        multi_thread_reactor
)

add_test(NAME multi_thread_reactor_tests COMMAND multi_thread_reactor_tests)
