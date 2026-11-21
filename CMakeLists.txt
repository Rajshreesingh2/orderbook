cmake_minimum_required(VERSION 3.16)
project(orderbook CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wpedantic)

add_library(ob INTERFACE)
target_include_directories(ob INTERFACE include)

add_executable(tests tests/test_order_book.cpp)
target_link_libraries(tests ob)
target_compile_options(tests PRIVATE -O1 -g -fsanitize=address,undefined)
target_link_options(tests PRIVATE -fsanitize=address,undefined)

add_executable(bench src/bench.cpp)
target_link_libraries(bench ob)
target_compile_options(bench PRIVATE -O2)

enable_testing()
add_test(NAME tests COMMAND tests)
