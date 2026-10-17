#!/bin/bash
# NvGfx: rebuild NVRM.kext from source. Reconstructs NullMoth's unpublished Darwin build of NVIDIA's RM (their
# build-nvrm.sh is referenced by kexts/NVRM/rmcc.py but was never published). See build/nvgfx/NVRM-HARNESS.md.
#
#   build/nvgfx/build_nvrm.sh [stage ...]     stages: tree rm kms completion glue link compare   (default: all)
#
# Environment:
#   OGKM_SRC      NVIDIA open-gpu-kernel-modules checkout with tag 610.57.04   (default ~/src/deps/open-gpu-610.57.04)
#   NVGFX_OUT     output root, NVRM goes to $NVGFX_OUT/nvrm                    (default ~/src/NvGfx-build)
#   JOBS          make parallelism                                            (default 8; issue I-046, keep it moderate)
#   NVRM_PARITY   1 = reproduce the shipped 2.1.0 RM: skip 610-fsp-timer-double-destroy.patch, which 2.1.0 lacks
#   NVGFX_REF_PKGROOT  unpacked 2.1.0 package root holding the shipped NVRM.kext, for the compare stage
#
# Output: $O/NVRM.kext (ad-hoc signed), $O/ogkm (patched RM tree with _out/Darwin_x86_64/{libnvkernel.a,
# compile_cmds.sh} and the modeset equivalents, the layout rmcc.py, build-completion-archive.py and accel_build.sh
# read as OGKM=$O/ogkm), $O/completion/libnvkms.a, $O/glue/*.o, $O/logs, $O/compare.txt.
# Builds only. Nothing is loaded or installed.
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
R=$(cd "$(dirname "$0")/../.." && pwd)
K=$R/kexts/NVRM
H=$R/build/nvgfx/nvrm
O=${NVGFX_OUT:-$HOME/src/NvGfx-build}/nvrm
OGKM_SRC=${OGKM_SRC:-$HOME/src/deps/open-gpu-610.57.04}
TAG=610.57.04
OGKM=$O/ogkm
JOBS=${JOBS:-8}
PARITY=${NVRM_PARITY:-0}
REF=${NVGFX_REF_PKGROOT:-$HOME/Hackintosh/tooling/quarantine/nullmoth-release-2.3.5/x/pkgroot}
CC=$(xcrun -f clang); CXX=$(xcrun -f clang++)
export SDKROOT=$(xcrun --show-sdk-path); SDK=$SDKROOT; KHDR="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
mkdir -p "$O/logs"

# The Darwin target. Upstream utils.mk adds no OS define for Darwin and nvUnixVersion.h refuses to build without one.
# The shipped binary shows NV_UNIX defined, NV_LINUX not, and nvBldVer's LINUX/BSD/SUNOS block in use, so NV_BSD.
# -mkernel is the kext code model (static relocations, no red zone, zero-initialised locals) and implies -fapple-kext
# for the DisplayPort C++; the shipped function sizes match only with it.
DARWIN_CFLAGS="-arch x86_64 -mmacosx-version-min=15.0 -mkernel -DNV_UNIX -DNV_BSD"
MAKEVARS=( -f Makefile -f "$H/darwin.mk" NV_VERBOSE=1 CC="$CC" CXX="$CXX" LD="$(xcrun -f ld)" AR="$(xcrun -f ar)"
  TARGET_OS=Darwin TARGET_ARCH=x86_64 NV_BUILD_USER=nvgfx NV_BUILD_HOST=nvgfx EXTRA_CFLAGS="$DARWIN_CFLAGS" )

say() { echo "== $*"; }

