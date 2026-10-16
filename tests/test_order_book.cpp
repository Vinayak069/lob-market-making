#include <gtest/gtest.h>

#include <map>
#include <random>

#include "lob/order_book.hpp"

using namespace lob;

TEST(OrderBook, RestsAndReportsBestPrices) {
    OrderBook b;
    std::vector<Fill> f;
    b.add_limit(1, Side::Buy, 100, 5, f);
    b.add_limit(2, Side::Buy, 101, 3, f);
    b.add_limit(3, Side::Sell, 105, 4, f);
    EXPECT_TRUE(f.empty());
    EXPECT_EQ(*b.best_bid(), 101);
    EXPECT_EQ(*b.best_ask(), 105);
    EXPECT_EQ(b.depth_at(Side::Buy, 100), 5);
    EXPECT_EQ(b.order_count(), 3u);
}

TEST(OrderBook, TimePriorityWithinALevel) {
    OrderBook b;
    std::vector<Fill> f;
    b.add_limit(1, Side::Buy, 100, 2, f);
    b.add_limit(2, Side::Buy, 100, 2, f);
    b.add_market(9, Side::Sell, 3, f);
    ASSERT_EQ(f.size(), 2u);
    EXPECT_EQ(f[0].maker, 1u);
    EXPECT_EQ(f[0].qty, 2);
    EXPECT_EQ(f[1].maker, 2u);
    EXPECT_EQ(f[1].qty, 1);
    EXPECT_EQ(b.depth_at(Side::Buy, 100), 1);
}

TEST(OrderBook, AggressiveLimitSweepsLevelsThenRests) {
    OrderBook b;
    std::vector<Fill> f;
    b.add_limit(1, Side::Sell, 101, 2, f);
    b.add_limit(2, Side::Sell, 102, 2, f);
    b.add_limit(3, Side::Sell, 104, 2, f);
    Qty filled = b.add_limit(4, Side::Buy, 102, 5, f);
    EXPECT_EQ(filled, 4);
    ASSERT_EQ(f.size(), 2u);
    EXPECT_EQ(f[0].price, 101);
    EXPECT_EQ(f[1].price, 102);
    EXPECT_EQ(*b.best_bid(), 102);  // remainder of 1 rests at its limit
    EXPECT_EQ(b.depth_at(Side::Buy, 102), 1);
    EXPECT_EQ(*b.best_ask(), 104);
}

TEST(OrderBook, CancelAndReduce) {
    OrderBook b;
    std::vector<Fill> f;
    b.add_limit(1, Side::Sell, 101, 5, f);
    b.add_limit(2, Side::Sell, 101, 5, f);
    EXPECT_EQ(*b.queue_ahead(2), 5);
    EXPECT_TRUE(b.reduce(1, 2));
    EXPECT_EQ(*b.queue_ahead(2), 2);  // reduced order keeps its place ahead
    EXPECT_FALSE(b.reduce(1, 4));     // size increases are refused
    EXPECT_TRUE(b.cancel(1));
    EXPECT_FALSE(b.cancel(1));
    EXPECT_EQ(*b.queue_ahead(2), 0);
    EXPECT_TRUE(b.cancel(2));
    EXPECT_FALSE(b.best_ask().has_value());
    EXPECT_EQ(b.level_count(Side::Sell), 0u);
}

TEST(OrderBook, RejectsBadInput) {
    OrderBook b;
    std::vector<Fill> f;
    EXPECT_THROW(b.add_limit(1, Side::Buy, 100, 0, f), std::invalid_argument);
    b.add_limit(1, Side::Buy, 100, 1, f);
    EXPECT_THROW(b.add_limit(1, Side::Buy, 99, 1, f), std::invalid_argument);
}

// Brute-force reference: every order kept in a vector, matched by scanning.
struct Reference {
    struct O { OrderId id; Side side; Price px; Qty q; std::uint64_t seq; };
    std::vector<O> book;
    std::uint64_t seq = 0;
    Qty limit(OrderId id, Side s, Price px, Qty q, bool market) {
        Qty filled = 0;
        while (q > 0) {
            int best = -1;
            for (int i = 0; i < static_cast<int>(book.size()); ++i) {
                const O& o = book[i];
                if (o.side == s) continue;
                bool ok = market || (s == Side::Buy ? o.px <= px : o.px >= px);
                if (!ok) continue;
                if (best < 0) { best = i; continue; }
                const O& c = book[best];
                bool better = s == Side::Buy ? o.px < c.px : o.px > c.px;
                if (better || (o.px == c.px && o.seq < c.seq)) best = i;
            }
            if (best < 0) break;
            Qty x = std::min(q, book[best].q);
            q -= x; filled += x; book[best].q -= x;
            if (book[best].q == 0) book.erase(book.begin() + best);
        }
        if (q > 0 && !market) book.push_back({id, s, px, q, seq++});
        return filled;
    }
    bool cancel(OrderId id) {
        for (auto it = book.begin(); it != book.end(); ++it)
            if (it->id == id) { book.erase(it); return true; }
        return false;
    }
    std::optional<Price> best(Side s) const {
        std::optional<Price> p;
        for (const O& o : book)
            if (o.side == s && (!p || (s == Side::Buy ? o.px > *p : o.px < *p))) p = o.px;
        return p;
    }
};

TEST(OrderBook, MatchesBruteForceReferenceOnRandomFlow) {
    std::mt19937_64 rng(42);
    OrderBook b(64);  // small chunks so the pool grows during the test
    Reference ref;
    std::vector<Fill> f;
    std::vector<OrderId> ids;
    for (OrderId id = 1; id <= 20000; ++id) {
        int op = rng() % 10;
        Side s = rng() % 2 ? Side::Buy : Side::Sell;
        Price px = 1000 + static_cast<Price>(rng() % 21) - 10;
        Qty q = 1 + static_cast<Qty>(rng() % 9);
        if (op < 6) {
            ASSERT_EQ(b.add_limit(id, s, px, q, f), ref.limit(id, s, px, q, false));
            ids.push_back(id);
        } else if (op < 8 && !ids.empty()) {
            OrderId victim = ids[rng() % ids.size()];
            ASSERT_EQ(b.cancel(victim), ref.cancel(victim));
        } else {
            ASSERT_EQ(b.add_market(id, s, q, f), ref.limit(id, s, 0, q, true));
        }
        ASSERT_EQ(b.best_bid(), ref.best(Side::Buy));
        ASSERT_EQ(b.best_ask(), ref.best(Side::Sell));
        if (b.best_bid() && b.best_ask()) {
            ASSERT_LT(*b.best_bid(), *b.best_ask());
        }
        ASSERT_EQ(b.order_count(), ref.book.size());
    }
}
