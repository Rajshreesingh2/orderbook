# Limit Order Book (v1: correctness baseline)

Price-time priority matching engine in C++20.

## Build & run
    cmake -B build && cmake --build build
    ./build/tests     # correctness (built with ASan + UBSan, asserts enabled)
    ./build/bench     # baseline latency numbers

## Design (v1)
- Bids: `std::map<Price, std::list<Order>, greater>`, asks: `std::map<Price, std::list<Order>>`
- `std::list` per price level = FIFO (time priority)
- `unordered_map<OrderId, Locator>` for cancel lookup (stored list iterator)
- Prices are integer ticks; trades execute at the resting order's price

## Baseline results (fill in from your machine)
| metric | ns |
|---|---|
| p50 | |
| p99 | |
| p99.9 | |

## Roadmap
1. Profile with `perf stat` / `perf record` (cache misses, allocations)
2. Replace `std::list` + `std::map` with contiguous / pooled storage
3. Remove heap allocation from the hot path
4. Reserve the id index, or use a flat hash map
5. Pin the thread to a core, re-measure p99 / p99.9
