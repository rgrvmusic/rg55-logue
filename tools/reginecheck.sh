#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gerard Braad (Regroove | RGRV)
#
# reginecheck.sh -- run bin/RG55.drmlgunit through ReGINE's dlhost and check what comes back.
#
# ReGINE is Regroove | RGRV's logue runtime, a separate commercial offering
# (https://rgrv.nl/regine/); its drumlogue host is the `dlhost` application. Nothing of ReGINE is
# in this repository. What this checks, and what it leaves to the device, is docs/reginecheck.md.
#
#   tools/reginecheck.sh
#   tools/reginecheck.sh --seconds 3      longer renders
#
# Environment:
#   DLHOST          the dlhost application. Default: dlhost on PATH.
#   QEMU, SYSROOT   run an armhf dlhost here: `qemu-arm-static -L <armhf sysroot>`. Both are
#                   dropped when dlhost runs here anyway.
#   SC55_ROM_DIR    the five SC-55 ROM files, named in the message about the userfs.
#
# The unit reads its ROMs from the userfs path it was built with
# (/var/lib/drumlogued/userfs/Programs, under SYSROOT when qemu is used), so they have to be
# there on the machine dlhost runs on.

set -u

cd "$(dirname "${BASH_SOURCE[0]}")/.."

ROOT=$PWD
HDR=$ROOT/rg55_drumlogue/header.c
CONFIG=$ROOT/rg55_drumlogue/config.mk
UNIT_FILE=$ROOT/bin/RG55.drmlgunit
OUT=$ROOT/out/dlhost
TIMEOUT=${TIMEOUT:-120}
SECONDS_FULL=2.0
SC55_ROM_DIR="${SC55_ROM_DIR:-$HOME/Projects/gbraad-music/rfx/data/sc55}"
ROMS="sc55_rom1.bin sc55_rom2.bin sc55_waverom1.bin sc55_waverom2.bin sc55_waverom3.bin"

