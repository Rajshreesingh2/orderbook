// Unit tests run against BOTH books, then a randomized differential test
// proves v2 produces exactly the same trades/state as the simple v1.
#include "order_book.hpp"
#include "order_book_v2.hpp"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <type_traits>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #c, __FILE__, __LINE__); std::abort(); } } while (0)

template <class B> B make_book() {
    if constexpr (std::is_same_v<B, OrderBookV2>) return OrderBookV2(0, 1000, 4096);
    else return B();
}

struct Res { bool ok; std::vector<Trade> t; };
template <class B> Res add(B& b, OrderId id, Side s, Price p, Qty q) {
    Res r; r.ok = b.add_limit(id, s, p, q, r.t); return r;
}

template <class B> void t_resting_and_best() {
    B b = make_book<B>();
    CHECK(add(b, 1, Side::Buy, 100, 10).t.empty());
    CHECK(add(b, 2, Side::Sell, 105, 5).t.empty());
    CHECK(*b.best_bid() == 100 && *b.best_ask() == 105);
}
template <class B> void t_full_match() {
    B b = make_book<B>();
    add(b, 1, Side::Sell, 100, 10);
    auto r = add(b, 2, Side::Buy, 100, 10);
    CHECK(r.t.size() == 1);
    CHECK(r.t[0].maker_id == 1 && r.t[0].taker_id == 2);
    CHECK(r.t[0].price == 100 && r.t[0].qty == 10);
    CHECK(b.order_count() == 0);
    CHECK(!b.best_ask() && !b.best_bid());
}
template <class B> void t_partial_fill() {
    B b = make_book<B>();
    add(b, 1, Side::Sell, 100, 4);
    auto r = add(b, 2, Side::Buy, 100, 10);
    CHECK(r.t.size() == 1 && r.t[0].qty == 4);
    CHECK(*b.best_bid() == 100 && b.volume_at(Side::Buy, 100) == 6);
    CHECK(!b.best_ask());
}
template <class B> void t_price_priority() {
    B b = make_book<B>();
    add(b, 1, Side::Sell, 102, 5);
    add(b, 2, Side::Sell, 101, 5);
    auto r = add(b, 3, Side::Buy, 102, 5);
    CHECK(r.t.size() == 1 && r.t[0].maker_id == 2 && r.t[0].price == 101);
}
template <class B> void t_time_priority() {
    B b = make_book<B>();
    add(b, 1, Side::Sell, 100, 5);
    add(b, 2, Side::Sell, 100, 5);
    auto r = add(b, 3, Side::Buy, 100, 5);
    CHECK(r.t.size() == 1 && r.t[0].maker_id == 1);
    CHECK(b.volume_at(Side::Sell, 100) == 5);
}
template <class B> void t_buy_sweeps_asks() {
    B b = make_book<B>();
    add(b, 1, Side::Sell, 100, 5);
    add(b, 2, Side::Sell, 101, 5);
    add(b, 3, Side::Sell, 102, 5);
    auto r = add(b, 4, Side::Buy, 101, 8);
    CHECK(r.t.size() == 2);
    CHECK(r.t[0].price == 100 && r.t[0].qty == 5);
    CHECK(r.t[1].price == 101 && r.t[1].qty == 3);
    CHECK(*b.best_ask() == 101 && b.volume_at(Side::Sell, 101) == 2);
}
template <class B> void t_sell_sweeps_bids() {
    B b = make_book<B>();
    add(b, 1, Side::Buy, 100, 5);
    add(b, 2, Side::Buy, 101, 5);
    add(b, 3, Side::Buy, 102, 5);
    auto r = add(b, 4, Side::Sell, 101, 8);
    CHECK(r.t.size() == 2);
    CHECK(r.t[0].price == 102 && r.t[0].qty == 5);
    CHECK(r.t[1].price == 101 && r.t[1].qty == 3);
    CHECK(*b.best_bid() == 101 && b.volume_at(Side::Buy, 101) == 2);
}
template <class B> void t_cancel() {
    B b = make_book<B>();
    add(b, 1, Side::Buy, 100, 5);
    add(b, 2, Side::Buy, 100, 5);
    CHECK(b.cancel(1));
    CHECK(!b.cancel(1));
    CHECK(b.volume_at(Side::Buy, 100) == 5);
    CHECK(b.cancel(2));
    CHECK(!b.best_bid());
    CHECK(b.order_count() == 0);
}
template <class B> void t_cancel_middle_of_level() {
    B b = make_book<B>();
    add(b, 1, Side::Sell, 100, 1);
    add(b, 2, Side::Sell, 100, 2);
    add(b, 3, Side::Sell, 100, 3);
    CHECK(b.cancel(2));
    auto r = add(b, 4, Side::Buy, 100, 4);
    CHECK(r.t.size() == 2 && r.t[0].maker_id == 1 && r.t[1].maker_id == 3 && r.t[1].qty == 3);
    CHECK(b.order_count() == 0);
}
template <class B> void t_cancel_best_advances() {
    B b = make_book<B>();
    add(b, 1, Side::Buy, 100, 1);
    add(b, 2, Side::Buy, 101, 1);
    add(b, 3, Side::Buy, 102, 1);
    add(b, 4, Side::Sell, 110, 1);
    add(b, 5, Side::Sell, 109, 1);
    CHECK(b.cancel(3)); CHECK(*b.best_bid() == 101);
    CHECK(b.cancel(2)); CHECK(*b.best_bid() == 100);
    CHECK(b.cancel(5)); CHECK(*b.best_ask() == 110);
    CHECK(b.cancel(4)); CHECK(!b.best_ask());
}
template <class B> void t_cancel_filled_fails() {
    B b = make_book<B>();
    add(b, 1, Side::Sell, 100, 5);
    add(b, 2, Side::Buy, 100, 5);
    CHECK(!b.cancel(1));
}
template <class B> void t_rejects_bad_input() {
    B b = make_book<B>();
    CHECK(add(b, 1, Side::Buy, 100, 5).ok);
    CHECK(!add(b, 1, Side::Buy, 100, 5).ok);   // duplicate id
    CHECK(!add(b, 2, Side::Buy, 100, 0).ok);   // zero qty
}
template <class B> void run_unit_tests() {
    t_resting_and_best<B>();   t_full_match<B>();         t_partial_fill<B>();
    t_price_priority<B>();     t_time_priority<B>();      t_buy_sweeps_asks<B>();
    t_sell_sweeps_bids<B>();   t_cancel<B>();             t_cancel_middle_of_level<B>();
    t_cancel_best_advances<B>(); t_cancel_filled_fails<B>(); t_rejects_bad_input<B>();
}

