#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>

#include "mm/market_data.hpp"

using namespace mm;

TEST(MarketData, ParsesBookTicker) {
    auto q = parse_book_ticker("4240934975210,191.90700000,21.00000000,191.90800000,26.00000000,1710979200000,1710979200008", 0.001);
    ASSERT_TRUE(q);
    EXPECT_EQ(q->bid, 191907);
    EXPECT_EQ(q->ask, 191908);
    EXPECT_DOUBLE_EQ(q->bid_qty, 21.0);
    EXPECT_EQ(q->ts_ms, 1710979200000);
    EXPECT_FALSE(parse_book_ticker("update_id,best_bid_price,best_bid_qty,best_ask_price,best_ask_qty,transaction_time,event_time", 0.001));
}

TEST(MarketData, ParsesAggTrade) {
    auto t = parse_agg_trade("1,191.908,3.5,10,11,1710979200005,true", 0.001);
    ASSERT_TRUE(t);
    EXPECT_EQ(t->price, 191908);
    EXPECT_TRUE(t->buyer_is_maker);
    EXPECT_FALSE(parse_agg_trade("1,191.908,3.5,10,11,1710979200005,maybe", 0.001));
}

TEST(MarketData, MergesStreamsByTimeWithTradesFirstOnTies) {
    const std::string bp = ::testing::TempDir() + "book.csv", tp = ::testing::TempDir() + "trades.csv";
    std::ofstream(bp) << "update_id,best_bid_price,best_bid_qty,best_ask_price,best_ask_qty,transaction_time,event_time\n"
                         "1,1.000,1,1.001,1,100,100\n2,1.000,1,1.001,2,200,200\n";
    std::ofstream(tp) << "agg_trade_id,price,quantity,first_trade_id,last_trade_id,transact_time,is_buyer_maker\n"
                         "1,1.001,1,1,1,150,false\n2,1.000,1,2,2,200,true\n";
    EventStream s(bp, tp, 0.001);
    Event e;
    std::vector<std::pair<std::int64_t, bool>> seen;  // (ts, is_trade)
    while (s.next(e)) seen.emplace_back(e.ts(), e.kind == Event::Kind::Trade);
    std::vector<std::pair<std::int64_t, bool>> want{{100, false}, {150, true}, {200, true}, {200, false}};
    EXPECT_EQ(seen, want);
    std::remove(bp.c_str());
    std::remove(tp.c_str());
}