stage_tree() {
  if [ ! -d "$OGKM/.git" ]; then
    git -C "$OGKM_SRC" rev-parse -q --verify "refs/tags/$TAG" >/dev/null || { echo "STOP: $OGKM_SRC has no tag $TAG"; exit 2; }
    git clone -q --shared "$OGKM_SRC" "$OGKM"
    git -C "$OGKM" -c advice.detachedHead=false checkout -q "$TAG"
  fi
  [ "$(git -C "$OGKM" rev-parse HEAD)" = "$(git -C "$OGKM" rev-parse "$TAG^{commit}")" ] || { echo "STOP: $OGKM is not at $TAG"; exit 2; }
  local p=$K/patches/610-fsp-timer-double-destroy.patch applied=0
  git -C "$OGKM" apply --reverse --check "$p" 2>/dev/null && applied=1
  if [ "$PARITY" = 1 ] && [ $applied = 1 ]; then git -C "$OGKM" apply --reverse "$p"; say "fsp patch removed (parity)"; fi
  if [ "$PARITY" != 1 ] && [ $applied = 0 ]; then git -C "$OGKM" apply "$p"; say "fsp patch applied"; fi
  # Nothing else may differ from the tag: the display-completion patch is applied by build-completion-archive.py
  # to a copy of nvkms-kapi.c, never to the tree.
  local dirty; dirty=$(git -C "$OGKM" status --porcelain --untracked-files=no | awk '{print $2}' | sort | tr '\n' ' ')
  local want=""; [ "$PARITY" != 1 ] && want="src/nvidia/src/kernel/gpu/fsp/kern_fsp.c src/nvidia/src/kernel/gpu/timer/timer_ostimer.c "
  [ "$dirty" = "$want" ] || { echo "STOP: unexpected changes in $OGKM: $dirty"; exit 2; }
  say "tree $OGKM at $TAG, fsp patch $([ "$PARITY" = 1 ] && echo off || echo on)"
}

# build one upstream component: objects, archive, compile_cmds.sh
build_component() {   # <dir under src> <archive name> [extra make vars]
  local dir=$1 lib=$2; shift 2
  local d=$OGKM/src/$dir out=$OGKM/src/$dir/_out/Darwin_x86_64
  # make does not track CFLAGS: start over whenever the flags or the compiler change
  local stamp="$DARWIN_CFLAGS | $("$CC" --version | head -1)"
  if [ -d "$out" ] && [ "$(cat "$out/.nvgfx-flags" 2>/dev/null)" != "$stamp" ]; then rm -rf "$out"; say "$dir: flags changed, clean build"; fi
  mkdir -p "$out"; echo "$stamp" > "$out/.nvgfx-flags"
  say "$dir: compiling (make -j$JOBS), log $O/logs/$dir.log"
  ( cd "$d" && nice make "${MAKEVARS[@]}" "$@" -j"$JOBS" NVGFX_LIB="$lib" nvgfx-archive ) > "$O/logs/$dir.log" 2>&1 \
    || { grep -E "error:|\*\*\*" "$O/logs/$dir.log" | head -20; echo "STOP: $dir build failed"; exit 3; }
  ( cd "$d" && make "${MAKEVARS[@]}" "$@" -n -B nvgfx-objs 2>/dev/null ) | grep -E ' -c [^ ]+ -o ' > "$out/compile_cmds.sh"
  local n_obj n_cmd; n_obj=$(( cd "$d" && make "${MAKEVARS[@]}" "$@" nvgfx-print-objs 2>/dev/null ) | wc -l | tr -d ' ')
  n_cmd=$(wc -l < "$out/compile_cmds.sh" | tr -d ' ')
  [ "$n_obj" = "$n_cmd" ] || { echo "STOP: $dir compile_cmds.sh has $n_cmd lines for $n_obj objects"; exit 3; }
  say "$dir: $n_obj objects, $out/$lib $(stat -f %z "$out/$lib") B, warnings $(grep -c 'warning:' "$O/logs/$dir.log" || true)"
}

stage_rm()  { build_component nvidia libnvkernel.a; }
stage_kms() { build_component nvidia-modeset libnvkms.a SHADER_OBJS=; }   # shaders come from kexts/NVRM/nvkms_shaders.cpp

stage_completion() {
  rm -rf "$O/completion"
  python3 "$K/build-completion-archive.py" --vendor "$OGKM/src/nvidia-modeset" --output "$O/completion" 2>&1 \
    | grep -v "has no symbols" | sed 's/^/   /'
  local syms; syms=$(nm "$O/completion/libnvkms.a" 2>/dev/null)
  grep -q " t _GetFlipPendingStatus$" <<< "$syms" || { echo "STOP: completion archive lacks GetFlipPendingStatus"; exit 4; }
}

