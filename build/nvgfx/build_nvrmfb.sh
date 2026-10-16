#!/bin/bash
# build_nvrmfb.sh: build NVRMFB.kext (the IOFramebuffer) from kexts/NVRMFB/fb into $OUTROOT/NVRMFB.kext.
# Upstream ships no script for it. Source: kexts/NVRMFB/fb/nvrm-fb.cpp (the shipped 2.1.0 binary carries its classes
# NVRMBootFB, NVRMFBClaim, NVRMNVDAFramebuffer, NVRMBacklightHandler and its debug.nvrmfb.* sysctls; kexts/NVRM/fb is
# an older copy without the vram/flip-latch sysctls). Its "../" headers come from kexts/NVRMFB; nvrm_vram_abi.h lives
# only in kexts/NVRM, so that directory is searched after kexts/NVRMFB. Flags: kext_flags.sh (same as NVAccel).
# Environment: OGKM (open-gpu-kernel-modules 610.57.04), OUTROOT (default ~/src/NvGfx-build/kexts).
set -uo pipefail
. "$(dirname "$0")/kext_flags.sh"
SRC="$REPO/kexts/NVRMFB"; OUT="$OUTROOT/NVRMFB"
rm -rf "$OUT"; mkdir -p "$OUT/NVRMFB.kext/Contents/MacOS"
$CXX "${KFLAGS[@]}" -I"$SRC" -I"$REPO/kexts/NVRM" "${RMINC[@]}" "${KMSINC[@]}" "${RMDEFS[@]}" -I"$SRC/fb" \
    -c "$SRC/fb/nvrm-fb.cpp" -o "$OUT/nvrm-fb.o" 2>"$OUT/fb.err" || { grep error "$OUT/fb.err" | head; exit 1; }
$CXX "${LDFLAGS[@]}" "$OUT/nvrm-fb.o" -o "$OUT/NVRMFB.kext/Contents/MacOS/NVRMFB" 2>"$OUT/fbld.err" || { head "$OUT/fbld.err"; exit 1; }
cp "$SRC/fb/Info.plist" "$OUT/NVRMFB.kext/Contents/Info.plist"
kext_report "$OUT/NVRMFB.kext/Contents/MacOS/NVRMFB"
