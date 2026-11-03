#!/usr/bin/env bash
# One entry point for the loop of building, testing and reviewing a terrain from bash on Windows.
#
#   tools/dev.sh build [Debug|Release]
#   tools/dev.sh test  [Debug|Release] [ctest arguments...]     e.g. -L fast, -R test_relief
#   tools/dev.sh review <script.lua> --bounds minx,minz,maxx,maxz [--res M] [--level L | --no-sea]
#                       [--sun az,el] [--target x,y,z] [--shot yaw,pitch,dist]... [--param k=v]...
#                       [--seed N] [--out DIR] [--type Release|Debug]
#   tools/dev.sh sweep <script.lua> --vary key=v1,v2,... [--ref image]... [review options...]  one sheet.png
#   tools/dev.sh dem lat,lon --out DIR [--zoom 14] [--tiles 3]                  a real height field to measure as a reference
#   tools/dev.sh lands <script.lua> --bounds minx,minz,maxx,maxz [--res M] [--param k=v]... [--out DIR]
#   tools/dev.sh lands-check <script.lua> --bounds ... --expect count=N,area=MIN..MAX,land=MIN..MAX
#   tools/dev.sh clean [DAYS]                                   removes out/ entries untouched for DAYS (7)
#   tools/dev.sh lint                                           mechanical checks of the CLAUDE.md rules over src/ and include/
#   tools/dev.sh headings [doc]                                 line numbers of a doc's headings (default docs/archive/decisions.md)
#   tools/dev.sh docs-check                                     the docs agree with the repo: paths, suites, op count, todo
#   tools/dev.sh api [name]                                     every script function and the keys it accepts, from the engine
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
    # The status is the build's, not grep's: an old exe left by a previous build must not pass for this one.
    cmd //c "$(cygpath -w "$ROOT/build.cmd") $type" 2>&1 \
        | { grep -E " error |error:|warning C|FAILED|CMake Error|^ninja: build stopped" || true; } \
        || { echo "dev.sh: build failed" >&2; exit 1; }
}

run_tests() {
    local type="Release"
    if [[ "${1:-}" == Debug || "${1:-}" == Release ]]; then type="$1"; shift; fi
    local lock="$ROOT/out/.ctest.lock"
    mkdir -p "$ROOT/out"
    mkdir "$lock" 2>/dev/null || { echo "dev.sh: another test run holds $lock" >&2; exit 1; }
    # Expanded now: the local is gone by the time the trap runs.
    trap "rmdir '$lock'" EXIT
    # Exits with ctest's status (pipefail), so a failing suite fails the command. Each suite writes its own
    # directory and test_datasets is RUN_SERIAL, so suites run 4 at a time unless -j or the variable says otherwise.
    (cd "$ROOT/build/$type" && CTEST_PARALLEL_LEVEL="${CTEST_PARALLEL_LEVEL:-4}" "$CTEST" --output-on-failure "$@" 2>&1 \
        | { grep -E "Test +#|tests passed|FAILED|Failed|SEGFAULT|Timeout|^FAIL|^ +(actual|expected|error) +:|^-- " || true; })
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
            --log-level warn "${params[@]}" "${args[@]}" 2>/dev/null \
            || echo "dev.sh review: terrain_viewer exited with $? (it crashes on shutdown after the shots)" >&2
    fi
    grep -o '"timings_ms": {[^}]*}' "$out/metadata.json" | grep -o '"total": [0-9.]*\|"script": [0-9.]*' \
        | tr '\n' ' '
    echo
    echo "review: $out (metrics.json, hillshade.png$( [[ ${#shots[@]} -gt 0 ]] && echo ", view_*.png"))"
}

