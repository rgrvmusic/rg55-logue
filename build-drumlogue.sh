#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gerard Braad (Regroove | RGRV)
# Build the RG55 drumlogue unit in the logue SDK container.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

LOGUE_SDK_PATH="${LOGUE_SDK_PATH:-$HOME/Projects/logue/logue-sdk}"
PLATFORM_PATH="$LOGUE_SDK_PATH/platform"
CONTAINER_IMAGE="${CONTAINER_IMAGE:-ghcr.io/gbraad-logue/logue-sdk/dev-env:latest}"
CONTAINER_RUNTIME="${CONTAINER_RUNTIME:-podman}"
UNIT=rg55_drumlogue

if [ ! -d "$PLATFORM_PATH/drumlogue" ]; then
    echo "Error: drumlogue platform not found at $PLATFORM_PATH/drumlogue" >&2
    echo "       Set LOGUE_SDK_PATH to your logue-sdk checkout." >&2
    exit 1
fi

tools/prepare-libemusc.sh

echo "========================================"
echo "Building $UNIT for drumlogue"
echo "========================================"
echo "Platform:  $PLATFORM_PATH"
echo "libEmuSC:  $SCRIPT_DIR/libemusc"
echo "Container: $CONTAINER_IMAGE"
echo ""

mkdir -p bin

# The container user (uid 584287) creates build/ and .dep/ inside the unit
# directory, so that directory has to be world-writable.
chmod 777 "$UNIT" 2>/dev/null || true

# The platform is the SDK; the repo is read-only at /rg55 for common/, and
# libEmuSC is read-only at /emusc. The unit directory is the only writable one.
"$CONTAINER_RUNTIME" run --rm \
    -v "$PLATFORM_PATH:/workspace:rw" \
    -v "$SCRIPT_DIR/$UNIT:/workspace/drumlogue/$UNIT:rw" \
    -v "$SCRIPT_DIR:/rg55:ro" \
    -v "$SCRIPT_DIR/libemusc:/emusc:ro" \
    -h logue-sdk \
    "$CONTAINER_IMAGE" \
    /app/cmd_entry bash -c "
        source /app/drumlogue/environment && \
        cd /workspace/drumlogue/$UNIT && \
        make clean EMUSC_SRC=/emusc/libemusc/src COMMON_PATH=/rg55/common && \
        make install EMUSC_SRC=/emusc/libemusc/src COMMON_PATH=/rg55/common
    "

find "$UNIT" -maxdepth 1 -name '*.drmlgunit' -exec cp {} bin/ \;

echo ""
echo "Built: bin/RG55.drmlgunit"
echo "Install it in /Units/Synths on the drumlogue, then select 'RG-55' on the device."
