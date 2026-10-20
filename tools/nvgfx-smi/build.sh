#!/bin/sh
# Build nvgfx-smi. Headers come straight from the NVIDIA open-gpu tree.
# <wtl> fork: nvgfx | author: watchthelight </wtl>
set -e
cd "$(dirname "$0")"
OG="${OPEN_GPU:-$HOME/src/deps/open-gpu-610.57.04}"
[ -d "$OG/src/common/sdk/nvidia/inc" ] || { echo "open-gpu tree not found at $OG (set OPEN_GPU)" >&2; exit 1; }
xcrun clang -arch x86_64 -mmacosx-version-min=15.0 -O2 -Wall -Wno-unused-function \
    -I"$OG/src/common/sdk/nvidia/inc" \
    -I"$OG/src/nvidia/arch/nvalloc/unix/include" \
    -I"$OG/src/common/inc" \
    -framework IOKit -framework CoreFoundation \
    -o nvgfx-smi nvgfx-smi.c
echo "built $(pwd)/nvgfx-smi"
