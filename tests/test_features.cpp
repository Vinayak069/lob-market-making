#include <gtest/gtest.h>

#include "mm/features.hpp"

using namespace mm;

TEST(Features, OfiFollowsContKukanovStoikov) {
    Quote a{0, 100, 101, 10, 10};
    Quote bid_up{0, 101, 102, 4, 10};  // bid improves, ask also moves up
    EXPECT_DOUBLE_EQ(ofi_increment(a, bid_up), 4 + 10);
    Quote bid_grows{0, 100, 101, 15, 10};
    EXPECT_DOUBLE_EQ(ofi_increment(a, bid_grows), 5);
    Quote ask_grows{0, 100, 101, 10, 13};
    EXPECT_DOUBLE_EQ(ofi_increment(a, ask_grows), -3);
    Quote both_down{0, 99, 100, 7, 6};
    EXPECT_DOUBLE_EQ(ofi_increment(a, both_down), -10 - 6);
}

TEST(Features, BucketsRollAndCountSkippedQuietTime) {
    FeatureTracker t(1000, 5);
    EXPECT_FALSE(t.on_quote({100, 100, 101, 10, 10}));
    EXPECT_FALSE(t.on_trade({200, 101, 2, false}));
    EXPECT_FALSE(t.on_quote({300, 100, 101, 12, 10}));
    auto done = t.on_quote({3500, 100, 101, 12, 10});  // 2 quiet buckets skipped
    ASSERT_TRUE(done);
    EXPECT_EQ(done->end_ms, 1000);
    EXPECT_DOUBLE_EQ(done->ofi, 2);
    EXPECT_DOUBLE_EQ(done->trade_imbalance, 2);
    EXPECT_EQ(t.last_skipped(), 2);
    EXPECT_FALSE(t.ready());  // 3 of 5 buckets of history so far
    t.on_quote({5100, 100, 101, 12, 10});
    EXPECT_TRUE(t.ready());
    FeatureVector f = t.features();
    EXPECT_DOUBLE_EQ(f.ofi_5, 2);
    EXPECT_DOUBLE_EQ(f.ti_5, 2);
    EXPECT_NEAR(f.book_imb, 2.0 / 22.0, 1e-12);
}