# Runs `review` once per value of one parameter and lays the results out in one labelled image, sheet.png:
# a column per value (the references first), a row per image (hillshade, then each --shot). Each label
# carries the numbers that catch a broken variant: slope p99.9 (spikes past 85), closed depressions and
# steep share. `--vary stage=1,2,3,4` on a staged script shows which stage drifts from the reference.
sweep() {
    local script="$1"; shift
    local vary="" refs=() rest=() shots=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --vary) vary="$2"; shift 2 ;;
            --ref) refs+=("$(realpath "$2")"); shift 2 ;;
            --shot) rest+=("$1" "$2"); shots=$((shots + 1)); shift 2 ;;
            *) rest+=("$1"); shift ;;
        esac
    done
    [[ "$vary" == *=* ]] || { echo "dev.sh sweep: --vary key=v1,v2,... is required" >&2; exit 2; }
    local key="${vary%%=*}" name; name="$(basename "$script" .lua)"
    local out="$ROOT/out/sweep/$name" tiles size=512 font="fontfile='C\:/Windows/Fonts/arial.ttf'"
    tiles="$out/tiles"
    rm -rf "$tiles"; mkdir -p "$tiles"
    IFS=, read -ra values <<<"${vary#*=}"
    # One tile: the image fitted into a square on black, its label on top. Missing images give a black tile.
    tile() {
        printf '%s' "$3" > "$tiles/label.txt"
        local input=(-f lavfi -i "color=black:s=${size}x${size}") fit=""
        [[ -f "$1" ]] && { input=(-i "$1"); fit="scale=$size:$size:force_original_aspect_ratio=decrease,pad=$size:$size:-1:-1,"; }
        # One pixel format for every tile, or the sequence that tiles them breaks on the first change.
        (cd "$tiles" && ffmpeg -loglevel error -y "${input[@]}" -frames:v 1 \
            -vf "${fit}format=rgb24,drawtext=$font:expansion=none:textfile=label.txt:fontsize=18:fontcolor=yellow:box=1:boxcolor=black@0.6:x=6:y=6" "$2")
    }
    local columns=$(( ${#refs[@]} + ${#values[@]} )) row n=0 img
    for ((row = 0; row <= shots; row++)); do
        for img in "${refs[@]}"; do
            if [[ $row == 0 ]]; then tile "$img" "$(printf 't%03d.png' $n)" "reference $(basename "$img")"
            else tile "" "$(printf 't%03d.png' $n)" ""; fi
            n=$((n + 1))
        done
        for v in "${values[@]}"; do
            local dir="$out/${key}_$v"
            if [[ $row == 0 ]]; then
                echo "== $key=$v"
                review "$script" "${rest[@]}" --param "$key=$v" --out "$dir" | sed -n 1,4p
            fi
            local m="$dir/metrics.json" label="$key=$v"
            if [[ $row == 0 && -f "$m" ]]; then
                local p999 pits steep
                p999="$(grep -o '"slope_p999": [0-9.]*' "$m" | grep -o '[0-9.]*$')"
                pits="$(grep -o '"pit_share": [0-9.]*' "$m" | grep -o '[0-9.]*$')"
                steep="$(grep -o '"steep45": [0-9.]*' "$m" | grep -o '[0-9.]*$')"
                label+="  p99.9 ${p999} deg  pits $(awk "BEGIN { printf \"%.2f\", $pits * 100 }") %  >45 deg $(awk "BEGIN { printf \"%.1f\", $steep * 100 }") %"
                awk "BEGIN { exit !($p999 > 85) }" && label+="  SPIKES"
            fi
            [[ $row == 0 ]] && img="$dir/hillshade.png" || img="$dir/view_$((row - 1)).png"
            tile "$img" "$(printf 't%03d.png' $n)" "$label"
            n=$((n + 1))
        done
    done
    (cd "$tiles" && ffmpeg -loglevel error -y -framerate 1 -i t%03d.png -frames:v 1 \
        -vf "tile=${columns}x$((shots + 1)):padding=4" "$out/sheet.png")
    echo "sweep: $out/sheet.png"
}

# A real height field as a reference: N x N tiles of the AWS Terrain Tiles (open data, terrarium PNG, metres =
# R * 256 + G + B / 256 - 32768) centred on lat,lon, joined into `height.raw` with a `metadata.json`, so
# terrain_metrics measures it and draws its hillshade like any export. The spacing is the Mercator pixel at
# that latitude: zoom 14 is about 7.5 m at 40 degrees, a tile 256 samples.
dem() {
    local at="$1"; shift
    local zoom=14 count=3 out=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --zoom) zoom="$2"; shift 2 ;;
            --tiles) count="$2"; shift 2 ;;
            --out) out="$2"; shift 2 ;;
            *) echo "dev.sh dem: unknown option $1" >&2; exit 2 ;;
        esac
    done
    [[ -n "$out" ]] || { echo "dev.sh dem: --out DIR is required" >&2; exit 2; }
    mkdir -p "$out/tiles"
    local x0 y0 res
    read -r x0 y0 res < <(awk -v at="$at" -v z="$zoom" -v n="$count" 'BEGIN {
        split(at, p, ","); pi = atan2(0, -1); lat = p[1] * pi / 180; s = 2 ^ z
        x = (p[2] + 180) / 360 * s; y = (1 - log(sin(lat) / cos(lat) + 1 / cos(lat)) / pi) / 2 * s
        printf "%d %d %.4f\n", x - n / 2, y - n / 2, 156543.034 * cos(lat) / s }')
    local i j files=()
    for ((j = 0; j < count; j++)); do
        for ((i = 0; i < count; i++)); do
            local f="$out/tiles/$((y0 + j))_$((x0 + i)).png"
            [[ -f "$f" ]] || curl -sfo "$f" "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/$zoom/$((x0 + i))/$((y0 + j)).png" \
                || { echo "dev.sh dem: cannot fetch tile $zoom/$((x0 + i))/$((y0 + j))" >&2; exit 1; }
            files+=("$(cygpath -w "$f")")
        done
    done
    DEM_FILES="$(IFS='|'; echo "${files[*]}")" DEM_COUNT="$count" DEM_OUT="$(cygpath -w "$out/height.raw")" \
        powershell.exe -NoProfile -Command '
        Add-Type -AssemblyName System.Drawing
        $files = $env:DEM_FILES.Split("|"); $n = [int]$env:DEM_COUNT; $w = 256 * $n
        $h = New-Object float[] ($w * $w)
        for ($t = 0; $t -lt $files.Length; $t++) {
            $bmp = [System.Drawing.Bitmap]::new($files[$t])
            $data = $bmp.LockBits([System.Drawing.Rectangle]::new(0, 0, 256, 256), "ReadOnly", "Format24bppRgb")
            $px = New-Object byte[] ($data.Stride * 256)
            [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $px, 0, $px.Length)
            $ox = ($t % $n) * 256; $oz = [math]::Floor($t / $n) * 256
            for ($z = 0; $z -lt 256; $z++) { for ($x = 0; $x -lt 256; $x++) {
                $o = $z * $data.Stride + 3 * $x  # BGR order
                $h[($oz + $z) * $w + $ox + $x] = $px[$o + 2] * 256 + $px[$o + 1] + $px[$o] / 256 - 32768 } }
            $bmp.UnlockBits($data); $bmp.Dispose()
        }
        $bytes = New-Object byte[] ($h.Length * 4); [Buffer]::BlockCopy($h, 0, $bytes, 0, $bytes.Length)
        [IO.File]::WriteAllBytes($env:DEM_OUT, $bytes)'
    local size=$((256 * count))
    printf '{ "width": %d, "height": %d, "resolution": %s, "source": "AWS Terrain Tiles terrarium z%d around %s" }\n' \
        "$size" "$size" "$res" "$zoom" "$at" > "$out/metadata.json"
    echo "dem: $out ($size x $size samples, $res m, $(awk "BEGIN { printf \"%.1f\", $size * $res / 1000 }") km)"
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

