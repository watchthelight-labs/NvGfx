/*
 * NvGfx: give the GPU its platform power budget (notes/power.md).
 *
 * The RTX 5070 Laptop boots at its VBIOS default limit (50 W here). Windows lifts it to the laptop's 115 W through
 * NVIDIA's NVPCF driver; on Linux nvidia-powerd does. GSP lifts the enforced limit to 115 W as soon as
 * it gets INTERNAL_PMGR_PFM_REQ_HNDLR_CONFIGURE_TGP_MODE with bEnable=1. With boot-arg nvrmtgp=1 this file does that
 * once the GPU is up, only on AC (_PSR), and follows plug and unplug with a 5 s poll.
 *
 *   debug.nvgfx_tgp_enable       RW  1 = auto (on while on AC), 0 = force off (live rollback)
 *   debug.nvgfx_tgp_state        RO  what GSP was last told: 0 off, 1 on, -1 nothing yet
 *   debug.nvgfx_tgp_ac           RO  last _PSR reading: 1 AC, 0 battery, -1 unknown
 *   debug.nvgfx_tgp_last_status  RO  NV_STATUS of the last control
 *   debug.nvgfx_tgp_applies      RO  controls sent
 */
// <wtl> fork: nvgfx | author: watchthelight </wtl>

#include "nv-xnu.h"
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <sys/sysctl.h>
#include <IOKit/IOLib.h>
#include "nvrm_physctl_abi.h"
extern "C" void nv_xnu_power_refresh(void);   // nv_xnu_power.cpp, upstream 2.4.1

extern "C" NV_STATUS rm_nvgfx_phys_control(nv_state_t *nv, NvU32 mode, NvU32 hClient, NvU32 hObject, NvU32 cmd,
                                           void *pParams, NvU32 paramsSize);

#define NVGFX_CTRL_CONFIGURE_TGP_MODE 0x20800ad2u   // NV2080_CTRL_CMD_INTERNAL_PMGR_PFM_REQ_HNDLR_CONFIGURE_TGP_MODE
#define NVGFX_TGP_FIRST_MS  2000u
#define NVGFX_TGP_PERIOD_MS 5000u
#define NVGFX_TGP_MAX_FAILS 5

static int gTgpEnable = 1, gTgpState = -1, gTgpAc = -1, gTgpLastStatus = 0, gTgpApplies = 0;
SYSCTL_INT(_debug, OID_AUTO, nvgfx_tgp_enable, CTLFLAG_RW | CTLFLAG_LOCKED, &gTgpEnable, 0,
           "1 = TGP mode on while on AC (115 W class), 0 = force off (VBIOS default limit)");
SYSCTL_INT(_debug, OID_AUTO, nvgfx_tgp_state, CTLFLAG_RD | CTLFLAG_LOCKED, &gTgpState, 0, "TGP mode last sent: 0 off, 1 on, -1 none");
SYSCTL_INT(_debug, OID_AUTO, nvgfx_tgp_ac, CTLFLAG_RD | CTLFLAG_LOCKED, &gTgpAc, 0, "last _PSR: 1 AC, 0 battery, -1 unknown");
SYSCTL_INT(_debug, OID_AUTO, nvgfx_tgp_last_status, CTLFLAG_RD | CTLFLAG_LOCKED, &gTgpLastStatus, 0, "NV_STATUS of the last TGP control");
SYSCTL_INT(_debug, OID_AUTO, nvgfx_tgp_applies, CTLFLAG_RD | CTLFLAG_LOCKED, &gTgpApplies, 0, "TGP mode controls sent");

static thread_call_t gTgpCall;
static nv_state_t *gTgpNv;
static volatile SInt32 gTgpStopping, gTgpRunning;
static int gTgpFails;

static void nvgfx_tgp_arm(uint32_t ms)
{
    if (!gTgpCall || gTgpStopping) return;
    uint64_t dl;
    clock_interval_to_deadline(ms, kMillisecondScale, &dl);
    thread_call_enter_delayed(gTgpCall, dl);
}

static void nvgfx_tgp_tick_body(void);
static void nvgfx_tgp_tick(thread_call_param_t, thread_call_param_t)
{
    OSIncrementAtomic(&gTgpRunning);
    if (!gTgpStopping && gTgpNv) nvgfx_tgp_tick_body();
    OSDecrementAtomic(&gTgpRunning);
}

