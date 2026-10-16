# NVRM.kext build harness

NullMoth publishes the XNU glue for `NVRM.kext` (`kexts/NVRM`) but not the script that builds NVIDIA's resource
manager (RM) for Darwin: `kexts/NVRM/rmcc.py` refers to a `build-nvrm.sh` that was never released, and
`build-completion-archive.py` expects an already built `_out/Darwin_x86_64/{libnvkms.a,compile_cmds.sh}`.
`build/nvgfx/build_nvrm.sh` reconstructs that build from NVIDIA's own Makefiles and links `NVRM.kext` from source.

Status (2026-10-16): **complete through link and static comparison.** The rebuilt kext has the same imports, the
same exports and the same symbol inventory as the shipped 2.1.0 `NVRM`, and every RM and NVKMS function except four
in `nvkms-kapi.c` has identical code. It has not been loaded or run.

## Usage

```
build/nvgfx/build_nvrm.sh                       # all stages: tree rm kms completion glue link compare
build/nvgfx/build_nvrm.sh glue link compare     # only some stages
NVRM_PARITY=1 build/nvgfx/build_nvrm.sh         # reproduce the shipped 2.1.0 RM (no fsp patch)
```

Needs: Xcode Command Line Tools (tested with Apple clang 17.0.0 clang-1700.0.13.5, SDK 15.5 with Kernel.framework),
GNU make 3.81 (`/usr/bin/make`), python3, and NVIDIA `open-gpu-kernel-modules` with tag `610.57.04`
(`OGKM_SRC`, default `~/src/deps/open-gpu-610.57.04`). The compare stage uses the unpacked 2.1.0 package
(`NVGFX_REF_PKGROOT`) and, for the code comparison, `llvm-objdump` (`LLVM_OBJDUMP`, default MacPorts llvm-20).
`JOBS` defaults to 8. A full build takes about 45 s of compile on this machine (24 threads).

Output in `$NVGFX_OUT/nvrm` (default `~/src/NvGfx-build/nvrm`):

| path | what |
|---|---|
| `NVRM.kext` | the kext, `Info.plist` from `kexts/NVRM`, ad-hoc signed (sha256) |
| `ogkm/` | clone of the RM tree at the tag, fsp patch applied, with `src/nvidia/_out/Darwin_x86_64/{libnvkernel.a,compile_cmds.sh}` and `src/nvidia-modeset/_out/Darwin_x86_64/{libnvkms.a,compile_cmds.sh}` |
| `completion/libnvkms.a` | NVKMS archive with NullMoth's display-completion object (what the kext links) |
| `glue/` | the XNU glue objects |
| `compare.txt`, `codediff.txt` | comparison with the shipped binary |
| `logs/` | make and link logs |

`ogkm/` has the layout the other NullMoth scripts expect, so `OGKM=~/src/NvGfx-build/nvrm/ogkm` works for
`kexts/NVRM/rmcc.py`, `build/accel_build.sh` and `build/nmxaccel_build.sh` (checked: `accel_build.sh` builds
NVAccel against it).

## Stages

1. **tree**: `git clone --shared` of `OGKM_SRC`, checkout of `610.57.04`, then `kexts/NVRM/patches/610-fsp-timer-double-destroy.patch`
   with `git apply` (reversed again with `NVRM_PARITY=1`). Any other change in the tree stops the build. The
   display-completion patch is never applied to the tree.
2. **rm**: NVIDIA's `src/nvidia/Makefile` plus the overlay `build/nvgfx/nvrm/darwin.mk`:
   `make -f Makefile -f darwin.mk TARGET_OS=Darwin TARGET_ARCH=x86_64 NV_VERBOSE=1 EXTRA_CFLAGS="..." nvgfx-archive`.
   The overlay keeps every upstream object rule and CFLAG and only adds an archive target (`libtool -static`);
   upstream's `nv-kernel.o` step (GNU `ld -r --gc-sections`, `objcopy`) has no Mach-O equivalent and is not used.
   `compile_cmds.sh` is the dry run (`make -n -B ... nvgfx-objs`) filtered to the compile lines: one line per object,
   `<clang> <CFLAGS> -c <src> -o <obj> -MMD ... && sed ...`, exactly the format `rmcc.py` and
   `build-completion-archive.py` parse. 1055 objects.
