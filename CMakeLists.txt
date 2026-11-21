cmake_minimum_required(VERSION 3.16)
project(orderbook CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
add_compile_options(-Wall -Wextra -Wpedantic)

add_library(ob INTERFACE)
target_include_directories(ob INTERFACE include)
target_link_libraries(ob INTERFACE Threads::Threads)

add_executable(tests tests/test_order_book.cpp)
target_link_libraries(tests ob)
target_compile_options(tests PRIVATE -O1 -g -fsanitize=address,undefined)
target_link_options(tests PRIVATE -fsanitize=address,undefined)

add_executable(test_spsc tests/test_spsc.cpp)
target_link_libraries(test_spsc ob)
target_compile_options(test_spsc PRIVATE -O1 -g -fsanitize=address,undefined)
target_link_options(test_spsc PRIVATE -fsanitize=address,undefined)

add_executable(bench src/bench.cpp)
target_link_libraries(bench ob)
target_compile_options(bench PRIVATE -O2)

add_executable(bench_spsc src/bench_spsc.cpp)
target_link_libraries(bench_spsc ob)
target_compile_options(bench_spsc PRIVATE -O2)

enable_testing()
add_test(NAME tests COMMAND tests)
add_test(NAME test_spsc COMMAND test_spsc)
