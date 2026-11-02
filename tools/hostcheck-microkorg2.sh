#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gerard Braad (Regroove | RGRV)
# The host check for the RG55 microKORG2 oscillator: no container, no device, no cross compiler.
#
#   tools/hostcheck-microkorg2.sh
#
# The twin of tools/hostcheck-drumlogue.sh. test/mk2/harness.cc checks the stereo pair the unit
# writes against one engine driven by hand; test/mk2/notes.cc checks that no key leaves a note
# sounding. Both are built from the unit's own unit.cc, header.c and the sources and -D's in its
# config.mk, with test/mk2/stub/unit_osc.h standing in for the SDK header whose helpers are NEON.
#
# Environment:
#   LOGUE_SDK_PATH  the logue SDK checkout. Default: ~/Projects/logue/logue-sdk
#   SC55_ROM_DIR    the five SC-55 ROM files. Default: ~/Projects/gbraad-music/rfx/data/sc55

set -e

cd "$(dirname "${BASH_SOURCE[0]}")/.."

LOGUE_SDK_PATH="${LOGUE_SDK_PATH:-$HOME/Projects/logue/logue-sdk}"
SC55_ROM_DIR="${SC55_ROM_DIR:-$HOME/Projects/gbraad-music/rfx/data/sc55}"
UNIT=rg55_microkorg2
EMUSC_SRC=libemusc/libemusc/src
SDK_MK2="$LOGUE_SDK_PATH/platform/microkorg2/common"
SDK_COMMON="$LOGUE_SDK_PATH/platform/common"

if [ ! -f "$SDK_MK2/runtime.h" ] || [ ! -f "$SDK_COMMON/unit.h" ]; then
    echo "hostcheck: the microKORG2 SDK headers are not at $SDK_MK2" >&2
    echo "           Set LOGUE_SDK_PATH to your logue-sdk checkout." >&2
    exit 1
fi

ROMS="sc55_rom1.bin sc55_rom2.bin sc55_waverom1.bin sc55_waverom2.bin sc55_waverom3.bin"
for rom in $ROMS; do
    if [ ! -f "$SC55_ROM_DIR/$rom" ]; then
        echo "hostcheck: $SC55_ROM_DIR/$rom is missing" >&2
        echo "           Set SC55_ROM_DIR to the directory holding all five ROMs." >&2
        exit 1
    fi
done

./tools/prepare-libemusc.sh

# The unit's config.mk holds the -D's and the source list, so there is never a second copy here.
udefs() {
    sed -n 's/^UDEFS[[:space:]]*\(+*\)=[[:space:]]*//p' "$UNIT/config.mk" | tr '\n' ' ' |
        sed 's/-DRFX_USERFS_PROGRAMS_PATH=[^ ]*//g'
}
sources() {
    sed -n 's/^CXXSRC[[:space:]]*+= *\$(EMUSC_SRC)\///p' "$UNIT/config.mk" |
        sed "s|^|$EMUSC_SRC/|"
}

# The ROMs are linked into a scratch directory: the unit is pointed at it, and its rg55.log
# lands there rather than in the ROM set.
ROM_LINK_DIR=$(mktemp -d)
trap 'rm -rf "$ROM_LINK_DIR"' EXIT
for rom in $ROMS; do ln -s "$SC55_ROM_DIR/$rom" "$ROM_LINK_DIR/$rom"; done

DEFS=($(udefs) -DRFX_USERFS_PROGRAMS_PATH="$ROM_LINK_DIR" ${EXTRA_DEFS:-})
INCS=(-Itest/mk2/stub -I"$UNIT" -Icommon -I"$EMUSC_SRC" -I"$SDK_MK2" -I"$SDK_COMMON")
CXXFLAGS=(-std=gnu++17 -include emusc_compat.h -O1)

mkdir -p build/mk2
OBJ=()
for src in $(sources); do
    obj=build/mk2/$(basename "$src" .cc).o
    g++ "${CXXFLAGS[@]}" -c "$src" -o "$obj" "${INCS[@]}" "${DEFS[@]}"
    OBJ+=("$obj")
done
gcc -O1 -c "$UNIT/header.c" -o build/mk2/header.o "${DEFS[@]}" "${INCS[@]}"
g++ "${CXXFLAGS[@]}" -c "$UNIT/unit.cc" -o build/mk2/unit.o "${INCS[@]}" "${DEFS[@]}"

status=0
for tool in harness notes; do
    g++ "${CXXFLAGS[@]}" -o "build/mk2/$tool" "test/mk2/$tool.cc" \
        "${OBJ[@]}" build/mk2/unit.o build/mk2/header.o "${INCS[@]}" "${DEFS[@]}" -lm
    echo "== $tool"
    if ! "build/mk2/$tool"; then
        status=1
    fi
done
exit $status
