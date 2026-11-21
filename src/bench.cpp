// Side-by-side benchmark: v1 vs v2 on the exact same pre-generated operation stream.
// Usage: ./build/bench [cpu_core]     e.g.  ./build/bench 2   (pins the thread to core 2)
#include "order_book.hpp"
#include "order_book_v2.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#ifdef __linux__
#include <sched.h>
#endif

using Clock = std::chrono::steady_clock;
struct Op { OrderId id; Price price; Qty qty; Side side; bool cancel; };

static std::vector<Op> generate(int n) {
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<Price> price(9'900, 10'100);
    std::uniform_int_distribution<Qty> qty(1, 100);
    std::vector<Op> ops;
    ops.reserve(static_cast<std::size_t>(n));
    OrderId next = 1;
    for (int i = 0; i < n; ++i) {
        if (rng() % 100 < 25 && next > 1)
            ops.push_back({1 + rng() % (next - 1), 0, 0, Side::Buy, true});
        else
            ops.push_back({next++, price(rng), qty(rng), (rng() & 1) ? Side::Buy : Side::Sell, false});
    }
    return ops;
}

struct Result { double p50, p99, p999, max, mops; std::uint64_t trades, checksum; std::size_t resting; };

template <class Make>
static Result run(Make make, const std::vector<Op>& ops) {
    Result r{};
    std::vector<Trade> out; out.reserve(1024);
    {   // pass 1: per-operation latency
        auto book = make();
        std::vector<std::uint32_t> lat; lat.reserve(ops.size());
        for (const Op& op : ops) {
            const auto t0 = Clock::now();
            if (op.cancel) book.cancel(op.id);
            else { out.clear(); book.add_limit(op.id, op.side, op.price, op.qty, out); }
            const auto t1 = Clock::now();
            lat.push_back(static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
            if (!op.cancel) {
                r.trades += out.size();
                for (const Trade& t : out) r.checksum += static_cast<std::uint64_t>(t.price) * t.qty + t.maker_id * 31 + t.taker_id;
            }
        }
        std::sort(lat.begin(), lat.end());
        auto pct = [&](double p) { return static_cast<double>(lat[static_cast<std::size_t>(p * static_cast<double>(lat.size() - 1))]); };
        r.p50 = pct(0.50); r.p99 = pct(0.99); r.p999 = pct(0.999); r.max = static_cast<double>(lat.back());
        r.resting = book.order_count();
    }
    {   // pass 2: throughput with no per-op timers
        auto book = make();
        const auto t0 = Clock::now();
        for (const Op& op : ops) {
            if (op.cancel) book.cancel(op.id);
            else { out.clear(); book.add_limit(op.id, op.side, op.price, op.qty, out); }
        }
        const auto t1 = Clock::now();
        r.mops = static_cast<double>(ops.size()) / std::chrono::duration<double>(t1 - t0).count() / 1e6;
    }
    return r;
}

int main(int argc, char** argv) {
#ifdef __linux__
    if (argc > 1) {
        cpu_set_t set; CPU_ZERO(&set); CPU_SET(std::atoi(argv[1]), &set);
        if (sched_setaffinity(0, sizeof(set), &set) == 0) std::printf("pinned to core %s\n", argv[1]);
        else std::printf("could not pin to core %s\n", argv[1]);
    }
#else
    (void)argc; (void)argv;
#endif
    const int N = 1'000'000;
    const auto ops = generate(N);

    // How much of every number below is just the cost of calling the clock?
    std::vector<std::uint32_t> ov(200000);
    for (auto& x : ov) { auto a = Clock::now(); auto b = Clock::now();
        x = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()); }
    std::sort(ov.begin(), ov.end());

    const Result v1 = run([] { return OrderBook(); }, ops);
    const Result v2 = run([] { return OrderBookV2(9'000, 11'000, 1 << 20); }, ops);

    std::printf("ops: %d   timer overhead (median): %u ns  <- included in every latency below\n\n", N, ov[ov.size() / 2]);
    std::printf("%-12s %10s %10s\n", "", "v1", "v2");
    std::printf("%-12s %10.0f %10.0f   ns\n", "p50",   v1.p50,  v2.p50);
    std::printf("%-12s %10.0f %10.0f   ns\n", "p99",   v1.p99,  v2.p99);
    std::printf("%-12s %10.0f %10.0f   ns\n", "p99.9", v1.p999, v2.p999);
    std::printf("%-12s %10.0f %10.0f   ns (OS jitter; pin a core to reduce)\n", "max", v1.max, v2.max);
    std::printf("%-12s %10.2f %10.2f   Mops/s\n", "throughput", v1.mops, v2.mops);
    std::printf("\nspeedup (p50): %.1fx   (throughput): %.1fx\n", v1.p50 / v2.p50, v2.mops / v1.mops);
    std::printf("trades  v1=%llu v2=%llu   resting v1=%zu v2=%zu   results %s\n",
                (unsigned long long)v1.trades, (unsigned long long)v2.trades, v1.resting, v2.resting,
                (v1.trades == v2.trades && v1.checksum == v2.checksum && v1.resting == v2.resting) ? "IDENTICAL" : "MISMATCH!");
}
