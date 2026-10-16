#!/bin/bash
# build_nvaccel.sh [nvaccel|nmxaccel|all]: build the accelerator kexts from kexts/NVRM/accel/nvrm-accel.cpp for macOS 15
# and 26 into $OUTROOT/<NVAccel|NMXAccel>/<15|26>/<name>.kext, the Library/NullMoth/kexts/<major> layout of the package.
# Same command line as build/accel_build.sh and build/nmxaccel_build.sh (NMX_ACCEL selects NMXAccel, NM_TAHOE the 26
# build), with the RM -D set rebuilt in kext_flags.sh instead of NullMoth's private compile_cmds.sh.
# Environment: OGKM (open-gpu-kernel-modules 610.57.04), OUTROOT (default ~/src/NvGfx-build/kexts).
set -uo pipefail
. "$(dirname "$0")/kext_flags.sh"
SRC="$REPO/kexts/NVRM"; WHAT=${1:-all}

build_one() {   # build_one <NVAccel|NMXAccel> <15|26>
    local name=$1 major=$2 out="$OUTROOT/$1/$2" extra=() plist="$SRC/accel/Info.plist"
    [ "$name" = NMXAccel ] && { extra+=(-DNMX_ACCEL); plist="$SRC/accel/nmx/Info.plist"; }
    [ "$major" = 26 ] && extra+=(-DNM_TAHOE)
    rm -rf "$out"; mkdir -p "$out/$name.kext/Contents/MacOS"
    $CXX "${KFLAGS[@]}" -I"$SRC" "${RMINC[@]}" "${KMSINC[@]}" "${RMDEFS[@]}" ${extra[@]+"${extra[@]}"} \
        -I"$SRC/accel" -I"$SRC/accel/iofam" -I"$SRC/accel/re" \
        -c "$SRC/accel/nvrm-accel.cpp" -o "$out/accel.o" 2>"$out/accel.err" || { grep error "$out/accel.err" | head; return 1; }
    $CXX "${LDFLAGS[@]}" "$out/accel.o" -o "$out/$name.kext/Contents/MacOS/$name" 2>"$out/accelld.err" || { head "$out/accelld.err"; return 1; }
    cp "$plist" "$out/$name.kext/Contents/Info.plist"
    printf '%-3s ' "$major"; kext_report "$out/$name.kext/Contents/MacOS/$name"
}

ABI=$(sed -n 's/^#define NVRM_VRAM_ABI_VERSION \([0-9]*\)u.*/\1/p' "$SRC/nvrm_vram_abi.h")
[ -n "$ABI" ] || { echo "cannot read NVRM_VRAM_ABI_VERSION"; exit 1; }
case "$WHAT" in nvaccel) names=(NVAccel) ;; nmxaccel) names=(NMXAccel) ;; all) names=(NVAccel NMXAccel) ;; *) echo "usage: $0 [nvaccel|nmxaccel|all]"; exit 2 ;; esac
for n in "${names[@]}"; do for major in 15 26; do build_one "$n" "$major" || exit 1; done; done
echo "built from $(git -C "$REPO" rev-parse --short HEAD) (VRAM ABI $ABI)"
