# NvGfx fork notes

The `nvgfx` branch is NullMoth's 2.1.0 plus the changes in the README table. This file is the index of what the fork adds and how to switch it. The reasoning behind each change is in `notes/`.

## Branches and versions

- `main`: upstream as pulled (tags `v2.1.0` .. `v2.3.5`).
- `nvgfx-2.3.5` (current): our changes on top of the upstream 2.3.5 tag. Packages built from it are labelled `2.3.5-nvgfx.N`
  (`package.sh` writes the label into `NVGFX-VERSION`). Carried over from `nvgfx`: the build harness (`build/nvgfx`), the
  NVRM power glue (`nvrmtgp` 115 W, `nvrmgps`/`nvrmnvpcf`, `nvrmphysctl`), the WindowServer shader-cache fallback, the
  A8Unorm render-target fix (plugin and translator) and the zeroed `[[color(n)]]` reads when framebuffer fetch is off.
  2.3.5-nvgfx.3 adds upstream PR 273 (Liang Shuang: scaled desktop modes through the head viewport) ahead of its merge.
  Added on this branch (2.3.5-nvgfx.2): the never-written mip levels of a mipmapped 8-bit texture are filled from its
  level-0 upload (CoreAnimation uploads only level 0 of the Dock badge text and samples it trilinear, so the digit never
  showed), and the sampler a shader gets at a slot nothing was bound to reads level 0 only.
  Not carried over, because 2.3.x covers the same ground or the change was not yet measured against it: the dropped-draw
  fetch fix (upstream 351f5ed), the per-pipeline fetch passes for the Terminal overdraw, the event-driven present path
  and the surface dirty-mark rewire. Both of the last two come back only on evidence (see `notes/`).
- `nvgfx`: our changes on top of the upstream 2.1.0 tag (`2.1.0-nvgfx.N`, last tested `2.1.0-nvgfx.20`). Kept for the
  trace tooling on `dock-trace` and as the rollback.

## Boot-args added by the fork

All default off. With none set the kexts behave as upstream, apart from the Terminal fix and the present path, which are always on.

| Boot-arg | Kext | Effect |
|---|---|---|
| `nvrmtgp=1` | NVRM | at boot on AC, put the GSP in TGP mode (115 W on the RTX 5070 Laptop instead of the 50 W default) and follow `_PSR` afterwards |
| `nvrmgps=1` | NVRM | answer the GPS `_DSM` on the GPU node, so the RM's platform request handler finds GPS_2X |
| `nvrmnvpcf=1` | NVRM | answer the NVPCF `_DSM` on `\_SB.NPCF`, as Linux does |
| `nvrmphysctl=1` | NVRM | root-only control escape (0xE4) used by `nvgfx-smi --phys` and `--ctrl` |
| `nvgfxwsfetch=1` | plugin | enable framebuffer fetch in WindowServer only (Dock blur investigation) |
| `nvgfxfetchcopy=1` | plugin | emulate framebuffer fetch with copies in WindowServer when fetch is off |

Upstream's own boot-args (`nvfb`, `nvaccel`, `nvfbheads`, `nvrmacpibl`, `-nvrmnoflip`, `-nvaccelnoheadless` and the rest) are unchanged.

## Sysctls

- `debug.nvgfx_present_depth`: how many flips may be in flight (default 2).
- `debug.nvgfx_fb_*`: present path counters (submits, latched, refused, superseded, timeouts, commit and latch times, vblank hops).
- `debug.nvgfx_dirty_*`: why NVAccel surface dirty marks were refused or re-wired (parked VRAM, no resource, scanout).
- `debug.nvgfx_tgp_*`: TGP mode state and the last control results.

## Environment switches (plugin)

Read from `/Library/GPUBundles/nvmtl/nvrm610.conf` in release builds, since the plugin unsets `NVMTL_*` variables at load.

| Switch | Effect |
|---|---|
| `NVMTL_NVGFX_LEGACY_FBFETCH=1` | every render pass is a fetch pass again (2.1.0 behaviour, brings the Terminal overdraw back) |
| `NVMTL_NVGFX_LEGACY_SEMU=1` | 2.1.0's broad stencil-export detector |
| `NVMTL_NVGFX_FETCH_COPY=1` | framebuffer fetch emulated by copies for every process |
| `NVMTL_NVGFX_FCOPY_FORCE_FAIL=1` | make the fetch-copy pipeline fail, to test the rebuild-without-it path |
| `NVMTL_NVGFX_SURFOUT_TRACE=1` | syslog trace of surface copy-outs |
| `NVMTL_NVGFX_WSTRACE=1` | WindowServer trace: samplers, pipelines, small render targets, the backdrop composite draw, draws that sample small square textures |

## Where things are

| Path | What |
|---|---|
| `plugin/nvmtl_vk.c`, `plugin/nvmtl_vk.h` | render pass kinds per pipeline, pass restart, fetch-copy emulation |
| `plugin/NVMTLObjects.m` | encoder changes for the pass restart, WindowServer trace |
| `plugin/NVMTLLibraryLoad.m` | shader cache fallback folder |
| `plugin/nvgfx_fetch_copy.h`, `plugin/nvgfx_wstrace.h` | the fetch-copy and trace helpers |
| `kexts/NVRMFB/fb/nvrm-fb.cpp`, `kexts/NVRMFB/fb/nvgfx_present_abi.h` | the present path |
| `kexts/NVRM/accel/nvrm-accel.cpp` | dirty-mark accounting, parked VRAM re-wiring |
| `kexts/NVRM/nvrm_power.cpp`, `nvrm_physctl.c`, `nvrm_physctl_abi.h` | TGP mode, platform `_DSM`s, control escape |
| `kexts/NVRM/os-xnu.cpp`, `os-xnu2.cpp`, `NVRM.cpp` | the glue the harness needed to rebuild NVRM from source |
| `nvk/nvgfx/01-nv_push-opencl.patch` | one build fix on top of NullMoth's NVK patches |
| `build/nvgfx/` | build scripts, the NVRM harness, comparison tools, `package.sh` |
| `tools/nvgfx-smi`, `tools/nvgfx-burn` | telemetry and load tools |
| `notes/` | the working notes behind each change |

## Measured on the test machine

RTX 5070 Laptop (`10DE:2D18`), 2560x1600 at 165 Hz, macOS 26.7.1 (25G241), OpenCore 1.0.8, package 2.1.0-nvgfx.20:

- Metal 3 device, accelerated desktop, Liquid Glass renders.
- Present path: flips latched at the panel rate (165/s), window drags about 148 fps; shipped 2.1.0 on the same machine did about 54.
- Power: 115 W enforced with `nvrmtgp=1` (VBIOS default 50 W; Windows runs the same GPU at 115 W with Dynamic Boost).
- Terminal overdraw from 2.1.0: gone since nvgfx.1.
- WindowServer CPU after boot: normal since nvgfx.20 (before: about 150 % while it re-translated every shader).
- Still wrong: the Dock's blur draws as blocks (a WindowServer framebuffer-fetch problem, under investigation with the trace switches).

## Credit

All of the driver is NullMoth's work unless a commit on the `nvgfx` branch says otherwise. The full upstream history is kept here unchanged. Bug reports and fixes that apply to everyone go to NullMoth first.

Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)

Distributed under the PolyForm Noncommercial License 1.0.0 (`LICENSE`, https://polyformproject.org/licenses/noncommercial/1.0.0): noncommercial use only. Third-party parts keep their own terms, listed in `NOTICE`.