3. **kms**: the same for `src/nvidia-modeset` with `SHADER_OBJS=` on the command line, 225 objects. The shader blobs
   (upstream: `xz` plus `ld --format=binary`) come from `kexts/NVRM/nvkms_shaders.cpp` instead, which defines the same
   `_binary_*_shaders_xz_*` symbols in `__TEXT,__const`.
4. **completion**: NullMoth's `kexts/NVRM/build-completion-archive.py`, unchanged. It compiles a patched copy of
   `kapi/src/nvkms-kapi.c` with that file's `compile_cmds.sh` line, swaps it into a copy of `libnvkms.a`, and replaces
   `g_nvid_string.o` (the id string reads "NullMoth Systems").
5. **glue**: the nine `.cpp` files with the kernel C++ flags of `build/accel_build.sh` (`-fapple-kext -mkernel
   -nostdinc -I Kernel.framework/Headers`, RM and NVKMS include paths, the `-D` set from `compile_cmds.sh` line 1);
   `nvrm_pageoff.c` with `rmcc.py` (RM's own line for `osapi.c`); `nvrm_kapi_fields.c` with
   `build/nvgfx/nvrm/linecc.py` (the same idea with the NVKMS line for `nvkms-kapi.c`, since it reads NVKMS structs).
6. **link**: `clang++ -fapple-kext -nostdlib -Xlinker -kext <glue objects> libnvkernel.a completion/libnvkms.a
   -lkmodc++ -lkmod -lcc_kext`, glue in the order recovered from the shipped text layout. `NVRM.cpp` already defines
   `kmod_info`, `_start` and `_stop`.
7. **compare**: `build/nvgfx/nvrm/compare_nvrm.py` (file size, load commands, sections, imports, exports, all symbol
   names with multiplicity, per-function size) and `build/nvgfx/nvrm/codediff.py` (per-function disassembly with
   addresses normalised and inter-function padding trimmed).

## The Darwin flags, and how they were recovered

Upstream `utils.mk` sets no OS define for `TARGET_OS=Darwin`, and `nvUnixVersion.h` refuses to compile without one.
The shipped binary answers which one NullMoth used:

- `NV_UNIX` is defined: `device_ctrl.c` and `kernel_gsp.c` call `os_is_grid_supported()` only under `NV_UNIX`, and the
  shipped code calls it.
- `NV_LINUX` is not: `nvlink_lib_top_lock_alloc` returns 0 straight away, so `TOP_LEVEL_LOCKING_DISABLED` kept its
  default of 1 (`nvlink_lock.c` sets it to 0 under `NV_LINUX`).
- `nvBldVer.h`'s Linux/BSD/Solaris block is in use (`"Private r610_85 rel/gpu_drv/r610/r610_85-4 unknown"`).
  So `-DNV_BSD` (`NV_SUNOS` would compile the RM identically).
- `-mkernel`: the shipped `libspdm_generate_session_data_key` is 1568 bytes; plain flags give 784, and `-mkernel`
  (or `-ftrivial-auto-var-init=zero`, which it implies) gives exactly 1568. For C++ `-mkernel` implies
  `-fapple-kext`, which explains the shipped DisplayPort library's local copies of vtables and destructors and its
  missing C1 constructor aliases. With it, the symbol inventory matches exactly and `__got` is the same size.
- `-fcf-protection=branch` (`endbr64` at every exported RM function) and the frame-pointer prologues come from
  NVIDIA's own Makefile with clang, unchanged.

Result: `EXTRA_CFLAGS="-arch x86_64 -mmacosx-version-min=15.0 -mkernel -DNV_UNIX -DNV_BSD"`.

## Comparison with the shipped 2.1.0 NVRM

Shipped: `pkgroot/Library/Extensions/NVRM.kext/Contents/MacOS/NVRM`, 16787808 bytes. Rebuilt with `NVRM_PARITY=1`:

| check | result |
|---|---|
| load commands | identical (3 segments, symtab, dysymtab, UUID, source version 0.0, code signature; no LC_BUILD_VERSION in either) |
| segment sizes | identical (`__TEXT` 0xd80000, `__DATA` 0x16f000) |
| sections | `__text` +131 B (+35 B with the fsp patch), `__cstring` +22, `__TEXT,__const` -64, `__DATA,__const` -24, `__data` -33, `__bss` -34, `__common` -2, `__got` identical |
| undefined (XNU KPI imports) | identical, 472 |
| exported symbols | identical, 17679 |
| all defined symbol names, with multiplicity | identical, 33828 |
| function code (normalised disassembly) | 29376 of 29431 identical in parity mode |
| file size | 16780832 B with the fsp patch (-6976), differences in `__LINKEDIT` only (string table 1236600 vs 1259680, ld64 version) |
| code signature | ad-hoc, sha256 only, like the shipped one |

The 55 functions that differ in parity mode:

- **51 in the glue** (`kexts/NVRM/*.cpp`, of 625 glue functions): register allocation and scheduling differences.
  Some of them (for example `nv_xnu_kmap_overlap`) match exactly when compiled with LLVM 20.1.8, so the shipped glue
  was built with a newer compiler than the CLT's clang-1700.0.13.5, most likely Xcode 26's Apple clang. LLVM 20.1.8
  does not match the rest either (63 differ), so the exact compiler is not available here. Source drift between the
  binary and the v2.1.0 tag cannot be ruled out from this alone.
- **4 in `nvkms-kapi.c`** (`ApplyModeSetConfig`, `GetFlipPendingStatus`, `NvKmsKapiLayerConfigToKms`,
  `nvKmsKapiProbe`): the completion object, also built with a newer compiler. With LLVM 20.1.8 three of these match
  but `CreateSurface`, `EnumerateGpus` and `ValidateDisplayMode` then differ (zero-initialisation stores).

Everything compiled from NVIDIA's tree with the original `compile_cmds.sh` lines (1054 RM objects, 224 NVKMS
objects) is code-identical, so the RM archives were built with the CLT 16 compiler, and the glue and completion object
later with a newer one (`patches/README.md`: "With the original NVIDIA 610.57.04 Darwin modeset archive and compile
commands already built").

Other findings:

- **610-fsp-timer-double-destroy.patch is not in the shipped 2.1.0 RM.** `kfspCleanupBootState_IMPL` calls
  `tmrEventDestroy` unconditionally and `tmrEventDestroyOSTimer_OSTIMER` never clears `pOSTmrCBdata`; both
  `NV_PRINTF` line numbers (278 and 226) are the unpatched ones. The harness applies the patch by default; with it
  only `kern_fsp.c` and `timer_ostimer.c` functions change (12 more differ).
- 133 RM/NVKMS objects are not in the kext because nothing references them (the whole NVSwitch library,
  `rm-gpu-ops.o`, unused softfloat, `NVRM_ID`). That is plain archive linking, no `-dead_strip`.
- Text layout differs (archive members load in a different order: the shipped image places RM, then NVKMS, then
  more RM), so addresses differ while the code is the same.
- `kexts/NVRM/nvidia-modeset-os-interface.h` is NVIDIA's MIT-licensed header with whitespace changes and its SPDX line
  changed to PolyForm-Noncommercial; it is not used by the RM build (NVKMS compiles against its own copy).

## Open problems

1. Exact byte parity of the glue and the completion object needs the compiler NullMoth used (probably Xcode 26). Not
   needed for function: the code differs in register choice and scheduling only, as far as checked.
2. Not run. Loading `NVRM.kext` is a kext install and needs Bash's approval; the next step is a boot test of this
   build in place of the shipped one, with the shipped kext kept as the rollback.
3. The NVKMS id string still says "NullMoth Systems" (from `build-completion-archive.py`).
4. `NVRMFB.kext` and `NVRMAGDC.kext` are separate; they are not built here.
