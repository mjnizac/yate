#!/usr/bin/env bash
# One entry point for the loop of building, testing and reviewing a terrain from bash on Windows.
#
#   tools/dev.sh build [Debug|Release]
#   tools/dev.sh test  [Debug|Release] [ctest arguments...]     e.g. -L fast, -R test_relief
#   tools/dev.sh review <script.lua> --bounds minx,minz,maxx,maxz [--res M] [--level L | --no-sea]
#                       [--sun az,el] [--target x,y,z] [--shot yaw,pitch,dist]... [--param k=v]...
#                       [--seed N] [--out DIR] [--type Release|Debug]
#   tools/dev.sh lands <script.lua> --bounds minx,minz,maxx,maxz [--res M] [--param k=v]... [--out DIR]
#   tools/dev.sh lands-check <script.lua> --bounds ... --expect count=N,area=MIN..MAX,land=MIN..MAX
#
# `review` exports the script as raw floats, measures it (out/review/<name>/metrics.json), writes a
# hillshade, takes the viewer shots in one run, and prints a short summary. `lands` prints what a Lands
# script asked for next to what each id measured, and `lands-check` checks it. `test` refuses to start
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


# Exports a Lands script raw, then prints what it asked for (`script_report.lands` in metadata.json) next to
# what each id measured (`terrain_metrics --by-id`). The script must output `region_id`.
lands() {
    local script="$1"; shift
    local bounds="" res="16" type="Release" out="" params=()
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --bounds) bounds="$2"; shift 2 ;;
            --res) res="$2"; shift 2 ;;
            --param) params+=(--param "$2"); shift 2 ;;
            --out) out="$2"; shift 2 ;;
            --type) type="$2"; shift 2 ;;
            *) echo "dev.sh lands: unknown option $1" >&2; exit 2 ;;
        esac
    done
    [[ -n "$bounds" ]] || { echo "dev.sh lands: --bounds minx,minz,maxx,maxz is required" >&2; exit 2; }
    local name; name="$(basename "$script" .lua)"
    out="${out:-$ROOT/out/lands/$name}"
    mkdir -p "$out"
    local bin="$ROOT/build/$type/bin"
    IFS=, read -r x0 z0 x1 z1 <<<"$bounds"
    "$bin/terrain_export" --script "$script" --min "$x0,$z0" --max "$x1,$z1" --resolution "$res" \
        --raw --log-level warn --out "$out" "${params[@]}" || exit 1
    echo "asked (script_report.lands.elements):"
    grep -o '"elements":\[.*\]' "$out/metadata.json" | sed 's/^"elements":\[//; s/\]$//; s/},{/}\n{/g' \
        | sed 's/^/  /'
    echo "measured (terrain_metrics --by-id, positions in samples from ($x0, $z0)):"
    "$bin/terrain_metrics" "$out" --by-id | grep '"id"' | sed 's/^ */  /'
}

# `lands` with the answer checked: --expect count=N,area=MIN..MAX (m2, every land id),land=MIN..MAX (fraction
# of the window). Prints PASS or FAIL per term and exits 1 on any failure.
lands_check() {
    local script="$1"; shift
    local expect="" rest=()
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --expect) expect="$2"; shift 2 ;;
            *) rest+=("$1"); shift ;;
        esac
    done
    local report; report="$(lands "$script" "${rest[@]}")" || exit 1
    echo "$report"
    local areas; areas="$(echo "$report" | grep -o '"area_m2": [0-9.e+-]*' | grep -o '[0-9.e+-]*$')"
    local bounds="" i=0
    for ((i = 0; i < ${#rest[@]}; i++)); do [[ "${rest[$i]}" == --bounds ]] && bounds="${rest[$((i + 1))]}"; done
    IFS=, read -r x0 z0 x1 z1 <<<"$bounds"
    local failed=0 term
    IFS=, read -ra terms <<<"$expect"
    for term in "${terms[@]}"; do
        local key="${term%%=*}" value="${term#*=}" lo hi
        lo="${value%%..*}"; hi="${value##*..}"
        case "$key" in
            count)
                local n; n="$(echo "$areas" | grep -c . || true)"
                if [[ "$n" == "$value" ]]; then echo "PASS count $n"; else echo "FAIL count $n, expected $value"; failed=1; fi ;;
            area)
                local bad; bad="$(echo "$areas" | awk -v lo="$lo" -v hi="$hi" '$1 < lo || $1 > hi' | head -3 | tr '\n' ' ')"
                if [[ -z "$bad" ]]; then echo "PASS area every id in $lo..$hi"; else echo "FAIL area $bad outside $lo..$hi"; failed=1; fi ;;
            land)
                local f; f="$(echo "$areas" | awk -v w="$(awk "BEGIN { print $x1 - ($x0) }")" \
                    -v d="$(awk "BEGIN { print $z1 - ($z0) }")" '{ s += $1 } END { printf "%.4f", s / (w * d) }')"
                if awk -v f="$f" -v lo="$lo" -v hi="$hi" 'BEGIN { exit !(f >= lo && f <= hi) }'; then
                    echo "PASS land fraction $f"; else echo "FAIL land fraction $f outside $lo..$hi"; failed=1; fi ;;
            *) echo "dev.sh lands-check: unknown term $key (count, area, land)" >&2; exit 2 ;;
        esac
    done
    exit "$failed"
}

case "${1:-}" in
    build) shift; build "$@" ;;
    test) shift; run_tests "$@" ;;
    review) shift; review "$@" ;;
    lands) shift; lands "$@" ;;
    lands-check) shift; lands_check "$@" ;;
    *) sed -n '2,17p' "$0"; exit 2 ;;
esac
