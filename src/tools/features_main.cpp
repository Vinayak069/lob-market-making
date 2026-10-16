// Exports one row of order-flow features per time bucket as CSV, for offline research.
#include <cstdio>
#include <iostream>

#include "args.hpp"
#include "mm/features.hpp"

int main(int argc, char** argv) try {
    Args a(argc, argv);
    const double tick = a.num("tick", 0.001);
    mm::EventStream stream(a.str("book"), a.str("trades"), tick);
    mm::FeatureTracker tracker(static_cast<std::int64_t>(a.num("bucket-ms", 1000)));
    const auto bucket_ms = static_cast<long long>(a.num("bucket-ms", 1000));
    // Raw per-bucket order flow and the book at each bucket's end. Rolling features are
    // built offline from these rows, with the same definitions the simulator uses live.
    std::printf("end_ms,bid,ask,bid_qty,ask_qty,ofi,trade_imb,volume,n_trades,n_quotes\n");
    auto row = [](long long end, const mm::Quote& q, const mm::Bucket* b) {
        std::printf("%lld,%lld,%lld,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d\n", end, static_cast<long long>(q.bid),
                    static_cast<long long>(q.ask), q.bid_qty, q.ask_qty, b ? b->ofi : 0.0, b ? b->trade_imbalance : 0.0,
                    b ? b->volume : 0.0, b ? b->n_trades : 0, b ? b->n_quotes : 0);
    };
    mm::Event ev;
    while (stream.next(ev)) {
        auto done = ev.kind == mm::Event::Kind::Quote ? tracker.on_quote(ev.quote) : tracker.on_trade(ev.trade);
        if (!done) continue;
        row(done->end_ms, done->last, &*done);
        for (int k = 1; k <= tracker.last_skipped(); ++k) row(done->end_ms + k * bucket_ms, done->last, nullptr);
    }
    return 0;
} catch (const std::exception& e) {
    std::cerr << "features: " << e.what() << "\n";
    return 1;
}
