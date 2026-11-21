CXX ?= g++
CXXFLAGS = -std=c++20 -Wall -Wextra -Wpedantic -Iinclude -pthread
HDRS = $(wildcard include/*.hpp)

all: test bench

build/tests: tests/test_order_book.cpp $(HDRS)
	mkdir -p build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined $< -o $@

build/test_spsc: tests/test_spsc.cpp $(HDRS)
	mkdir -p build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined $< -o $@

build/tsan_spsc: tests/test_spsc.cpp $(HDRS)
	mkdir -p build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=thread $< -o $@

build/bench: src/bench.cpp $(HDRS)
	mkdir -p build
	$(CXX) $(CXXFLAGS) -O2 $< -o $@

build/bench_spsc: src/bench_spsc.cpp $(HDRS)
	mkdir -p build
	$(CXX) $(CXXFLAGS) -O2 $< -o $@

test: build/tests build/test_spsc
	./build/tests
	./build/test_spsc

tsan: build/tsan_spsc
	./build/tsan_spsc

bench: build/bench
	./build/bench

bench_spsc: build/bench_spsc
	./build/bench_spsc

clean:
	rm -rf build

.PHONY: all test tsan bench bench_spsc clean
