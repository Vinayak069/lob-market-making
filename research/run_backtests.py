"""Grid of market-making backtests: choose settings on the training days, report on the test days.

Every run uses a 0 bps maker fee; other fee levels are exact linear adjustments
(P&L at fee f = P&L at 0 - f/1e4 * maker notional), so they need no extra runs.

Usage: python research/run_backtests.py --data data --out out --symbol XRPUSDT
"""
from __future__ import annotations

import argparse
import itertools
import json
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

ROOT = Path(__file__).resolve().parents[1]
FEES_BPS = [2.0, 0.0, -0.5]  # Binance USD-M retail maker fee, zero, and a market-maker rebate
BASE = {"tick": 0.0001, "qty": 1000, "max-inv": 20000, "latency": 20, "requote": 100, "taker-bps": 5}


def run(day: str, cfg: dict, args) -> dict:
    cmd = [str(ROOT / "build" / "mmsim"),
           "--book", f"{args.data}/{args.symbol}-bookTicker-{day}.zip",
           "--trades", f"{args.data}/{args.symbol}-aggTrades-{day}.zip", "--maker-bps", "0"]
    for k, v in {**BASE, **cfg}.items():
        if v is not None:
            cmd += [f"--{k}", str(v)]
    out = json.loads(subprocess.run(cmd, check=True, capture_output=True, text=True).stdout)
    out["day"] = day
    return out


def summarise(rows: list[dict]) -> dict:
    df = pd.DataFrame(rows)
    s = {"days": len(df), "fills_per_day": float(df["n_fills"].mean()),
         "volume_xrp_per_day": float(df["volume"].mean()), "notional_usd_per_day": float(df["notional"].mean()),
         "max_abs_inventory": float(df["max_abs_inventory"].max()),
         "markout_1s_ticks": float(np.average(df["markout_1s"], weights=df["volume"])),
         "markout_10s_ticks": float(np.average(df["markout_10s"], weights=df["volume"])),
         "markout_60s_ticks": float(np.average(df["markout_60s"], weights=df["volume"]))}
    hourly = np.concatenate(df["hourly_pnl"].to_numpy())
    for fee in FEES_BPS:
        pnl = df["pnl"] - fee / 1e4 * df["notional"]
        hourly_fee = hourly - fee / 1e4 * np.repeat(df["notional"].to_numpy() / 24, [len(h) for h in df["hourly_pnl"]])
        s[f"fee_{fee:g}"] = {
            "pnl_usd_per_day": float(pnl.mean()),
            "pnl_bps_of_notional": float(pnl.sum() / df["notional"].sum() * 1e4),
            "profitable_days": int((pnl > 0).sum()),
            "hourly_sharpe": float(hourly_fee.mean() / hourly_fee.std()) if hourly_fee.std() > 0 else float("nan"),
        }
    return s


def label(cfg: dict) -> str:
    parts = [f"stick={cfg['stick']}", f"skew={cfg['skew'] or 'off'}"]
    parts.append(f"signal>{cfg['threshold']}" if cfg.get("model") else "no signal")
    return ", ".join(parts)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="data")
    ap.add_argument("--out", default="out")
    ap.add_argument("--symbol", default="XRPUSDT")
    ap.add_argument("--train-days", type=int, default=5)
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args()
    out = Path(args.out)
    days = sorted(p.name.split("-bookTicker-")[1][:-4] for p in Path(args.data).glob(f"{args.symbol}-bookTicker-*.zip"))
    train, test = days[: args.train_days], days[args.train_days:]
    model = str(out / "model_5s.txt")

    grid = [{"stick": k, "skew": sk, "model": model if th else None, "threshold": th}
            for k, sk, th in itertools.product([0, 1, 2, 3], [0, 5000], [None, 0.3, 0.5, 1.0])]
    with ThreadPoolExecutor(args.jobs) as ex:
        jobs = {(i, d): ex.submit(run, d, cfg, args) for i, cfg in enumerate(grid) for d in train}
        train_rows = {i: [jobs[(i, d)].result() for d in train] for i in range(len(grid))}
    table = []
    for i, cfg in enumerate(grid):
        s = summarise(train_rows[i])
        table.append({"config": label(cfg), **cfg, "pnl_per_day_fee0": s["fee_0"]["pnl_usd_per_day"],
                      "markout_10s": s["markout_10s_ticks"], "fills_per_day": s["fills_per_day"]})
    train_df = pd.DataFrame(table).sort_values("pnl_per_day_fee0", ascending=False)
    best = grid[int(train_df.index[0])]
    print(train_df.drop(columns=["model"]).to_string())

    # test: the chosen configuration plus an ablation path from the naive maker to it
    ablation = {
        "naive (chase the touch)": {"stick": 0, "skew": 0, "model": None, "threshold": None},
        "sticky queue": {"stick": best["stick"] or 2, "skew": 0, "model": None, "threshold": None},
        "sticky + inventory skew": {"stick": best["stick"] or 2, "skew": 5000, "model": None, "threshold": None},
        "selected on training days": best,
    }
    # reaction speed: order latency and the minimum time between quote updates, on the naive maker
    speeds = {"slow (300ms, requote 500ms)": (300, 500), "base (20ms, requote 100ms)": (20, 100),
              "fast (1ms, requote on every event)": (1, 0)}
    with ThreadPoolExecutor(args.jobs) as ex:
        tj = {(n, d): ex.submit(run, d, cfg, args) for n, cfg in ablation.items() for d in test}
        naive = ablation["naive (chase the touch)"]
        lj = {(n, d): ex.submit(run, d, {**naive, "latency": L, "requote": R}, args)
              for n, (L, R) in speeds.items() for d in test}
        test_rows = {n: [tj[(n, d)].result() for d in test] for n in ablation}
        lat_rows = {n: [lj[(n, d)].result() for d in test] for n in speeds}

    results = {"symbol": args.symbol, "train_days": train, "test_days": test, "base_params": BASE,
               "selected": {**best, "label": label(best)},
               "train_grid": train_df.drop(columns=["model"]).to_dict(orient="records"),
               "test": {n: summarise(r) for n, r in test_rows.items()},
               "test_speed_naive": {n: summarise(r) for n, r in lat_rows.items()},
               "test_daily": {n: [{"day": x["day"], "pnl_fee0": x["pnl"], "notional": x["notional"]} for x in r]
                              for n, r in test_rows.items()}}
    (out / "backtest_results.json").write_text(json.dumps(results, indent=2))

    figs = ROOT / "docs" / "figures"
    fig, ax = plt.subplots(figsize=(8, 4))
    for n, rows in test_rows.items():
        h = np.concatenate([x["hourly_pnl"] for x in rows])
        ax.plot(np.cumsum(h), label=n)
    ax.set_xlabel("hour (test days)")
    ax.set_ylabel("cumulative P&L, USD, 0 bps maker fee")
    ax.axhline(0, color="k", lw=0.6)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(figs / "mm_test_pnl.png", dpi=130)
    print(json.dumps({k: v for k, v in results.items() if k in ("selected", "test", "test_speed_naive")}, indent=1))


if __name__ == "__main__":
    main()
