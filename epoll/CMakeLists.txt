cmake_minimum_required(VERSION 3.16)

project(epoll
    VERSION 0.1.0
    DESCRIPTION "A small educational epoll reactor"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(epoll_reactor
    src/epoll_reactor.cpp
)

target_include_directories(epoll_reactor
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(epoll_reactor
    PUBLIC
        cxx_std_17
)

find_package(Threads REQUIRED)
target_link_libraries(epoll_reactor
    PUBLIC
        Threads::Threads
)

add_executable(epoll_demo
    examples/demo.cpp
)

target_link_libraries(epoll_demo
    PRIVATE
        epoll_reactor
)

add_executable(epoll_echo_server
    examples/echo_server.cpp
)

target_link_libraries(epoll_echo_server
    PRIVATE
        epoll_reactor
)

enable_testing()

add_executable(epoll_reactor_tests
    tests/epoll_reactor_test.cpp
)

target_link_libraries(epoll_reactor_tests
    PRIVATE
        epoll_reactor
)

add_test(NAME epoll_reactor_tests COMMAND epoll_reactor_tests)
