#!/usr/bin/env bash
# Download Binance USD-M futures top-of-book updates and aggregated trades for a symbol and date range.
# Usage: scripts/fetch_data.sh SOLUSDT 2024-03-21 2024-03-30 data/
set -euo pipefail
sym=$1; from=$2; to=$3; out=${4:-data}
base=https://data.binance.vision/data/futures/um/daily
mkdir -p "$out"
d=$from
while [[ "$d" < "$to" || "$d" == "$to" ]]; do
  for kind in bookTicker aggTrades; do
    f="$sym-$kind-$d.zip"
    [[ -f "$out/$f" ]] || curl -sSf --retry 4 -o "$out/$f" "${BINANCE_BASE:-$base}/$kind/$sym/$f"
  done
  d=$(date -I -d "$d + 1 day")
done
