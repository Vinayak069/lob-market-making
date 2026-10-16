#include "lob/order_book.hpp"

#include <stdexcept>

namespace lob {

OrderBook::OrderBook(std::size_t expected_orders) : chunk_size_(expected_orders) {
    index_.reserve(expected_orders);
}

OrderBook::Order* OrderBook::alloc() {
    if (free_.empty()) {
        chunks_.emplace_back(new Order[chunk_size_]);
        Order* base = chunks_.back().get();
        free_.reserve(free_.size() + chunk_size_);
        for (std::size_t i = chunk_size_; i-- > 0;) free_.push_back(base + i);
    }
    Order* o = free_.back();
    free_.pop_back();
    return o;
}

void OrderBook::release(Order* o) { free_.push_back(o); }

void OrderBook::unlink(Level& level, Order* o) {
    (o->prev ? o->prev->next : level.head) = o->next;
    (o->next ? o->next->prev : level.tail) = o->prev;
    level.total -= o->qty;
}

template <class Book, class Crosses>
Qty OrderBook::match(Book& book, OrderId taker, Qty qty, Crosses crosses, std::vector<Fill>& fills) {
    Qty filled = 0;
    while (qty > 0 && !book.empty()) {
        auto it = book.begin();
        if (!crosses(it->first)) break;
        Level& level = it->second;
        while (qty > 0 && level.head) {
            Order* maker = level.head;
            Qty q = std::min(qty, maker->qty);
            fills.push_back({maker->id, taker, it->first, q});
            qty -= q;
            filled += q;
            maker->qty -= q;
            level.total -= q;
            if (maker->qty == 0) {
                level.head = maker->next;
                if (level.head) level.head->prev = nullptr; else level.tail = nullptr;
                index_.erase(maker->id);
                release(maker);
            }
        }
        if (!level.head) book.erase(it);
    }
    return filled;
}

Qty OrderBook::add_limit(OrderId id, Side side, Price price, Qty qty, std::vector<Fill>& fills) {
    if (qty <= 0) throw std::invalid_argument("quantity must be positive");
    if (index_.count(id)) throw std::invalid_argument("duplicate order id");
    Qty filled = side == Side::Buy
        ? match(asks_, id, qty, [price](Price p) { return p <= price; }, fills)
        : match(bids_, id, qty, [price](Price p) { return p >= price; }, fills);
    Qty rest = qty - filled;
    if (rest > 0) {
        Order* o = alloc();
        *o = Order{id, price, rest, side, nullptr, nullptr};
        Level& level = side == Side::Buy ? bids_[price] : asks_[price];
        o->prev = level.tail;
        (level.tail ? level.tail->next : level.head) = o;
        level.tail = o;
        level.total += rest;
        index_.emplace(id, o);
    }
    return filled;
}

Qty OrderBook::add_market(OrderId id, Side side, Qty qty, std::vector<Fill>& fills) {
    if (qty <= 0) throw std::invalid_argument("quantity must be positive");
    auto any = [](Price) { return true; };
    return side == Side::Buy ? match(asks_, id, qty, any, fills) : match(bids_, id, qty, any, fills);
}

bool OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    Order* o = it->second;
    auto erase_from = [&](auto& book) {
        auto lv = book.find(o->price);
        unlink(lv->second, o);
        if (!lv->second.head) book.erase(lv);
    };
    if (o->side == Side::Buy) erase_from(bids_); else erase_from(asks_);
    index_.erase(it);
    release(o);
    return true;
}

bool OrderBook::reduce(OrderId id, Qty new_qty) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    Order* o = it->second;
    if (new_qty <= 0) return cancel(id);
    if (new_qty >= o->qty) return false;  // increasing size would have to lose priority
    Level& level = o->side == Side::Buy ? bids_.find(o->price)->second : asks_.find(o->price)->second;
    level.total -= o->qty - new_qty;
    o->qty = new_qty;
    return true;
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

Qty OrderBook::depth_at(Side side, Price price) const {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second.total;
    }
    auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second.total;
}

std::optional<Qty> OrderBook::queue_ahead(OrderId id) const {
    auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    Qty ahead = 0;
    for (const Order* o = it->second->prev; o; o = o->prev) ahead += o->qty;
    return ahead;
}

}  // namespace lob
