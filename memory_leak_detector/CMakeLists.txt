cmake_minimum_required(VERSION 3.16)

project(memory_leak_detector
    VERSION 0.1.0
    DESCRIPTION "A small educational memory leak detector"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(memory_leak_detector
    src/memory_leak_detector.cpp
)

target_include_directories(memory_leak_detector
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(memory_leak_detector
    PUBLIC
        cxx_std_17
)

find_package(Threads REQUIRED)
target_link_libraries(memory_leak_detector
    PUBLIC
        Threads::Threads
)

add_executable(memory_leak_detector_demo
    examples/demo.cpp
)

target_link_libraries(memory_leak_detector_demo
    PRIVATE
        memory_leak_detector
)

enable_testing()

add_executable(memory_leak_detector_tests
    tests/memory_leak_detector_test.cpp
)

target_link_libraries(memory_leak_detector_tests
    PRIVATE
        memory_leak_detector
)

add_test(NAME memory_leak_detector_tests COMMAND memory_leak_detector_tests)
