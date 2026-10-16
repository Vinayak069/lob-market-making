#pragma once
// Order-flow features over fixed time buckets, shared by the research export and the
// simulator so the model is trained on exactly what the strategy sees live.
#include <cstdint>
#include <deque>
#include <optional>

#include "mm/market_data.hpp"

namespace mm {

// Order flow imbalance from one top-of-book update to the next (Cont, Kukanov and Stoikov, 2014):
// bid-side additions minus bid-side removals, minus the same on the ask side.
double ofi_increment(const Quote& prev, const Quote& cur);

struct Bucket {
    std::int64_t end_ms = 0;
    double ofi = 0;              // summed OFI over the bucket
    double trade_imbalance = 0;  // aggressive buy volume minus aggressive sell volume
    double volume = 0;
    int n_trades = 0;
    int n_quotes = 0;
    Quote last{};  // book at the end of the bucket
};

struct FeatureVector {
    double ofi_1;        // last completed bucket
    double ofi_5;        // last 5 buckets
    double ti_5;         // trade imbalance, last 5 buckets
    double book_imb;     // (bid_qty - ask_qty) / (bid_qty + ask_qty), current
    double micro_dev;    // microprice minus mid, in ticks, current
    static constexpr int kSize = 5;
    double operator[](int i) const;
};

class FeatureTracker {
public:
    explicit FeatureTracker(std::int64_t bucket_ms = 1000, std::size_t history = 5);

    // Feed events in time order. Returns a completed bucket when `ts` crosses a boundary.
    std::optional<Bucket> on_quote(const Quote& q);
    std::optional<Bucket> on_trade(const Trade& t);

    // Empty buckets skipped by the last event that completed a bucket (quiet periods).
    int last_skipped() const { return last_skipped_; }
    bool ready() const { return has_quote_ && history_.size() == history_cap_; }
    // Features from the completed buckets and the current book.
    FeatureVector features() const;
    // Same, but with a given book (e.g. the book at the end of the last bucket).
    FeatureVector features_with_book(const Quote& book) const;
    const Quote& book() const { return book_; }

private:
    std::optional<Bucket> roll(std::int64_t ts);
    std::int64_t bucket_ms_;
    std::size_t history_cap_;
    std::deque<Bucket> history_;
    Bucket cur_;
    Quote book_{};
    bool has_quote_ = false;
    int last_skipped_ = 0;
};

}  // namespace mm
