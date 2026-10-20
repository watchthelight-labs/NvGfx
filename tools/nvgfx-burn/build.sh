#!/bin/sh
# Build nvgfx-burn (Metal compute load). The kernel is compiled at run time.
# <wtl> fork: nvgfx | author: watchthelight </wtl>
set -e
cd "$(dirname "$0")"
xcrun clang -arch x86_64 -mmacosx-version-min=15.0 -O2 -Wall -fobjc-arc \
    -framework Foundation -framework Metal \
    -o nvgfx-burn nvgfx-burn.m
echo "built $(pwd)/nvgfx-burn"
