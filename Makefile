CXX ?= g++
CXXFLAGS = -std=c++20 -Wall -Wextra -Wpedantic -Iinclude

all: test bench

build/tests: tests/test_order_book.cpp include/order_book.hpp include/order_book_v2.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined $< -o $@

build/bench: src/bench.cpp include/order_book.hpp include/order_book_v2.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) -O2 $< -o $@

test: build/tests
	./build/tests

bench: build/bench
	./build/bench

clean:
	rm -rf build

.PHONY: all test bench clean
