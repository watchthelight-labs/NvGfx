# NvGfx
<!-- <wtl> fork: nvgfx | author: watchthelight </wtl> -->

NvGfx is my fork of [NullMoth's NVIDIA Metal driver for macOS](https://github.com/nullmoth/nvidia-macos-driver), kept for one machine: a Lenovo Legion 5 15IAX10 with an RTX 5070 Laptop GPU (`10DE:2D18`) driving its 2560x1600 165 Hz panel, running macOS Tahoe 26.7.1 through OpenCore. All of the driver is NullMoth's work. The `nvgfx-2.4.3` branch (the default) carries my changes on top of their 2.4.1, listed below; `nvgfx` is the older line on 2.1.0, kept as the rollback and for its trace tooling. The upstream README is kept as [README.upstream.md](README.upstream.md), and `main` mirrors upstream unchanged.

Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)

This is source-available, not open source: the driver is under the PolyForm Noncommercial License 1.0.0 (`LICENSE`), so you may use, change and share it for noncommercial purposes only. Third-party parts keep their own terms (`NOTICE`).

## What the fork changes

| Change | Where | State |
|---|---|---|
| Dock badge digits: CoreAnimation uploads only level 0 of the mipmapped texture it draws badge text into and samples it trilinear, so the digit vanished. The plugin fills the never-written levels from the upload | `plugin/NVMTLObjects.m` | on 2.3.5 and later |
| Scaled desktop modes: upstream [PR 273](https://github.com/nullmoth/nvidia-macos-driver/pull/273) by Liang Shuang, carried here ahead of its merge. NVKMS rejected every mode where the desktop is scaled to the panel; the patch scales at the head viewport instead | `kexts/NVRM/patches/610-viewport-scaling.patch` | on 2.3.5 and later; PR 273 still open upstream |
| GPU power: `nvrmtgp=1` puts the GSP in TGP mode on AC at boot and follows `_PSR`, which lifts the RTX 5070 from the VBIOS default 50 W to 115 W; `nvrmgps=1` / `nvrmnvpcf=1` answer the GPS and NVPCF `_DSM`s; `nvrmphysctl=1` enables a root-only control escape for `nvgfx-smi` | `kexts/NVRM/nvrm_power.cpp`, `nvrm_physctl.c` | in daily use (115 W measured) |
| WindowServer shader cache: when the shared cache folder is refused by the sandbox, the plugin caches translated shaders in the process's own cache folder instead of re-translating every CoreAnimation shader each boot | `plugin/NVMTLLibraryLoad.m` | in daily use |
| A8Unorm render targets take alpha in the red lane; `[[color(n)]]` reads get a zeroed image when framebuffer fetch is off | `plugin/`, `translator/` | in upstream 2.3.6 (PRs 270 and 271) |
| Boot hold that waits for the bring-up: upstream releases its IORegistry hold at settle + 40 s whatever the card is doing, so a slow GSP bring-up (40 to 48 s seen on this laptop, 2.6 s on other boots) let WindowServer in early, it asserted on the unarmed framebuffer and the desktop ran unaccelerated; the fork keeps holding in 10 s steps while go(2) runs, `nvrmholdcap=<s>` sets the ceiling | `kexts/NVRM/NVRM.cpp` | 2.4.3-nvgfx.1, not yet booted |
| `nvgfx-smi`: read-only telemetry (power limit and draw, clocks, pstate, temperature, throttle reasons) through the RM's shared data; `nvgfx-burn`: a Metal load generator | `tools/nvgfx-smi`, `tools/nvgfx-burn` | works |
| Reproducible builds of the plugin, translator, NVK, NVRMFB, NVAccel and NVRM from this tree with the Command Line Tools, checked symbol by symbol against the shipped binaries, and `package.sh` to swap them into NullMoth's installer package | `build/nvgfx/` | works; `build/nvgfx/KEXTS.md`, `NVRM-HARNESS.md` |
| Event-driven present path: asynchronous page flips, latch on the flip event or vblank | `kexts/NVRMFB/fb/nvrm-fb.cpp` on the `nvgfx` branch | 2.1.0 only so far; on 2.3.5 upstream's own pacing is in use and the port is pending |
| Plugin work from the 2.3.7 line: function-table pipeline links, per-module memo of newFunctionWithName, the ahead pass on its own queue, one fetch descriptor set per pass, refused links remembered, async IOSurface upload at bind, repeated pipeline binds skipped, the encoder's sampler variant kept | `plugin/` | on 2.4.1; each measured on its own before it goes upstream |
| Terminal overdraw fix (fetch render passes only for shaders that read the framebuffer) | `plugin/` on the `nvgfx` branch | 2.1.0 only; upstream 2.3.0 fixed the same thing |

The notes in `notes/` are the working record for each of these. `NVGFX.md` lists every switch the fork adds and what each package label contains.

## What it can't do

- It is tested on one GPU, one panel and one laptop. Nothing here is a general release.
- It does not ship packages. You need NullMoth's 2.4.3 package (for the installer, the Mac app and the GSP firmware) and swap in the builds from this tree with `build/nvgfx/package.sh`.
- No sleep and wake (the laptop runs with sleep disabled).
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
SHIPPED=<2.4.3 pkgroot> build/nvgfx/kextcmp/compare.sh   # symbols, sections, vtables against the shipped kexts
build/nvgfx/package.sh 2.4.3-nvgfx.N                      # installer package with the builds swapped in
```

The kext builds are deterministic (same bytes on rebuild) with Apple clang 17.0.0 and 21.0.0 against the 15.5 SDK (`SDKROOT`).

## Installing and rolling back

Install the package `package.sh` produces with NullMoth's own `install.sh`; it backs up what it replaces. Rolling back is NullMoth's `uninstall.sh`, or installing the shipped 2.4.1 package again. Boot-args, sysctls and environment switches are in `NVGFX.md`; with none of them set the fork's kexts behave like upstream apart from the scaled-mode patch.

## Credits and licence

NullMoth Systems wrote the driver. Third-party parts, per `NOTICE`: NVIDIA's open GPU kernel modules (MIT / GPL-2.0), Mesa NVK (MIT), metal2vulkan by Anees Iqbal (LGPL-3.0 or later, the base of `translator/`), NVIDIA GSP firmware under NVIDIA's firmware licence. The fork, like upstream, is under the PolyForm Noncommercial License 1.0.0.
