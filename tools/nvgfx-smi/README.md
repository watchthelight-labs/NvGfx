# nvgfx-smi

Read-only GPU telemetry for NvGfx on macOS: power limit, power draw, clocks,
pstate, temperature and throttle reasons. It talks to the NVRM kext the same
way the NVK userland does and needs no root.

## Build and run

```
./build.sh                      # xcrun clang, x86_64, headers from ~/src/deps/open-gpu-610.57.04 (override with OPEN_GPU=...)
./nvgfx-smi                     # same as --once: human-readable report plus every RM status
./nvgfx-smi --json              # one report as JSON
./nvgfx-smi --watch 200         # CSV, one line every 200 ms until Ctrl-C (default 1000 ms)
./nvgfx-smi --verbose           # also log transport details and thermal sensors to stderr
```

CSV columns: `time,pstate,gpc_mhz,mem_mhz,power_mw_inst,power_mw_avg,limit_req_mw,limit_enf_mw,tgp_mw,temp_c,throttle_mask,throttle`.
A load to watch it against: `../nvgfx-burn/nvgfx-burn 60`.

## How it works

1. Open an NVAccel user client (open type `0x4E560000|0`, falls back to NVRM type 0), bind a `/dev/null` fd to it (selector 3).
2. `NV_ESC_CARD_INFO` for the gpu id, alloc `NV01_ROOT_CLIENT`, `NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2`, alloc `NV01_DEVICE_0` and `NV20_SUBDEVICE_0` (escape selector 1, as in nvrm_xnu.c).
3. Alloc `RM_USER_SHARED_DATA` (class `0x00DE`, "RUSD") under the subdevice with `polledDataMask = CLOCK|PERF|POWER|THERMAL`.
4. Map it the way NVK maps system memory: open a second ctl client, send `NV_ESC_RM_MAP_MEMORY` (read-only flag) on the first one naming the second's fd, then `IOConnectMapMemory64(type 0, kIOMapReadOnly)` on the second. The kext's `clientMemoryForType` hands out the RUSD pages.
5. Each sample reads the RUSD sections with the timestamp seqlock and also issues the plain controls below.

Only GET controls are sent. Everything is freed on exit (freeing the root client frees the tree).

## What works (2026-10-16, NvGfx on 15.8.1, RTX 5070 Laptop 10de:2d18)

| Call | Status |
|---|---|
| alloc NV01_ROOT_CLIENT, NV01_DEVICE_0, NV20_SUBDEVICE_0 | NV_OK |
| alloc NV00DE with polling, NV_ESC_RM_MAP_MEMORY, IOConnectMapMemory64 | NV_OK |
| NV2080_CTRL_CMD_PERF_GET_CURRENT_PSTATE (0x20802068) | NV_OK |
| NV2080_CTRL_CMD_CLK_PMUMON_CLK_DOMAINS_GET_SAMPLES (0x20801037) | NV_OK |
| NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE_V2 (0x20800513) | NV_OK, 1 sensor (index 0, target 0) |
| NV2080_CTRL_CMD_PERF_RATED_TDP_GET_CONTROL (0x2080206E) | NV_OK (client RM: action 0 DEFAULT) |
| NV2080_CTRL_CMD_PERF_RATED_TDP_GET_STATUS (0x2080206D) | 0x56 NV_ERR_NOT_SUPPORTED |
| NV2080_CTRL_CMD_PMGR_GET_MODULE_INFO (0x20802609) | NV_OK (moduleId 0) |
| NV2080_CTRL_CMD_PERF_GET_LEVEL_INFO_V2 (0x2080200B) | 0x1F NV_ERR_INVALID_ARGUMENT for most domain bits; only 0x1 and 0x10 are accepted |

RUSD fields, populated or not:

| Field | Data |
|---|---|
| clkPublicDomainInfos | yes: graphics, memory, video actual kHz and target MHz; SM reads 0 |
| clkThrottleInfo.reasonMask | yes |
| perfCurrentPstate | yes (P-state bit mask, same encoding as the control) |
| powerLimitGpu requested/enforced | yes |
| instPowerUsage.instGpuPower, avgPowerUsage.averageGpuPower | yes; module, memory and CPU power read 0 |
| powerPolicyStatus.tgpmW | no (timestamp stays 0) |
| temperatures[GPU], temperatures[MEMORY] | yes (NvTemp, 24.8 fixed point); board not populated |
| perfDevUtil | yes (gpu/memory busy, raw units) |

Field notes:
- `pstate`: P0 is full performance; P4 is what the GPU sits in at the desktop.
- `gpc_mhz`, `mem_mhz`: RUSD actual clocks. The PMUMON control gives the same graphics clock; its "active GPC" value stayed at 2790 MHz in every sample, so it is not a live reading.
- `limit_req_mw` / `limit_enf_mw`: the GPU power limit the RM asked for and the one being enforced.
- `power_mw_inst` / `power_mw_avg`: instant and averaged GPU power.
- `throttle`: decoded `RUSD_CLK_THROTTLE_REASON_*` bits (gpu_idle, app_clock_setting, sw_power_cap, hw_slowdown, sync_boost, sw_thermal_slowdown, hw_thermal_slowdown, hw_power_brake, display_clock_setting).
- The perf level clock domains (0x1 at 180 MHz, 0x10 at 405 MHz) do not match the graphics or memory clock; the open headers do not name the domain ids, so treat them as unidentified.

## Sample output

Idle (desktop, nothing running), `--once`:

```
pstate            RUSD P4   control P4
graphics clock    1417 MHz (target 1417 MHz)   PMUMON gpc 1417 MHz active 2790 MHz
memory clock      9001 MHz (target 9001 MHz)   PMUMON dram 9001 MHz
power limit       requested 50000 mW, enforced 50000 mW
power draw        instant 14749 mW, average 12539 mW, TGP n/a mW
temperature       GPU 59.8 C (RUSD)   control 59 C   memory 62.0 C
throttle reasons  none (0x0)
```

`--watch 200` with `nvgfx-burn 10` started at about 5 s (every fourth line):

```
time,pstate,gpc_mhz,mem_mhz,power_mw_inst,power_mw_avg,limit_req_mw,limit_enf_mw,tgp_mw,temp_c,throttle_mask,throttle
0.000,P4,1455,9001,10913,12339,50000,50000,n/a,57.8,0x0,none
4.891,P4,1440,9001,10996,11188,50000,50000,n/a,57.4,0x0,none
5.704,P0,1477,12001,38537,51400,50000,50000,n/a,63.6,0x4,sw_power_cap
6.522,P0,1905,12001,59811,51284,50000,50000,n/a,67.4,0x4,sw_power_cap
8.960,P0,1597,12001,43191,49282,50000,50000,n/a,65.8,0x4,sw_power_cap
12.203,P0,1807,12001,52794,50444,50000,50000,n/a,68.2,0x4,sw_power_cap
14.631,P0,1725,12001,48614,49995,50000,50000,n/a,68.4,0x4,sw_power_cap
16.255,P4,1417,9001,13818,14596,50000,50000,n/a,61.1,0x0,none
```

nvgfx-burn reached about 12.0 to 12.3 TFLOP/s FP32 during that run. Under load the
average draw sits right on the 50 W limit, the graphics clock moves between about
1600 and 1900 MHz, and the only throttle reason is `sw_power_cap`. The temperature
stays under 70 C, so the GPU is held back by the power limit, not by heat.
