#pragma once
// Shared synthetic order flow used by the pipeline test and benchmark.
#include "order_book.hpp"
#include <cstdint>
#include <random>
#include <vector>

struct Op {
    OrderId      id;
    Price        price;
    Qty          qty;
    Side         side;
    std::uint8_t kind;   // 0 = add limit, 1 = cancel, 2 = stop (end of stream)
};

inline std::vector<Op> generate_ops(int n, std::uint64_t seed = 42) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<Price> price(9'900, 10'100);
    std::uniform_int_distribution<Qty> qty(1, 100);
    std::vector<Op> ops;
    ops.reserve(static_cast<std::size_t>(n));
    OrderId next = 1;
    for (int i = 0; i < n; ++i) {
        if (rng() % 100 < 25 && next > 1)
            ops.push_back({1 + rng() % (next - 1), 0, 0, Side::Buy, 1});
        else
            ops.push_back({next++, price(rng), qty(rng), (rng() & 1) ? Side::Buy : Side::Sell, 0});
    }
    return ops;
}

struct Digest {
    std::uint64_t trades = 0, checksum = 0;
    std::size_t   resting = 0;
    bool operator==(const Digest& o) const {
        return trades == o.trades && checksum == o.checksum && resting == o.resting;
    }
};

template <class Book>
inline void apply(Book& book, const Op& op, std::vector<Trade>& out, Digest& d) {
    if (op.kind == 1) { book.cancel(op.id); return; }
    out.clear();
    book.add_limit(op.id, op.side, op.price, op.qty, out);
    d.trades += out.size();
    for (const Trade& t : out)
        d.checksum += static_cast<std::uint64_t>(t.price) * t.qty + t.maker_id * 31 + t.taker_id;
}