# Deletes what out/ holds that has not changed for DAYS (default 7): old experiments, never a running suite's lock.
clean() {
    find "$ROOT/out" -mindepth 1 -maxdepth 1 -mtime "+${1:-7}" ! -name .ctest.lock -exec rm -rf {} +
}

# Mechanical checks of CLAUDE.md's rules over src/ and include/, in seconds and with no model: no new/delete/malloc
# outside engine/memory (placement new is the allowed form), no side effects in ENGINE_ASSERT, every executable
# routes operator new, and the kernel push constant stays at 128 bytes. Prints one line per problem, exits 1 on any.
lint() {
    local found=0 hits
    hits="$(grep -rnE '(^|[^_[:alnum:].>"])(new [[:alpha:]_]|delete[ \[;])' --include=*.cpp --include=*.hpp "$ROOT/src" "$ROOT/include" \
        | grep -vE '/memory/|= *delete|operator|^[^:]*:[0-9]+: *(//|\*)' || true)"
    [[ -z "$hits" ]] || { echo "new/delete outside the memory system:"; echo "$hits"; found=1; }
    hits="$(grep -rnE '\b(malloc|calloc|realloc|free)\(' --include=*.cpp --include=*.hpp "$ROOT/src" "$ROOT/include" | grep -v '/memory/' || true)"
    [[ -z "$hits" ]] || { echo "malloc family outside the memory system:"; echo "$hits"; found=1; }
    hits="$(grep -rnE 'ENGINE_ASSERT\([^;]*(\+\+|--|[^=!<>]=[^=])' --include=*.cpp --include=*.hpp "$ROOT/src" "$ROOT/include" || true)"
    [[ -z "$hits" ]] || { echo "side effect inside ENGINE_ASSERT:"; echo "$hits"; found=1; }
    hits="$(awk '/^add_executable\(/ { name = $0; inside = 1; has = 0 } inside && /new_delete\.cpp/ { has = 1 }
        inside && /\)/ { if (!has) print FILENAME ": " name; inside = 0 }' "$ROOT/CMakeLists.txt")"
    [[ -z "$hits" ]] || { echo "executable without src/memory/new_delete.cpp:"; echo "$hits"; found=1; }
    grep -rqE 'static_assert\(sizeof\(KernelPushConstants\) *== *128' "$ROOT/include" "$ROOT/src" \
        || { echo "static_assert of the 128-byte KernelPushConstants is missing"; found=1; }
    [[ "$found" == 0 ]] && echo "lint: ok"
    return "$found"
}

