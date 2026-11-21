// Usage: ./build/bench_spsc [producer_core consumer_core]     e.g.  ./build/bench_spsc 2 4
// Pin the two threads to DIFFERENT physical cores for meaningful numbers.
#include "order_book_v2.hpp"
#include "spsc_queue.hpp"
#include "workload.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
static int g_core_a = -1, g_core_b = -1;   // -1 = don't pin

static double secs(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

// ---- 1. one-way throughput: producer thread -> consumer thread ---------------------------
template <class Q>
static double throughput(std::uint64_t n) {
    auto q = std::make_unique<Q>();
    const auto t0 = Clock::now();
    std::thread producer([&] {
        if (g_core_a >= 0) pin_this_thread(g_core_a);
        for (std::uint64_t i = 0; i < n; ++i) push_blocking(*q, i);
    });
    if (g_core_b >= 0) pin_this_thread(g_core_b);
    std::uint64_t sum = 0, v = 0;
    for (std::uint64_t i = 0; i < n; ++i) { pop_blocking(*q, v); sum += v; }
    producer.join();
    const auto t1 = Clock::now();
    if (sum != n * (n - 1) / 2) { std::puts("CORRUPTION!"); std::abort(); }
    return static_cast<double>(n) / secs(t0, t1) / 1e6;
}

// ---- 2. ping-pong: how long does a message take to get to the other thread? ----------------
struct RoundTrip { double p50, p99, p999; };
template <class Q>
static RoundTrip pingpong(int rounds) {
    auto ab = std::make_unique<Q>();
    auto ba = std::make_unique<Q>();
    std::thread echo([&] {
        if (g_core_a >= 0) pin_this_thread(g_core_a);
        std::uint64_t v;
        for (;;) { pop_blocking(*ab, v); push_blocking(*ba, v); if (v == ~0ull) break; }
    });
    if (g_core_b >= 0) pin_this_thread(g_core_b);
    std::vector<double> lat; lat.reserve(static_cast<std::size_t>(rounds));
    std::uint64_t v = 0;
    for (int i = 0; i < rounds; ++i) {
        const auto t0 = Clock::now();
        push_blocking(*ab, static_cast<std::uint64_t>(i));
        pop_blocking(*ba, v);
        const auto t1 = Clock::now();
        lat.push_back(secs(t0, t1) * 1e9 / 2.0);                      // one-way = round trip / 2
    }
    push_blocking(*ab, ~0ull); pop_blocking(*ba, v);
    echo.join();
    std::sort(lat.begin(), lat.end());
    auto pct = [&](double p) { return lat[static_cast<std::size_t>(p * static_cast<double>(lat.size() - 1))]; };
    return {pct(0.50), pct(0.99), pct(0.999)};
}

// ---- 3. pipeline: network thread -> SPSC -> matching thread, vs one thread doing everything ----
static double pipeline_direct(const std::vector<Op>& ops, Digest& d) {
    OrderBookV2 book(9'000, 11'000, 1 << 20);
    std::vector<Trade> out; out.reserve(1024);
    const auto t0 = Clock::now();
    for (const Op& op : ops) apply(book, op, out, d);
    const auto t1 = Clock::now();
    d.resting = book.order_count();
    return static_cast<double>(ops.size()) / secs(t0, t1) / 1e6;
}
static double pipeline_queued(const std::vector<Op>& ops, Digest& d) {
    auto q = std::make_unique<SpscQueue<Op, 1 << 14>>();
    OrderBookV2 book(9'000, 11'000, 1 << 20);
    std::vector<Trade> out; out.reserve(1024);
    const auto t0 = Clock::now();
    std::thread producer([&] {
        if (g_core_a >= 0) pin_this_thread(g_core_a);
        for (const Op& op : ops) push_blocking(*q, op);
        push_blocking(*q, Op{0, 0, 0, Side::Buy, 2});
    });
    if (g_core_b >= 0) pin_this_thread(g_core_b);
    Op op{};
    for (;;) { pop_blocking(*q, op); if (op.kind == 2) break; apply(book, op, out, d); }
    producer.join();
    const auto t1 = Clock::now();
    d.resting = book.order_count();
    return static_cast<double>(ops.size()) / secs(t0, t1) / 1e6;
}

int main(int argc, char** argv) {
    if (argc > 2) { g_core_a = std::atoi(argv[1]); g_core_b = std::atoi(argv[2]);
                    std::printf("threads pinned to cores %d and %d\n", g_core_a, g_core_b); }
    else std::printf("threads NOT pinned (run with:  ./build/bench_spsc 2 4)\n");
    std::printf("hardware threads: %u\n\n", std::thread::hardware_concurrency());
    constexpr std::size_t C = 1 << 16;
    const std::uint64_t N = 5'000'000;

    std::printf("1) one-way throughput, %llu messages (higher is better)\n", (unsigned long long)N);
    const double m1 = throughput<MutexQueue<std::uint64_t, C>>(N);
    const double m2 = throughput<SpscQueueNaive<std::uint64_t, C>>(N);
    const double m3 = throughput<SpscQueue<std::uint64_t, C>>(N);
    std::printf("   %-16s %8.1f Mmsg/s\n   %-16s %8.1f Mmsg/s\n   %-16s %8.1f Mmsg/s   (%.1fx vs mutex)\n\n",
                "MutexQueue", m1, "SpscQueueNaive", m2, "SpscQueue", m3, m3 / m1);

    std::printf("2) one-way hand-off latency via ping-pong (lower is better; includes ~20 ns clock cost)\n");
    std::printf("   %-16s %8s %8s %8s\n", "", "p50 ns", "p99 ns", "p99.9 ns");
    const RoundTrip r1 = pingpong<MutexQueue<std::uint64_t, 1024>>(200'000);
    const RoundTrip r2 = pingpong<SpscQueueNaive<std::uint64_t, 1024>>(200'000);
    const RoundTrip r3 = pingpong<SpscQueue<std::uint64_t, 1024>>(200'000);
    std::printf("   %-16s %8.0f %8.0f %8.0f\n   %-16s %8.0f %8.0f %8.0f\n   %-16s %8.0f %8.0f %8.0f\n\n",
                "MutexQueue", r1.p50, r1.p99, r1.p999, "SpscQueueNaive", r2.p50, r2.p99, r2.p999,
                "SpscQueue", r3.p50, r3.p99, r3.p999);

    std::printf("3) order-flow pipeline, 1M ops into OrderBookV2\n");
    const auto ops = generate_ops(1'000'000);
    Digest dd, dq;
    const double direct = pipeline_direct(ops, dd);
    const double queued = pipeline_queued(ops, dq);
    std::printf("   one thread, no queue   %8.2f Mops/s\n   2 threads via SpscQueue%8.2f Mops/s   results %s\n",
                direct, queued, dd == dq ? "IDENTICAL" : "MISMATCH!");
}
