# I-048: Terminal text overdraw with NullMoth 2.1.0

Read-only review of `git diff v2.0.4 v2.1.0` (20 commits, 2026-10-15/16). Nothing built or run. Line numbers are in v2.1.0.

Symptom: Terminal.app paints new glyphs over old ones in the same cells; old pixels are never erased. Visible in
screenshots (so it is composited content, not scan-out). Persists with `debug.nvaccel_surface_gpu_on=0` and with an
opaque profile. 2.0.2 / 2.0.4 are clean.

## First: how switches reach the driver

- `plugin/NVMTLObjects.m:80-124`: in a release build (`NVMTL_RELEASE`, set by `build/build_plugin.sh` when
  `RELEASE` is set, which is how packages are built) the plugin **unsets every `NVMTL_*`, `NVK_*`, `NAK_*`, `NVRM_*`
  environment variable at load**, then reads `/Library/GPUBundles/nvmtl/nvrm610.conf`. So `open --env` or
  `launchctl setenv` tests do nothing on the shipped build. Every test below is a line in nvrm610.conf (put it inside the
  `# --- my settings, kept on update ---` block), which applies to every process, WindowServer included, after a log
  out / log in (or reboot). Editing that file is a system change: needs Bash's OK.
- In release builds `nvlog` only goes to syslog for failures (`plugin/NVMTLDevice.m:99-112`), so "framebuffer fetch ON"
  lines will not show up in `/tmp/nvmtl.log`.

## Ruled out

- **Gamma tables** (`kexts/NVRMFB/fb/nvrm-fb.cpp` `nvrmSetGamma`, 13f9849): an output LUT on the display head only.
  Screenshots never pass through it. Switch anyway: boot-arg `-nvrmnogamma`.
- **#196 completion marks** (`plugin/NVMTLEventSync.m:78-118`, `plugin/NVMTLObjects.m:4862-4869`,
  `kexts/NVRM/accel/nvrm-accel.cpp` `nvSurfWaitReady`): only decide *when* a swap happens. With
  `debug.nvaccel_surface_gpu_on=0` the probe answers `NVRM_SS_OFF` and the plugin goes back to
  `waitUntilCompleted` in WindowServer, i.e. 2.0.4 behaviour. Bug persists that way, so this is not it.
- **Load actions / clear colour**: `nvmtlBuildPass` (`NVMTLObjects.m:2649-2735`) and `nvmtl_load_keeps`
  (`NVMTLObjects.m:131`) are unchanged apart from the attachment-less branch. No change to blend state code.
- **copy_memory_logical, atomic_image_format, inline retry, argbuf samplers** (translator): they only act on modules
  that failed to translate in 2.0.4 (CopyMemory type mismatch, image atomics, "pointer placeholder" errors), so
  shaders that worked in 2.0.4 come out the same.

## Candidates, most likely first

### 1. Framebuffer fetch is now ON in every app process (high for an in-app Metal renderer, the cheapest test)

Commit 416ee60. `plugin/nvmtl_vk.c:2948` `nvmtl_fbfetch_on()`: on in every process except WindowServer, unless
`NVMTL_NO_FBFETCH=1`. When on, in **every** process and **every** render pass, whether or not a shader reads `[[color(n)]]`:

- `nvmtl_vk.c:356` every colour-attachment image gets `VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT`
  (and `:1501` the view-usage override too). Before: usage as the app asked.
- `nvmtl_vk.c:3363-3379` (`nvmtl_make_render_pass`) and `:4899-4914` (`nvmtl_make_render_pass_shape`): every colour
  reference switches from `COLOR_ATTACHMENT_OPTIMAL` to `GENERAL`, is also listed as an input attachment, and a
  third, by-region self-dependency is added. Before: plain colour attachments, 2 dependencies.
- `nvmtl_vk.c:3050-3057`: a fetching draw binds set 3 to the pass's views behind a by-region barrier.

