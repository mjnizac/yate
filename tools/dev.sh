#!/usr/bin/env bash
# One entry point for the loop of building, testing and reviewing a terrain from bash on Windows.
#
#   tools/dev.sh build [Debug|Release]
#   tools/dev.sh test  [Debug|Release] [ctest arguments...]     e.g. -L fast, -R test_relief
#   tools/dev.sh review <script.lua> --bounds minx,minz,maxx,maxz [--res M] [--level L | --no-sea]
#                       [--sun az,el] [--target x,y,z] [--shot yaw,pitch,dist]... [--param k=v]...
#                       [--seed N] [--out DIR] [--type Release|Debug]
#
# `review` exports the script as raw floats, measures it (out/review/<name>/metrics.json), writes a
# hillshade, takes the viewer shots in one run, and prints a short summary. `test` refuses to start
# while another suite runs: two suites share out/ and fail each other.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VS_ROOT="${VS_ROOT:-/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools}"
CTEST="$VS_ROOT/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe"

build() {
    local type="${1:-Release}"
    cmd //c "$(cygpath -w "$ROOT/build.cmd") $type" 2>&1 | grep -E " error |warning C|FAILED|^ninja: build stopped" || true
    test -x "$ROOT/build/$type/bin/terrain_export.exe" || { echo "dev.sh: build failed" >&2; exit 1; }
}

run_tests() {
    local type="Release"
    if [[ "${1:-}" == Debug || "${1:-}" == Release ]]; then type="$1"; shift; fi
    local lock="$ROOT/out/.ctest.lock"
    mkdir -p "$ROOT/out"
    mkdir "$lock" 2>/dev/null || { echo "dev.sh: another test run holds $lock" >&2; exit 1; }
    # Expanded now: the local is gone by the time the trap runs.
    trap "rmdir '$lock'" EXIT
    (cd "$ROOT/build/$type" && "$CTEST" --output-on-failure "$@" 2>&1 \
        | grep -E "Test +#|tests passed|FAILED|Failed|SEGFAULT|Timeout|^FAIL" || true)
}

review() {
    local script="$1"; shift
    local bounds="" res="8" sea="--no-sea" sun="315,35" target="0,0,0" seed="0" out="" type="Release"
    local shots=() params=()
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --bounds) bounds="$2"; shift 2 ;;
            --res) res="$2"; shift 2 ;;
            --level) sea="--level $2"; shift 2 ;;
            --no-sea) sea="--no-sea"; shift ;;
            --sun) sun="$2"; shift 2 ;;
            --target) target="$2"; shift 2 ;;
            --shot) shots+=("$2"); shift 2 ;;
            --param) params+=(--param "$2"); shift 2 ;;
            --seed) seed="$2"; shift 2 ;;
            --out) out="$2"; shift 2 ;;
            --type) type="$2"; shift 2 ;;
            *) echo "dev.sh review: unknown option $1" >&2; exit 2 ;;
        esac
    done
    [[ -n "$bounds" ]] || { echo "dev.sh review: --bounds minx,minz,maxx,maxz is required" >&2; exit 2; }
    local name; name="$(basename "$script" .lua)"
    out="${out:-$ROOT/out/review/$name}"
    mkdir -p "$out"
    local bin="$ROOT/build/$type/bin"
    IFS=, read -r x0 z0 x1 z1 <<<"$bounds"

    "$bin/terrain_export" --script "$script" --min "$x0,$z0" --max "$x1,$z1" --resolution "$res" \
        --seed "$seed" --raw --log-level warn --out "$out" "${params[@]}"
    # shellcheck disable=SC2086
    "$bin/terrain_metrics" "$out" $sea --json > "$out/metrics.json"
    # shellcheck disable=SC2086
    "$bin/terrain_metrics" "$out" $sea --hillshade "$out/hillshade.png" --sun "$sun" | tail -n 4

    if [[ ${#shots[@]} -gt 0 ]]; then
        local args=() i=0
        for shot in "${shots[@]}"; do
            args+=(--shot "$shot=$out/view_$i.png"); i=$((i + 1))
        done
        "$bin/terrain_viewer" --script "$script" --seed "$seed" --target "$target" --ring 5 \
            --log-level warn "${params[@]}" "${args[@]}"
    fi
    grep -o '"timings_ms": {[^}]*}' "$out/metadata.json" | grep -o '"total": [0-9.]*\|"script": [0-9.]*' \
        | tr '\n' ' '
    echo
    echo "review: $out (metrics.json, hillshade.png$( [[ ${#shots[@]} -gt 0 ]] && echo ", view_*.png"))"
}

case "${1:-}" in
    build) shift; build "$@" ;;
    test) shift; run_tests "$@" ;;
    review) shift; review "$@" ;;
    *) sed -n '2,13p' "$0"; exit 2 ;;
esac
