"""Does order flow explain and predict short-horizon price moves?

Reads the per-second bucket files written by `features`, builds the same features the
simulator uses live, and fits linear models on the first days, testing on the rest.

Usage: python research/ofi_research.py out/ --train-days 5
Writes out/ofi_results.json, out/model_<h>s.txt and docs/figures/ofi_*.png.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

FEATURES = ["ofi_1", "ofi_5", "ti_5", "book_imb", "micro_dev"]
HORIZONS = [1, 5, 10, 30]


def load_day(path: Path) -> pd.DataFrame:
    d = pd.read_csv(path)
    d["mid"] = (d["bid"] + d["ask"]) / 2
    # identical definitions to FeatureTracker: last bucket, last 5 buckets, book at bucket end
    d["ofi_1"] = d["ofi"]
    d["ofi_5"] = d["ofi"].rolling(5).sum()
    d["ti_5"] = d["trade_imb"].rolling(5).sum()
    tot = d["bid_qty"] + d["ask_qty"]
    d["book_imb"] = (d["bid_qty"] - d["ask_qty"]) / tot
    d["micro_dev"] = (d["bid"] * d["ask_qty"] + d["ask"] * d["bid_qty"]) / tot - d["mid"]
    d["dmid_now"] = d["mid"].diff()  # move during the bucket, for the contemporaneous fit
    for h in HORIZONS:
        d[f"fwd_{h}s"] = d["mid"].shift(-h) - d["mid"]
    d["day"] = path.stem.replace("features-", "")
    return d.dropna(subset=["ofi_5"])


def ols(X: np.ndarray, y: np.ndarray) -> np.ndarray:
    A = np.column_stack([np.ones(len(X)), X])
    return np.linalg.lstsq(A, y, rcond=None)[0]


def r2(y: np.ndarray, yhat: np.ndarray) -> float:
    return float(1 - np.sum((y - yhat) ** 2) / np.sum((y - y.mean()) ** 2))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--train-days", type=int, default=5)
    args = ap.parse_args()
    out = Path(args.outdir)
    root = Path(__file__).resolve().parents[1]
    figs = root / "docs" / "figures"
    figs.mkdir(parents=True, exist_ok=True)

    files = sorted(out.glob("features-*.csv"))
    days = [load_day(f) for f in files]
    train = pd.concat(days[: args.train_days])
    test = pd.concat(days[args.train_days:])
    res: dict = {"train_days": [d["day"].iloc[0] for d in days[: args.train_days]],
                 "test_days": [d["day"].iloc[0] for d in days[args.train_days:]],
                 "buckets_train": len(train), "buckets_test": len(test)}

    # 1) contemporaneous: how much of each second's mid move does that second's OFI explain?
    contemp = {}
    for name, df in [("train", train), ("test", test)]:
        df = df.dropna(subset=["dmid_now"])
        x = df["ofi"].to_numpy()[:, None]
        b = ols(x, df["dmid_now"].to_numpy())
        contemp[name] = {"r2": r2(df["dmid_now"].to_numpy(), b[0] + x[:, 0] * b[1]), "slope_ticks_per_unit": float(b[1])}
    res["contemporaneous_ofi"] = contemp

    # 2) predictive: features known at the end of second t vs the mid move over the next h seconds
    pred = {}
    for h in HORIZONS:
        tr, te = train.dropna(subset=[f"fwd_{h}s"]), test.dropna(subset=[f"fwd_{h}s"])
        b = ols(tr[FEATURES].to_numpy(), tr[f"fwd_{h}s"].to_numpy())
        yhat_tr = b[0] + tr[FEATURES].to_numpy() @ b[1:]
        yhat_te = b[0] + te[FEATURES].to_numpy() @ b[1:]
        y_te = te[f"fwd_{h}s"].to_numpy()
        per_feature = {}
        for f in FEATURES:
            bf = ols(tr[[f]].to_numpy(), tr[f"fwd_{h}s"].to_numpy())
            per_feature[f] = r2(y_te, bf[0] + te[f].to_numpy() * bf[1])
        # when the model is confident, how often is the sign right?
        strong = np.abs(yhat_te) > 0.5
        pred[f"{h}s"] = {
            "r2_train": r2(tr[f"fwd_{h}s"].to_numpy(), yhat_tr),
            "r2_test": r2(y_te, yhat_te),
            "corr_test": float(np.corrcoef(y_te, yhat_te)[0, 1]),
            "r2_test_single_feature": per_feature,
            "share_of_seconds_with_|pred|>0.5": float(strong.mean()),
            "sign_hit_rate_when_|pred|>0.5": float((np.sign(yhat_te[strong]) == np.sign(y_te[strong]))[y_te[strong] != 0].mean())
            if strong.any() else float("nan"),
            "coef": dict(zip(["intercept", *FEATURES], map(float, b))),
        }
        (out / f"model_{h}s.txt").write_text(" ".join(f"{v:.10g}" for v in b) + "\n")
    res["predictive"] = pred
    res["avg_spread_ticks"] = float(pd.concat(days)["ask"].sub(pd.concat(days)["bid"]).mean())
    (out / "ofi_results.json").write_text(json.dumps(res, indent=2))

    # figure: binned OFI against same-second mid move (test days)
    t = test.dropna(subset=["dmid_now"]).copy()
    t["bin"] = pd.qcut(t["ofi"], 20, duplicates="drop")
    g = t.groupby("bin", observed=True).agg(ofi=("ofi", "mean"), dmid=("dmid_now", "mean"))
    fig, ax = plt.subplots(1, 2, figsize=(10, 4))
    ax[0].plot(g["ofi"] / 1e3, g["dmid"], marker="o")
    ax[0].set_xlabel("order flow imbalance in the second (thousand XRP)")
    ax[0].set_ylabel("mid change in the same second (ticks)")
    ax[0].set_title(f"contemporaneous, R² = {contemp['test']['r2']:.2f} (test days)")
    hs = list(pred)
    ax[1].bar(hs, [pred[h]["r2_test"] * 100 for h in hs])
    ax[1].set_ylabel("out-of-sample R² (%)")
    ax[1].set_xlabel("prediction horizon")
    ax[1].set_title("predictive, features known at t")
    fig.tight_layout()
    fig.savefig(figs / "ofi_research.png", dpi=130)
    print(json.dumps(res, indent=2))


if __name__ == "__main__":
    main()
