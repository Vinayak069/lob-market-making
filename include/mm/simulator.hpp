#pragma once
// Event-driven backtest of a passive market maker quoting one order per side.
//
// The real order book only shows aggregate depth, so our orders' fills are simulated:
//  * joining the best price puts us at the back of the displayed queue;
//  * trades at our price consume the queue ahead of us before they reach us;
//  * if displayed depth shrinks below our queue position, cancellations ahead of us are assumed;
//  * a trade through our price, or the opposite side moving onto it, fills us completely;
//  * every new order and cancel takes `latency_ms` to reach the exchange.
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "mm/features.hpp"
#include "mm/market_data.hpp"

namespace mm {

struct LinearModel {
    double intercept = 0;
    std::array<double, FeatureVector::kSize> coef{};
    double predict(const FeatureVector& f) const;
    // Reads "intercept c0 c1 c2 c3 c4" (whitespace separated).
    static LinearModel load(const std::string& path);
};

struct SimConfig {
    double tick = 0.001;
    double order_qty = 1.0;
    double max_inventory = 20.0;
    std::int64_t latency_ms = 20;
    std::int64_t min_requote_ms = 100;
    double maker_fee_bps = 2.0;  // negative for a rebate
    double taker_fee_bps = 5.0;  // paid only when flattening at the end of the day
    double skew_unit = 0.0;      // back off one tick per this much inventory on the side that adds to it
    int max_backoff_ticks = 5;
    // Keep a resting order while it is at most this many ticks behind the best price and no
    // further forward than the strategy wants, instead of cancelling to chase the touch.
    // Keeping it preserves queue position for when the price comes back. 0 always chases.
    int stick_ticks = 0;
    std::optional<LinearModel> model;  // predicted mid move (ticks) over the model's horizon
    double signal_threshold = 0.5;     // ticks
    std::int64_t bucket_ms = 1000;
};

struct DayResult {
    double pnl = 0;            // quote currency, after fees and flattening
    double fees = 0;
    double flatten_cost = 0;   // spread and taker fee paid to close the end-of-day position
    double volume = 0;         // base quantity filled
    double notional = 0;
    int n_fills = 0;
    double max_abs_inventory = 0;
    std::array<double, 3> markout_ticks{};  // average signed mid move after fills at 1s, 10s, 60s
    std::vector<double> hourly_pnl;
};

class MarketMakerSim {
public:
    explicit MarketMakerSim(SimConfig cfg);
    void on_event(const Event& ev);
    DayResult finish();  // flattens inventory at the touch and returns the day's result

    // Introspection for tests.
    double inventory() const { return inventory_; }
    double cash() const { return cash_; }
    struct Working {
        bool live = false;
        std::int64_t price = 0;
        double remaining = 0;
        double queue_ahead = 0;  // +infinity while we sit behind the best price
    };
    const Working& bid() const { return side_[0]; }
    const Working& ask() const { return side_[1]; }

private:
    struct Target {
        bool live = false;
        std::int64_t price = 0;
        bool operator==(const Target& o) const { return live == o.live && (!live || price == o.price); }
    };
    struct Pending {
        bool active = false;
        std::int64_t at_ms = 0;
        Target target;
    };
    struct MarkoutFill {
        std::int64_t ts;
        int sign;
        std::int64_t price;
        double qty;
    };

    void activate_pending(std::int64_t ts);
    void on_trade(const Trade& t);
    void on_quote(const Quote& q);
    void fill(int s, double qty, std::int64_t ts);
    void decide(std::int64_t ts);
    void update_markouts(std::int64_t ts);
    void record_hour(std::int64_t ts);
    double mid() const { return 0.5 * static_cast<double>(book_.bid + book_.ask); }
    double mark_to_market() const;

    SimConfig cfg_;
    FeatureTracker tracker_;
    Quote book_{};
    bool has_book_ = false;
    std::array<Working, 2> side_{};   // 0 = bid, 1 = ask
    std::array<Pending, 2> pending_{};
    std::array<Target, 2> intended_{};
    std::int64_t last_decision_ms_ = -1'000'000'000;
    double inventory_ = 0, cash_ = 0;
    DayResult res_;
    std::array<std::int64_t, 3> horizons_{1000, 10000, 60000};
    std::array<std::deque<MarkoutFill>, 3> markout_q_;
    std::array<double, 3> markout_sum_{}, markout_qty_{};
    std::int64_t next_hour_ms_ = 0;
    double last_hour_mtm_ = 0;
};

}  // namespace mm
