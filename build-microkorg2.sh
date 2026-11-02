#!/bin/bash
# SPDX-License-Identifier: MIT
# Build the RG55 microKORG2 oscillator in the logue SDK container.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

LOGUE_SDK_PATH="${LOGUE_SDK_PATH:-$HOME/Projects/logue/logue-sdk}"
PLATFORM_PATH="$LOGUE_SDK_PATH/platform"
CONTAINER_IMAGE="${CONTAINER_IMAGE:-ghcr.io/gbraad-logue/logue-sdk/dev-env:latest}"
UNIT=rg55_microkorg2

if [ ! -d "$PLATFORM_PATH/microkorg2" ]; then
    echo "Error: microKORG2 platform not found at $PLATFORM_PATH/microkorg2" >&2
    echo "       Set LOGUE_SDK_PATH to your logue-sdk checkout." >&2
    exit 1
fi

tools/prepare-libemusc.sh

echo "========================================"
echo "Building $UNIT for microKORG2"
echo "========================================"
echo "Platform:  $PLATFORM_PATH"
echo "libEmuSC:  $SCRIPT_DIR/libemusc"
echo "Container: $CONTAINER_IMAGE"
echo ""

mkdir -p bin

# The container user creates build/ and .dep/ inside the unit directory, and
# the SDK's post-link hook chowns them to its uid: the unit has to be the
# container user's.
chmod 777 "$UNIT" 2>/dev/null || true

# The platform is the SDK; the repo is read-only at /rg55 for common/, and
# libEmuSC is read-only at /emusc. The unit directory is the only writable one.
podman run --rm \
    -v "$PLATFORM_PATH:/workspace:rw" \
    -v "$SCRIPT_DIR/$UNIT:/workspace/microkorg2/$UNIT:rw" \
    -v "$SCRIPT_DIR:/rg55:ro" \
    -v "$SCRIPT_DIR/libemusc:/emusc:ro" \
    -h logue-sdk \
    "$CONTAINER_IMAGE" \
    /app/cmd_entry bash -c "
        source /app/microkorg2/environment && \
        cd /workspace/microkorg2/$UNIT && \
        make clean EMUSC_SRC=/emusc/libemusc/src COMMON_PATH=/rg55/common && \
        make install EMUSC_SRC=/emusc/libemusc/src COMMON_PATH=/rg55/common
    "

find "$UNIT" -maxdepth 1 -name '*.mk2unit' -exec cp {} bin/ \;

echo ""
echo "Built: bin/RG55.mk2unit"
echo "Install it in a SlotN folder under Units/Oscs on the microKORG2's drive,"
echo "then select 'RG-55' as the oscillator."
