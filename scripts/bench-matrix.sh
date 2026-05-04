#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-cmake}"
REFERENCES="${REFERENCES:-$ROOT/resources/references.json.gz}"
QUERIES="${QUERIES:-$ROOT/test/test-data.json}"
RESULTS_DIR="${RESULTS_DIR:-$ROOT/bench/results}"

mkdir -p "$RESULTS_DIR" "$ROOT/build"

cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)"

for nlist in 4096 8192 16384; do
  index="$ROOT/build/index_k${nlist}.ivfi16"
  if [[ ! -f "$index" ]]; then
    "$BUILD_DIR/build-index" --references "$REFERENCES" --out "$index" --nlist "$nlist"
  fi

  for pair in 8:16 12:24 16:32 24:48; do
    base="${pair%%:*}"
    ambig="${pair##*:}"
    for bbox in off ambiguous-only always; do
      out="$RESULTS_DIR/offline_k${nlist}_p${base}_${ambig}_${bbox}.json"
      "$BUILD_DIR/validate-index" \
        --index "$index" \
        --queries "$QUERIES" \
        --base-nprobe "$base" \
        --ambig-nprobe "$ambig" \
        --bbox-mode "$bbox" \
        --no-exact-fallback \
        --out-json "$out"
    done
  done
done
