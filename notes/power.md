# GPU power limit on macOS

## What we see (2026-10-16, 15.8.1, RTX 5070 Laptop 10de:2d18)

- `tools/nvgfx-smi` (RUSD) reports a 50 W power limit, requested and enforced, and the only throttle reason
  under load is `sw_power_cap`. Windows runs the same GPU at 115 W.
- On macOS the RM gets no platform power input: every _DSM except NBCI answers "not supported", there is no
  NVPCF device, no AC/battery state, and `nv_is_chassis_notebook()` is FALSE. The Linux open driver without
  NVIDIA's closed `nvidia-powerd` behaves the same, so 50 W looks like the RM default with no platform input.
- Firmware (SSDT-21): the GPU node `\_SB.PC00.RP01.PXSX` has the GPS _DSM (rev 0x200, functions 0, 0x12,
  0x13, 0x20 PSHARESTATUS, 0x2A PSHAREPARAMS; only when NVS `GPSS != 0`). `\_SB.NPCF` (HID NVDA0820) has the
  NVPCF _DSM (rev 0x200, functions 0 to 10; only when `PXSX.NPCS != 0`). Both read Arg3 as a Buffer.

## Boot-args (all default off; with none set the kext behaves exactly as before)

| boot-arg | effect |
|---|---|
| `nvrmacpibl=1` | unchanged: NBCI _DSM (0, 0x10, 0x14) and _DOD on the GPU node, for the backlight |
| `nvrmgps=1` | GPS _DSM on the GPU node, any subfunction, rev as RM passes it. Lets the RM's Platform Request Handler find GPS_2X and run |
| `nvrmnvpcf=1` | NVPCF _DSM on `\_SB.NPCF`, as Linux does with the `\_SB.NPCF._DSM` path |
| `nvrmphysctl=1` | enables the root-only escape below |

`nvrmgps` and `nvrmnvpcf` do not need `nvrmacpibl`. Either of them also turns on:

- `nv_is_chassis_notebook()`: TRUE if ACPI has the NPCF device or a PNP0C0A battery.
- `nv_acpi_is_battery_present()`: Linux rule, `_BIX[4]` (or `_BIF[3]`) battery technology == 1. Not `_STA`
  (BAT0 reports `_STA` 0 under macOS here). Evaluated once.
- `nv_acpi_get_powersource()`: `_PSR` on the ACPI0003 device (ADP0), `*ac_plugged = (_PSR == 1)`. RM reads it
  once at init (osinit.c, mobile config). No AC change notifications yet.

The new ACPI calls are refused when interrupts are off or in the ISR. Logs: `NVRM-xnu: GPS _DSM 0x2a rev 0x200
in N B -> st (n B, ioret x)`, `NVPCF _DSM ...`, `_PSR on ADP0 -> ...`, first 20 calls then every 256th.

## Escape NVRM_ESC_PHYS_CTRL (0xE4)

ABI in `kexts/NVRM/nvrm_physctl_abi.h`, sent through the normal escape selector (1) on an NVRM/NVAccel RM
client. Root only (`kauth_cred_issuser`), and only with `nvrmphysctl=1`.

```
struct NVRMPhysCtrlEsc {      // 4128 bytes
    u32 version;              // 1
    u32 cmd;                  // RM control
    u32 paramsSize;           // <= 4096
    u32 status;               // out: NV_STATUS, or 0x80000001 off / 0x80000002 bad version / 0x80000003 bad size
    u32 mode;                 // 0 GSP subdevice, 1 GSP device, 2 GSP client, 3 kernel internal call
    u32 hClient, hObject;     // mode 3 only
    u32 reserved;
    u8  params[4096];         // in/out
};
```

Modes 0 to 2 call `GPU_GET_PHYSICAL_RMAPI(pGpu)->Control(hInternalClient, hInternal{Subdevice,Device,Client})`:
straight to GSP, like the RM's own internal controls. Mode 3 calls `rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL)`
on a CPU-RM object the caller owns, so controls flagged RMCTRL_FLAGS_INTERNAL (the 0x20800Axx family) pass the
internal-call check. Both run in `rm_nvgfx_phys_control()` (`kexts/NVRM/nvrm_physctl.c`) with the API lock and all
GPU locks held, after `FULL_GPU_SANITY_CHECK`.

## nvgfx-smi --phys

```
sudo tools/nvgfx-smi/nvgfx-smi --phys CMD [WORD...] [--size N] [--target sub|dev|client] [--kernel]
```

CMD and WORDs are hex, WORDs are little-endian 32-bit params, `--size` pads the params to N bytes. Without
`--kernel` it goes to GSP on RM's internal client; with `--kernel` it is an internal call on the tool's own
subdevice (or device/client). The reply prints the status and the params as words.

Caution: these controls write GPU power policy. Start with GET controls; a bad SET can hang GSP or the GPU.

## N-73 result and the nvrmtgp manager (N-74)
- N-73: GSP `INTERNAL_PMGR_PFM_REQ_HNDLR_CONFIGURE_TGP_MODE` (0x20800AD2, 1-byte `bEnable`) = 1 lifts the enforced limit
  from the VBIOS 50 W to 115 W. Burn 12.1 to 17.8 TFLOP/s, about 2.3 GHz, up to 116 W, 66 C, no throttle.
- `nvrmtgp=1` (kexts/NVRM/nvrm_power.cpp): after the GPU is up, sends TGP mode on while `_PSR` says AC and off on
  battery, polled every 5 s, only on change, through `rm_nvgfx_phys_control` (internal client, physical API).
- Sysctls: `debug.nvgfx_tgp_enable` (RW, 1 auto, 0 force off), `debug.nvgfx_tgp_state`, `debug.nvgfx_tgp_ac`,
  `debug.nvgfx_tgp_last_status`, `debug.nvgfx_tgp_applies`.
- `thread_call_cancel_wait` is not exported to kexts (install CHECK caught it); stop uses cancel, wait, free.