usage() {
    awk 'NR == 1 { next }
         /^# reginecheck\.sh/ { on = 1 }
         on { if (!/^#/) exit; sub(/^# ?/, ""); print }' "$0"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --seconds) SECONDS_FULL="${2:?--seconds takes a value}"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "reginecheck: unknown option '$1'" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

if [ ! -f "$UNIT_FILE" ]; then
    echo "reginecheck: no $UNIT_FILE (run ./build-drumlogue.sh)" >&2
    exit 1
fi

# ------------------------------------------------------------------- dlhost, and how to run it ---
# dlhost is a licensed application, not a build of something in a neighbouring checkout: it comes
# from DLHOST= or from PATH.
DLHOST=${DLHOST:-}
[ -z "$DLHOST" ] && DLHOST=$(command -v dlhost 2>/dev/null || true)
if [ -z "$DLHOST" ] || [ ! -x "$DLHOST" ]; then
    echo "reginecheck: no dlhost. ReGINE is a separate, licensed product:" >&2
    echo "             https://rgrv.nl/regine/" >&2
    echo "             Put its dlhost on PATH, or name it as DLHOST=/path/to/dlhost." >&2
    exit 1
fi

QEMU=${QEMU-qemu-arm-static}
USE_QEMU=
if "$DLHOST" --help >/dev/null 2>&1; then
    :
elif [ -n "$QEMU" ] && command -v "$QEMU" >/dev/null 2>&1; then
    if [ -z "${SYSROOT:-}" ]; then
        echo "reginecheck: $DLHOST is armhf and does not run here; set SYSROOT to an armhf sysroot" >&2
        exit 1
    fi
    if ! "$QEMU" -L "$SYSROOT" "$DLHOST" --help >/dev/null 2>&1; then
        echo "reginecheck: $DLHOST does not run under $QEMU -L $SYSROOT either" >&2
        exit 1
    fi
    USE_QEMU=1
else
    echo "reginecheck: $DLHOST does not run here (dlhost is armhf)" >&2
    echo "             Set SYSROOT= to an armhf sysroot to run it under ${QEMU:-qemu-arm-static}," >&2
    echo "             or run this on a 32-bit ARM host." >&2
    exit 1
fi

# The userfs the unit was compiled with, as the machine running dlhost sees it.
USERFS=""
[ -n "$USE_QEMU" ] && USERFS=$SYSROOT
USERFS=$USERFS/var/lib/drumlogued/userfs/Programs
missing=()
for rom in $ROMS; do
    [ -f "$USERFS/$rom" ] || missing+=("$rom")
done
if [ ${#missing[@]} -gt 0 ]; then
    echo "reginecheck: the unit reads the SC-55 ROMs from $USERFS" >&2
    echo "             missing: ${missing[*]}" >&2
    echo "             Install them there on the machine dlhost runs on (they are in" >&2
    echo "             $SC55_ROM_DIR here; the path itself is the userfs the unit was built for)." >&2
    exit 1
fi

mkdir -p "$OUT"

dl() {
    local log=$1; shift
    local rc
    if [ -n "$USE_QEMU" ]; then
        timeout "$TIMEOUT" "$QEMU" -L "$SYSROOT" "$DLHOST" --unit "$UNIT_FILE" "$@" >"$log" 2>&1
    else
        timeout "$TIMEOUT" "$DLHOST" --unit "$UNIT_FILE" "$@" >"$log" 2>&1
    fi
    rc=$?
    [ $rc -eq 124 ] && echo "reginecheck: timed out after ${TIMEOUT}s" >>"$log"
    return $rc
}

# ------------------------------------------------------------------------- the report's fields ---
metric()   { awk -v k="$2" '$1 == k { print $3; exit }' "$1"; }
frames_of(){ sed -n 's/^rendered *: *\([0-9][0-9]*\) frames.*/\1/p' "$1" | head -1; }
chans_of() { sed -n 's/^rendered *:.* \([0-9][0-9]*\) ch.*/\1/p' "$1" | head -1; }
field()    { sed -n "s/^$2 *: *//p" "$1" | head -1; }

panel_of() {
    sed -n 's/^ *\[ *\([0-9][0-9]*\)\] *\([^ ]*\) *\([^ ]*\) *min *\(-\{0,1\}[0-9][0-9]*\) *max *\(-\{0,1\}[0-9][0-9]*\) *init *\(-\{0,1\}[0-9][0-9]*\) *center *\(-\{0,1\}[0-9][0-9]*\).*/\2|\3|\4|\5|\6|\7/p' "$1"
}

# ------------------------------------------------------------- what the unit's own source says ---
hdr() { sed -n "s/^ *\.$1 *= *\"\{0,1\}\([^\",]*\)\"\{0,1\}[,U]* *$/\1/p" "$HDR" | head -1; }
panel_expected() {
    sed -n 's/^ *{ *\(-\{0,1\}[0-9][0-9]*\) *, *\(-\{0,1\}[0-9][0-9]*\) *, *\(-\{0,1\}[0-9][0-9]*\) *, *\(-\{0,1\}[0-9][0-9]*\) *, *k_unit_param_type_\([a-z_]*\) *,.*{"\([^"]*\)"}.*/\1|\2|\3|\4|\5|\6/p' "$HDR"
}

exp_name=$(hdr name)
exp_dev=$(hdr dev_id); exp_dev=${exp_dev%U}
exp_unit=$(hdr unit_id); exp_unit=${exp_unit%U}
exp_presets=$(hdr num_presets)
exp_params=$(hdr num_params)
exp_module=$(sed -n 's/^PROJECT_TYPE *:= *//p' "$CONFIG" | head -1)
mapfile -t EXP < <(panel_expected | grep -v '|none|')
PANEL_TEXT=$(printf '%s\n' "${EXP[@]}")
exp_note=$(awk -F'|' '$5 == "midi_note" { print $4; exit }' <<<"$PANEL_TEXT")
pidx=$(awk -F'|' '$6 == "PROGRAM" { print NR - 1; exit }' <<<"$PANEL_TEXT")
lidx=$(awk -F'|' '$6 == "LEVEL" { print NR - 1; exit }' <<<"$PANEL_TEXT")

for v in exp_name exp_dev exp_unit exp_module exp_params; do
    if [ -z "${!v}" ]; then
        echo "reginecheck: header.c/config.mk does not declare $v" >&2
        exit 1
    fi
done
if [ ${#EXP[@]} -eq 0 ] || [ -z "$exp_note" ] || [ -z "$pidx" ] || [ -z "$lidx" ]; then
    echo "reginecheck: header.c's panel has no PROGRAM, LEVEL or midi_note row" >&2
    exit 1
fi

# ------------------------------------------------------------------------------- the results ---
PASSED=0; FAILED=0; SKIPPED=0
pass() { printf '  [PASS] %s\n' "$1"; PASSED=$((PASSED + 1)); }
fail() { printf '  [FAIL] %s\n' "$1"; FAILED=$((FAILED + 1)); }
skip() { printf '  [SKIP] %s\n' "$1"; SKIPPED=$((SKIPPED + 1)); }
say()  { printf '  [note] %s\n' "$1"; }

gt() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a > b) }'; }

summary() {
    echo ""
    if [ $FAILED -eq 0 ]; then
        printf '%d checks passed' "$PASSED"
        [ $SKIPPED -gt 0 ] && printf ', %d skipped' "$SKIPPED"
        printf '. Renders and reports are in out/dlhost/.\n'
        exit 0
    fi
    printf '%d passed, %d FAILED' "$PASSED" "$FAILED" >&2
    [ $SKIPPED -gt 0 ] && printf ', %d skipped' "$SKIPPED" >&2
    printf '.\n' >&2
    exit 1
}

echo ""
echo "dlhost : $DLHOST${USE_QEMU:+ (under $QEMU -L $SYSROOT)}"
echo "unit   : $UNIT_FILE"
echo ""
echo "== RG55"

# ---- loads, and everything the header says about itself
L=$OUT/RG55.info.log
dl "$L" --info; rc=$?
if [ $rc -ne 0 ]; then
    if grep -q 'dev_id\|licence\|license\|another developer' "$L"; then
        skip "loads: this dlhost's licence refused the unit ($(head -1 "$L"))"
        say "ReGINE's Developer Version loads only its licensee's units; these checks need one"
        say "licensed for dev_id $exp_dev, which is the unit's own."
        summary
    fi
    fail "loads: --info exited $rc ($(head -1 "$L"))"
    summary
fi
pass "loads: --info"

got_name=$(field "$L" name)
if [ "$got_name" = "$exp_name" ]; then
    pass "name: $got_name"
else
    fail "the header says '$got_name', header.c declares '$exp_name'"
fi

got_ids=$(field "$L" ids)
want_ids=$(printf 'dev 0x%08x unit 0x%08x' "$((exp_dev))" "$((exp_unit))")
if [ "$got_ids" = "$want_ids" ]; then
    pass "ids: $got_ids"
else
    fail "the header says '$got_ids', header.c declares '$want_ids'"
fi

got_module=$(field "$L" module)
if [ "$got_module" = "$exp_module" ]; then
    pass "module: $got_module"
else
    fail "the header's module is '$got_module', config.mk builds '$exp_module'"
fi

got_presets=$(field "$L" presets)
if [ "$got_presets" = "$exp_presets" ]; then
    pass "presets: $got_presets"
else
    fail "the header says $got_presets presets, header.c declares $exp_presets"
fi

got_params=$(field "$L" params)
if [ "$got_params" = "$exp_params" ] && [ "$exp_params" = "${#EXP[@]}" ]; then
    pass "rows: $got_params declared"
else
    fail "the header declares $got_params rows, header.c has $exp_params (${#EXP[@]} in the panel)"
fi

# ---- the panel, against header.c
mapfile -t PANEL < <(panel_of "$L")
bad=0
if [ ${#PANEL[@]} -ne ${#EXP[@]} ]; then
    fail "panel: the header has ${#PANEL[@]} rows, header.c has ${#EXP[@]}"
    bad=1
fi
i=0
while [ $i -lt ${#EXP[@]} ]; do
    IFS='|' read -r emin emax ecen einit etype elabel <<<"${EXP[$i]}"
    if [ $i -ge ${#PANEL[@]} ]; then
        fail "panel [$i]: '$elabel' is missing from the header"
        bad=1
    else
        IFS='|' read -r label type min max init center <<<"${PANEL[$i]}"
        if [ "$label" != "$elabel" ] || [ "$type" != "$etype" ] || [ "$min" != "$emin" ] ||
           [ "$max" != "$emax" ] || [ "$init" != "$einit" ] || [ "$center" != "$ecen" ]; then
            fail "panel [$i]: the device reads '$label' $type $min..$max init $init center $center; header.c has $elabel $etype $emin..$emax init $einit center $ecen"
            bad=1
        fi
    fi
    i=$((i + 1))
done
[ $bad -eq 0 ] && pass "panel: ${#EXP[@]} rows, as header.c declares them"

# ---- the note render: not silent, finite, inside +/-1.0, and the WAV it says it wrote
WAV=$OUT/RG55.wav
L=$OUT/RG55.note.log
dl "$L" --note "$exp_note" --seconds "$SECONDS_FULL" --out "$WAV"; rc=$?
peak=$(metric "$L" peak); rms=$(metric "$L" rms)
if [ $rc -ne 0 ]; then
    fail "the note render exited $rc ($(tail -1 "$L"))"
elif [ "$peak" = "nan" ]; then
    fail "the note render produced non-finite samples ($(grep -m1 non-finite "$L"))"
elif [ -z "$peak" ] || [ -z "$rms" ]; then
    fail "the note render reported no peak or rms"
elif gt "$peak" 0.0 && ! gt "$peak" 1.0 && gt "$rms" 0.0; then
    pass "render: note $exp_note, ${SECONDS_FULL}s, peak $peak rms $rms"
else
    fail "the note render came back at peak $peak rms $rms (silence, or past the +/-1.0 the device takes)"
fi

if [ -f "$WAV" ]; then
    frames=$(frames_of "$L"); chans=$(chans_of "$L")
    if [ -n "$frames" ] && [ -n "$chans" ]; then
        want_bytes=$((44 + frames * chans * 2))
        got_bytes=$(stat -c %s "$WAV")
        if [ "$got_bytes" = "$want_bytes" ]; then
            pass "wav: $frames frames, $got_bytes bytes, as the report says"
        else
            fail "the WAV is $got_bytes bytes, the report's $frames frames are $want_bytes"
        fi
    else
        fail "the render report has no frame or channel count"
    fi
else
    fail "no WAV at $WAV"
fi

# ---- the gate path: the sequencer's own events, which have to render the same note
GWAV=$OUT/RG55.gate.wav
L=$OUT/RG55.gate.log
dl "$L" --note "$exp_note" --gate --seconds "$SECONDS_FULL" --out "$GWAV"; rc=$?
if [ $rc -ne 0 ]; then
    fail "the gate render exited $rc ($(tail -1 "$L"))"
elif [ -f "$WAV" ] && [ -f "$GWAV" ] && cmp -s "$WAV" "$GWAV"; then
    pass "gate: gate_on renders the same note as note_on, to the sample"
else
    fail "gate_on renders peak $(metric "$L" peak) rms $(metric "$L" rms), not the note's $peak/$rms"
fi

# ---- PROGRAM: a different instrument has to be a different sound
P0=$OUT/RG55.program$pidx-0.wav
P1=$OUT/RG55.program$pidx-1.wav
dl "$OUT/RG55.program0.log" --raw "$pidx=0" --note "$exp_note" --seconds "$SECONDS_FULL" --out "$P0"; rc0=$?
dl "$OUT/RG55.program1.log" --raw "$pidx=1" --note "$exp_note" --seconds "$SECONDS_FULL" --out "$P1"; rc1=$?
if [ $rc0 -ne 0 ] || [ $rc1 -ne 0 ]; then
    fail "the two PROGRAM renders exited $rc0 and $rc1"
elif [ -f "$P0" ] && [ -f "$P1" ] && ! cmp -s "$P0" "$P1"; then
    pass "program: PROGRAM 0 and 1 render differently"
else
    fail "PROGRAM 0 and 1 rendered the same sound"
fi

# ---- LEVEL: silence at the bottom, and louder at the top than at its default
LV0=$OUT/RG55.level-0.wav
LV100=$OUT/RG55.level-100.wav
dl "$OUT/RG55.level0.log" --raw "$lidx=0" --note "$exp_note" --seconds "$SECONDS_FULL" --out "$LV0"; rc0=$?
dl "$OUT/RG55.level100.log" --raw "$lidx=100" --note "$exp_note" --seconds "$SECONDS_FULL" --out "$LV100"; rc1=$?
p0=$(metric "$OUT/RG55.level0.log" peak); p100=$(metric "$OUT/RG55.level100.log" peak)
if [ $rc0 -ne 0 ] || [ $rc1 -ne 0 ]; then
    fail "the two LEVEL renders exited $rc0 and $rc1"
else
    if [ "$p0" = "nan" ] || { [ -n "$p0" ] && ! gt "$p0" 0.000001; }; then
        pass "level: 0 is silence (peak $p0)"
    else
        fail "LEVEL 0 rendered peak $p0, not silence"
    fi
    if [ "$p100" = "nan" ] || [ -z "$p100" ] || [ -z "$peak" ]; then
        fail "LEVEL 100 reported no usable peak ($p100)"
    elif gt "$p100" "$peak"; then
        pass "level: 100 (peak $p100) is louder than the default (peak $peak)"
    else
        fail "LEVEL 100 rendered peak $p100, not louder than the default's $peak"
    fi
fi

summary
