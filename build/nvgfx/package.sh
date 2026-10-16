#!/bin/bash
# NvGfx: make an installable package from the shipped NullMoth 2.1.0 pkgroot with our builds swapped in.
#   REF  = shipped pkgroot (default: the audited 2.1.0 package)
#   OUT  = output pkgroot (default ~/src/NvGfx-build/pkg/<label>)
#   Components to swap are given as env vars pointing at built files (unset = keep shipped):
#     NVGFX_PLUGIN (NVMTLDriver), NVGFX_XLATE (libnvmtl_translate.dylib), NVGFX_NVK (libvulkan_nouveau.dylib),
#     NVGFX_NVRM, NVGFX_NVRMFB, NVGFX_NVACCEL15, NVGFX_NVACCEL26 (.kext bundles)
# The result keeps install.sh/uninstall.sh unchanged; SHA256SUMS, BASELINE-IDENTITY.json and NVGFX-VERSION are rewritten.
set -euo pipefail
LABEL=${1:?usage: package.sh <label, e.g. 2.1.0-nvgfx.1>}
REF=${REF:-$HOME/Hackintosh/tooling/quarantine/nullmoth-release-2.1.0/x/pkgroot}
OUT=${OUT:-$HOME/src/NvGfx-build/pkg/$LABEL}
R=$(cd "$(dirname "$0")/../.." && pwd)
rm -rf "$OUT"; mkdir -p "$(dirname "$OUT")"; cp -Rp "$REF" "$OUT"
B=$OUT/Library/GPUBundles/NVMTLDriver.bundle
swap_file() { [ -n "${1:-}" ] || return 0; cp "$1" "$2"; codesign -f -s - "$2" 2>/dev/null; echo "swapped $(basename "$2") <- $1"; }
swap_kext() { [ -n "${1:-}" ] || return 0; rm -rf "$2"; cp -Rp "$1" "$2"; echo "swapped $(basename "$2") <- $1"; }
swap_file "${NVGFX_PLUGIN:-}" "$B/Contents/MacOS/NVMTLDriver"
swap_file "${NVGFX_XLATE:-}" "$B/Contents/MacOS/libnvmtl_translate.dylib"
NVK_DST=$(cd "$OUT" && find . -name libvulkan_nouveau.dylib | head -1); [ -n "${NVGFX_NVK:-}" ] && swap_file "$NVGFX_NVK" "$OUT/$NVK_DST"
swap_kext "${NVGFX_NVRM:-}" "$OUT/Library/Extensions/NVRM.kext"
swap_kext "${NVGFX_NVRMFB:-}" "$OUT/Library/Extensions/NVRMFB.kext"
swap_kext "${NVGFX_NVACCEL15:-}" "$OUT/Library/Extensions/NVAccel.kext"
[ -d "$OUT/Library/NullMoth/kexts/15/NVAccel.kext" ] && swap_kext "${NVGFX_NVACCEL15:-}" "$OUT/Library/NullMoth/kexts/15/NVAccel.kext"
[ -d "$OUT/Library/NullMoth/kexts/26/NVAccel.kext" ] && swap_kext "${NVGFX_NVACCEL26:-}" "$OUT/Library/NullMoth/kexts/26/NVAccel.kext"
codesign -f -s - "$B" 2>/dev/null   # the shipped bundle is ad-hoc signed as a bundle
# Rewrite the identity of the four paired kexts the same way the shipped file lists them, then every checksum.
/usr/bin/python3 - "$OUT" <<'PY'
import hashlib, json, os, sys
o = sys.argv[1]; p = os.path.join(o, "BASELINE-IDENTITY.json")
d = json.load(open(p))
for name, ent in d.get("paired_kexts", {}).items():
    f = os.path.join(o, "Library/Extensions", name + ".kext", "Contents/MacOS", name)
    if os.path.isfile(f):
        ent["sha256"] = hashlib.sha256(open(f, "rb").read()).hexdigest()
d["nvgfx"] = open(os.path.join(o, "NVGFX-VERSION")).read().strip() if os.path.exists(os.path.join(o, "NVGFX-VERSION")) else "nvgfx"
json.dump(d, open(p, "w"), indent=2)
PY
(cd "$OUT" && awk '{print $2}' SHA256SUMS | while read -r f; do shasum -a 256 "$f"; done > SHA256SUMS.new && mv SHA256SUMS.new SHA256SUMS)
printf '%s\nbase NullMoth %s, NvGfx %s\n' "$LABEL" "$(cat "$REF/VERSION")" "$(git -C "$R" rev-parse --short HEAD)" > "$OUT/NVGFX-VERSION"
(cd "$OUT" && shasum -a 256 -c SHA256SUMS --quiet) && echo "package ok: $OUT"
