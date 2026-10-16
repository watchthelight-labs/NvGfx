# NVRMFB, NVAccel and NMXAccel: reproducible builds

Phase 3 of the NvGfx plan: build the framebuffer and accelerator kexts from this tree so they can replace the shipped
2.1.0 binaries next to the shipped NVRM.kext, then change them (165 Hz present path in `kexts/NVRMFB/fb/nvrm-fb.cpp`
and `kexts/NVRM/accel/nvrm-accel.cpp`).

## Build

```
build/nvgfx/build_nvrmfb.sh            # -> ~/src/NvGfx-build/kexts/NVRMFB/NVRMFB.kext
build/nvgfx/build_nvaccel.sh [nvaccel|nmxaccel|all]
                                       # -> ~/src/NvGfx-build/kexts/{NVAccel,NMXAccel}/{15,26}/<name>.kext
SHIPPED=<2.1.0 pkgroot> build/nvgfx/kextcmp/compare.sh   # symbol, section, vtable and plist check
```

`OGKM` points at open-gpu-kernel-modules 610.57.04 (default `~/src/deps/open-gpu-610.57.04`), `OUTROOT` at the output
root. Toolchain: Command Line Tools, Apple clang 17.0.0 (clang-1700.0.13.5), SDK 15.5, ld-1167.5. Each kext is one
translation unit and builds in about a second. The builds are deterministic (same md5 on rebuild).

## Which source ships

- NVRMFB 2.1.0 is `kexts/NVRMFB/fb`, not `kexts/NVRM/fb`. The shipped binary has `NVRMBootFB`, `NVRMFBClaim`,
  `NVRMNVDAFramebuffer`, `NVRMBacklightHandler` and the `debug.nvrmfb.vram_*` / `flip_latch*` sysctls, and our build
  of `kexts/NVRMFB/fb` reproduces its `__cstring` section byte for byte. `kexts/NVRM/fb` is an older copy
  (1656 lines against 3406). `kexts/NVRM/patches/README.md` says the same ("use the NVRMFB framebuffer source").
- `nvrm-fb.cpp` includes `nvrm_vram_abi.h`, which exists only in `kexts/NVRM`, so the build searches `kexts/NVRMFB`
  first and `kexts/NVRM` second. `nv-xnu.h` and `nvrm_surfshare_abi.h` differ between the two directories but the fb
  does not include either.
- NVAccel (both macOS 15 and 26) is `kexts/NVRM/accel/nvrm-accel.cpp`. `Library/Extensions/NVAccel.kext` is the same
  file as `Library/NullMoth/kexts/15/NVAccel.kext`.

## Flags

Kernel flags are the ones in `build/accel_build.sh`: `-arch x86_64 -fapple-kext -mkernel -nostdinc`, Kernel.framework
headers, `-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -std=c++17 -fno-rtti -fno-exceptions -fno-builtin
-fno-common -O2`; link `-nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext`. `-DNM_TAHOE` selects the macOS 26
build, `-DNMX_ACCEL` the NMXAccel build. Include paths are the RM and NVKMS header directories from the same script.

Upstream also adds the `-D` set from NullMoth's private `_out/Darwin_x86_64/compile_cmds.sh`. `kext_flags.sh`
rebuilds it from `src/nvidia/Makefile` and `utils.mk` of 610.57.04 (x86_64, Darwin, release). It does not matter:
with and without that set the fb and accel objects are byte-identical, so the kexts use none of those macros, even
through the NVIDIA headers.

The NVIDIA patches in `kexts/NVRM/patches` touch only `.c` files (`nvkms-kapi.c`, `kern_fsp.c`, `timer_ostimer.c`),
so the stock 610.57.04 headers give the same `NvKmsKapi*` layouts the shipped NVRM was built with.

## Comparison with shipped 2.1.0

| kext | size ref / new | exports | undefined | `__cstring` | data symbol sizes | vtable slots | Info.plist |
| --- | --- | --- | --- | --- | --- | --- | --- |
| NVRMFB | 306528 / 310560 | 171 = 171 | 471 = 471 | identical | identical | 1374, identical | identical |
| NVAccel 15 | 243512 / 239288 | 350 = 350 | 882 = 882 | identical | identical | 3936, identical | identical |
| NVAccel 26 | 246040 / 245880 | 418 = 418 | 856 = 856 | identical | identical | 3936, identical | identical |
| NMXAccel 15 | 227680 / 234144 | 336 / 350 | 868 / 875 | differs | 10 differ | 1 slot differs | identical |
| NMXAccel 26 | 235512 / 236712 | 412 / 418 | 843 / 849 | differs | 1 differs | identical | identical |

"Vtable slots identical" means every slot of every `__ZTV` resolves to the same symbol in both binaries.
The `__text` list of functions is identical for NVRMFB and NVAccel (187, 330 and 398 functions); only the code differs
(NVRMFB 94510 bytes against 93678, NVAccel 56233 against 56345). That is a different compiler: the shipped kexts were
not built with clang-1700.0.13.5 (MacPorts clang 20 gives 89438 bytes, further away), most likely a newer Xcode. The
shipped linker also puts `__got` after `__data` where ld-1167.5 puts it first. Neither binary has `LC_BUILD_VERSION`.

NMXAccel: the shipped 2.1.0 NMXAccel was built from an older accelerator source than the v2.1.0 tag. Ours adds the
`surface_gpu*` and `iop_copy_skip` sysctls, `NMXVidMemory::free`/`releaseVram`/`releaseParked`, and imports
`IOLockSleepDeadline`, `IOLockWakeup`, `clock_interval_to_deadline`, `absolutetime_to_nanoseconds`, `proc_selfname`,
`strcmp` (all xnu KPIs). It is not a flag problem. NMXAccel drives AMD and Intel GPUs and is not needed on the Legion.

## Interface to NVRM

Neither NVRMFB nor NVAccel links against NVRM: none of their undefined symbols is exported by NVRM, and neither lists
`com.nullmoth.NVRM` in `OSBundleLibraries`. They reach NVRM at run time through `callPlatformFunction` names and
registry properties (`nvkms-kapi`, `phys-for-va`, `NVRMGpuPower`, `NVRMGpuReady`, `NVRMBacklight`, `NVRMBootRaster`,
`nvGpuVaAlloc/Free/Map`, `nvNewAccelClient`, `nvSurfVram`, `nvSurfShareRm`, `nvSurfPageoff`, ...). All of those
strings are present in the shipped NVRM, and they sit in the `__cstring` sections that match byte for byte. The shared
versioned layouts are `NVRM_VRAM_ABI_VERSION 2`, `NVRM_GPUVA_ABI_VERSION 1`, `NVRM_FLIP_ABI_VERSION 2`; any change to
`nvrm_vram_abi.h`, `nvrm_gpuva_abi.h` or `nvrm_surfshare_abi.h` needs a matching NVRM, which we cannot build yet.

## Before a first boot test

- Code generation differs from the shipped build (compiler only). Behaviour should match, but timing-sensitive paths
  (flip latch, vblank pacing) are exactly what we plan to change, so first boot the unmodified rebuild and compare
  with N-63 (NullMoth 2.1.0) before changing anything.
- The kexts are unsigned and change the kernel collection: that needs Bash's approval, a rollback plan (shipped
  2.1.0 binaries, or back to NullMoth 2.0.2 as in N-64), installed the same way as the shipped package.
- Ship NVRMFB and NVAccel together with the shipped NVRM 2.1.0 only (patches/README: the allocation interface is
  paired). Do not mix with 2.0.2 NVRM.
- NVAccel 26 and NMXAccel are built but not needed on macOS 15.8.1.
