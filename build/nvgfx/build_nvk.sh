#!/bin/bash
# NvGfx: build the Vulkan driver (libvulkan_nouveau.dylib, Mesa NVK + nvk/nvk-macos.patch) from this checkout, the way
# NullMoth's build/build263.sh does it, but from repo-relative inputs and as the release build 2.0.5 and later ship
# (nvk/2.0.5/README.md: -Dbuildtype=release -Db_ndebug=true; build263.sh itself still says debugoptimized).
#
# Inputs:
#   NVGFX_DEPS  (default ~/src/deps)            mesa-<nvk/MESA_BASE>.tar.gz, a Mesa snapshot without .git
#   NVGFX_TOOLS (default ~/.local/nvgfx-llvm)   prefix with LLVMSPIRVLib.pc (lib/pkgconfig); mesa_clc and vtn_bindgen2
#                                              are built and installed into its bin/ by step 1 when missing
#   LLVM_CONFIG (default MacPorts llvm-20)     llvm-config of the LLVM that SPIRV-LLVM-Translator was built against
#   OGKM        (default ~/src/deps/open-gpu-610.57.04)  open-gpu-kernel-modules at tag 610.57.04; NVK's NVRM backend
#                                              includes its headers through nvkmd/nvrm/open-gpu-kernel-modules (a symlink)
#   NVGFX_OUT   (default ~/src/NvGfx-build)    work and output tree; this script uses NVGFX_OUT/nvk
#   NVGFX_REF_PKGROOT                          unpacked 2.1.0 pkg root to compare against (optional)
#   JOBS (default 8), HOST_TOOLS=1 forces step 1, CLEAN=1 re-extracts the NVK tree.
# Needs: CLT (SDK 15.5 matches 2.1.0), meson, ninja, pkg-config, glslangValidator, SPIRV-Tools (MacPorts), python3 with
# mako/yaml/packaging for meson, rustup stable, bindgen-cli and cbindgen in ~/.cargo/bin (cargo install bindgen-cli cbindgen). Meson downloads the Rust crate wraps
# NAK needs on first setup.
set -euo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
DEPS=${NVGFX_DEPS:-$HOME/src/deps}
T=${NVGFX_TOOLS:-$HOME/.local/nvgfx-llvm}
LLVM_CONFIG=${LLVM_CONFIG:-/opt/local/libexec/llvm-20/bin/llvm-config}
O=${NVGFX_OUT:-$HOME/src/NvGfx-build}/nvk
REF=${NVGFX_REF_PKGROOT:-$HOME/Hackintosh/tooling/quarantine/nullmoth-release-2.1.0/x/pkgroot}
J=${JOBS:-8}
OGKM=${OGKM:-$HOME/src/deps/open-gpu-610.57.04}
BASE=$(tr -d ' \n' < "$R/nvk/MESA_BASE")
TAR=$DEPS/mesa-$BASE.tar.gz
PATCH=$R/nvk/nvk-macos.patch
LLVM_LIB=$("$LLVM_CONFIG" --libdir)
export PATH=$T/bin:$HOME/.cargo/bin:/opt/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
export PKG_CONFIG_PATH=$T/lib/pkgconfig:$T/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
export LIBCLANG_PATH=${LIBCLANG_PATH:-$LLVM_LIB}   # bindgen (NAK)
[ -f "$TAR" ] || { echo "STOP: no $TAR"; exit 2; }
grep -q 'NVIDIA_VERSION = 610.57.04' "$OGKM/version.mk" 2>/dev/null || { echo "STOP: OGKM=$OGKM is not open-gpu-kernel-modules 610.57.04"; exit 2; }
mkdir -p "$O"
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$O/build_nvk.log"; }
extract() {  # extract() <dest>: clean Mesa tree, no .git (Mesa would stamp git-<sha> into the binary)
  rm -rf "$1" "$1.x"; mkdir -p "$1.x"; tar xzf "$TAR" -C "$1.x"; mv "$1.x/mesa-$BASE" "$1"; rmdir "$1.x"
  [ -e "$1/.git" ] && { echo "STOP: $1 has a .git"; exit 2; }; return 0
}

# 1. host tools: mesa_clc + vtn_bindgen2 from the unpatched tree (the patch's clc_helpers.cpp hunk includes
#    clang/Options/OptionUtils.h, which LLVM 20 does not have; NVK itself never compiles clc_helpers.cpp).
if [ -n "${HOST_TOOLS:-}" ] || [ ! -x "$T/bin/mesa_clc" ] || [ ! -x "$T/bin/vtn_bindgen2" ]; then
  log "step 1: host mesa_clc/vtn_bindgen2 with $("$LLVM_CONFIG" --version) into $T"
  extract "$O/host-src"
  printf "[binaries]\nllvm-config = '%s'\n" "$LLVM_CONFIG" > "$O/native-llvm.ini"
  rm -rf "$O/host-build"
  meson setup "$O/host-build" "$O/host-src" --native-file "$O/native-llvm.ini" --prefix="$T" -Dbuildtype=release \
    -Dmesa-clc=enabled -Dinstall-mesa-clc=true -Dllvm=enabled -Dshared-llvm=enabled \
    -Dc_link_args=-Wl,-rpath,$LLVM_LIB -Dcpp_link_args=-Wl,-rpath,$LLVM_LIB \
    -Dvulkan-drivers= -Dgallium-drivers= -Dplatforms= -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
    -Degl=disabled -Dglx=disabled -Dglvnd=disabled -Dshared-glapi=disabled -Dvulkan-layers= -Dtools= \
    -Dbuild-tests=false -Dexpat=disabled -Dzstd=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
    -Dvalgrind=disabled -Dxlib-lease=disabled -Dgallium-rusticl=false > "$O/host-setup.log" 2>&1 \
    || { echo "STOP: host meson setup failed"; tail -30 "$O/host-setup.log"; exit 3; }
  nice ninja -C "$O/host-build" -j"$J" > "$O/host-ninja.log" 2>&1 || { echo "STOP: host build failed"; grep -A5 FAILED "$O/host-ninja.log" | head -30; exit 4; }
  meson install -C "$O/host-build" > "$O/host-install.log" 2>&1 || { echo "STOP: host install failed"; tail "$O/host-install.log"; exit 4; }
  "$T/bin/mesa_clc" --help > /dev/null || { echo "STOP: $T/bin/mesa_clc does not run"; exit 4; }
