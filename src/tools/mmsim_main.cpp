// Runs one day of the market-making backtest and prints the result as one JSON object.
#include <cstdio>
#include <iostream>

#include "args.hpp"
#include "mm/simulator.hpp"

int main(int argc, char** argv) try {
    Args a(argc, argv);
    mm::SimConfig cfg;
    cfg.tick = a.num("tick", 0.001);
    cfg.order_qty = a.num("qty", 1.0);
    cfg.max_inventory = a.num("max-inv", 20.0);
    cfg.latency_ms = static_cast<std::int64_t>(a.num("latency", 20));
    cfg.min_requote_ms = static_cast<std::int64_t>(a.num("requote", 100));
    cfg.maker_fee_bps = a.num("maker-bps", 2.0);
    cfg.taker_fee_bps = a.num("taker-bps", 5.0);
    cfg.skew_unit = a.num("skew", 0.0);
    cfg.stick_ticks = static_cast<int>(a.num("stick", 0));
    cfg.signal_threshold = a.num("threshold", 0.5);
    if (a.has("model")) cfg.model = mm::LinearModel::load(a.str("model"));

    mm::EventStream stream(a.str("book"), a.str("trades"), cfg.tick);
    mm::MarketMakerSim sim(cfg);
    mm::Event ev;
    long events = 0;
    while (stream.next(ev)) {
        sim.on_event(ev);
        ++events;
    }
    mm::DayResult r = sim.finish();
    std::printf("{\"events\":%ld,\"pnl\":%.6f,\"fees\":%.6f,\"flatten_cost\":%.6f,\"volume\":%.4f,\"notional\":%.4f,"
                "\"n_fills\":%d,\"max_abs_inventory\":%.4f,\"markout_1s\":%.5f,\"markout_10s\":%.5f,\"markout_60s\":%.5f,"
                "\"hourly_pnl\":[",
                events, r.pnl, r.fees, r.flatten_cost, r.volume, r.notional, r.n_fills, r.max_abs_inventory,
                r.markout_ticks[0], r.markout_ticks[1], r.markout_ticks[2]);
    for (std::size_t i = 0; i < r.hourly_pnl.size(); ++i) std::printf("%s%.6f", i ? "," : "", r.hourly_pnl[i]);
    std::printf("]}\n");
    return 0;
} catch (const std::exception& e) {
    std::cerr << "mmsim: " << e.what() << "\n";
    return 1;
}