Why it could stack text: an app that draws its layer contents with Metal (CoreAnimation in-process, CoreText/CG with
Metal acceleration, or the app's own renderer) clears the dirty area with a `MTLLoadActionClear` or a clear/copy-blend
quad. The clear is still requested with the same load op, but the image is now in a GENERAL feedback-loop layout with an
extra usage bit; on NVK that can change how the image is laid out or how the clear is done (fast clear vs draw), and
the extra usage on IOSurface-backed images can change which surface path is taken. Also Apple's frameworks pick
programmable-blending (fetch) shader variants on Apple-family GPUs (the identity this driver reports). In 2.0.4 such a
read could not resolve; now it reads live data, and a fetch-based "copy"/"clear" that reads the wrong attachment or
reads before the clear lands would blend new glyphs over old pixels instead of replacing them.
Weakness: WindowServer keeps fetch off, so this only explains the bug if Terminal's own process renders with Metal.

Test: `NVMTL_NO_FBFETCH=1` in nvrm610.conf, log out/in, run the same Terminal test. Cheap. Also read-only check whether
Terminal even loads the bundle: `vmmap $(pgrep -x Terminal) | grep -i -E 'nvmtl|GPUBundles'`.
Fix if confirmed: only declare input attachments for a render pass whose pipelines read them (or create a fetch variant
of the render pass lazily when a fetching pipeline is first bound) and leave image usage alone unless a fetching pipeline
exists; at minimum default fetch off outside an allow-list (Blender) the way WindowServer is handled.

### 2. Stencil-emulation detector has false positives, and it changes the stencil reference of normal draws (medium-high, applies inside WindowServer too)

Commit 5092fcb. `plugin/nvmtl_vk.c:2962-2972` `nvmtl_spirv_stencil_emu`:

```c
if (op == 72 && wc == 5 && w[i + 3] == 35 && w[i + 4] == 160u) return 1;   // OpMemberDecorate .. Offset 160
```

This marks a pipeline as "stencil export emulated" if **any** struct member anywhere in the fragment shader sits at byte
offset 160: a uniform or storage buffer struct of more than 160 bytes (colour matrices, transforms, CA/CI parameter
blocks) matches. The translator only lowers export when the shader has a `FragStencilRefEXT` output and no push
constants (`translator/translator/src/native/stencil_export.rs`), but the plugin never checks that.

For a marked pipeline every draw goes through `NVMTL_SEMU_DRAW` (`nvmtl_vk.c:5127-5146`): stencil reference forced to
**0** for the main draw, then, if any stencil write mask bit is set, the draw is **replayed once per write-mask bit** with
reference = that bit, write mask = that bit, depth writes off. Before 2.1.0: one draw with the app's reference.

Why it could stack text: when the compositor (CoreAnimation in WindowServer) clips with stencil (rounded window corners,
masks, clipped layer trees), a content or background draw tested against reference N runs with reference 0 instead:
it is drawn where it should not be and skipped where it should be, so the region that should have been repainted keeps
last frame's pixels; the per-bit replays then draw translucent content several times. With stencil off
(`st_wmask` = 0) the only side effect is the reference reset, which is harmless; so this only bites stencil-using passes.

Switch: none in 2.1.0 (no env/conf key). Test needs a rebuild.
Fix: in `nvmtl_spirv_stencil_emu` only accept an `OpMemberDecorate Offset 160` whose struct is the pointee of a
`PushConstant` (storage class 9) `OpTypePointer` that an `OpVariable` uses, or have the translator emit a unique marker
(e.g. an `OpName`/`OpString` like `nvmtl.stencil_emu`) and test for that. Also leave the reference alone when the write
mask is 0.

### 3. NVK 2.0.5 is a release build (medium, global)

Commit 9d287ca, `nvk/2.0.5/README.md`: the `libvulkan_nouveau.dylib` shipped with 2.1.0 is the same Mesa tree rebuilt
`-Dbuildtype=release -Db_ndebug=true`, plus copy-engine selection and a NAK stencil-export change. Release builds
compile out asserts and change optimisation, which exposes uninitialised state and asserts with side effects; any of
that in NVK's clear, render-pass or image layout code would show as missed clears. Not reviewable here (binary, and
`nvk/nvk-macos.patch` was not updated in this range).

Switch: `NVMTL_ICD_LIB=<path>` (`plugin/nvmtl_vk.c:109`) selects the Vulkan driver; `NVMTL_NVK_DYLIB` (`:4203`) the
SASS hook library. Test: keep 2.1.0, copy 2.0.4's `libvulkan_nouveau.dylib` next to it under a new name and set both
keys in nvrm610.conf. The README says the C exports are unchanged, so the 2.1.0 plugin should load it (2.0.4's NAK aborts
on stencil export, which only Blender uses).

### 4. `[[color(n)]]` inputs moved to set 3 while WindowServer has fetch off (low-medium)

`translator/translator/src/passes/stage_input/mod.rs:855` now decorates every framebuffer-fetch input at set 3
(before: the resource set, binding 192+n). In WindowServer `fetch_mask` is 0 (`nvmtl_vk.c:3890`), so the pipeline
layout has no set 3 and nothing is bound there. If WindowServer's CoreAnimation shaders use programmable blending, their
destination read now goes to an unbound set (the same "built anyway" warning as 2.0.4, `nvmtl_vk.c:1965`), and with the
release NVK (no asserts) that read is silently undefined instead of whatever 2.0.4 produced.
Switch: `NVMTL_FBFETCH_WS=1` in nvrm610.conf turns real fetch on in WindowServer. Risky (untested on the desktop by
upstream); try only after test 1, with the 2.0.2 rollback ready.

### 5. Others, low

- Attachment-less render passes now run instead of being refused (`NVMTLObjects.m:2724-2733`). Only reached with no
  colour and no depth/stencil attachment, which a compositor does not do.
- `simd_is_first` / `simd_broadcast_first` now use the first active lane
  (`translator/.../air_calls/integer_simd.rs`). Changes all shaders using them; compositor shaders rarely do.
- Kernel grid flag (`kernel_grid.rs`, `nvmtl_vk.c` indirect dispatch): only differs when bit 31 of the pushed
  grid is set, which only the indirect path pushes.
- Sampler `gpuResourceID` now carries a heap slot (`NVMTLEmbedded.m`): affects which sampler, not clearing.
- BAR1 left where the firmware placed it (`kexts/NVRM/NVRM.cpp:986`, bb29c59): changes CPU mappings of VRAM; could give
  stale reads of shared IOSurfaces, but that would show old content, not text drawn on top. Switch: none besides
  `-nvrmsmallbar` (different path entirely); worth one look at `log show` for which bar1 line printed.

## Suggested test order on hardware (each one conf line, log out/in, same Terminal test, then remove)

1. `NVMTL_NO_FBFETCH=1` (candidate 1).
2. `NVMTL_ICD_LIB=/Library/GPUBundles/nvmtl/libvulkan_nouveau-2.0.4.dylib` and
   `NVMTL_NVK_DYLIB=` same path (candidate 3).
3. If neither: candidate 2 needs a rebuilt plugin (one-line detector fix); build from our fork, never from upstream
   binaries, per project rules.
4. Only then `NVMTL_FBFETCH_WS=1` (candidate 4).

Rollback for every step: delete the line (or restore 2.0.2, already proven in N-63).
