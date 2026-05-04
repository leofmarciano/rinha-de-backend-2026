#!/usr/bin/env sh
set -eu

build_dir="${1:-build-cmake-coverage}"
repo_dir="$(pwd)"

case "$build_dir" in
  /*) build_abs="$build_dir" ;;
  *) build_abs="$repo_dir/$build_dir" ;;
esac

if ! command -v gcov >/dev/null 2>&1; then
  echo "gcov not found" >&2
  exit 1
fi

if [ ! -d "$build_abs" ]; then
  echo "coverage build directory not found: $build_dir" >&2
  exit 1
fi

tmp_dir="$build_abs/coverage-gcov"
rm -rf "$tmp_dir"
mkdir -p "$tmp_dir"

find "$build_abs/CMakeFiles/rinha_core.dir/src" -name '*.gcno' -print | while IFS= read -r gcno; do
  case "$gcno" in
    *"/src/http/server.cc.gcno")
      # The HTTP server is a long-running process and needs a lifecycle hook before it can be
      # measured cleanly by the native unit runner.
      continue
      ;;
  esac
  (cd "$tmp_dir" && gcov "$gcno" >/dev/null)
done

awk -v repo="$repo_dir/" '
  FNR == 1 {
    include = 0
  }
  /^ *-: *0:Source:/ {
    source = $0
    sub(/^ *-: *0:Source:/, "", source)
    include = index(source, repo "src/") == 1 && source ~ /\.cc$/
  }
  /^ *([0-9]+|#+):/ {
    if (!include) next
    total++
    split($0, parts, ":")
    if (parts[1] + 0 > 0) covered++
  }
  END {
    pct = total == 0 ? 0 : covered * 100 / total
    printf("coverage lines: %.2f%% (%d/%d)\n", pct, covered, total)
    if (total == 0) exit 1
  }
' "$tmp_dir"/*.gcov
