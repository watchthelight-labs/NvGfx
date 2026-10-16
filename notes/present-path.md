# Present path: event-driven page flips (165 Hz work)

Status 2026-10-16: code written and built (NVRMFB, NVAccel 15 and 26, NMXAccel builds too). Not loaded, not booted.
Measured problem (NullMoth 2.0.2, same design in 2.1.0): window drags reach 54 flips/s on the 165 Hz panel (165/3),
copy mode 78 fps with tearing. Goal: at least 160 flips/s with no tearing or strobe and correct IOSurface reuse.

## What the display pipe family actually does

Reverse engineered from IOAcceleratorFamily2 485.2 in SystemKernelExtensions.kc on 15.8.1 (read only; scratch notes
and disassembly kept outside the repo). Confidence high for all of this:

- WindowServer can queue up to 4 transactions per pipe (a ring at pipe+0x118, `wait_for_queue_slot_gated` sleeps when
  full). The family keeps **one pending** transaction (pipe+0x248) and one live one (pipe+0x250).
- `event_interrupt_gated` is the only caller of `performTransaction`. It does nothing while a transaction is pending, and
  it waits for the transaction's GPU render fence first. So `performTransaction(T2)` never runs before
  `isTransactionComplete(T1)` has returned true.
- `isTransactionComplete` is only ever asked about the pending transaction: right after `performTransaction`, and in
  `transaction_interrupt_gated` after each `signalTransactionInterrupt`. A signal that finds it false is spent: the pipe
  waits for the next signal. The 300 ms timeout only logs.
- `completeTransaction(T)` moves the previous live transaction to the finished list and makes T live. Finishing calls
  `IOSurface::decrement_use_count` on its plane surfaces. **Completing T2 is what hands T1's surface back to
  WindowServer.** So a transaction may only complete once its own flip has latched: before that, T1 is still scanning
  out and WindowServer may draw into it (tearing).
- `signalTransactionInterrupt`'s argument is ignored.

Consequence for the brief: a driver FIFO deeper than one never fills on 15.8.1, and "complete before latch" would break
the reuse contract. The throughput gain has to come from removing latency between "flip latched" and "transaction
complete" and between "transaction performed" and "flip committed". Legacy spends that time in `IOSleep(1)` polls
(pre-wait and latch wait inside NVRMFB, `flip_us` 3.9 ms average, 18 ms max) and in thread call hops, while holding the
single global display gate.

## Design as built

### NVRMFB (`kexts/NVRMFB/fb/nvrm-fb.cpp`)

- New private interface `nvgfx_present_abi.h` (in `kexts/NVRMFB/fb`, included by NVAccel by relative path):
  platform functions `nvgfxFlipSubmit` and `nvgfxPresentHook`. The flip request is the unchanged `NVRMFlipRequest`
  (FLIP ABI 2). NVRM neither provides nor calls them.
- `flipAsync` (`nvgfxFlipSubmit`): one pure flip, built exactly like the legacy lean pure flip, committed through
  `submitConfig(..., async=true)`, which skips both the pre-wait and the latch wait. Returns `kIOReturnBusy` without
  doing anything if the head's newest completion slot still reads NOT_BEGUN (one latched front plus one pending, the
  "max 2 outstanding" of the brief) or a legacy commit is in progress.
- Latch detection, no sleeps: `presentPoll` reads `getFlipPendingStatus` (lock free, NullMoth's KAPI patch: BEGUN or
  FINISHED = latched) from three places, whichever comes first: the KAPI `FLIP_OCCURRED` event (`eventCallback`, NVRM's
  event thread call, NVKMS lock released), the hardware vblank thread call (`hwVblDeliver`), and the fallback VBL timer
  (`vblFire`). On latch the pending surface becomes `fFrontSurf` and the hook gets `LATCHED` with the caller's token.
- Timeout: a flip still pending after 100 ms (the legacy bound) faults presentation exactly like the legacy latch wait
  (`fSubmissionFaulted`), unless the head went dark (display or GPU sleep), which retires it as `SUPERSEDED` and forgets
  the slot.
- Legacy commits (mode set, home flip, wake re-flip, head on/off) take a hold (`NVRMPresentHold`) so async flips answer
  busy while the legacy pre-wait and commit run; once the pre-wait has proven the last flip latched, a still recorded
  async flip is reported `SUPERSEDED` before the legacy commit replaces it. GPU standby (`NVRMGpuPower` 0) and `stop`
  do the same.
