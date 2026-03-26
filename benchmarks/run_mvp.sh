#!/usr/bin/env bash
set -euo pipefail

# Always run from the repo root
cd "$(dirname "$0")/.."

echo "Running MVP parity benchmarks (Apple ld vs sold)..."

python3 benchmarks/run.py \
  --corpus benchmarks/corpus.toml \
  --out-dir benchmarks/out-mvp \
  --sold-bin build/ld64 \
  --warmups 3 \
  --runs 10 \
  --min-win-pct 0.0 \
  "$@"
