# Limit Order Book in C++20

A price-time priority matching engine, built twice: a simple, obviously-correct v1, and a v2 optimized for tail latency. A randomized differential test checks that v2 produces exactly the same trades and book state as v1 on 3.6M operations.

## Build and run
```
make test          # unit + differential tests (AddressSanitizer + UBSan)
make bench         # v1 vs v2 benchmark
./build/bench 2    # same, pinned to CPU core 2
```

## Results
1M operations (75% limit orders, 25% cancels, 200 price levels), WSL2, pinned to one core. Timer overhead (~15 ns) is included in every latency.

| | v1 | v2 |
|---|---|---|
| p50 | 111 ns | 111 ns |
| p99 | 632 ns | 444 ns |
| p99.9 | 1,461 ns | 686 ns |
| max | 7.9 ms | 0.8 ms |
| Throughput | 8.0 M ops/s | 17.6 M ops/s |

Median latency is unchanged; the gains are in tail latency and throughput, which is where heap allocation and cache misses hurt.

## Design
| | v1 | v2 |
|---|---|---|
| Orders | `std::list` node per order | preallocated arena + free list |
| Price levels | `std::map` | flat array indexed by price tick |
| Order id lookup | `std::unordered_map` | open-addressing hash, fixed capacity |
| Trade output | new `vector` per call | caller-provided, reused vector |


	MutexQueue	SpscQueueNaive	SpscQueue
Throughput (Mmsg/s)	9.2	17.5	252
One-way latency p50 (ns)	346	132	54
One-way latency p99 (ns)	732	175	144

Replace the pipeline line with: Pipeline (1M orders): single thread 12.6 Mops/s vs two threads via SpscQueue 11.7 Mops/s, results identical. The queue decouples ingestion from matching; it doesn't add matching throughput.
| Best price | tree walk | cached best bid/ask index |

## Limits of v2 (deliberate trade-offs)
- Prices must lie in a fixed range; others are rejected.
- Fixed order capacity; when the arena is full, new orders are rejected.
- Order id 0 is reserved.


