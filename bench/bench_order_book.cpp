// Throughput of the matching engine on a synthetic order flow: 60% limit orders
// clustered around the mid, 30% cancels of random live orders, 10% market orders.
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

#include "lob/order_book.hpp"

int main(int argc, char** argv) {
    const long n = argc > 1 ? std::atol(argv[1]) : 5'000'000;
    std::mt19937_64 rng(7);
    std::normal_distribution<double> offset(0.0, 8.0);
    lob::OrderBook book(1 << 20);
    std::vector<lob::Fill> fills;
    fills.reserve(1024);
    std::vector<lob::OrderId> live;
    live.reserve(1 << 20);
    long fills_total = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (lob::OrderId id = 1; id <= static_cast<lob::OrderId>(n); ++id) {
        fills.clear();
        unsigned op = rng() % 10;
        lob::Side side = rng() & 1 ? lob::Side::Buy : lob::Side::Sell;
        if (op < 6) {
            // passive side of the mid with occasional crossing orders
            lob::Price off = static_cast<lob::Price>(std::abs(offset(rng)));
            lob::Price px = side == lob::Side::Buy ? 10'000 - off + 1 : 10'000 + off - 1;
            book.add_limit(id, side, px, 1 + static_cast<lob::Qty>(rng() % 10), fills);
            live.push_back(id);
        } else if (op < 9 && !live.empty()) {
            std::size_t i = rng() % live.size();
            book.cancel(live[i]);
            live[i] = live.back();
            live.pop_back();
        } else {
            book.add_market(id, side, 1 + static_cast<lob::Qty>(rng() % 20), fills);
        }
        fills_total += static_cast<long>(fills.size());
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("ops=%ld fills=%ld resting=%zu time=%.3fs throughput=%.2fM ops/s mean=%.0f ns/op\n", n, fills_total,
                book.order_count(), secs, n / secs / 1e6, secs / n * 1e9);
}
