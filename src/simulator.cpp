#include "mm/simulator.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace mm {

namespace {
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr std::int64_t kHourMs = 3'600'000;
}  // namespace

double LinearModel::predict(const FeatureVector& f) const {
    double y = intercept;
    for (int i = 0; i < FeatureVector::kSize; ++i) y += coef[i] * f[i];
    return y;
}

LinearModel LinearModel::load(const std::string& path) {
    std::ifstream in(path);
    LinearModel m;
    if (!(in >> m.intercept)) throw std::runtime_error("cannot read model " + path);
    for (double& c : m.coef)
        if (!(in >> c)) throw std::runtime_error("model needs intercept plus 5 coefficients");
    return m;
}

MarketMakerSim::MarketMakerSim(SimConfig cfg) : cfg_(std::move(cfg)), tracker_(cfg_.bucket_ms) {}

void MarketMakerSim::on_event(const Event& ev) {
    const std::int64_t ts = ev.ts();
    activate_pending(ts);
    if (has_book_) {
        update_markouts(ts);
        record_hour(ts);
    }
    if (ev.kind == Event::Kind::Trade) {
        on_trade(ev.trade);
        tracker_.on_trade(ev.trade);
    } else {
        on_quote(ev.quote);
        tracker_.on_quote(ev.quote);
    }
    if (has_book_ && book_.bid < book_.ask && ts - last_decision_ms_ >= cfg_.min_requote_ms) decide(ts);
}

void MarketMakerSim::activate_pending(std::int64_t ts) {
    for (int s = 0; s < 2; ++s) {
        Pending& p = pending_[s];
        if (!p.active || p.at_ms > ts) continue;
        p.active = false;
        side_[s] = Working{};  // the old order's cancel lands together with the new order
        if (!p.target.live || !has_book_) continue;
        const std::int64_t price = p.target.price;
        const bool crosses = s == 0 ? price >= book_.ask : price <= book_.bid;
        if (crosses) {  // post-only: the exchange rejects it, so ask again on the next decision
            intended_[s] = Target{};
            continue;
        }
        const std::int64_t best = s == 0 ? book_.bid : book_.ask;
        const double shown = s == 0 ? book_.bid_qty : book_.ask_qty;
        const bool improves = s == 0 ? price > best : price < best;
        side_[s] = Working{true, price, cfg_.order_qty, improves ? 0.0 : (price == best ? shown : kInf)};
    }
}

void MarketMakerSim::on_trade(const Trade& t) {
    const int s = t.buyer_is_maker ? 0 : 1;  // an aggressive seller trades against bids
    Working& w = side_[s];
    if (!w.live) return;
    const bool through = s == 0 ? t.price < w.price : t.price > w.price;
    if (through) {
        fill(s, w.remaining, t.ts_ms);
    } else if (t.price == w.price && w.queue_ahead != kInf) {
        w.queue_ahead -= t.qty;
        if (w.queue_ahead < 0) {
            double q = std::min(w.remaining, -w.queue_ahead);
            w.queue_ahead = 0;
            fill(s, q, t.ts_ms);
        }
    }
}

void MarketMakerSim::on_quote(const Quote& q) {
    book_ = q;
    has_book_ = true;
    for (int s = 0; s < 2; ++s) {
        Working& w = side_[s];
        if (!w.live) continue;
        const bool crossed = s == 0 ? w.price >= q.ask : w.price <= q.bid;
        if (crossed) {  // someone quoted at our price on the other side: they would have traded with us
            fill(s, w.remaining, q.ts_ms);
            continue;
        }
        const std::int64_t best = s == 0 ? q.bid : q.ask;
        const double shown = s == 0 ? q.bid_qty : q.ask_qty;
        if (w.price == best) {
            w.queue_ahead = std::min(w.queue_ahead, shown);  // depth that left must have been ahead of us
        } else if (s == 0 ? w.price > best : w.price < best) {
            w.queue_ahead = 0;  // the level emptied without trading through us: we are alone at the top
        }
    }
}