static void nvgfx_tgp_tick_body(void)
{
    // <wtl> The answer nv_acpi_get_powersource gives is a cached one since 2.4.1, refreshed only at start and on
    // wake. This runs on our own thread with no firmware lock held, so walk the registry first; otherwise the TGP
    // mode never notices the charger going away. </wtl>
    nv_xnu_power_refresh();
    NvU32 plugged = 0;
    NV_STATUS ps = nv_acpi_get_powersource(&plugged);
    gTgpAc = ps == NV_OK ? (plugged ? 1 : 0) : -1;
    // Unknown power source: keep what was applied rather than flapping; first decision needs a reading.
    const int want = gTgpAc < 0 ? gTgpState : (gTgpEnable && gTgpAc == 1) ? 1 : 0;

    if (want >= 0 && want != gTgpState) {
        NvU8 b = (NvU8)want;
        NV_STATUS st = rm_nvgfx_phys_control(gTgpNv, NVRM_PHYSCTL_PHYS_SUBDEV, 0, 0, NVGFX_CTRL_CONFIGURE_TGP_MODE, &b, sizeof b);
        gTgpLastStatus = (int)st;
        gTgpApplies++;
        if (st == NV_OK) {
            gTgpState = want;
            gTgpFails = 0;
            kprintf("NVRM-xnu: nvgfx TGP mode %s (%s, enable %d) -> 0x%x\n", want ? "on" : "off",
                    gTgpAc == 1 ? "AC" : "battery", gTgpEnable, st);
            IOLog("NVRM-xnu: nvgfx TGP mode %s (%s) -> 0x%x\n", want ? "on" : "off", gTgpAc == 1 ? "AC" : "battery", st);
        } else if (++gTgpFails >= NVGFX_TGP_MAX_FAILS) {
            kprintf("NVRM-xnu: nvgfx TGP mode %s failed %d times (last 0x%x), giving up\n", want ? "on" : "off", gTgpFails, st);
            IOLog("NVRM-xnu: nvgfx TGP mode failed %d times (last 0x%x), giving up\n", gTgpFails, st);
            return;
        }
    }
    nvgfx_tgp_arm(NVGFX_TGP_PERIOD_MS);
}

extern "C" void nvgfx_power_start(nv_state_t *nv)
{
    int v = 0;
    if (!PE_parse_boot_argn("nvrmtgp", &v, sizeof(v)) || !v) return;
    if (gTgpCall || !nv) return;
    static bool sysctlsUp;
    if (!sysctlsUp) {
        sysctlsUp = true;
        sysctl_register_oid(&sysctl__debug_nvgfx_tgp_enable); sysctl_register_oid(&sysctl__debug_nvgfx_tgp_state);
        sysctl_register_oid(&sysctl__debug_nvgfx_tgp_ac); sysctl_register_oid(&sysctl__debug_nvgfx_tgp_last_status);
        sysctl_register_oid(&sysctl__debug_nvgfx_tgp_applies);
    }
    gTgpNv = nv;
    gTgpStopping = 0;
    gTgpCall = thread_call_allocate(nvgfx_tgp_tick, NULL);
    if (!gTgpCall) { kprintf("NVRM-xnu: nvgfx TGP: no thread call\n"); return; }
    kprintf("NVRM-xnu: nvgfx TGP mode manager armed (nvrmtgp=1), first check in %u ms\n", NVGFX_TGP_FIRST_MS);
    nvgfx_tgp_arm(NVGFX_TGP_FIRST_MS);
}

extern "C" void nvgfx_power_stop(void)
{
    if (!gTgpCall) return;
    OSCompareAndSwap(0, 1, &gTgpStopping);
    // thread_call_cancel_wait is not exported to kexts: cancel, then wait for a running tick (it never re-arms once
    // gTgpStopping is set), then free (thread_call_free fails while the call is still pending).
    thread_call_cancel(gTgpCall);
    for (int i = 0; i < 1000 && gTgpRunning; i++) IOSleep(10);
    thread_call_cancel(gTgpCall);
    for (int i = 0; i < 100 && !thread_call_free(gTgpCall); i++) { IOSleep(10); thread_call_cancel(gTgpCall); }
    gTgpCall = NULL;
    gTgpNv = NULL;
}
