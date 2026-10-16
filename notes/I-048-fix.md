# I-048 fix: framebuffer fetch only where a shader reads it, precise stencil-export detector

Follows `notes/I-048-overdraw.md` (candidates 1 and 2). Built, not yet tested on hardware.

## What changed

### 1. Framebuffer fetch is per pipeline again (candidate 1)

Upstream 2.1.0 (416ee60) made every render pass in every app except WindowServer a fetch pass: colour references in
`GENERAL` layout, the colour attachments also listed as input attachments, and a third, by-region self-dependency.
That was done so a fetching pipeline would be render-pass compatible with whatever pass it lands in.

NvGfx:

- `plugin/nvmtl_vk.c` `nvmtl_rp_fetch()` (about line 2983) decides the render pass kind per pipeline: a fetch pass only
  when the pipeline's fragment shader reads the framebuffer (`fetch_mask != 0`, i.e. the SPIR-V has an input attachment
  at set 3, binding 192+n, which the translator emits only for `[[color(n)]]` reads). Every other pipeline gets exactly
  the 2.0.4 render pass: `COLOR_ATTACHMENT_OPTIMAL`, no input attachments, two dependencies.
- `nvmtl_make_render_pass` (about :3449) and `nvmtl_make_render_pass_shape` (about :4988) use it.
- Pipeline creation computes `fetch_mask` before the render pass is made (about :3943) and records the kind in the new
  `nvk_pipeline.rp_fetch` (`plugin/nvmtl_vk.h:41`). `nvmtl_vk_cmd_begin_pass` records the kind of the open pass in the new
  `nvk_cmdbuf.rp_fetch` (`nvmtl_vk.c` about :4811, `nvmtl_vk.h:70`).
- The two kinds are not render-pass compatible in Vulkan, so when an encoder's next draw uses a pipeline of the other
  kind, `-[NVMTLRenderCommandEncoder nvmtlFetchRestart:]` (`plugin/NVMTLObjects.m` about :2591) ends the pass and
  resumes it for that pipeline with every attachment loaded (`NVMTL_LOAD_ALL`), the same way the mesh path resumes a
  pass. Viewport, scissor, cull and depth/stencil state are set again; an open visibility query is closed and reopened
  at the same offset (endEncoding already sums several queries per offset). The check runs at draw time in
  `nvmtlBindSamplerPipeline` (about :2641); `setRenderPipelineState` no longer binds a pipeline of the other kind into
  the open pass (about :2853), its first draw does after the resume.
