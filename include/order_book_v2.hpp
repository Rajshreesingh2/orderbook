#pragma once
// OrderBookV2: same behavior as OrderBook (v1), built for speed.
//
//   v1                              v2
//   std::list node per order   ->   preallocated node arena + free list (no heap alloc per order)
//   std::map price levels      ->   flat array indexed by (price - min_price)
//   unordered_map id index     ->   open-addressing hash table, fixed capacity
//   vector<Trade> per call     ->   caller-provided, reusable output vector
//   walk map to find best      ->   cached best_bid_/best_ask_ level indices
//
// Limits (the price of speed):
//   * prices must lie in [min_price, max_price]; others are rejected
//   * at most max_orders resting orders; when the arena is full, add_limit rejects
//     (even an order that would have matched fully - conservative on purpose)
//   * order id 0 is reserved (used as the "empty" marker in the hash table)
//   * caller should reserve() the trades vector once, so push_back never allocates
#include "order_book.hpp"   // shared types: OrderId, Price, Qty, Side, Trade
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

class OrderBookV2 {
    static constexpr std::uint32_t NIL  = 0xFFFFFFFFu;
    static constexpr std::size_t   NPOS = static_cast<std::size_t>(-1);

    struct Node {                 // 32 bytes: two nodes per cache line
        OrderId       id;
        Qty           qty;
        std::uint32_t prev;       // doubly linked list inside a price level
        std::uint32_t next;       // (also the free-list link when the node is unused)
        std::int32_t  level;
        Side          side;
    };

    struct Level {                // FIFO queue of nodes at one price
        std::uint32_t head  = NIL;
        std::uint32_t tail  = NIL;
        Qty           total = 0;
    };

public:
    OrderBookV2(Price min_price, Price max_price, std::size_t max_orders)
        : min_price_(min_price),
          n_levels_(static_cast<std::int32_t>(max_price - min_price + 1)),
          bids_(static_cast<std::size_t>(n_levels_)),
          asks_(static_cast<std::size_t>(n_levels_)),
          nodes_(max_orders),
          best_bid_(-1),
          best_ask_(n_levels_) {
        std::size_t cap = 16;
        while (cap < max_orders * 2) cap <<= 1;   // load factor <= 0.5
        mask_ = cap - 1;
        keys_.assign(cap, 0);
        vals_.assign(cap, 0);
        for (std::size_t i = 0; i < max_orders; ++i)
            nodes_[i].next = (i + 1 < max_orders) ? static_cast<std::uint32_t>(i + 1) : NIL;
        free_head_ = max_orders ? 0 : NIL;
    }

    // Trades are appended to `trades` (caller clears it between calls).
    bool add_limit(OrderId id, Side side, Price price, Qty qty, std::vector<Trade>& trades) {
        if (qty == 0 || id == 0) return false;
        if (price < min_price_ || price >= min_price_ + n_levels_) return false;
        if (free_head_ == NIL || find_slot(id) != NPOS) return false;

        const std::int32_t lvl = static_cast<std::int32_t>(price - min_price_);
        qty = (side == Side::Buy) ? match<true>(id, lvl, qty, trades)
                                  : match<false>(id, lvl, qty, trades);
        if (qty > 0) rest(id, side, lvl, qty);
        return true;
    }

    bool cancel(OrderId id) {
        if (id == 0) return false;
        const std::size_t slot = find_slot(id);
        if (slot == NPOS) return false;

        const std::uint32_t n = vals_[slot];
        Node& nd = nodes_[n];
        const bool is_buy = (nd.side == Side::Buy);
        Level& L = (is_buy ? bids_ : asks_)[static_cast<std::size_t>(nd.level)];

        if (nd.prev != NIL) nodes_[nd.prev].next = nd.next; else L.head = nd.next;
        if (nd.next != NIL) nodes_[nd.next].prev = nd.prev; else L.tail = nd.prev;
        L.total -= nd.qty;

        if (L.head == NIL) {
            if (is_buy  && best_bid_ == nd.level) advance_bid();
            if (!is_buy && best_ask_ == nd.level) advance_ask();
        }
        erase_slot(slot);
        free_node(n);
        --count_;
        return true;
    }

