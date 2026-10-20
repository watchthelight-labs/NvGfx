#!/bin/bash
# <wtl> fork: nvgfx | author: watchthelight </wtl>
# commentpass.sh: prove that a comment-only pass left the code alone. Builds NVRMFB, NVAccel 15/26, the Metal plugin
# (release) and the NVRM glue objects (plus a relinked NVRM.kext) from a checkout, then compares the __TEXT,__text
# hash of every output before and after. A __cstring difference is allowed only where __LINE__ lands in a string, so
# those diffs are printed for the caller to judge.
#
#   commentpass.sh build <outdir> [checkout]            build everything from <checkout> (default: this repo) into <outdir>
#   commentpass.sh compare <outdir-before> <outdir-after>
#   commentpass.sh run <checkout-before> <checkout-after> [outroot]   both builds, then the comparison
#
# Environment: SDKROOT (default the 15.5 SDK), JOBS (default 8), NVRM_SHARED (an existing build/nvgfx/build_nvrm.sh
# output with the compiled RM tree and completion archive, default ~/src/NvGfx-build/nvrm; it is linked into <outdir>/nvrm
# so only the glue and link stages run). Builds only; nothing is installed.
set -uo pipefail
export SDKROOT=${SDKROOT:-/Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk}
export JOBS=${JOBS:-8}
NVRM_SHARED=${NVRM_SHARED:-$HOME/src/NvGfx-build/nvrm}
HERE=$(cd "$(dirname "$0")" && pwd)

do_build() {   # do_build <outdir> <checkout>
  local out=$1 repo=$2 rc=0
  mkdir -p "$out/nvrm"
  for d in ogkm completion; do
    [ -e "$out/nvrm/$d" ] || ln -s "$NVRM_SHARED/$d" "$out/nvrm/$d" || { echo "no $NVRM_SHARED/$d: run build_nvrm.sh first"; return 1; }
  done
  echo "== build $repo -> $out"
  OUTROOT=$out/kexts "$repo/build/nvgfx/build_nvrmfb.sh" || rc=1
  OUTROOT=$out/kexts "$repo/build/nvgfx/build_nvaccel.sh" all || rc=1
  RELEASE=1 NVGFX_OUT=$out "$repo/build/nvgfx/build_plugin.sh" || rc=1
  NVGFX_OUT=$out "$repo/build/nvgfx/build_nvrm.sh" glue link || rc=1
  return $rc
}

text_hash() { otool -s __TEXT __text "$1" | tail -n +3 | shasum | cut -c1-16; }
cstr_dump() { otool -v -s __TEXT __cstring "$1" | tail -n +3 | sed 's/^[0-9a-f]* *//'; }

outputs() {   # outputs <outdir>: name<TAB>path, in a fixed order
  local o=$1
  echo "NVRMFB	$o/kexts/NVRMFB/NVRMFB.kext/Contents/MacOS/NVRMFB"
  echo "NVAccel-15	$o/kexts/NVAccel/15/NVAccel.kext/Contents/MacOS/NVAccel"
  echo "NVAccel-26	$o/kexts/NVAccel/26/NVAccel.kext/Contents/MacOS/NVAccel"
  echo "NVMTLDriver	$o/plugin/NVMTLDriver"
  local f; for f in $(ls "$o/nvrm/glue/"*.o 2>/dev/null | sort); do echo "glue/$(basename "$f")	$f"; done
  echo "NVRM.kext	$o/nvrm/NVRM.kext/Contents/MacOS/NVRM"
}

do_compare() {   # do_compare <before> <after>
  local a=$1 b=$2 bad=0 name pa pb ha hb ca cb
  printf '%-26s %-18s %-18s %s\n' binary before.__text after.__text result
  while IFS=$'\t' read -r name pa; do
    pb=${pa/#$a/$b}
    if [ ! -f "$pa" ] || [ ! -f "$pb" ]; then printf '%-26s %-18s %-18s %s\n' "$name" - - "MISSING ($([ -f "$pa" ] || echo before) $([ -f "$pb" ] || echo after))"; bad=1; continue; fi
    ha=$(text_hash "$pa"); hb=$(text_hash "$pb")
    if [ "$ha" = "$hb" ]; then r=IDENTICAL; else r=DIFFERS; bad=1; fi
    ca=$(cstr_dump "$pa" | shasum | cut -c1-16); cb=$(cstr_dump "$pb" | shasum | cut -c1-16)
    [ "$ca" = "$cb" ] || r="$r (__cstring differs)"
    printf '%-26s %-18s %-18s %s\n' "$name" "$ha" "$hb" "$r"
    if [ "$ca" != "$cb" ]; then
      diff <(cstr_dump "$pa") <(cstr_dump "$pb") | grep '^[<>]' | head -20 | sed 's/^/    /'
    fi
  done < <(outputs "$a")
  [ $bad = 0 ] && echo "== all __text sections identical" || echo "== DIFFERENCES FOUND"
  return $bad
}

case ${1:-} in
  build)   do_build "${2:?outdir}" "${3:-$(cd "$HERE/../../.." && pwd)}" ;;
  compare) do_compare "${2:?before}" "${3:?after}" ;;
  run)     root=${4:-$HOME/src/NvGfx-build/commentpass}
           do_build "$root/before" "${2:?checkout-before}" && do_build "$root/after" "${3:?checkout-after}" && do_compare "$root/before" "$root/after" ;;
  *)       sed -n '3,15p' "$0"; exit 2 ;;
esac
