#include <gtest/gtest.h>

#include <cmath>

#include "mm/simulator.hpp"

using namespace mm;

namespace {

Event Q(std::int64_t ts, std::int64_t bid, std::int64_t ask, double bq = 10, double aq = 10) {
    Event e{};
    e.kind = Event::Kind::Quote;
    e.quote = Quote{ts, bid, ask, bq, aq};
    return e;
}

Event T(std::int64_t ts, std::int64_t px, double qty, bool seller_aggressor) {
    Event e{};
    e.kind = Event::Kind::Trade;
    e.trade = Trade{ts, px, qty, seller_aggressor};
    return e;
}

SimConfig base() {
    SimConfig c;
    c.tick = 1.0;
    c.order_qty = 1;
    c.latency_ms = 10;
    c.min_requote_ms = 0;
    c.maker_fee_bps = 0;
    c.taker_fee_bps = 0;
    return c;
}

}  // namespace

TEST(Simulator, OrdersGoLiveOnlyAfterLatency) {
    MarketMakerSim sim(base());
    sim.on_event(Q(0, 100, 101));
    EXPECT_FALSE(sim.bid().live);
    sim.on_event(Q(5, 100, 101));
    EXPECT_FALSE(sim.bid().live);
    sim.on_event(Q(10, 100, 101));
    ASSERT_TRUE(sim.bid().live);
    EXPECT_EQ(sim.bid().price, 100);
    EXPECT_EQ(sim.ask().price, 101);
    EXPECT_DOUBLE_EQ(sim.bid().queue_ahead, 10);  // joined behind the displayed size
}

TEST(Simulator, TradesConsumeQueueAheadBeforeFillingUs) {
    MarketMakerSim sim(base());
    sim.on_event(Q(0, 100, 101));
    sim.on_event(Q(10, 100, 101));
    sim.on_event(T(20, 100, 6, true));
    EXPECT_DOUBLE_EQ(sim.bid().queue_ahead, 4);
    EXPECT_DOUBLE_EQ(sim.inventory(), 0);
    sim.on_event(T(21, 100, 4.5, true));
    EXPECT_DOUBLE_EQ(sim.inventory(), 0.5);  // 0.5 of that trade reached us
    EXPECT_DOUBLE_EQ(sim.bid().remaining, 0.5);
}

TEST(Simulator, CancellationsAheadMoveUsUp) {
    MarketMakerSim sim(base());
    sim.on_event(Q(0, 100, 101));
    sim.on_event(Q(10, 100, 101));
    sim.on_event(Q(20, 100, 101, 3, 10));  // displayed bid shrank to 3 without trades
    EXPECT_DOUBLE_EQ(sim.bid().queue_ahead, 3);
}

TEST(Simulator, TradeThroughFillsCompletely) {
    MarketMakerSim sim(base());
    sim.on_event(Q(0, 100, 101));
    sim.on_event(Q(10, 100, 101));
    sim.on_event(T(20, 99, 0.1, true));
    EXPECT_DOUBLE_EQ(sim.inventory(), 1);
    EXPECT_DOUBLE_EQ(sim.cash(), -100);
}

TEST(Simulator, RoundTripEarnsSpreadMinusFees) {
    SimConfig c = base();
    c.maker_fee_bps = 1;
    MarketMakerSim sim(c);
    sim.on_event(Q(0, 100, 101));
    sim.on_event(Q(10, 100, 101));
    sim.on_event(T(20, 99, 1, true));    // our bid fills at 100
    sim.on_event(T(21, 102, 1, false));  // our ask fills at 101
    DayResult r = sim.finish();
    EXPECT_NEAR(r.pnl, 1.0 - 1e-4 * (100 + 101), 1e-12);
    EXPECT_EQ(r.n_fills, 2);
    EXPECT_DOUBLE_EQ(r.flatten_cost, 0);
}

TEST(Simulator, InventoryLimitStopsTheBid) {
    SimConfig c = base();
    c.max_inventory = 1;
    MarketMakerSim sim(c);
    sim.on_event(Q(0, 100, 101));
    sim.on_event(Q(10, 100, 101));
    sim.on_event(T(20, 99, 1, true));  // long 1, at the limit
    sim.on_event(Q(30, 100, 101));
    sim.on_event(Q(40, 100, 101));
    EXPECT_FALSE(sim.bid().live);
    EXPECT_TRUE(sim.ask().live);
}

