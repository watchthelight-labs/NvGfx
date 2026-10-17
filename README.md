# NvGfx

> **Heads up:** a good portion of the changes in this fork are AI generated. I do understand
> how they work, and I'm happy to explain any of it. If something looks wrong or you have a
> question, open an issue or ping me on Discord (`watchthelight`).

NvGfx is my fork of [NullMoth's NVIDIA Metal driver for macOS](https://github.com/nullmoth/nvidia-macos-driver), kept for one machine: a Lenovo Legion 5 15IAX10 with an RTX 5070 Laptop GPU (`10DE:2D18`) driving its 2560x1600 165 Hz panel, running macOS Tahoe 26.7.1 through OpenCore. All of the driver is NullMoth's work; the `nvgfx` branch carries my changes on top of their 2.1.0, listed below. The upstream README is kept as [README.upstream.md](README.upstream.md), and `main` mirrors upstream unchanged.

Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)

This is source-available, not open source: the driver is under the PolyForm Noncommercial License 1.0.0 (`LICENSE`), so you may use, change and share it for noncommercial purposes only. Third-party parts keep their own terms (`NOTICE`).

## What the fork changes

| Change | Where | State |
|---|---|---|
| Terminal overdraw fix: framebuffer fetch render passes only for pipelines whose shader reads the framebuffer, and a precise stencil-export detector. 2.1.0 made every pass a fetch pass, which drew new text over old in Terminal | `plugin/nvmtl_vk.c`, `plugin/NVMTLObjects.m` | in daily use since 2.1.0-nvgfx.1 |
| Event-driven present path: asynchronous page flips, latch on the flip event or vblank, `debug.nvgfx_present_depth` | `kexts/NVRMFB/fb/nvrm-fb.cpp`, `kexts/NVRM/accel/nvrm-accel.cpp` | in daily use; flips latch at 165/s, window drags about 148 fps (shipped 2.1.0: about 54) |
| GPU power: `nvrmtgp=1` puts the GSP in TGP mode on AC at boot and follows `_PSR`, which lifts the RTX 5070 from the VBIOS default 50 W to 115 W; `nvrmgps=1` / `nvrmnvpcf=1` answer the GPS and NVPCF `_DSM`s; `nvrmphysctl=1` enables a root-only control escape for `nvgfx-smi` | `kexts/NVRM/nvrm_power.cpp`, `nvrm_physctl.c` | in daily use (115 W measured) |
| WindowServer shader cache: when the shared cache folder is refused by the sandbox, the plugin caches translated shaders in the process's own cache folder instead of re-translating every CoreAnimation shader each boot | `plugin/NVMTLLibraryLoad.m` | in daily use since nvgfx.20 |
| `nvgfx-smi`: read-only telemetry (power limit and draw, clocks, pstate, temperature, throttle reasons) through the RM's shared data; `nvgfx-burn`: a Metal load generator | `tools/nvgfx-smi`, `tools/nvgfx-burn` | works |
| Reproducible builds of the plugin, translator, NVK, NVRMFB, NVAccel and NVRM from this tree with the Command Line Tools, checked symbol by symbol against the shipped 2.1.0 binaries, and `package.sh` to swap them into NullMoth's installer package | `build/nvgfx/` | works; `build/nvgfx/KEXTS.md`, `NVRM-HARNESS.md` |
| WindowServer tracing and framebuffer-fetch emulation for the Dock blur investigation (`nvgfxwsfetch=1`, `nvgfxfetchcopy=1`, `NVMTL_NVGFX_WSTRACE`) | `plugin/` | inert unless switched on; the blur problem is not solved |

The notes in `notes/` are the working record for each of these: `I-048-overdraw.md` and `I-048-fix.md` (the Terminal fix), `present-path.md` (165 Hz), `power.md` (TGP and the platform `_DSM`s). `NVGFX.md` lists every switch the fork adds.

## What it can't do

- It is tested on one GPU, one panel and one laptop. Nothing here is a general release.
- It does not ship packages. You need NullMoth's 2.1.0 package (for the installer, the Mac app and the GSP firmware) and swap in the builds from this tree with `build/nvgfx/package.sh`.
- No sleep and wake (the laptop runs with sleep disabled).
- The Dock blur draws as blocks on this GPU (a WindowServer framebuffer-fetch problem); the tracing switches above exist to study it, not to fix it.
- Upstream's `AGENTS.md` and `CONTRIBUTING.md` are NullMoth's rules for their repository and are kept as they wrote them. For this fork, the note at the top applies: I read and test every change, and I can explain mine.

## Building

Everything builds on macOS with the Xcode Command Line Tools and a few dependencies kept outside the tree (NVIDIA open-gpu-kernel-modules 610.57.04 for the RM headers, a Mesa snapshot and Vulkan-Headers for NVK and the plugin, LLVM with SPIR-V for the translator). Each `build/nvgfx/build_*.sh` names its inputs at the top and defaults to `~/src/deps` and `~/src/NvGfx-build`:

```
build/nvgfx/build_plugin.sh       # NVMTLDriver
build/nvgfx/build_xlate.sh        # libnvmtl_translate
build/nvgfx/build_nvk.sh          # libvulkan_nouveau (NVK with NullMoth's patches)
build/nvgfx/build_nvrmfb.sh       # NVRMFB.kext
build/nvgfx/build_nvaccel.sh all  # NVAccel.kext for macOS 15 and 26
build/nvgfx/build_nvrm.sh         # NVRM.kext (see build/nvgfx/NVRM-HARNESS.md)
SHIPPED=<2.1.0 pkgroot> build/nvgfx/kextcmp/compare.sh   # symbols, sections, vtables against the shipped kexts
build/nvgfx/package.sh 2.1.0-nvgfx.N                      # installer package with the builds swapped in
```

The kext builds are deterministic (same bytes on rebuild) with Apple clang 17.0.0.

## Installing and rolling back

Install the package `package.sh` produces with NullMoth's own `install.sh`; it backs up what it replaces. Rolling back is NullMoth's `uninstall.sh`, or installing the shipped 2.1.0 package again. Boot-args, sysctls and environment switches are in `NVGFX.md`; with none of them set the fork's kexts behave like upstream apart from the Terminal fix and the present path.

## Credits and licence

NullMoth Systems wrote the driver. Third-party parts, per `NOTICE`: NVIDIA's open GPU kernel modules (MIT / GPL-2.0), Mesa NVK (MIT), metal2vulkan by Anees Iqbal (LGPL-3.0 or later, the base of `translator/`), NVIDIA GSP firmware under NVIDIA's firmware licence. The fork, like upstream, is under the PolyForm Noncommercial License 1.0.0.