fi

# 2. NVK: clean tree + patch, build263.sh's options, release build.
S=$O/mesa; B=$S/build-nvk
STAMP="$BASE $(shasum -a256 "$PATCH" | cut -c1-16)"
if [ -n "${CLEAN:-}" ] || [ "$(cat "$O/tree.stamp" 2>/dev/null)" != "$STAMP" ]; then
  log "step 2: fresh tree mesa-$BASE + $(basename "$PATCH")"
  extract "$S"
  patch -d "$S" -p1 --no-backup-if-mismatch < "$PATCH" > "$O/patch.log" 2>&1 || { echo "STOP: patch failed"; grep -v '^patching file' "$O/patch.log"; exit 3; }
  echo "$STAMP" > "$O/tree.stamp"
fi
ln -sfn "$OGKM" "$S/src/nouveau/vulkan/nvkmd/nvrm/open-gpu-kernel-modules"
if [ ! -f "$B/build.ninja" ]; then
  rm -rf "$B"
  meson setup "$B" "$S" -Dbuildtype=release -Db_ndebug=true -Dvulkan-drivers=nouveau -Dgallium-drivers= -Dplatforms= \
    -Degl=disabled -Dglx=disabled -Dglvnd=disabled -Dllvm=disabled -Dmesa-clc=system -Dshared-glapi=disabled \
    -Dvulkan-layers= -Dtools= -Dbuild-tests=false -Dexpat=disabled -Dzstd=disabled -Dlibunwind=disabled \
    -Dlmsensors=disabled -Dvalgrind=disabled -Dxlib-lease=disabled -Dgallium-rusticl=false -Dspirv-tools=disabled \
    > "$O/setup.log" 2>&1 || { echo "STOP: meson setup failed"; tail -30 "$O/setup.log"; exit 3; }
fi
meson configure "$B" "-Dc_args=-ffile-prefix-map=$S=/src -ffile-prefix-map=$HOME=/build" \
  "-Dcpp_args=-ffile-prefix-map=$S=/src -ffile-prefix-map=$HOME=/build" \
  "-Drust_args=--remap-path-prefix=$S=/src --remap-path-prefix=$HOME=/build" > "$O/configure.log" 2>&1 \
  || { echo "STOP: meson configure failed"; tail -5 "$O/configure.log"; exit 3; }
log "step 2: ninja -j$J"
t0=$(date +%s)
set +e; nice ninja -C "$B" -j"$J" -k 0 src/nouveau/vulkan/libvulkan_nouveau.dylib > "$O/ninja.log" 2>&1; rc=$?; set -e
log "ninja rc=$rc $(( $(date +%s) - t0 ))s errors=$(grep -c -E 'error(\[E[0-9]+\])?:' "$O/ninja.log" || true) FAILED=$(grep -c '^FAILED' "$O/ninja.log" || true)"
[ $rc = 0 ] || { grep -A8 '^FAILED' "$O/ninja.log" | head -40; exit 4; }

# 3. checks: no strong "dynamically looked up" references (dlopen in WindowServer fails on them), strip -S, compare.
L=$B/src/nouveau/vulkan/libvulkan_nouveau.dylib
U=$(nm -m "$L" | grep '(undefined)' | grep -v weak | grep 'dynamically looked up' || true)
[ -z "$U" ] || { echo "STOP: unresolved strong references:"; echo "$U"; exit 5; }
D=$O/libvulkan_nouveau.dylib; cp "$L" "$D"; strip -S "$D"   # debug-map stabs name every .o by absolute path
log "built: $(shasum -a256 "$D" | cut -c1-12) uuid $(otool -l "$D" | awk '/uuid/{print $2; exit}') $(stat -f %z "$D") bytes  $D"
SHIP=$(find "$REF" -name libvulkan_nouveau.dylib 2>/dev/null | head -1 || true)
if [ -n "$SHIP" ]; then
  nm -gU "$SHIP" | awk '{print $3}' | sort > "$O/exports.2.1.0"; nm -gU "$D" | awk '{print $3}' | sort > "$O/exports.built"
  otool -L "$SHIP" | tail -n +2 | awk '{print $1}' > "$O/libs.2.1.0"; otool -L "$D" | tail -n +2 | awk '{print $1}' > "$O/libs.built"
  log "exports: 2.1.0 $(wc -l < "$O/exports.2.1.0" | tr -d ' ') built $(wc -l < "$O/exports.built" | tr -d ' ')" \
      "missing $(comm -23 "$O/exports.2.1.0" "$O/exports.built" | wc -l | tr -d ' ') extra $(comm -13 "$O/exports.2.1.0" "$O/exports.built" | wc -l | tr -d ' ')"
  comm -3 "$O/exports.2.1.0" "$O/exports.built" | head -40
  diff "$O/libs.2.1.0" "$O/libs.built" > /dev/null && log "linked libraries: same as 2.1.0" || { log "linked libraries differ:"; diff "$O/libs.2.1.0" "$O/libs.built" || true; }
  strings -a "$D" | grep -q 'desc_index > 0' && log "WARNING: assert strings present (not an NDEBUG build)" || true
fi
echo "$D"