    std::optional<Price> best_bid() const {
        return best_bid_ >= 0 ? std::optional<Price>(min_price_ + best_bid_) : std::nullopt;
    }
    std::optional<Price> best_ask() const {
        return best_ask_ < n_levels_ ? std::optional<Price>(min_price_ + best_ask_) : std::nullopt;
    }
    Qty volume_at(Side side, Price price) const {
        if (price < min_price_ || price >= min_price_ + n_levels_) return 0;
        const auto i = static_cast<std::size_t>(price - min_price_);
        return (side == Side::Buy ? bids_ : asks_)[i].total;
    }
    std::size_t order_count() const { return count_; }

private:
    Price                     min_price_;
    std::int32_t              n_levels_;
    std::vector<Level>        bids_, asks_;   // indexed by price - min_price_
    std::vector<Node>         nodes_;         // the arena
    std::int32_t              best_bid_;      // highest non-empty bid level, -1 if none
    std::int32_t              best_ask_;      // lowest non-empty ask level, n_levels_ if none
    std::vector<OrderId>      keys_;          // hash table: id -> node index (0 = empty slot)
    std::vector<std::uint32_t> vals_;
    std::size_t               mask_ = 0;
    std::uint32_t             free_head_ = NIL;
    std::size_t               count_ = 0;

    // ---- matching -------------------------------------------------------
    template <bool IsBuy>
    Qty match(OrderId taker, std::int32_t limit, Qty qty, std::vector<Trade>& trades) {
        std::vector<Level>& book = IsBuy ? asks_ : bids_;
        std::int32_t& best       = IsBuy ? best_ask_ : best_bid_;

        while (qty > 0 && (IsBuy ? (best < n_levels_ && best <= limit)
                                 : (best >= 0 && best >= limit))) {
            Level& L = book[static_cast<std::size_t>(best)];
            while (qty > 0 && L.head != NIL) {
                Node& m = nodes_[L.head];
                const Qty fill = std::min(qty, m.qty);
                trades.push_back({taker, m.id, min_price_ + best, fill});
                qty   -= fill;
                m.qty -= fill;
                L.total -= fill;
                if (m.qty == 0) {
                    const std::uint32_t dead = L.head;
                    L.head = m.next;
                    if (L.head == NIL) L.tail = NIL; else nodes_[L.head].prev = NIL;
                    erase_slot(find_slot(m.id));
                    free_node(dead);
                    --count_;
                }
            }
            if (L.head == NIL) {
                if constexpr (IsBuy) advance_ask(); else advance_bid();
            }
        }
        return qty;
    }

    void rest(OrderId id, Side side, std::int32_t lvl, Qty qty) {
        const std::uint32_t n = alloc_node();
        nodes_[n] = Node{id, qty, NIL, NIL, lvl, side};
        Level& L = (side == Side::Buy ? bids_ : asks_)[static_cast<std::size_t>(lvl)];
        nodes_[n].prev = L.tail;
        if (L.tail != NIL) nodes_[L.tail].next = n; else L.head = n;
        L.tail = n;
        L.total += qty;
        insert_slot(id, n);
        ++count_;
        if (side == Side::Buy) { if (lvl > best_bid_) best_bid_ = lvl; }
        else                   { if (lvl < best_ask_) best_ask_ = lvl; }
    }

    // Move the cached best pointer to the next non-empty level.
    void advance_ask() {
        do { ++best_ask_; } while (best_ask_ < n_levels_ && asks_[static_cast<std::size_t>(best_ask_)].head == NIL);
    }
    void advance_bid() {
        do { --best_bid_; } while (best_bid_ >= 0 && bids_[static_cast<std::size_t>(best_bid_)].head == NIL);
    }

    // ---- arena ----------------------------------------------------------
    std::uint32_t alloc_node() {
        const std::uint32_t n = free_head_;
        free_head_ = nodes_[n].next;
        return n;
    }
    void free_node(std::uint32_t n) {
        nodes_[n].next = free_head_;
        free_head_ = n;
    }

    // ---- open-addressing hash table (linear probing, backward-shift delete) ----
    static std::size_t hash(OrderId id) {
        return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ull) >> 32);
    }
    std::size_t find_slot(OrderId id) const {
        std::size_t i = hash(id) & mask_;
        while (keys_[i] != 0) {
            if (keys_[i] == id) return i;
            i = (i + 1) & mask_;
        }
        return NPOS;
    }
    void insert_slot(OrderId id, std::uint32_t node) {
        std::size_t i = hash(id) & mask_;
        while (keys_[i] != 0) i = (i + 1) & mask_;
        keys_[i] = id;
        vals_[i] = node;
    }
    void erase_slot(std::size_t i) {
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (keys_[j] == 0) break;
            const std::size_t k = hash(keys_[j]) & mask_;   // where keys_[j] wants to live
            const bool stays = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
            if (stays) continue;
            keys_[i] = keys_[j];
            vals_[i] = vals_[j];
            i = j;
        }
        keys_[i] = 0;
    }
};