void MarketMakerSim::fill(int s, double qty, std::int64_t ts) {
    Working& w = side_[s];
    const int sign = s == 0 ? 1 : -1;
    const double px = static_cast<double>(w.price) * cfg_.tick;
    const double fee = cfg_.maker_fee_bps / 1e4 * px * qty;
    inventory_ += sign * qty;
    cash_ -= sign * px * qty + fee;
    res_.fees += fee;
    res_.volume += qty;
    res_.notional += px * qty;
    ++res_.n_fills;
    res_.max_abs_inventory = std::max(res_.max_abs_inventory, std::abs(inventory_));
    for (auto& q : markout_q_) q.push_back({ts, sign, w.price, qty});
    w.remaining -= qty;
    if (w.remaining <= 1e-12) {
        w = Working{};
        intended_[s] = Target{};
        last_decision_ms_ = std::numeric_limits<std::int64_t>::min() / 2;  // requote straight away
    }
}

void MarketMakerSim::decide(std::int64_t ts) {
    last_decision_ms_ = ts;
    std::array<int, 2> back{0, 0};
    if (cfg_.skew_unit > 0) {
        if (inventory_ > 0) back[0] = static_cast<int>(std::floor(inventory_ / cfg_.skew_unit));
        if (inventory_ < 0) back[1] = static_cast<int>(std::floor(-inventory_ / cfg_.skew_unit));
    }
    if (cfg_.model && tracker_.ready()) {
        const double pred = cfg_.model->predict(tracker_.features());
        if (pred < -cfg_.signal_threshold) ++back[0];  // price expected to fall: do not buy at the touch
        if (pred > cfg_.signal_threshold) ++back[1];
    }
    for (int& b : back) b = std::min(b, cfg_.max_backoff_ticks);
    const double eps = 1e-9;
    std::array<Target, 2> want{
        Target{inventory_ + cfg_.order_qty <= cfg_.max_inventory + eps, book_.bid - back[0]},
        Target{inventory_ - cfg_.order_qty >= -cfg_.max_inventory - eps, book_.ask + back[1]},
    };
    for (int s = 0; s < 2; ++s) {
        const Target& have = intended_[s];
        if (cfg_.stick_ticks > 0 && have.live && want[s].live) {
            const std::int64_t best = s == 0 ? book_.bid : book_.ask;
            const std::int64_t behind = s == 0 ? best - have.price : have.price - best;
            const bool not_too_aggressive = s == 0 ? have.price <= want[s].price : have.price >= want[s].price;
            if (behind >= 0 && behind <= cfg_.stick_ticks && not_too_aggressive) continue;
        }
        if (want[s] == intended_[s]) continue;
        intended_[s] = want[s];
        pending_[s] = Pending{true, ts + cfg_.latency_ms, want[s]};
    }
}

void MarketMakerSim::update_markouts(std::int64_t ts) {
    for (int h = 0; h < 3; ++h) {
        auto& q = markout_q_[h];
        while (!q.empty() && q.front().ts + horizons_[h] <= ts) {
            const auto& f = q.front();
            markout_sum_[h] += f.sign * (mid() - static_cast<double>(f.price)) * f.qty;
            markout_qty_[h] += f.qty;
            q.pop_front();
        }
    }
}

double MarketMakerSim::mark_to_market() const { return cash_ + inventory_ * mid() * cfg_.tick; }

void MarketMakerSim::record_hour(std::int64_t ts) {
    if (next_hour_ms_ == 0) {
        next_hour_ms_ = (ts / kHourMs + 1) * kHourMs;
        last_hour_mtm_ = mark_to_market();
        return;
    }
    while (ts >= next_hour_ms_) {
        const double m = mark_to_market();
        res_.hourly_pnl.push_back(m - last_hour_mtm_);
        last_hour_mtm_ = m;
        next_hour_ms_ += kHourMs;
    }
}

DayResult MarketMakerSim::finish() {
    if (has_book_ && inventory_ != 0) {
        const bool sell = inventory_ > 0;
        const double qty = std::abs(inventory_);
        const double px = static_cast<double>(sell ? book_.bid : book_.ask) * cfg_.tick;
        const double fee = cfg_.taker_fee_bps / 1e4 * px * qty;
        cash_ += (sell ? 1 : -1) * px * qty - fee;
        res_.fees += fee;
        res_.flatten_cost = qty * std::abs(mid() * cfg_.tick - px) + fee;
        inventory_ = 0;
    }
    res_.pnl = cash_;
    if (has_book_ && next_hour_ms_ != 0) res_.hourly_pnl.push_back(cash_ - last_hour_mtm_);
    for (int h = 0; h < 3; ++h) res_.markout_ticks[h] = markout_qty_[h] > 0 ? markout_sum_[h] / markout_qty_[h] : 0.0;
    return res_;
}

}  // namespace mm