- KAPI surface cache: lookups (`flipCacheFind`) under the existing spin lock; inserts and evictions (`flipCacheCreate`)
  only with the global gate (excludes VRAM grant/release, so the allocation check and `dupMemory` see the same memory)
  plus `fPresentLock`. Eviction never picks `fFrontSurf` or `fPendSurf` (a timed-out flip stays protected). Deviation:
  a retired front's cache entry is **not** destroyed; WindowServer cycles through a few surfaces and destroying the entry
  would turn every flip into a `createSurface`. Entries still go when evicted or when the epoch changes.

### NVAccel (`kexts/NVRM/accel/nvrm-accel.cpp`)

- `debug.nvgfx_present_depth`: 1 = legacy single async slot, unchanged code path (`nvAccelIopAsyncStart` /
  `nvAccelIopAsyncRun`, flip through `nvFlipToSurfacePure`, which still polls inside NVRMFB). 2 (default) or 3 = present
  queue.
- Present queue, per head, 8 slots: QUEUED, SUBMITTING, SUBMITTED. `performTransaction` puts the flip in the queue
  (`nvPresEnqueue`) and returns. `nvPresPump` commits the oldest queued flip if none is in flight: inline from
  `performTransaction` when the surface has no pending #196 GPU mark (non-blocking check), otherwise on the per-head
  worker thread call, which runs `nvSurfWaitReady` first (the GPU-readiness wait stays, off WindowServer's thread).
  Busy answers are retried after 1 ms (up to 200 times, then the transaction completes without its frame).
- `nvPresHook` (called by NVRMFB): `LATCHED`/`SUPERSEDED` makes the surface `gIopFlipFront` (the old front becomes
  `gIopFlipPrev`, the one before is released, same retention as legacy), frees the slot and calls
  `signalTransactionInterrupt`. `FAILED` completes the transaction without its frame.
- `isTransactionComplete(txn)` = not busy in the legacy slot and `txn` not queued or in flight in the present queue.
- The depth bounds queued plus in-flight transactions per head (waits up to 50 x 1 ms for room, like the legacy slot,
  then drops). With the family's single pending transaction the occupancy should never exceed 1:
  `debug.nvgfx_present_occupancy_max` checks that on the machine.
- Refusals keep the legacy rule: 3 in a row set `debug.nvaccel_iop_async=0`, which also turns the queue off (it needs
  async on).

### Vblank

`hwVblIntr` now records NVKMS's vblank timestamp, but the VBL proc is still delivered from the thread call: the NVKMS
vblank callback runs inside NVKMS's timer worker with NVKMS's lock held (`os-nvkms-xnu.cpp` `kms_timer_execute`), and
IOFramebuffer's VBL handler can move the hardware cursor, whose commit goes back into NVKMS: calling it there would
deadlock. `IOFBInterruptProc` takes no timestamp anyway, so there is nothing to pass it. The hop is now measured
(`debug.nvgfx_fb_vbl_hop_us_*`) and the same thread call also polls the async flip. The timer fallback is unchanged.

## State machine (one head)

```
NVAccel slot:  FREE -> QUEUED -(pump)-> SUBMITTING -(nvgfxFlipSubmit ok)-> SUBMITTED -(hook LATCHED/SUPERSEDED)-> FREE (surface = front)
                          ^                  |  busy: back to QUEUED, retry in 1 ms (200 tries)
                          |                  |  refused / not ready / tries used up: FREE, transaction completes, frame dropped
                          +------------------+
                                                SUBMITTED -(hook FAILED, 100 ms timeout)-> FREE, frame dropped, head faulted
Family:        queued (ring of 4) -> pending (perform; isTransactionComplete false while QUEUED..SUBMITTED) -> live -> finished
NVRMFB head:   idle -(flipAsync commit)-> pending (fPendSurf) -(slot BEGUN/FINISHED)-> idle, fFrontSurf = that surface
                                         pending -(legacy commit after its pre-wait, GPU standby, stop)-> idle (SUPERSEDED)
                                         pending -(100 ms, head lit)-> faulted (fPendDead, like the legacy latch timeout)
```

## Locking order

`NVRMDisplayGate` (global recursive) -> `fPresentLock` (per framebuffer, recursive) -> `fFlipCacheLock` (spin) ->
NVKMS's `gKmsLock` (inside KAPI calls). NVAccel's `gPresLock` is a leaf spin lock: nothing is called while it is held.

- `fPresentLock` is held across one KAPI commit or one cache insert, never across a wait or sleep.
- Nothing takes `fPresentLock` or the gate while holding `gKmsLock`: `hwVblIntr` (runs under `gKmsLock`) only records
  and schedules. `eventCallback` runs with `gKmsLock` released, so it polls directly.
- The NVAccel hook is always called with `fPresentLock` released, and never calls back into NVRMFB synchronously (it
  signals the pipe and kicks the worker thread call).
- The async path takes the gate only on a cache miss, before `fPresentLock`.

Deviation from the brief: `NVRMDisplayGate` is still one global lock. Splitting it per head would touch every mode set,
hot-plug, power and VRAM path. What the brief wanted from it is achieved for pipelined flips: they no longer take the
gate (except a cache miss) and never wait while holding a lock, so cursor updates and VRAM grant/release no longer queue
behind flip latch waits. In legacy mode (depth 1) the gate is held across the waits exactly as before.

## Failure modes and what they look like

| Failure | Effect | Counter / log |
|---|---|---|
| Flip never latches (NVKMS or display engine stuck) | presentation faulted after 100 ms, as legacy; transaction completes, frame dropped | `nvgfx_fb_timeouts`, "did not latch within 100 ms" |
| Flip event lost | vblank thread call or VBL timer finds the latch one vblank later | `nvgfx_fb_latch_us_max` grows |
| Legacy commit during a drag (mode set, home flip) | async flips answer busy for its duration, retried every 1 ms | `nvgfx_fb_hold`, `nvgfx_present_busy` |
| NVKMS refuses async flips | 3 in a row turn async off (legacy rule), queue off with it | `nvaccel_iop_async_refused`, `nvgfx_fb_refused` |
| Present hook missing (old NVRMFB) | legacy async slot used | `nvgfx_present_nohook`, "has no present hook" |
| Dropped frame (refused, timeout) | its transaction completes while the previous surface is still on the panel, so WindowServer may draw into the visible surface for a frame. Same hazard as the legacy async refusal path. | `nvgfx_present_dropped` |
| Framebuffer stops while an event is in flight | `eventCallback` may read `sPresentFb` just before `stop` clears it. The object outlives `stop`, so this is only a window if the framebuffer is also freed at once (not seen in practice) | none |

## Test plan

Prerequisites: Bash's OK to install the two kexts (unsigned, kernel collection change); NVRM stays the shipped 2.1.0.
Boot first with the depth at 1 to confirm the legacy path is intact, then A/B live.

1. Baseline on the rebuilt kexts, legacy: `sudo sysctl -w debug.nvgfx_present_depth=1`, then drag a window for 10 s
   while running `~/Hackintosh/native/tools/gfxbench/fps.sh 10 legacy`. Expect about 54 flips/s, as 2.0.2/2.1.0.
2. Pipelined: `sudo sysctl -w debug.nvgfx_present_depth=2`; same drag with `fps.sh 10 depth2`. `avg_flip_us` reads 0
   here (it counts the legacy `nvrmfb_flip_*` only); read `debug.nvgfx_fb_commit_us_sum` / `nvgfx_fb_submits` and
   `nvgfx_fb_latch_us_sum` / `nvgfx_fb_latched` instead.
3. Counters to save with each run (`counters.sh` greps nvaccel|nvrmfb|nvrm_ only; add `sysctl debug | grep nvgfx`):
   `nvgfx_present_queued = submitted + dropped (+ in flight)`, `latched` close to `submitted`, `dropped` 0,
   `occupancy_max` 1, `nvgfx_fb_timeouts` 0, `nvgfx_fb_busy` small, `nvgfx_fb_latch_us_max` under one period (6 ms)
   plus a little, `nvgfx_fb_vbl_hop_us_max` small.
4. Visual: drag, scroll in Safari and Terminal, resize, Mission Control. No tearing, no black strobe, no older frame
   flashing back. Also mouse movement during a drag (cursor commits now interleave with async flips).
5. Mode and power paths while presenting: change refresh rate (165 / 60), display sleep and wake, system sleep and wake,
   Night Shift on (gamma commits). Check `nvgfx_fb_superseded` moves on these and nothing faults.
6. depth 3: same as 2; `occupancy_max` should still be 1 (confirms the family serialization on the machine).
7. Copy mode (`debug.nvaccel_iop_flip=0`) and `debug.nvaccel_iop_async=0`: unchanged legacy paths.

## Rollback

- Live: `sudo sysctl -w debug.nvgfx_present_depth=1` puts NVAccel back on the legacy path (NVRMFB's legacy flip is
  unchanged apart from taking the new per-head lock around its commit). Not persistent: the default is 2 after a boot.
- Kexts: reinstall the shipped NullMoth 2.1.0 NVRMFB and NVAccel (or go back to NullMoth 2.0.2 as in N-64), the same
  way as any driver package change. OpenCore boot picker plus the USB stick remain the recovery path if a boot hangs.
