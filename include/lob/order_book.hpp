#pragma once
// Price-time priority limit order book.
//
// Prices are integer ticks. Each price level keeps its orders in an intrusive
// doubly linked list, so add-to-tail, fill-from-head and cancel-by-id are all O(1)
// once the level is found (O(log L) in the number of levels). Orders are drawn from
// a pool to avoid an allocation per order on the hot path.
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lob {

using Price = std::int64_t;
using Qty = std::int64_t;
using OrderId = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

struct Fill {
    OrderId maker;
    OrderId taker;
    Price price;
    Qty qty;
};

class OrderBook {
public:
    explicit OrderBook(std::size_t expected_orders = 1 << 16);
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    // Matches against the opposite side up to `price`, then rests any remainder.
    // Returns the quantity filled immediately. Fills are appended to `fills`.
    Qty add_limit(OrderId id, Side side, Price price, Qty qty, std::vector<Fill>& fills);

    // Matches against the opposite side at any price; an unfilled remainder is dropped.
    Qty add_market(OrderId id, Side side, Qty qty, std::vector<Fill>& fills);

    bool cancel(OrderId id);

    // Reduces a resting order's quantity in place, keeping its queue position.
    bool reduce(OrderId id, Qty new_qty);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    Qty depth_at(Side side, Price price) const;
    // Quantity resting ahead of `id` at its price level.
    std::optional<Qty> queue_ahead(OrderId id) const;
    std::size_t order_count() const { return index_.size(); }
    std::size_t level_count(Side side) const { return side == Side::Buy ? bids_.size() : asks_.size(); }

private:
    struct Order {
        OrderId id;
        Price price;
        Qty qty;
        Side side;
        Order* prev;
        Order* next;
    };
    struct Level {
        Qty total = 0;
        Order* head = nullptr;
        Order* tail = nullptr;
    };

    template <class Book, class Crosses>
    Qty match(Book& book, OrderId taker, Qty qty, Crosses crosses, std::vector<Fill>& fills);

    Order* alloc();
    void release(Order* o);
    void unlink(Level& level, Order* o);

    std::map<Price, Level, std::greater<Price>> bids_;
    std::map<Price, Level> asks_;
    std::unordered_map<OrderId, Order*> index_;
    std::vector<std::unique_ptr<Order[]>> chunks_;
    std::vector<Order*> free_;
    std::size_t chunk_size_;
};

}  // namespace lob
