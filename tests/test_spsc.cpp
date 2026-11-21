// Correctness tests for the three queues, plus an end-to-end pipeline test:
//   producer thread -> SPSC queue -> consumer thread -> OrderBookV2
// must give exactly the same result as running the same orders on one thread.
// Build with ASan+UBSan (make test_spsc) AND with ThreadSanitizer (make tsan).
#include "order_book_v2.hpp"
#include "spsc_queue.hpp"
#include "workload.hpp"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #c, __FILE__, __LINE__); std::abort(); } } while (0)

#if defined(__SANITIZE_THREAD__)
constexpr std::uint64_t kN = 100'000;      // TSan is ~10x slower
constexpr int kPipelineOps = 100'000;
#else
constexpr std::uint64_t kN = 1'000'000;
constexpr int kPipelineOps = 400'000;
#endif

// A multi-word payload: if a consumer ever saw a half-written element, the fields
// would disagree with each other and valid() would fail.
struct Payload { std::uint64_t a, b, c, d; };
static Payload make_payload(std::uint64_t i) { return {i, i * 3 + 1, ~i, i ^ 0x5555ull}; }
static bool valid(const Payload& p, std::uint64_t i) {
    return p.a == i && p.b == i * 3 + 1 && p.c == ~i && p.d == (i ^ 0x5555ull);
}

template <template <class, std::size_t> class Q>
static void single_thread() {
    auto qp = std::make_unique<Q<Payload, 8>>();
    auto& q = *qp;
    Payload p{};
    CHECK(!q.try_pop(p));                                      // empty
    for (std::uint64_t i = 0; i < 8; ++i) CHECK(q.try_push(make_payload(i)));
    CHECK(!q.try_push(make_payload(99)));                      // full
    for (std::uint64_t i = 0; i < 8; ++i) { CHECK(q.try_pop(p)); CHECK(valid(p, i)); }
    CHECK(!q.try_pop(p));                                      // empty again
    std::uint64_t pushed = 0, popped = 0;                      // wrap the indices many times
    for (int round = 0; round < 5000; ++round) {
        const int burst = 1 + round % 8;
        for (int k = 0; k < burst; ++k) CHECK(q.try_push(make_payload(pushed++)));
        for (int k = 0; k < burst; ++k) { CHECK(q.try_pop(p)); CHECK(valid(p, popped++)); }
    }
}

template <template <class, std::size_t> class Q>
static void two_threads() {
    auto q = std::make_unique<Q<Payload, 1024>>();             // small ring: forces full/empty races
    std::thread producer([&] { for (std::uint64_t i = 0; i < kN; ++i) push_blocking(*q, make_payload(i)); });
    Payload p{};
    for (std::uint64_t i = 0; i < kN; ++i) { pop_blocking(*q, p); CHECK(valid(p, i)); }   // in order, untorn
    producer.join();
    CHECK(!q->try_pop(p));
}

static Digest run_single_thread(const std::vector<Op>& ops) {
    OrderBookV2 book(9'000, 11'000, 1 << 20);
    std::vector<Trade> out; out.reserve(1024);
    Digest d;
    for (const Op& op : ops) apply(book, op, out, d);
    d.resting = book.order_count();
    return d;
}

static Digest run_pipeline(const std::vector<Op>& ops) {
    auto q = std::make_unique<SpscQueue<Op, 1 << 12>>();
    std::thread producer([&] {
        for (const Op& op : ops) push_blocking(*q, op);
        push_blocking(*q, Op{0, 0, 0, Side::Buy, 2});          // stop marker
    });
    OrderBookV2 book(9'000, 11'000, 1 << 20);
    std::vector<Trade> out; out.reserve(1024);
    Digest d;
    Op op{};
    for (;;) { pop_blocking(*q, op); if (op.kind == 2) break; apply(book, op, out, d); }
    producer.join();
    d.resting = book.order_count();
    return d;
}

int main() {
    single_thread<MutexQueue>();     std::cout << "MutexQueue      single-thread ok\n";
    single_thread<SpscQueueNaive>(); std::cout << "SpscQueueNaive  single-thread ok\n";
    single_thread<SpscQueue>();      std::cout << "SpscQueue       single-thread ok\n";

    two_threads<MutexQueue>();       std::cout << "MutexQueue      two-thread ok (" << kN << " messages, in order, untorn)\n";
    two_threads<SpscQueueNaive>();   std::cout << "SpscQueueNaive  two-thread ok\n";
    two_threads<SpscQueue>();        std::cout << "SpscQueue       two-thread ok\n";

    const auto ops = generate_ops(kPipelineOps);
    const Digest ref = run_single_thread(ops);
    for (int rep = 0; rep < 3; ++rep) CHECK(run_pipeline(ops) == ref);
    std::cout << "pipeline (producer -> SPSC -> order book) identical to single-thread on "
              << kPipelineOps << " ops x3 runs\n";
    std::cout << "All SPSC tests passed\n";
}
