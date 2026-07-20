cmake_minimum_required(VERSION 3.16)

project(connection_pool
    VERSION 0.1.0
    DESCRIPTION "A small educational database connection pool"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(connection_pool
    src/connection_pool.cpp
)

target_include_directories(connection_pool
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

find_package(Threads REQUIRED)
target_link_libraries(connection_pool
    PUBLIC
        Threads::Threads
)

add_executable(connection_pool_demo
    examples/demo.cpp
)

target_link_libraries(connection_pool_demo
    PRIVATE
        connection_pool
)

enable_testing()

add_executable(connection_pool_tests
    tests/connection_pool_test.cpp
)

target_link_libraries(connection_pool_tests
    PRIVATE
        connection_pool
)

add_test(NAME connection_pool_tests COMMAND connection_pool_tests)