# Lines of the Markdown headings of a doc, `line: # heading`, to read a section by offset instead of the file.
headings() { grep -nE '^#{1,4} ' "$ROOT/${1:-docs/archive/decisions.md}"; }

# Checks that the docs agree with the repo: every path they quote exists, the suites and the op count in status.md
# match ctest and OpKind, no finished task stays in todo.md. Scripts nothing refers to are only listed.
docs_check() {
    local failed=0 docs=("$ROOT/AGENTS.md" "$ROOT/CLAUDE.md" "$ROOT"/docs/*.md) path hits
    # Paths of other projects the docs analyse, not ours.
    local external='^(include/heman\.h|src/(distance|generate|ops)\.c|src/(island-shape|lava|map|noisy-edges)\.js|src/data/|src/model/)'
    while read -r path; do
        [[ "$path" == *'*'* || "$path" =~ $external ]] && continue
        [[ "$path" == lib/* ]] && path="assets/shaders/$path"
        [[ -e "$ROOT/$path" ]] || { echo "missing path quoted in the docs: $path"; failed=1; }
    done < <(grep -ohE '`(docs|src|include|tests|assets|tools|lib)/[A-Za-z0-9_./-]+`' "${docs[@]}" | tr -d '`' | sort -u)

    local ctest_exe="$CTEST" suites status_suites
    if [[ -f "$ROOT/build/Release/CTestTestfile.cmake" ]]; then
        suites="$(cd "$ROOT/build/Release" && "$ctest_exe" -N | grep -oE 'test_[a-z_0-9]+' | sort -u)"
        status_suites="$(grep -oE 'test_[a-z_0-9]+' "$ROOT/docs/status.md" | sort -u)"
        hits="$(diff <(echo "$suites") <(echo "$status_suites") || true)"
        [[ -z "$hits" ]] || { echo "suites differ between ctest (<) and docs/status.md (>):"; echo "$hits"; failed=1; }
    else
        echo "docs-check: build/Release is not configured, suites not compared"
    fi

    local words=(zero one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen
                 seventeen eighteen nineteen twenty twenty-one twenty-two twenty-three twenty-four twenty-five)
    local ops claimed
    ops="$(awk '/^enum class OpKind/ { on = 1; next } on && /^    Count/ { exit } on && /^    [A-Z][A-Za-z0-9]*( = 0)?,/ { n++ } END { print n }' \
        "$ROOT/include/engine/terrain/graph.hpp")"
    claimed="$(grep -oiE '[a-z-]+ ops:' "$ROOT/docs/status.md" | head -1 | cut -d' ' -f1 | tr 'A-Z' 'a-z' || true)"
    [[ "${words[$ops]:-$ops}" == "$claimed" ]] || { echo "docs/status.md says '$claimed ops', OpKind has $ops"; failed=1; }

    hits="$(grep -nE '^- \[x\]' "$ROOT/docs/todo.md" || true)"
    [[ -z "$hits" ]] || { echo "finished tasks left in docs/todo.md (they live in git):"; echo "$hits"; failed=1; }

    # One pass for every name, `path:name` per hit; a script naming itself does not count.
    local scripts mentioned script
    scripts="$(git -C "$ROOT" ls-files 'assets/scripts/*.lua')"
    mentioned="$(cd "$ROOT" && { grep -rowFI --exclude-dir=actual -f <(sed 's|.*/||; s|\.lua$||' <<<"$scripts") \
        docs tests assets/lua assets/scripts src 2>/dev/null || true; } \
        | awk -F: '{ n = split($1, p, "/"); if (p[n] != $NF ".lua") print $NF }' | sort -u)"
    while read -r script; do
        grep -qxF "$(basename "$script" .lua)" <<<"$mentioned" || echo "warning: no doc, test or script mentions $script"
    done <<<"$scripts"
    [[ "$failed" == 0 ]] && echo "docs-check: ok"
    return "$failed"
}

# Runs a one-line script through terrain_export and prints what the engine said about it, without the log prefix.
# The sandbox has no pcall, so every question about the API is one script and one error.
ask_engine() {
    local dir="$ROOT/out/api" body="$1" file
    file="$dir/$(echo "$body" | md5sum | cut -c1-12).lua"
    printf 'local function main(params)\n%s\nend\nreturn main\n' "$body" > "$file"
    { "$ROOT/build/Release/bin/terrain_export" --script "$file" --min 0,0 --max 64,64 --resolution 8 \
        --out "$dir/run" --log-level error 2>&1 || true; } \
        | sed -n '/\[script\] /{s/.*\[script\] //; s/^main failed: //; s/^[^ ]*:[0-9]*: //; p}' | head -1
}

# One function: the keys it accepts, from its own unknown-key error. A call that first needs a graph value under
# some key is given one and asked again; if it still refuses, its own message (which names what it needs) is kept.
api_probe() {
    local name="$1" said given="" try
    for try in 1 2 3 4; do
        said="$(ask_engine "    return { h = { value = $name{ ${given}zz_probe = 1 }, range = { 0, 1 } } }")"
        [[ "$said" =~ needs\ a\ graph\ value\ as\ \'([a-z_]+)\' ]] || break
        given+="${BASH_REMATCH[1]} = Terrain.Const{ value = 0 }, "
    done
    if [[ "$said" == *"does not take 'zz_probe'; it accepts "* ]]; then echo "$name: ${said##*it accepts }"
    else echo "$name ! $said"; fi
}

# The catalogue, built from the engine once per build of terrain_export (about 20 s) and cached next to it:
# `name: key, key, ...`, or `name ! <what it needs first>`. With a name, only the lines that match, and the line
# of the reference where that function is described, to read only that section.
api() {
    local exe="$ROOT/build/Release/bin/terrain_export.exe" cache="$ROOT/build/Release/api.txt"
    test -x "$exe" || { echo "dev.sh api: build Release first" >&2; exit 1; }
    if [[ ! -f "$cache" || "$exe" -nt "$cache" ]]; then
        mkdir -p "$ROOT/out/api"
        local tables names
        tables="$(ask_engine '    local t = {}
    for k, v in pairs(_G) do if type(v) == "table" and getmetatable(v) == nil and k:match("^%u") then t[#t + 1] = k end end
    error(table.concat(t, " "), 0)')"
        names="$(for table in $tables; do ask_engine "    local t = {}
    for k, v in pairs($table) do if type(v) == \"function\" and k:match(\"^%a\") then t[#t + 1] = \"$table.\" .. k end end
    error(table.concat(t, \" \"), 0)" & done; wait)"
        echo "$names" | tr ' ' '\n' | grep . | sort | xargs -P 4 -I{} "$0" _api-probe {} | sort > "$cache.tmp"
        mv "$cache.tmp" "$cache"
    fi
    [[ -z "${1:-}" ]] && { cat "$cache"; return; }
    grep -i -- "^[^ :]*$1" "$cache" || { echo "dev.sh api: no function matches $1" >&2; exit 1; }
    local name; name="$(grep -i -m1 -o -- "^[^ :]*$1[^ :]*" "$cache")"
    # Its own heading, else its table's (`Lands.*`), else its first mention.
    local ref="$ROOT/docs/lua_reference.md" line
    line="$(grep -n -m1 -E "^#+ .*\`${name}\b" "$ref" || grep -n -m1 -E "^#+ .*\`${name%%.*}\.\*" "$ref" \
        || grep -n -m1 -F "$name" "$ref" || true)"
    if [[ -n "$line" ]]; then echo "docs/lua_reference.md:${line%%:*}"; else echo "(not in docs/lua_reference.md)"; fi
}

case "${1:-}" in
    build) shift; build "$@" ;;
    api) shift; api "$@" ;;
    _api-probe) api_probe "$2" ;;
    test) shift; run_tests "$@" ;;
    review) shift; review "$@" ;;
    sweep) shift; sweep "$@" ;;
    dem) shift; dem "$@" ;;
    lands) shift; lands "$@" ;;
    lands-check) shift; lands_check "$@" ;;
    clean) shift; clean "$@" ;;
    lint) lint ;;
    docs-check) docs_check ;;
    headings) shift; headings "$@" ;;
    *) sed -n '2,21p' "$0"; exit 2 ;;
esac