TEST(Simulator, InventorySkewBacksOffTheAddingSide) {
    SimConfig c = base();
    c.skew_unit = 1;
    MarketMakerSim sim(c);
    sim.on_event(Q(0, 100, 101));
    sim.on_event(Q(10, 100, 101));
    sim.on_event(T(20, 99, 1, true));  // long 1
    sim.on_event(Q(30, 100, 101));
    sim.on_event(Q(40, 100, 101));
    ASSERT_TRUE(sim.bid().live);
    EXPECT_EQ(sim.bid().price, 99);
    EXPECT_EQ(sim.ask().price, 101);
}

TEST(Simulator, PostOnlyOrderIsRejectedIfTheBookMovedThrough) {
    MarketMakerSim sim(base());
    sim.on_event(Q(0, 100, 101));     // decides to bid 100, ask 101
    sim.on_event(Q(10, 101, 102));    // bid 100 still fine, ask 101 would now cross the 101 bid
    EXPECT_TRUE(sim.bid().live);
    EXPECT_FALSE(sim.ask().live);
}

TEST(Simulator, FlattensAtTheTouchAndChargesTakerFee) {
    SimConfig c = base();
    c.taker_fee_bps = 10;
    MarketMakerSim sim(c);
    sim.on_event(Q(0, 100, 102));
    sim.on_event(Q(10, 100, 102));
    sim.on_event(T(20, 99, 1, true));  // long 1 at 100
    DayResult r = sim.finish();        // sell at the 100 bid
    EXPECT_NEAR(r.pnl, -100 * 10e-4, 1e-12);
    EXPECT_NEAR(r.flatten_cost, 1 + 0.1, 1e-12);
}

TEST(Simulator, MarkoutsMeasureAdverseSelection) {
    MarketMakerSim sim(base());
    sim.on_event(Q(0, 100, 101));
    sim.on_event(Q(10, 100, 101));
    sim.on_event(T(20, 99, 1, true));         // bought at 100
    sim.on_event(Q(30, 95, 96, 10, 10));      // price falls
    sim.on_event(Q(70'000, 95, 96, 10, 10));  // past every markout horizon
    DayResult r = sim.finish();
    EXPECT_DOUBLE_EQ(r.markout_ticks[0], 95.5 - 100);
    EXPECT_DOUBLE_EQ(r.markout_ticks[2], 95.5 - 100);
}

TEST(Simulator, StickyOrdersKeepQueuePositionWhenThePriceMovesAway) {
    SimConfig c = base();
    c.stick_ticks = 2;
    MarketMakerSim sim(c);
    sim.on_event(Q(0, 100, 101));
    sim.on_event(Q(10, 100, 101));
    sim.on_event(T(15, 100, 7, true));      // 3 left ahead of us
    sim.on_event(Q(20, 101, 102, 10, 10));  // best bid moves up a tick
    sim.on_event(Q(40, 101, 102, 10, 10));
    ASSERT_TRUE(sim.bid().live);
    EXPECT_EQ(sim.bid().price, 100);        // stayed, one tick behind
    sim.on_event(Q(50, 100, 101, 8, 10));   // price comes back: we are near the front
    EXPECT_DOUBLE_EQ(sim.bid().queue_ahead, 3);
    const double before = sim.inventory();  // (our ask was lifted when the bid rose to 101)
    sim.on_event(Q(60, 98, 99, 10, 10));    // the book moved through our bid: filled
    EXPECT_DOUBLE_EQ(sim.inventory(), before + 1);
}

TEST(Simulator, ChasingRejoinsAtTheBackOfTheQueue) {
    MarketMakerSim sim(base());
    sim.on_event(Q(0, 100, 101));
    sim.on_event(Q(10, 100, 101));
    sim.on_event(Q(20, 101, 102, 10, 10));
    sim.on_event(Q(30, 101, 102, 10, 10));
    ASSERT_TRUE(sim.bid().live);
    EXPECT_EQ(sim.bid().price, 101);
    EXPECT_DOUBLE_EQ(sim.bid().queue_ahead, 10);
}
