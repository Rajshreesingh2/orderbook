# Low-latency building blocks in C++20

1. **Limit order book** — price-time priority matching engine, built twice (simple v1, fast v2).
2. **Lock-free SPSC queue** — three implementations compared, plus a producer → queue → order-book pipeline.

Every optimized component is checked against a simple reference: a randomized differential test (v1 vs v2, 3.6M random ops) and a pipeline test (2 threads vs 1 thread, identical results). Tests run under AddressSanitizer, UBSan and ThreadSanitizer.

## Build and run
```
make test          # unit + differential + SPSC tests (ASan + UBSan)
make tsan          # SPSC tests under ThreadSanitizer
make bench         # order book: v1 vs v2
./build/bench 2    #   ...pinned to CPU core 2
make bench_spsc    # queues + pipeline
./build/bench_spsc 2 4   # ...producer on core 2, consumer on core 4 (use two different physical cores)
```

## 1. Order book

| | v1 | v2 |
|---|---|---|
| Orders | `std::list` node per order | preallocated arena + free list |
| Price levels | `std::map` | flat array indexed by price tick |
| Order id lookup | `std::unordered_map` | open-addressing hash, fixed capacity |
| Trade output | new `vector` per call | caller-provided, reused vector |
| Best price | tree walk | cached best bid/ask index |

Results: 1M operations (75% limit orders, 25% cancels, 200 price levels), WSL2, pinned to one core. Timer overhead (~15 ns) is included in every latency.

| | v1 | v2 |
|---|---|---|
| p50 | 111 ns | 111 ns |
| p99 | 632 ns | 444 ns |
| p99.9 | 1,461 ns | 686 ns |
| max | 7.9 ms | 0.8 ms |
| Throughput | 8.0 M ops/s | 17.6 M ops/s |

Median latency is unchanged; the gains are in tail latency and throughput, which is where heap allocation and cache misses hurt.

Limits of v2 (deliberate trade-offs): prices must lie in a fixed range; fixed order capacity (a full arena rejects new orders); order id 0 is reserved.

## 2. SPSC queue (`include/spsc_queue.hpp`)

| | MutexQueue | SpscQueueNaive | SpscQueue |
|---|---|---|---|
| Synchronization | `std::mutex` | atomics, seq_cst | atomics, acquire/release |
| head/tail layout | — | same cache line (false sharing) | separate 64-byte lines |
| Reads other side's index | always (under lock) | on every call | only when ring looks full/empty (cached copy) |

Why it is correct: the producer is the only writer of `tail_` and publishes a slot with a *release* store; the consumer reads `tail_` with an *acquire* load, so it sees the element's contents. The consumer returns slots to the producer the same way through `head_`. Indices only grow and wrap by masking, which removes the "full vs empty" ambiguity.

Results (fill in from your machine: `./build/bench_spsc <core> <core>`):

| | MutexQueue | SpscQueueNaive | SpscQueue |
|---|---|---|---|
| Throughput (Mmsg/s) | | | |
| One-way latency p50 (ns) | | | |
| One-way latency p99 (ns) | | | |

Pipeline (1M orders): single thread ___ Mops/s vs two threads via SpscQueue ___ Mops/s, results identical.

## Next
Bitmap of non-empty price levels, market/IOC orders, kernel-bypass style input, cache-miss profiling.
