# Intel Xe on the NvGfx stack (branch `xe`)

This branch is where the Intel side of NvGfx comes together: a kernel driver for Intel Xe GPUs, the same Metal-to-Vulkan plugin that runs NVIDIA cards here, and Mesa's ANV Vulkan driver in between. The aim is one driver for every Xe part (Tiger Lake to Arrow Lake and Arc first, then Lunar Lake, Battlemage and Panther Lake) on macOS 11 to 26.

Status on 2026-10-21: the kernel driver builds and links, its first hardware run is queued, and ANV builds for macOS and passes a simulated test on every Intel generation. Nothing has rendered on Intel hardware with this branch yet; [VedCloud's Meteor Lake port](https://github.com/VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics) has, with a similar stack on macOS 15. Progress is tracked in [XeGfx's issues](https://github.com/watchthelight-labs/XeGfx/issues) and in [issue #12](https://github.com/watchthelight-labs/NvGfx/issues/12) here.

## What lives where

| Path | What it is |
|---|---|
| `gpu/xegfx/` | [XeGfx](https://github.com/watchthelight-labs/XeGfx) as a git subtree (BSD-2): the FreeBSD kernel API on XNU, the build harness, the checks. drm core and i915 come unchanged from FreeBSD's drm-kmod. See its [HOW-IT-WORKS](https://github.com/watchthelight-labs/XeGfx/blob/main/docs/HOW-IT-WORKS.md). |
| `gpu/xkpi/` | The build overlay: replacements for the few drm-kmod files that are GPL-only, so the kext contains no GPL-only code. Provenance in `gpu/xkpi/NOTICE.md`. |
| `gpu/userland/` | How to build Mesa ANV and libdrm for macOS (`BUILD.md`, `scripts/`), the Darwin patches, and the `vkprobe` test. |
| `kexts/NMIntelFB/` | A flip-only framebuffer for the built-in panel that adopts the mode the firmware lit (untested). |
| `kexts/IGPUConsoleRange/` | Lets macOS's own framebuffer driver use a firmware console that sits in stolen memory (tested on Arrow Lake in Hybrid mode). |
| `plugin/`, `kexts/NVRM/accel/` | The Metal plugin and accelerator. The plugin already loads ANV for Intel accelerators; the Intel accelerator build is the next step. |
| `build/nvgfx/build_xegfx.sh` | Builds the Intel kext with the overlay and every check. |

## Building the kernel driver

Sources go in `~/src/xe-src` (`linux`, `freebsd`, `drm-kmod` at `c16de0f`, and `linux-firmware` with `mtl_guc_70.bin` and `LICENSE.i915`), plus [kcabi](https://github.com/watchthelight-labs/kcabi) in `~/src/kcabi`. Then:

```
build/nvgfx/build_xegfx.sh
```

It compiles every source (418 today), refuses GPL-only sources or headers, links against public kernel interfaces only, and fails if the kext redefines a kernel symbol. Installing and testing: `gpu/xegfx/CONTRIBUTING.md`.

## Building ANV

See `gpu/userland/BUILD.md`. In short: libdrm `b97cbde` and Mesa `bffd919` with the two Darwin patches, `-Dvulkan-drivers=intel`, LLVM only at build time. The current futex code needs macOS 14.4 or later; older systems need a fallback (XeGfx issue tracker).

## Built on, and learned from

XeGfx is built on other people's work. Thanks to all of them.

**Code XeGfx builds or ships** (their licences apply to their files):

| Project | What we use |
|---|---|
| [freebsd/drm-kmod](https://github.com/freebsd/drm-kmod) | drm core, TTM, dma-buf and i915 sources, compiled unchanged |
| [FreeBSD LinuxKPI](https://github.com/freebsd/freebsd-src/tree/main/sys/compat/linuxkpi) and [FreeBSD kernel](https://github.com/freebsd/freebsd-src) | the Linux kernel API layer, plus `subr_eventhandler.c` and FreeBSD's headers |
| [Linux](https://github.com/torvalds/linux) i915 and DRM | the driver drm-kmod tracks; the `sync_file` uapi header |
| [linux-firmware](https://gitlab.com/kernel-firmware/linux-firmware) | Intel's GuC firmware (`LICENSE.i915`) |
| [nullmoth/nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver) | the Metal plugin and accelerator NvGfx forks; its old AMD/Intel port (commit `bf2dee3`) supplies the GPL-free dma-buf, sync_file and fence-unwrap replacements and the Mesa/libdrm Darwin patches used in NvGfx's `xe` branch |
| [Mesa](https://gitlab.freedesktop.org/mesa/mesa) ANV and [libdrm](https://gitlab.freedesktop.org/mesa/drm) | the planned Vulkan userspace |
| [steelbrain/metal2vulkan](https://github.com/steelbrain/metal2vulkan) | the Metal-to-SPIR-V translator inside the NvGfx plugin; its coverage table and refusals list, and its Arrow Lake ANV validation profile |
| [watchthelight-labs/kcabi](https://github.com/watchthelight-labs/kcabi) | kernel collection link and ABI checks |

**Projects we studied** (ideas, findings and warnings only; no code copied):

| Project | What we learned |
|---|---|
| [b00t0x/WhateverXe](https://github.com/b00t0x/WhateverXe), [WhateverXe-Graphics](https://github.com/b00t0x/WhateverXe-Graphics), [b00t0x/OpenCore-Legacy-Patcher](https://github.com/b00t0x/OpenCore-Legacy-Patcher) | Xe-LP on Apple's Tiger Lake driver across Big Sur to Tahoe; forcing BAR0 MMIO to strong uncached; stolen-memory decoding; adopting the firmware's pipes without a modeset; since 0.4.0 Tiger Lake laptop panels too, by reading panel facts from the firmware's VBT and keeping the eDP scrambler and framing bits in step with the panel during link training; shipping the graphics stack as a signed /Library package instead of System patches; 0.6.0 (October 2026) adds Alder Lake and Raptor Lake laptops, defers framebuffer opens until AGDC registration, and states Xe-LPG is not planned; Graphics 0.3.0 converts shaders to AIR with the system compiler |
| [sgiammori/NootedGreen](https://github.com/sgiammori/NootedGreen) | how far the Apple TGL donor approach reaches on Alder Lake and Raptor Lake |
| [JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe) | keeping the firmware's eDP mode, the boot-programmable mode trap in WindowServer, the GGTT[0] machine-check hazard, Tahoe IOSurface offsets; holding the boot plane dark until WindowServer owns the display, a scheduler fix for a context-status event that was being dropped, and blit paths that fail on wide or oddly pitched IOSurface planes; later, render-engine workarounds applied after every engine start, power rerouting so hybrid P-cores get correct HWP units, a per-command-buffer leak that grew WindowServer past 10 GB, and eDP backlight over DPCD AUX |
| [m4j2rpf766-crypto/IntelIGPU-Tahoe](https://github.com/m4j2rpf766-crypto/IntelIGPU-Tahoe) | handing the panel over from IONDRVFramebuffer on Tahoe; isolating the iGPU at boot |
| [VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics](https://github.com/VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics) (earlier: [VedCloud2/CoreUltraGraphics](https://github.com/VedCloud2/CoreUltraGraphics)) | the first i915-on-XNU port to drive real Intel hardware: an i915-derived kernel driver on LinuxKPI, Mesa ANV and a NullMoth-derived Metal plugin, with Metal and Vulkan working on a Meteor Lake Dell Latitude 5350 under macOS 15; GuC submission with Linux's own MTL firmware, a GPU-composited desktop, and two lessons for our LinuxKPI: a lost wake-up that looked like GuC timeouts, and starting the stolen-memory allocator after the firmware's console image; on 2026-10-23 they confirmed that ANV's sync_file probe can be passed from userspace with stand-in descriptors, the route XeGfx 0.0.12 takes |
| [kitsuneislife/nmx-intel](https://github.com/kitsuneislife/nmx-intel) | i915 through NullMoth's port lighting a Raptor Lake laptop panel on macOS, and a framebuffer that copies frames composited on another GPU to the Intel panel; its pitfalls notes taught us to reserve the boot framebuffer's stolen pages and GGTT range before i915 allocates, and to give the framebuffer an `IOFBDependentID` |
| [IT-EXPRESS-Bayern/LunarBridge-Hackintosh](https://github.com/IT-EXPRESS-Bayern/LunarBridge-Hackintosh) | Lunar Lake (Xe2) groundwork on Tahoe |
| [wallali77666-cyber/iris-xe-macos-driver](https://github.com/wallali77666-cyber/iris-xe-macos-driver) | a NullMoth-style stack aimed at Tiger Lake |
| [zep245/IrisXe-macos-driver](https://github.com/zep245/IrisXe-macos-driver) | i915 and Mesa ANV running on Tiger Lake under Sequoia through NullMoth's Intel path; sandboxed apps see no GPU because the sandbox refuses `opendir(/dev/dri)`, and the in-kernel LinuxKPI panics it ran into (thread_call limits, `cond_resched` inside `ggtt_pin`, TLB flush timeouts) |
| [CENSEDAXIS20025/ASUS-FX608LP-Hackintosh-Multiboot](https://github.com/CENSEDAXIS20025/ASUS-FX608LP-Hackintosh-Multiboot) | the same Core Ultra 9 275HX, 7D67 iGPU and RTX 5070 Laptop running macOS on the RTX with the iGPU blocked and the MUX set to discrete only |
| [samranahmad09/WhateverXe-RaptorLakeP](https://github.com/samranahmad09/WhateverXe-RaptorLakeP) | checking a bring-up by capturing i915's registers on Linux next to a macOS readout, and a blitter ring self-test |
| [lutzbichler/drm-kmod](https://github.com/lutzbichler/drm-kmod) (xe patches) and [drm-kmod #316](https://github.com/freebsd/drm-kmod/issues/316) | the state of the xe driver outside Linux |
| [drm-kmod #315](https://github.com/freebsd/drm-kmod/issues/315), [#418](https://github.com/freebsd/drm-kmod/pull/418), [#468](https://github.com/freebsd/drm-kmod/pull/468) | LinuxKPI bugs FreeBSD already fixed for Meteor Lake and Arc, to carry over |
| [openbsd/src](https://github.com/openbsd/src) inteldrm | attaching i915 in stages and deferring the heavy probe |
| [acidanthera/WhateverGreen](https://github.com/acidanthera/WhateverGreen) and [OpenCorePkg](https://github.com/acidanthera/OpenCorePkg) | the Intel framebuffer background every Hackintosh graphics project builds on |

