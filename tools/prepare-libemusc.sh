#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gerard Braad (Regroove | RGRV)
# Check out libEmuSC and apply the patch the units build with.

set -e

cd "$(dirname "$0")/.."

[ -d libemusc/libemusc/src ] || git submodule update --init libemusc

patch=$PWD/libemusc.patch

if git -C libemusc apply --reverse --check "$patch" 2>/dev/null; then
    echo "libEmuSC: patch already applied"
elif git -C libemusc apply --check "$patch" 2>/dev/null; then
    git -C libemusc apply "$patch"
    echo "libEmuSC: patch applied"
else
    echo "libEmuSC: libemusc.patch does not apply; is the submodule at f503c9be?" >&2
    exit 1
fi