- Safety net: `nvmtl_vk_cmd_bind_set` (about :3136) skips a fetching draw if the open pass is not a fetch pass.
- Unchanged on purpose: colour-attachment images still get `VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT` (`:356`, `:1501`)
  in every process where fetch is on. An image is made before anyone knows whether a fetching pipeline will draw into
  it (Blender's pool textures), so dropping the bit would break Eevee. It is a capability flag, not a layout change.
  `NVMTL_NO_FBFETCH=1` still removes it (and all fetch) if a test points at it.
- WindowServer: unchanged (fetch stays off there unless `NVMTL_FBFETCH_WS=1`).

### 2. Stencil-export detector matches only the translator's block (candidate 2)

Upstream 2.1.0 (5092fcb) marked a fragment shader as "stencil export emulated" when any `OpMemberDecorate .. Offset 160`
appeared, so any uniform or storage struct larger than 160 bytes matched, and every draw of that pipeline ran with
stencil reference 0 plus one replay per stencil write-mask bit (`NVMTL_SEMU_DRAW`).

`translator/translator/src/native/stencil_export.rs` emits exactly one shape, and only when the shader had a
`FragStencilRefEXT` output and no push constants of its own:

```
%S = OpTypeStruct %uint            ; one member, OpTypeInt 32 0
OpDecorate %S Block
OpMemberDecorate %S 0 Offset 160
%P = OpTypePointer PushConstant %S
%v = OpVariable %P PushConstant
```

`nvmtl_spirv_stencil_emu` (`plugin/nvmtl_vk.c` about :3041, helpers `nvmtl_spirv_def` :3011 and
`nvmtl_spirv_semu_block` :3021) now requires all of it: member 0, a one-member struct of a 32-bit unsigned int,
Block-decorated, pointed to by a PushConstant pointer that a PushConstant variable uses. `NVMTL_SEMU_DRAW` itself is
unchanged.

Checked with hand-built SPIR-V: the translator shape matches; the same struct in Uniform storage with member 5 at 160
does not; a float member does not. The 597 cached modules in `~/Library/Caches/nvmtl` (older runs) match neither the old
nor the new test, so that cache says nothing either way.

### 3. A/B switches (conf lines in `/Library/GPUBundles/nvmtl/nvrm610.conf`)

- `NVMTL_NVGFX_LEGACY_FBFETCH=1`: every render pass a fetch pass again, as upstream 2.1.0 (`nvmtl_fbfetch_legacy`,
  about :2972). No pass restarts happen in that mode, because every pipeline and pass is the same kind.
- `NVMTL_NVGFX_LEGACY_SEMU=1`: the 2.1.0 "any member at offset 160" test (`nvmtl_semu_legacy`, about :3000).
- Both set = upstream 2.1.0 behaviour. They are two switches so the two candidates can be told apart.

Both are read once per process through `getenv`, which in a release build only sees conf-file lines
(`NVMTLObjects.m` `nvmtl_conf_knobs` clears inherited `NVMTL_*` variables).

## Why this should fix stacked text

Terminal (and any app that draws its layers with Metal in its own process) has no shader that reads `[[color(n)]]`,
so with this build its render passes are byte-for-byte the 2.0.4 ones again: plain colour attachments in
`COLOR_ATTACHMENT_OPTIMAL`, no input attachments, no self-dependency. That removes the feedback-loop setup that 2.1.0
added to every one of its passes, which is the only render-pass change between 2.0.4 and 2.1.0 for such an app. And
no ordinary shader with a large uniform block is routed through the stencil replay any more, so its draws keep the
app's stencil reference and run once, as in 2.0.4 (this part also applies inside WindowServer).

If stacked text is still there with this build, the remaining 2.1.0 differences for Terminal are the input-attachment
usage bit on images (test: add `NVMTL_NO_FBFETCH=1`) and the NVK 2.0.5 release build (candidate 3).

## Why Blender Eevee keeps working (by construction)

- Eevee's lighting shaders read the G-buffer with `[[color(n)]]`, so their `fetch_mask` is non-zero and their render
  passes are exactly the 2.1.0 fetch passes (same input attachments, layouts and self-dependency, same set 3 binding,
  same by-region barrier before each fetching draw).
- When Eevee switches from its G-buffer pipelines (no fetch) to a lighting pipeline in one encoder, the pass is resumed
  as a fetch pass with every attachment loaded. The end of one pass instance and the start of the next carry the full
  external dependencies, so the lighting draw reads everything the G-buffer draws wrote, as it did in 2.1.0.
- Image usage is unchanged, so every Eevee texture can still be an input attachment however early it was made.
- Eevee's stencil-export shaders are lowered by the translator into the exact block the detector now requires, so
  they still match, and `NVMTL_SEMU_DRAW` is unchanged.
- Cost: each switch between kinds is one pass end and resume (a full barrier). On an immediate-mode NVIDIA GPU with
  `LOAD`/`STORE` attachments that is cheap; Eevee switches a few times per frame, not per draw.

## Build

`RELEASE=1 build/nvgfx/build_plugin.sh` (output `~/src/NvGfx-build/plugin/NVMTLDriver` by default):

- 320 warnings, the same set as the unmodified tree (compared by message text, ignoring line numbers).
- `nm -gU` exports: 961, identical to the shipped 2.1.0 `NVMTLDriver` and to our unmodified build.
- sha256 is in the commit message of this change and in the hand-off report.

## How to test (needs Bash's OK: it writes to /Library)

1. Have a working 2.1.0 install (Terminal shows stacked text). Rollback ready: 2.0.2 (proven in N-63), or the saved
   2.1.0 binary from step 2.
2. Back up the shipped binary:
   `sudo cp /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver ~/NVMTLDriver-2.1.0.bak`
3. Install ours:
   `sudo cp ~/src/NvGfx-build/plugin/NVMTLDriver /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver`
   and confirm the sha with `shasum -a256`. The shipped bundle is ad-hoc signed as a bundle (`_CodeSignature`), so
   re-sign it after the copy: `sudo codesign -f -s - /Library/GPUBundles/NVMTLDriver.bundle`.
4. Log out and back in (the plugin and the conf are read at process start; WindowServer restarts with the session).
5. Terminal workload: open Terminal, run `for i in $(seq 1 400); do printf '%05d %s\n' $i "$(date +%s%N)"; done`,
   then `clear`, then `top -l 3`, then scroll back and forth, resize the window. Stacked text shows as glyphs drawn on
   top of earlier glyphs in the same cells.
6. Screenshot: `screencapture -x ~/Desktop/i048-nvgfx.png` (screenshots read composited content, which is where the
   bug was visible). Save it with the attempt's evidence.
7. A/B on the same build, one conf line at a time inside the `# --- my settings, kept on update ---` block, log
   out/in between, same workload and screenshot each time:
   - `NVMTL_NVGFX_LEGACY_FBFETCH=1` (fetch as 2.1.0)
   - `NVMTL_NVGFX_LEGACY_SEMU=1` (detector as 2.1.0)
   - both (should reproduce the bug if the fix is what fixed it)
   - `NVMTL_NO_FBFETCH=1` (no fetch at all, no usage bit)
8. Blender check: Eevee material preview of a lit scene still shades (not black), as with upstream 2.1.0.
9. Rollback: copy `~/NVMTLDriver-2.1.0.bak` back (or reinstall 2.0.2), log out/in; remove any conf lines added.
