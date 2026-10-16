#include "mm/features.hpp"

#include <algorithm>
#include <stdexcept>

namespace mm {

double ofi_increment(const Quote& prev, const Quote& cur) {
    double e = 0;
    if (cur.bid >= prev.bid) e += cur.bid_qty;
    if (cur.bid <= prev.bid) e -= prev.bid_qty;
    if (cur.ask <= prev.ask) e -= cur.ask_qty;
    if (cur.ask >= prev.ask) e += prev.ask_qty;
    return e;
}

double FeatureVector::operator[](int i) const {
    switch (i) {
        case 0: return ofi_1;
        case 1: return ofi_5;
        case 2: return ti_5;
        case 3: return book_imb;
        case 4: return micro_dev;
    }
    throw std::out_of_range("feature index");
}

FeatureTracker::FeatureTracker(std::int64_t bucket_ms, std::size_t history)
    : bucket_ms_(bucket_ms), history_cap_(history) {}

std::optional<Bucket> FeatureTracker::roll(std::int64_t ts) {
    if (cur_.end_ms == 0) {
        cur_.end_ms = (ts / bucket_ms_ + 1) * bucket_ms_;
        return std::nullopt;
    }
    if (ts < cur_.end_ms) return std::nullopt;
    Bucket done = cur_;
    done.last = book_;
    history_.push_back(done);
    if (history_.size() > history_cap_) history_.pop_front();
    // Empty buckets in a quiet market still count as elapsed time.
    std::int64_t next_end = (ts / bucket_ms_ + 1) * bucket_ms_;
    std::int64_t skipped = (next_end - done.end_ms) / bucket_ms_ - 1;
    last_skipped_ = static_cast<int>(skipped);
    std::int64_t keep = std::min<std::int64_t>(skipped, static_cast<std::int64_t>(history_cap_));
    for (std::int64_t k = skipped - keep + 1; k <= skipped; ++k) {
        Bucket empty;
        empty.end_ms = done.end_ms + k * bucket_ms_;
        empty.last = book_;
        history_.push_back(empty);
        if (history_.size() > history_cap_) history_.pop_front();
    }
    cur_ = Bucket{};
    cur_.end_ms = next_end;
    return done;
}

std::optional<Bucket> FeatureTracker::on_quote(const Quote& q) {
    auto done = roll(q.ts_ms);
    if (has_quote_) cur_.ofi += ofi_increment(book_, q);
    book_ = q;
    has_quote_ = true;
    ++cur_.n_quotes;
    return done;
}

std::optional<Bucket> FeatureTracker::on_trade(const Trade& t) {
    auto done = roll(t.ts_ms);
    cur_.trade_imbalance += t.buyer_is_maker ? -t.qty : t.qty;
    cur_.volume += t.qty;
    ++cur_.n_trades;
    return done;
}

FeatureVector FeatureTracker::features() const { return features_with_book(book_); }

FeatureVector FeatureTracker::features_with_book(const Quote& book) const {
    FeatureVector f{};
    if (!history_.empty()) f.ofi_1 = history_.back().ofi;
    for (const auto& b : history_) {
        f.ofi_5 += b.ofi;
        f.ti_5 += b.trade_imbalance;
    }
    double tot = book.bid_qty + book.ask_qty;
    f.book_imb = tot > 0 ? (book.bid_qty - book.ask_qty) / tot : 0.0;
    double mid = 0.5 * (book.bid + book.ask);
    double micro = tot > 0 ? (book.bid * book.ask_qty + book.ask * book.bid_qty) / tot : mid;
    f.micro_dev = micro - mid;
    return f;
}

}  // namespace mm
