#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gerard Braad (Regroove | RGRV)
# The host check for the RG55 drumlogue unit: no container, no device, no cross compiler.
#
#   tools/hostcheck-drumlogue.sh
#
# test/drum/harness.cc is built from the unit's own unit.cc, header.c and the sources and -D's in
# its config.mk, then plays the unit through the callbacks the device calls: a gate, the gate's
# note as a note, a held key and a newer one, PROGRAM and both ends of LEVEL.
#
# Environment:
#   LOGUE_SDK_PATH  the logue SDK checkout. Default: ~/Projects/logue/logue-sdk
#   SC55_ROM_DIR    the five SC-55 ROM files. Default: ~/Projects/gbraad-music/rfx/data/sc55

set -e

cd "$(dirname "${BASH_SOURCE[0]}")/.."

LOGUE_SDK_PATH="${LOGUE_SDK_PATH:-$HOME/Projects/logue/logue-sdk}"
SC55_ROM_DIR="${SC55_ROM_DIR:-$HOME/Projects/gbraad-music/rfx/data/sc55}"
UNIT=rg55_drumlogue
EMUSC_SRC=libemusc/libemusc/src
SDK_DRUMLOGUE="$LOGUE_SDK_PATH/platform/drumlogue/common"
SDK_COMMON="$LOGUE_SDK_PATH/platform/common"

if [ ! -f "$SDK_DRUMLOGUE/unit.h" ]; then
    echo "hostcheck: the drumlogue SDK headers are not at $SDK_DRUMLOGUE" >&2
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

DEFS=($(udefs) -DRFX_USERFS_PROGRAMS_PATH="$ROM_LINK_DIR")
SRC=($(sources))

mkdir -p build
gcc -O1 -c "$UNIT/header.c" -o build/rg55-drum-header.o "${DEFS[@]}" \
    -I"$UNIT" -I"$SDK_DRUMLOGUE" -I"$SDK_COMMON"
g++ -std=gnu++17 -include emusc_compat.h -O1 -o build/hostcheck \
    test/drum/harness.cc "$UNIT/unit.cc" build/rg55-drum-header.o "${SRC[@]}" \
    -I"$UNIT" -Icommon -I"$EMUSC_SRC" -I"$SDK_DRUMLOGUE" -I"$SDK_COMMON" \
    "${DEFS[@]}" -lm

build/hostcheck