// ---- v2-only: limits ------------------------------------------------------
static void test_v2_limits() {
    OrderBookV2 b(100, 110, 4);
    std::vector<Trade> t;
    CHECK(!b.add_limit(1, Side::Buy, 99, 1, t));      // below range
    CHECK(!b.add_limit(1, Side::Buy, 111, 1, t));     // above range
    CHECK(!b.add_limit(0, Side::Buy, 105, 1, t));     // id 0 reserved
    for (OrderId i = 1; i <= 4; ++i) CHECK(b.add_limit(i, Side::Buy, 100 + (Price)i, 1, t));
    CHECK(!b.add_limit(5, Side::Buy, 100, 1, t));     // arena full
    CHECK(b.cancel(2));                               // free one node...
    CHECK(b.add_limit(5, Side::Buy, 100, 1, t));      // ...and it is reusable
    CHECK(b.cancel(5));                               // arena has 3 orders, 1 spare slot
    // churn far beyond capacity: nodes must be recycled, never leaked
    for (OrderId i = 100; i < 100000; ++i) {
        t.clear();
        CHECK(b.add_limit(i, Side::Sell, 110, 1, t));
        CHECK(b.cancel(i));
    }
    CHECK(b.order_count() == 3);
}

// ---- differential test: v1 (simple, obviously correct) vs v2 (fast) ------------
static void differential(std::uint64_t seed, int n_ops, int span) {
    OrderBook a;
    OrderBookV2 b(0, 1000, 1 << 18);
    std::mt19937_64 rng(seed);
    std::vector<Trade> ta, tb;
    OrderId next = 1;
    for (int i = 0; i < n_ops; ++i) {
        const int r = static_cast<int>(rng() % 100);
        if (r < 30 && next > 1) {
            const OrderId id = 1 + rng() % (next - 1);
            CHECK(a.cancel(id) == b.cancel(id));
        } else {
            const Side s = (rng() & 1) ? Side::Buy : Side::Sell;
            const Price p = 500 + static_cast<Price>(rng() % static_cast<std::uint64_t>(span)) - span / 2;
            const Qty q = 1 + static_cast<Qty>(rng() % 50);
            const bool dup = (r >= 95 && next > 1);   // sometimes reuse an id
            const OrderId id = dup ? 1 + rng() % (next - 1) : next++;
            ta.clear(); tb.clear();
            const bool oa = a.add_limit(id, s, p, q, ta);
            const bool ob = b.add_limit(id, s, p, q, tb);
            CHECK(oa == ob);
            CHECK(ta.size() == tb.size());
            for (std::size_t k = 0; k < ta.size(); ++k) {
                CHECK(ta[k].taker_id == tb[k].taker_id && ta[k].maker_id == tb[k].maker_id);
                CHECK(ta[k].price == tb[k].price && ta[k].qty == tb[k].qty);
            }
        }
        CHECK(a.order_count() == b.order_count());
        CHECK(a.best_bid() == b.best_bid());
        CHECK(a.best_ask() == b.best_ask());
    }
    for (Price p = 0; p <= 1000; ++p) {
        CHECK(a.volume_at(Side::Buy, p)  == b.volume_at(Side::Buy, p));
        CHECK(a.volume_at(Side::Sell, p) == b.volume_at(Side::Sell, p));
    }
}

int main() {
    run_unit_tests<OrderBook>();
    std::cout << "v1 unit tests passed\n";
    run_unit_tests<OrderBookV2>();
    std::cout << "v2 unit tests passed\n";
    test_v2_limits();
    std::cout << "v2 limit tests passed\n";
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        differential(seed, 200000, 10);    // tight prices: lots of matching
        differential(seed, 200000, 100);
        differential(seed, 200000, 800);   // wide prices: deep book
    }
    std::cout << "differential tests passed (v1 == v2 on 3.6M random ops)\n";
    std::cout << "All tests passed\n";
}