GLUE_CXX=( NVRM os-xnu os-xnu2 nv_xnu_stubs os-nvkms-xnu nvkms_shaders nvrm_fbinfo nvrm_gpuva nvrm_surfshare )
# Our glue (power, phys control) is linked only where the sources exist, so the same harness reproduces an upstream
# tree (parity check against the shipped NVRM) and builds the fork.
OURS_CXX=(); OURS_C=(); [ -f "$K/nvrm_power.cpp" ] && OURS_CXX+=(nvrm_power); [ -f "$K/nvrm_physctl.c" ] && OURS_C+=(nvrm_physctl)
stage_glue() {
  local NV=$OGKM/src/nvidia KMS=$OGKM/src/nvidia-modeset C=$OGKM/src/common G=$O/glue
  rm -rf "$G"; mkdir -p "$G"
  # Same include set and flags as build/accel_build.sh (NullMoth's kernel C++ flags), RM's -D set from compile_cmds.sh.
  local RMINC=( -I"$NV/arch/nvalloc/unix/include" -I"$NV/arch/nvalloc/common/inc" -I"$NV/arch/nvalloc/common/inc/gsp"
    -I"$C/sdk/nvidia/inc" -I"$C/sdk/nvidia/inc/hw" -I"$C/inc" -I"$C/shared/inc" -I"$NV/inc" -I"$NV/inc/os" -I"$NV/inc/kernel"
    -I"$NV/kernel/inc" -I"$NV/interface" -I"$C/nvlink/interface" -I"$C/inc/swref" -I"$C/inc/swref/published"
    -I"$NV/inc/libraries" -I"$NV/src/libraries" -I"$NV/generated" -I"$NV/src/mm/uvm/interface"
    -I"$C/uproc/os/libos-v2.0.0/include" -I"$C/uproc/os/common/include" -I"$C/inc/displayport" -I"$C/nvlink/inband/interface" )
  local KMSINC=( -I"$KMS/os-interface/include" -I"$KMS/kapi/interface" -I"$KMS/kapi/include" -I"$C/unix/nvidia-push/interface"
    -I"$KMS/interface" -I"$KMS/include" -I"$C/unix/common/inc" -I"$C/modeset" -I"$C/unix/common/utils/interface" )
  local RMDEFS=(); while read -r d; do RMDEFS+=("$d"); done < <(head -1 "$NV/_out/Darwin_x86_64/compile_cmds.sh" | tr ' ' '\n' | grep -E '^-D' | sed -e 's/"//g' | grep -v -E '^-D(NVRM|_LANGUAGE_C)$')
  local CXXFLAGS=( -arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -mkernel -nostdinc -I"$KHDR" -I"$K" "${RMINC[@]}" "${KMSINC[@]}"
    -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT "${RMDEFS[@]}"
    -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common
    -Wall -Wno-unused-parameter -Wno-inconsistent-missing-override -Wno-unused-function -O2
    -ffile-prefix-map="$R"=/src -ffile-prefix-map="$HOME"=/build )
  local f pids=()
  for f in "${GLUE_CXX[@]}" "${OURS_CXX[@]}"; do
    "$CXX" "${CXXFLAGS[@]}" -c "$K/$f.cpp" -o "$G/$f.o" 2> "$G/$f.err" &
    pids+=($!)
  done
  local rc=0; for p in "${pids[@]}"; do wait "$p" || rc=1; done
  [ $rc = 0 ] || { grep -h "error:" "$G"/*.err | head -20; echo "STOP: glue C++ failed"; exit 5; }
  # RM-internal C glue: compiled with RM's own command line (rmcc.py, osapi.c's line) or NVKMS's (linecc.py, nvkms-kapi.c's).
  OGKM=$OGKM python3 "$K/rmcc.py" "$K/nvrm_pageoff.c" "$G/nvrm_pageoff.o" 2> "$G/nvrm_pageoff.err" || { cat "$G/nvrm_pageoff.err"; exit 5; }
  for f in "${OURS_C[@]}"; do OGKM=$OGKM python3 "$K/rmcc.py" "$K/$f.c" "$G/$f.o" 2> "$G/$f.err" || { cat "$G/$f.err"; exit 5; }; done
  python3 "$H/linecc.py" "$KMS" kapi/src/nvkms-kapi.c "$K/nvrm_kapi_fields.c" "$G/nvrm_kapi_fields.o" 2> "$G/nvrm_kapi_fields.err" || { cat "$G/nvrm_kapi_fields.err"; exit 5; }
  say "glue: $(ls "$G"/*.o | wc -l | tr -d ' ') objects, warnings $(cat "$G"/*.err | grep -c 'warning:' || true)"
}

stage_link() {
  local G=$O/glue X=$O/NVRM.kext
  rm -rf "$X"; mkdir -p "$X/Contents/MacOS"
  local ours=(); local f; for f in "${OURS_CXX[@]}" "${OURS_C[@]}"; do ours+=("$G/$f.o"); done
  # Input order as recovered from the shipped image's text layout: nv_xnu_stubs, nvkms_shaders (first in
  # __TEXT,__const), fbinfo, gpuva, surfshare, NVRM, os-nvkms-xnu, os-xnu, os-xnu2, kapi_fields, pageoff, then
  # archive members in load order (RM first: access_cntr_buffer.o follows pageoff in the shipped text).
  "$CXX" -arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -nostdlib -Xlinker -kext \
    "$G/nv_xnu_stubs.o" "$G/nvkms_shaders.o" "$G/nvrm_fbinfo.o" "$G/nvrm_gpuva.o" "$G/nvrm_surfshare.o" "$G/NVRM.o" \
    "$G/os-nvkms-xnu.o" "$G/os-xnu.o" "$G/os-xnu2.o" "$G/nvrm_kapi_fields.o" "$G/nvrm_pageoff.o" "${ours[@]}" \
    "$OGKM/src/nvidia/_out/Darwin_x86_64/libnvkernel.a" "$O/completion/libnvkms.a" \
    -lkmodc++ -lkmod -lcc_kext -o "$X/Contents/MacOS/NVRM" > "$O/logs/link.log" 2>&1 \
    || { grep -v "built for newer macOS" "$O/logs/link.log" | head -30; echo "STOP: link failed"; exit 6; }
  cp "$K/Info.plist" "$X/Contents/Info.plist"
  codesign -f -s - --digest-algorithm=sha256 "$X" > /dev/null 2>&1 || echo "note: ad-hoc codesign failed"
  say "NVRM.kext $(stat -f %z "$X/Contents/MacOS/NVRM") B sha $(shasum -a256 "$X/Contents/MacOS/NVRM" | cut -c1-12) $X"
}

stage_compare() {
  local S; S=$(find "$REF" -path '*NVRM.kext/Contents/MacOS/NVRM' 2>/dev/null | head -1)
  [ -n "$S" ] || { echo "note: no shipped NVRM under $REF, compare skipped"; return 0; }
  python3 "$H/compare_nvrm.py" "$S" "$O/NVRM.kext/Contents/MacOS/NVRM" "$O/compare.txt" | head -12
  if [ -x "${LLVM_OBJDUMP:-/opt/local/libexec/llvm-20/bin/llvm-objdump}" ]; then
    python3 "$H/codediff.py" "$S" "$O/NVRM.kext/Contents/MacOS/NVRM" 400 > "$O/codediff.txt"; head -1 "$O/codediff.txt"
  fi
  say "full report: $O/compare.txt"
}

STAGES=("$@"); [ ${#STAGES[@]} -gt 0 ] || STAGES=(tree rm kms completion glue link compare)
for s in "${STAGES[@]}"; do
  case $s in tree|rm|kms|completion|glue|link|compare) "stage_$s" ;; *) echo "unknown stage $s"; exit 1 ;; esac
done
