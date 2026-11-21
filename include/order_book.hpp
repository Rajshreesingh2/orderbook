#pragma once
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>
#include <algorithm>

using OrderId = std::uint64_t;
using Price   = std::int64_t;   // price in integer ticks (never use float for prices)
using Qty     = std::uint32_t;

enum class Side { Buy, Sell };

struct Order {
    OrderId id;
    Side    side;
    Price   price;
    Qty     qty;   // remaining quantity
};

struct Trade {
    OrderId taker_id;  // incoming order
    OrderId maker_id;  // resting order
    Price   price;     // trade happens at the resting order's price
    Qty     qty;
};

struct AddResult {
    bool               accepted;  // false on duplicate id or zero qty
    std::vector<Trade> trades;
};

class OrderBook {
public:
    // Match against the opposite side first, rest any remainder on the book.
    AddResult add_limit(OrderId id, Side side, Price price, Qty qty) {
        if (qty == 0 || index_.count(id)) return {false, {}};

        AddResult res{true, {}};
        Order o{id, side, price, qty};

        if (side == Side::Buy)
            match(asks_, o, [](Price best, Price limit) { return best <= limit; }, res.trades);
        else
            match(bids_, o, [](Price best, Price limit) { return best >= limit; }, res.trades);

        if (o.qty > 0) rest(o);
        return res;
    }

    // Same interface as OrderBookV2: trades are appended to a caller-provided vector.
    bool add_limit(OrderId id, Side side, Price price, Qty qty, std::vector<Trade>& out) {
        AddResult r = add_limit(id, side, price, qty);
        out.insert(out.end(), r.trades.begin(), r.trades.end());
        return r.accepted;
    }

    // Cancel a resting order. Returns false if the id is unknown (or already filled).
    bool cancel(OrderId id) {
        auto it = index_.find(id);
        if (it == index_.end()) return false;
        const Locator loc = it->second;
        if (loc.side == Side::Buy) erase_from(bids_, loc);
        else                       erase_from(asks_, loc);
        index_.erase(it);
        return true;
    }

    std::optional<Price> best_bid() const {
        return bids_.empty() ? std::nullopt : std::optional<Price>(bids_.begin()->first);
    }
    std::optional<Price> best_ask() const {
        return asks_.empty() ? std::nullopt : std::optional<Price>(asks_.begin()->first);
    }
    Qty volume_at(Side side, Price price) const {
        return side == Side::Buy ? level_volume(bids_, price) : level_volume(asks_, price);
    }
    std::size_t order_count() const { return index_.size(); }

private:
    using Level = std::list<Order>;  // FIFO queue = time priority within a price
    using Bids  = std::map<Price, Level, std::greater<Price>>;  // best (highest) first
    using Asks  = std::map<Price, Level>;                       // best (lowest) first

    struct Locator { Side side; Price price; Level::iterator it; };

    Bids bids_;
    Asks asks_;
    std::unordered_map<OrderId, Locator> index_;  // id -> location, for O(1) cancel lookup

    template <class Book, class Crosses>
    void match(Book& book, Order& o, Crosses crosses, std::vector<Trade>& trades) {
        while (o.qty > 0 && !book.empty()) {
            auto lvl = book.begin();
            if (!crosses(lvl->first, o.price)) break;
            Level& level = lvl->second;
            while (o.qty > 0 && !level.empty()) {
                Order& maker = level.front();
                Qty fill = std::min(o.qty, maker.qty);
                trades.push_back({o.id, maker.id, maker.price, fill});
                o.qty -= fill;
                maker.qty -= fill;
                if (maker.qty == 0) {
                    index_.erase(maker.id);
                    level.pop_front();
                }
            }
            if (level.empty()) book.erase(lvl);
        }
    }

    void rest(const Order& o) {
        auto& level = (o.side == Side::Buy) ? bids_[o.price] : asks_[o.price];
        level.push_back(o);
        index_[o.id] = {o.side, o.price, std::prev(level.end())};
    }

    template <class Book>
    void erase_from(Book& book, const Locator& loc) {
        auto lvl = book.find(loc.price);
        lvl->second.erase(loc.it);
        if (lvl->second.empty()) book.erase(lvl);
    }

    template <class Book>
    static Qty level_volume(const Book& book, Price price) {
        auto it = book.find(price);
        if (it == book.end()) return 0;
        Qty total = 0;
        for (const auto& o : it->second) total += o.qty;
        return total;
    }
};
