cmake_minimum_required(VERSION 3.16)

project(ring_buffer
    VERSION 0.1.0
    DESCRIPTION "A small educational ring buffer"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(ring_buffer INTERFACE)

target_include_directories(ring_buffer
    INTERFACE
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(ring_buffer
    INTERFACE
        cxx_std_17
)

find_package(Threads REQUIRED)
target_link_libraries(ring_buffer
    INTERFACE
        Threads::Threads
)

add_executable(ring_buffer_demo
    examples/demo.cpp
)

target_link_libraries(ring_buffer_demo
    PRIVATE
        ring_buffer
)

enable_testing()

add_executable(ring_buffer_tests
    tests/ring_buffer_test.cpp
)

target_link_libraries(ring_buffer_tests
    PRIVATE
        ring_buffer
)

add_test(NAME ring_buffer_tests COMMAND ring_buffer_tests)
