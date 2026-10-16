/*
 * NvGfx present path: the interface between NVAccel and NVRMFB for event-driven page flips.
 * Copyright (c) 2026 NullMoth Systems (driver), present path changes by watchthelight.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Private to NVAccel and NVRMFB, which ship together. NVRM is not involved: it neither provides nor calls
 * these functions, so the shipped 2.1.0 NVRM pairs with them unchanged. The flip request itself is the
 * existing struct NVRMFlipRequest (nvrm_gpuva_abi.h, NVRM_FLIP_ABI_VERSION 2), not changed here.
 *
 * Both functions are reached through NVRMNVDAFramebuffer::callPlatformFunction. An older NVRMFB that does not
 * know the names passes them up to IOService, which answers kIOReturnUnsupported, and NVAccel then keeps the
 * legacy flip path.
 *
 * NVGFX_FN_PRESENT_HOOK  param1 = struct NVGfxPresentHook *. Registers (or, with fn == NULL, removes) the
 *                        callback NVRMFB calls when a flip submitted with NVGFX_FN_FLIP_SUBMIT has latched on
 *                        the panel, failed, or was replaced by another commit. One hook per framebuffer.
 * NVGFX_FN_FLIP_SUBMIT   param1 = struct NVGfxFlipSubmit *. Commits one pure flip and returns without waiting
 *                        for it to latch. Returns:
 *                          kIOReturnSuccess   committed; the hook reports the token later
 *                          kIOReturnBusy      a flip on this head has not latched yet; nothing was done
 *                          kIOReturnNotReady  the head is not lit, asleep, stopping, or presentation faulted
 *                          kIOReturnIOError   refused (req->flipResult says why, -1 for a local refusal)
 *                          kIOReturnBadArgument  version or argument mismatch
 *
 * Hook rules: NVRMFB calls it from a thread (KAPI event thread, vblank thread call or the submitting thread),
 * never from interrupt context and never with NVKMS's lock held, but possibly with NVRMFB's own present lock
 * held. The hook must therefore not call back into NVRMFB synchronously; it records the state and defers work.
 */
#pragma once

#define NVGFX_PRESENT_ABI_VERSION 1u

#define NVGFX_FN_PRESENT_HOOK "nvgfxPresentHook"
#define NVGFX_FN_FLIP_SUBMIT  "nvgfxFlipSubmit"

enum {
    NVGFX_PRESENT_LATCHED    = 0,   // the flip with this token is on the panel; the previous front is free
    NVGFX_PRESENT_FAILED     = 1,   // it never latched (timeout); presentation is now faulted on this head
    NVGFX_PRESENT_SUPERSEDED = 2,   // another commit (mode set, home flip, wake) replaced it after it latched,
                                    // or the head went dark with it queued (GPU standby, stop)
};
// A submitter answered kIOReturnBusy is not told when the head frees up: it retries on its own timer.

typedef void (*nvgfx_present_hook_t)(void *refcon, unsigned long long token, int status);

struct NVGfxPresentHook {
    unsigned int         version;
    nvgfx_present_hook_t fn;
    void                *refcon;
};

struct NVRMFlipRequest;
struct NVGfxFlipSubmit {
    unsigned int            version;
    struct NVRMFlipRequest *req;      // kapiMemory, width, height, pitch, allocationCookie; flipResult is written
    unsigned long long      token;    // nonzero, chosen by the caller, echoed to the hook
};
