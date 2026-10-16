// nvgfx-smi: read-only GPU telemetry for NvGfx on macOS.
//
// Talks to the NVRM kext the same way the NVK userland does (nvrm_xnu.c in
// nvk/nvk-macos.patch): an IOUserClient connection per "fd", selector 1 is the
// RM escape (NV_ESC_*), selector 3 binds a posix fd number to the connection,
// and IOConnectMapMemory64(type 0) maps whatever the last NV_ESC_RM_MAP_MEMORY
// set up on that connection.
//
// What it does: allocate a root client, a device and a subdevice, read a few
// RM controls, allocate the RM user shared data page (class NV00DE, "RUSD")
// with polling for clocks, perf, power and thermal, map it read-only, and read
// it with the timestamp seqlock. Nothing is written to the GPU: only GET
// controls are issued, and the allocations are freed on exit (the RM also
// frees them when the connection closes).
//
// Usage: nvgfx-smi [--once] [--json] [--watch [ms]] [--verbose]

#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "nvtypes.h"
#include "nvmisc.h"
#include "nvos.h"
#include "nvstatus.h"
#include "nv-ioctl.h"
#include "nv-ioctl-numbers.h"
#include "nv_escape.h"
#include "nv-unix-nvos-params-wrappers.h"
#include "class/cl0000.h"
#include "class/cl0080.h"
#include "class/cl2080.h"
#include "class/cl00de.h"
#include "ctrl/ctrl0000/ctrl0000gpu.h"
#include "ctrl/ctrl2080/ctrl2080perf.h"
#include "ctrl/ctrl2080/ctrl2080clk.h"
#include "ctrl/ctrl2080/ctrl2080thermal.h"
#include "ctrl/ctrl2080/ctrl2080pmgr.h"
#include "ctrl/ctrl00de.h"

#ifndef NV2080_CTRL_CMD_PERF_RATED_TDP_GET_STATUS
#define NV2080_CTRL_CMD_PERF_RATED_TDP_GET_STATUS 0x2080206dU
#endif

enum { SEL_GO = 0, SEL_ESC = 1, SEL_GETFD = 2, SEL_BINDFD = 3 };

static int gVerbose;
static volatile sig_atomic_t gStop;

// ---------------------------------------------------------------- transport

typedef struct { io_connect_t conn; int fd; } Conn;

static int conn_open(Conn *c, unsigned type /* 0 ctl, 1 device */)
{
    memset(c, 0, sizeof *c);
    c->fd = -1;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVAccel"));
    uint32_t open_type = 0x4E560000u | type;
    kern_return_t kr = KERN_FAILURE;
    if (svc) {
        kr = IOServiceOpen(svc, mach_task_self(), open_type, &c->conn);
        IOObjectRelease(svc);
        if (kr != KERN_SUCCESS && gVerbose)
            fprintf(stderr, "nvgfx-smi: IOServiceOpen(NVAccel, 0x%08x) -> 0x%08x, trying NVRM\n", open_type, kr);
    }
    if (kr != KERN_SUCCESS) {
        svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVRM"));
        if (!svc) { fprintf(stderr, "nvgfx-smi: no NVAccel and no NVRM service\n"); return -1; }
        kr = IOServiceOpen(svc, mach_task_self(), type, &c->conn);
        IOObjectRelease(svc);
    }
    if (kr != KERN_SUCCESS) { fprintf(stderr, "nvgfx-smi: IOServiceOpen(type %u) -> 0x%08x\n", type, kr); return -1; }
    // The kext names clients by a posix fd number (MAP_MEMORY passes it), so bind a real one.
    c->fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (c->fd < 0) { IOServiceClose(c->conn); c->conn = 0; return -1; }
    uint64_t in = (uint64_t)c->fd;
    kr = IOConnectCallScalarMethod(c->conn, SEL_BINDFD, &in, 1, NULL, NULL);
    if (kr != KERN_SUCCESS) fprintf(stderr, "nvgfx-smi: bind fd %d -> 0x%08x\n", c->fd, kr);
    return 0;
}

static void conn_close(Conn *c)
{
    if (c->conn) IOServiceClose(c->conn);
    if (c->fd >= 0) close(c->fd);
    c->conn = 0; c->fd = -1;
}

// Returns the IOKit status; RM status lives in the params.
static kern_return_t esc(Conn *c, uint32_t cmd, void *data, uint32_t size)
{
    uint64_t in = cmd; size_t out = size;
    kern_return_t kr = IOConnectCallMethod(c->conn, SEL_ESC, &in, 1, data, size, NULL, NULL, data, &out);
    if (kr != KERN_SUCCESS && gVerbose) fprintf(stderr, "nvgfx-smi: esc 0x%02x (%u bytes) -> kr 0x%08x\n", cmd, size, kr);
    return kr;
}

// ---------------------------------------------------------------- RM helpers

typedef struct {
    Conn ctl;
    NvHandle hClient, hDevice, hSubdev, hRusd;
    nv_ioctl_card_info_t card;
    // RUSD mapping
    Conn mapConn;
    const volatile NV00DE_SHARED_DATA *rusd;
    mach_vm_size_t rusdLen;
    NvU32 rusdAllocStatus, rusdMapStatus;
    kern_return_t rusdMapKr;
} Rm;

static NvU32 rm_alloc(Rm *rm, NvHandle parent, NvHandle *h, NvU32 cls, void *params, NvU32 psize)
{
    NVOS21_PARAMETERS p;
    memset(&p, 0, sizeof p);
    p.hRoot = rm->hClient;
    p.hObjectParent = parent;
    p.hObjectNew = *h;
    p.hClass = cls;
    p.pAllocParms = params;
    p.paramsSize = psize;
    if (esc(&rm->ctl, NV_ESC_RM_ALLOC, &p, sizeof p) != KERN_SUCCESS && p.status == NV_OK) return NV_ERR_GENERIC;
    *h = p.hObjectNew;
    return p.status;
}

static NvU32 rm_free(Rm *rm, NvHandle h)
{
    NVOS00_PARAMETERS p;
    memset(&p, 0, sizeof p);
    p.hRoot = rm->hClient;
    p.hObjectParent = h == rm->hClient ? 0 : rm->hClient;
    p.hObjectOld = h;
    if (esc(&rm->ctl, NV_ESC_RM_FREE, &p, sizeof p) != KERN_SUCCESS && p.status == NV_OK) return NV_ERR_GENERIC;
    return p.status;
}

static NvU32 rm_ctrl(Rm *rm, NvHandle h, NvU32 cmd, void *params, NvU32 psize)
{
    NVOS54_PARAMETERS p;
    memset(&p, 0, sizeof p);
    p.hClient = rm->hClient;
    p.hObject = h;
    p.cmd = cmd;
    p.params = params;
    p.paramsSize = psize;
    if (esc(&rm->ctl, NV_ESC_RM_CONTROL, &p, sizeof p) != KERN_SUCCESS && p.status == NV_OK) return NV_ERR_GENERIC;
    return p.status;
}

// Small status log so every control's outcome is printed once.
#define MAX_LOG 32
static struct { const char *name; NvU32 cmd; NvU32 status; } gLog[MAX_LOG];
static int gLogN;
static void log_status(const char *name, NvU32 cmd, NvU32 st)
{
    for (int i = 0; i < gLogN; i++)
        if (gLog[i].cmd == cmd && !strcmp(gLog[i].name, name)) { gLog[i].status = st; return; }
    if (gLogN < MAX_LOG) { gLog[gLogN].name = name; gLog[gLogN].cmd = cmd; gLog[gLogN].status = st; gLogN++; }
}

static int rm_open(Rm *rm)
{
    memset(rm, 0, sizeof *rm);
    rm->mapConn.fd = -1;
    rm->rusdAllocStatus = rm->rusdMapStatus = NV_ERR_NOT_SUPPORTED;
    if (conn_open(&rm->ctl, 0)) return -1;

    kern_return_t kr = esc(&rm->ctl, NV_ESC_CARD_INFO, &rm->card, sizeof rm->card);
    if (kr != KERN_SUCCESS || !rm->card.valid) {
        fprintf(stderr, "nvgfx-smi: NV_ESC_CARD_INFO -> kr 0x%08x valid %d (driver not up?)\n", kr, rm->card.valid);
        return -1;
    }

    NvU32 st = rm_alloc(rm, 0, &rm->hClient, NV01_ROOT_CLIENT, NULL, 0);
    log_status("alloc NV01_ROOT_CLIENT", NV01_ROOT_CLIENT, st);
    if (st != NV_OK) { fprintf(stderr, "nvgfx-smi: alloc root client -> RM 0x%08x\n", st); return -1; }

    NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS id;
    memset(&id, 0, sizeof id);
    id.gpuId = rm->card.gpu_id;
    st = rm_ctrl(rm, rm->hClient, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &id, sizeof id);
    log_status("NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2", NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, st);
    if (st != NV_OK) { fprintf(stderr, "nvgfx-smi: GPU_GET_ID_INFO_V2 -> RM 0x%08x\n", st); return -1; }

    NV0080_ALLOC_PARAMETERS a80;
    memset(&a80, 0, sizeof a80);
    a80.deviceId = id.deviceInstance;
    a80.hClientShare = rm->hClient;
    rm->hDevice = 0xcaf00001;
    st = rm_alloc(rm, rm->hClient, &rm->hDevice, NV01_DEVICE_0, &a80, sizeof a80);
    log_status("alloc NV01_DEVICE_0", NV01_DEVICE_0, st);
    if (st != NV_OK) { fprintf(stderr, "nvgfx-smi: alloc device -> RM 0x%08x\n", st); return -1; }

    NV2080_ALLOC_PARAMETERS a20;
    memset(&a20, 0, sizeof a20);
    a20.subDeviceId = id.subDeviceInstance;
    rm->hSubdev = 0xcaf00002;
    st = rm_alloc(rm, rm->hDevice, &rm->hSubdev, NV20_SUBDEVICE_0, &a20, sizeof a20);
    log_status("alloc NV20_SUBDEVICE_0", NV20_SUBDEVICE_0, st);
    if (st != NV_OK) { fprintf(stderr, "nvgfx-smi: alloc subdevice -> RM 0x%08x\n", st); return -1; }
    return 0;
}

// RUSD: allocate NV00DE under the subdevice with polling, then map it.
// Mapping follows NVK's nvRmApiMapMemory for system memory: open a second ctl
// connection, issue NV_ESC_RM_MAP_MEMORY on the first one naming the second's
// fd, then IOConnectMapMemory64 on the second connection.
static void rusd_open(Rm *rm)
{
    NV00DE_ALLOC_PARAMETERS ap;
    memset(&ap, 0, sizeof ap);
    ap.polledDataMask = NV00DE_RUSD_POLL_CLOCK | NV00DE_RUSD_POLL_PERF | NV00DE_RUSD_POLL_POWER | NV00DE_RUSD_POLL_THERMAL;
    rm->hRusd = 0xcaf00003;
    NvU32 st = rm_alloc(rm, rm->hSubdev, &rm->hRusd, RM_USER_SHARED_DATA, &ap, sizeof ap);
    log_status("alloc NV00DE RM_USER_SHARED_DATA (poll CLOCK|PERF|POWER|THERMAL)", RM_USER_SHARED_DATA, st);
    if (st != NV_OK) {
        // Retry without polling: the non-polled fields may still be useful.
        ap.polledDataMask = 0;
        rm->hRusd = 0xcaf00003;
        NvU32 st2 = rm_alloc(rm, rm->hSubdev, &rm->hRusd, RM_USER_SHARED_DATA, &ap, sizeof ap);
        log_status("alloc NV00DE RM_USER_SHARED_DATA (no polling)", RM_USER_SHARED_DATA | 0x10000, st2);
        if (st2 != NV_OK) { rm->hRusd = 0; rm->rusdAllocStatus = st2; return; }
    }
    rm->rusdAllocStatus = NV_OK;

    if (conn_open(&rm->mapConn, 0)) { rm->rusdMapStatus = NV_ERR_GENERIC; return; }
    nv_ioctl_nvos33_parameters_with_fd p;
    memset(&p, 0, sizeof p);
    p.params.hClient = rm->hClient;
    p.params.hDevice = rm->hSubdev;
    p.params.hMemory = rm->hRusd;
    p.params.offset = 0;
    p.params.length = sizeof(NV00DE_SHARED_DATA);
    p.params.flags = DRF_DEF(OS33, _FLAGS, _ACCESS, _READ_ONLY);
    p.fd = rm->mapConn.fd;
    kern_return_t kr = esc(&rm->ctl, NV_ESC_RM_MAP_MEMORY, &p, sizeof p);
    rm->rusdMapStatus = (kr != KERN_SUCCESS && p.params.status == NV_OK) ? NV_ERR_GENERIC : p.params.status;
    log_status("NV_ESC_RM_MAP_MEMORY on NV00DE", 0x4E, rm->rusdMapStatus);
    if (rm->rusdMapStatus != NV_OK) return;

    mach_vm_address_t addr = 0; mach_vm_size_t len = 0;
    kr = IOConnectMapMemory64(rm->mapConn.conn, 0, mach_task_self(), &addr, &len, kIOMapAnywhere | kIOMapReadOnly);
    rm->rusdMapKr = kr;
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "nvgfx-smi: IOConnectMapMemory64 (RUSD) -> 0x%08x\n", kr);
        rm->rusdMapStatus = NV_ERR_GENERIC;
        return;
    }
    if (len < sizeof(NV00DE_SHARED_DATA))
        fprintf(stderr, "nvgfx-smi: RUSD mapping is 0x%llx bytes, struct is 0x%zx\n", (unsigned long long)len, sizeof(NV00DE_SHARED_DATA));
    rm->rusd = (const volatile NV00DE_SHARED_DATA *)(uintptr_t)addr;
    rm->rusdLen = len;
}

static void rm_close(Rm *rm)
{
    if (rm->rusd) IOConnectUnmapMemory64(rm->mapConn.conn, 0, mach_task_self(), (mach_vm_address_t)(uintptr_t)rm->rusd);
    rm->rusd = NULL;
    if (rm->hClient) {
        if (rm->hRusd && rm->rusdMapStatus == NV_OK) {
            NVOS34_PARAMETERS u;
            memset(&u, 0, sizeof u);
            u.hClient = rm->hClient; u.hDevice = rm->hSubdev; u.hMemory = rm->hRusd;
            esc(&rm->ctl, NV_ESC_RM_UNMAP_MEMORY, &u, sizeof u);
        }
        rm_free(rm, rm->hClient);   // frees everything under it
    }
    conn_close(&rm->mapConn);
    conn_close(&rm->ctl);
}

// Seqlock read of one RUSD section (timestamp first, copy, timestamp again).
// Returns the timestamp, or 0 if invalid/unavailable.
static NvU64 rusd_read(const volatile void *src, void *dst, size_t n)
{
    for (int tries = 0; tries < 20; tries++) {
        NvU64 t0 = *(const volatile NvU64 *)src;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (!RUSD_SEQ_DATA_VALID(t0)) { if (t0 == RUSD_TIMESTAMP_INVALID) break; continue; }
        memcpy(dst, (const void *)src, n);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (*(const volatile NvU64 *)src == t0) return t0;
    }
    memset(dst, 0, n);
    return 0;
}

// ---------------------------------------------------------------- sample

#define NA 0xFFFFFFFFu
#define NAS (-2147483647 - 1)

typedef struct {
    double t;
    // RUSD
    NvU64 tsClk, tsThr, tsPstate, tsLimit, tsTemp, tsAvg, tsInst, tsTgp, tsUtil;
    NvU32 gpcMHz, memMHz, gpcTargetMHz, memTargetMHz, vidMHz, smMHz;
    NvU32 thrMask;
    NvU32 rusdPstate;
    NvU32 limReq, limEnf;
    NvS32 tempGpuRaw, tempMemRaw, tempBoardRaw;
    NvU32 avgGpu, avgModule, avgMem;
    NvU32 instGpu, instModule, instCpu;
    NvU32 tgp;
    NvU32 gpuBusy, memBusy;
    // controls
    NvU32 pstateCtrl;      // bit mask
    NvU32 pmuGpcKHz, pmuMemKHz, pmuActiveGpcKHz, pmuNvdKHz;
    NvS32 ctrlTempC;
} Sample;

static NV2080_CTRL_CLK_PMUMON_CLK_DOMAINS_GET_SAMPLES_PARAMS gPmu;
static int gThermSensor = -1;   // sensor index of the GPU, found once

static double now_s(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec + tv.tv_usec / 1e6; }

static void thermal_find_sensor(Rm *rm)
{
    static NV2080_CTRL_THERMAL_SYSTEM_EXECUTE_V2_PARAMS p;
    memset(&p, 0, sizeof p);
    p.clientAPIVersion = THERMAL_SYSTEM_API_VER;
    p.clientAPIRevision = THERMAL_SYSTEM_API_REV;
    p.clientInstructionSizeOf = sizeof(NV2080_CTRL_THERMAL_SYSTEM_INSTRUCTION);
    p.executeFlags = NV2080_CTRL_THERMAL_SYSTEM_EXECUTE_FLAGS_IGNORE_FAIL;
    p.instructionListSize = 1;
    p.instructionList[0].opcode = NV2080_CTRL_THERMAL_SYSTEM_GET_INFO_SENSORS_AVAILABLE_OPCODE;
    NvU32 st = rm_ctrl(rm, rm->hSubdev, NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE_V2, &p, sizeof p);
    log_status("NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE_V2 (sensors available)", 0x20800513 ^ 1, st);
    if (st != NV_OK || !p.instructionList[0].executed || p.instructionList[0].result != NV_OK) {
        if (gVerbose) fprintf(stderr, "thermal: sensors-available st 0x%x executed %u result 0x%x\n", st,
                              p.instructionList[0].executed, p.instructionList[0].result);
        gThermSensor = 0;   // try sensor 0 anyway
        return;
    }
    NvU32 n = p.instructionList[0].operands.getInfoSensorsAvailable.availableSensors;
    if (gVerbose) fprintf(stderr, "thermal: %u sensor(s) available\n", n);
    if (n > 8) n = 8;
    // For each sensor ask its target, and each target's type; type 1 is the GPU.
    memset(&p, 0, sizeof p);
    p.clientAPIVersion = THERMAL_SYSTEM_API_VER;
    p.clientAPIRevision = THERMAL_SYSTEM_API_REV;
    p.clientInstructionSizeOf = sizeof(NV2080_CTRL_THERMAL_SYSTEM_INSTRUCTION);
    p.executeFlags = NV2080_CTRL_THERMAL_SYSTEM_EXECUTE_FLAGS_IGNORE_FAIL;
    for (NvU32 i = 0; i < n; i++) {
        p.instructionList[2 * i].opcode = NV2080_CTRL_THERMAL_SYSTEM_GET_INFO_SENSOR_TARGET_OPCODE;
        p.instructionList[2 * i].operands.getInfoSensorTarget.sensorIndex = i;
        p.instructionList[2 * i + 1].opcode = NV2080_CTRL_THERMAL_SYSTEM_GET_STATUS_SENSOR_READING_OPCODE;
        p.instructionList[2 * i + 1].operands.getStatusSensorReading.sensorIndex = i;
    }
    p.instructionListSize = 2 * n;
    st = rm_ctrl(rm, rm->hSubdev, NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE_V2, &p, sizeof p);
    gThermSensor = 0;
    if (st != NV_OK) return;
    for (NvU32 i = 0; i < n; i++) {
        NV2080_CTRL_THERMAL_SYSTEM_INSTRUCTION *t = &p.instructionList[2 * i], *r = &p.instructionList[2 * i + 1];
        if (gVerbose)
            fprintf(stderr, "thermal: sensor %u target %u (res 0x%x) reading %d (res 0x%x)\n", i,
                    t->operands.getInfoSensorTarget.targetIndex, t->result,
                    r->operands.getStatusSensorReading.value, r->result);
    }
}

static NvU32 thermal_read(Rm *rm, NvS32 *out)
{
    static NV2080_CTRL_THERMAL_SYSTEM_EXECUTE_V2_PARAMS p;
    memset(&p, 0, sizeof p);
    p.clientAPIVersion = THERMAL_SYSTEM_API_VER;
    p.clientAPIRevision = THERMAL_SYSTEM_API_REV;
    p.clientInstructionSizeOf = sizeof(NV2080_CTRL_THERMAL_SYSTEM_INSTRUCTION);
    p.executeFlags = NV2080_CTRL_THERMAL_SYSTEM_EXECUTE_FLAGS_IGNORE_FAIL;
    p.instructionListSize = 1;
    p.instructionList[0].opcode = NV2080_CTRL_THERMAL_SYSTEM_GET_STATUS_SENSOR_READING_OPCODE;
    p.instructionList[0].operands.getStatusSensorReading.sensorIndex = (NvU32)(gThermSensor < 0 ? 0 : gThermSensor);
    NvU32 st = rm_ctrl(rm, rm->hSubdev, NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE_V2, &p, sizeof p);
    if (st == NV_OK && (!p.instructionList[0].executed || p.instructionList[0].result != NV_OK))
        st = p.instructionList[0].result ? p.instructionList[0].result : NV_ERR_GENERIC;
    *out = st == NV_OK ? p.instructionList[0].operands.getStatusSensorReading.value : NAS;
    return st;
}

static void take_sample(Rm *rm, Sample *s)
{
    memset(s, 0, sizeof *s);
    s->t = now_s();
    s->gpcMHz = s->memMHz = s->gpcTargetMHz = s->memTargetMHz = s->vidMHz = s->smMHz = NA;
    s->thrMask = s->rusdPstate = s->limReq = s->limEnf = NA;
    s->tempGpuRaw = s->tempMemRaw = s->tempBoardRaw = NAS;
    s->avgGpu = s->avgModule = s->avgMem = s->instGpu = s->instModule = s->instCpu = s->tgp = NA;
    s->gpuBusy = s->memBusy = NA;
    s->pstateCtrl = NA;
    s->pmuGpcKHz = s->pmuMemKHz = s->pmuActiveGpcKHz = s->pmuNvdKHz = NA;
    s->ctrlTempC = NAS;

    if (rm->rusd) {
        const volatile NV00DE_SHARED_DATA *d = rm->rusd;
        RUSD_CLK_PUBLIC_DOMAIN_INFOS clk;
        if ((s->tsClk = rusd_read(&d->clkPublicDomainInfos, &clk, sizeof clk))) {
            s->gpcMHz = clk.info[RUSD_CLK_PUBLIC_DOMAIN_GRAPHICS].actualClkKHz / 1000;
            s->memMHz = clk.info[RUSD_CLK_PUBLIC_DOMAIN_MEMORY].actualClkKHz / 1000;
            s->vidMHz = clk.info[RUSD_CLK_PUBLIC_DOMAIN_VIDEO].actualClkKHz / 1000;
            s->smMHz  = clk.info[RUSD_CLK_PUBLIC_DOMAIN_SM].actualClkKHz / 1000;
            s->gpcTargetMHz = clk.info[RUSD_CLK_PUBLIC_DOMAIN_GRAPHICS].targetClkMHz;
            s->memTargetMHz = clk.info[RUSD_CLK_PUBLIC_DOMAIN_MEMORY].targetClkMHz;
        }
        static RUSD_CLK_THROTTLE_INFO thr;
        if ((s->tsThr = rusd_read(&d->clkThrottleInfo, &thr, sizeof thr))) s->thrMask = thr.reasonMask;
        RUSD_PERF_CURRENT_PSTATE ps;
        if ((s->tsPstate = rusd_read(&d->perfCurrentPstate, &ps, sizeof ps))) s->rusdPstate = ps.currentPstate;
        RUSD_POWER_LIMITS lim;
        if ((s->tsLimit = rusd_read(&d->powerLimitGpu, &lim, sizeof lim))) { s->limReq = lim.info.requestedmW; s->limEnf = lim.info.enforcedmW; }
        RUSD_TEMPERATURE tmp;
        if ((s->tsTemp = rusd_read(&d->temperatures[RUSD_TEMPERATURE_TYPE_GPU], &tmp, sizeof tmp))) s->tempGpuRaw = tmp.temperature;
        if (rusd_read(&d->temperatures[RUSD_TEMPERATURE_TYPE_MEMORY], &tmp, sizeof tmp)) s->tempMemRaw = tmp.temperature;
        if (rusd_read(&d->temperatures[RUSD_TEMPERATURE_TYPE_BOARD], &tmp, sizeof tmp)) s->tempBoardRaw = tmp.temperature;
        RUSD_AVG_POWER_USAGE avg;
        if ((s->tsAvg = rusd_read(&d->avgPowerUsage, &avg, sizeof avg))) { s->avgGpu = avg.info.averageGpuPower; s->avgModule = avg.info.averageModulePower; s->avgMem = avg.info.averageMemoryPower; }
        RUSD_INST_POWER_USAGE inst;
        if ((s->tsInst = rusd_read(&d->instPowerUsage, &inst, sizeof inst))) { s->instGpu = inst.info.instGpuPower; s->instModule = inst.info.instModulePower; s->instCpu = inst.info.instCpuPower; }
        RUSD_POWER_POLICY_STATUS pp;
        if ((s->tsTgp = rusd_read(&d->powerPolicyStatus, &pp, sizeof pp))) s->tgp = pp.info.tgpmW;
        RUSD_PERF_DEVICE_UTILIZATION ut;
        if ((s->tsUtil = rusd_read(&d->perfDevUtil, &ut, sizeof ut))) { s->gpuBusy = ut.info.gpuPercentBusy; s->memBusy = ut.info.memoryPercentBusy; }
    }

    NV2080_CTRL_PERF_GET_CURRENT_PSTATE_PARAMS cp = {0};
    NvU32 st = rm_ctrl(rm, rm->hSubdev, NV2080_CTRL_CMD_PERF_GET_CURRENT_PSTATE, &cp, sizeof cp);
    log_status("NV2080_CTRL_CMD_PERF_GET_CURRENT_PSTATE", NV2080_CTRL_CMD_PERF_GET_CURRENT_PSTATE, st);
    if (st == NV_OK) s->pstateCtrl = cp.currPstate;

    // PMUMON returns the samples published since the last call; keep super between calls.
    st = rm_ctrl(rm, rm->hSubdev, NV2080_CTRL_CMD_CLK_PMUMON_CLK_DOMAINS_GET_SAMPLES, &gPmu, sizeof gPmu);
    log_status("NV2080_CTRL_CMD_CLK_PMUMON_CLK_DOMAINS_GET_SAMPLES", NV2080_CTRL_CMD_CLK_PMUMON_CLK_DOMAINS_GET_SAMPLES, st);
    if (st == NV_OK && gPmu.super.numSamples > 0) {
        NvU32 n = gPmu.super.numSamples;
        if (n > NV2080_CTRL_CLK_PMUMON_CLK_DOMAINS_SAMPLE_COUNT) n = NV2080_CTRL_CLK_PMUMON_CLK_DOMAINS_SAMPLE_COUNT;
        NV2080_CTRL_CLK_PMUMON_CLK_DOMAINS_SAMPLE *x = &gPmu.samples[n - 1];
        s->pmuGpcKHz = x->gpcClkFreqKHz; s->pmuMemKHz = x->dramClkFreqKHz;
        s->pmuActiveGpcKHz = x->activeGpcClkFreqKHz; s->pmuNvdKHz = x->nvdClkFreqKHz;
    }

    NvS32 t;
    st = thermal_read(rm, &t);
    log_status("NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE_V2 (sensor reading)", NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE_V2, st);
    if (st == NV_OK) s->ctrlTempC = t;
}

// ---------------------------------------------------------------- one-shot info

typedef struct {
    NvU32 tdpStatusSt, tdpCtrlSt, modSt, levelSt;
    NV2080_CTRL_PERF_RATED_TDP_STATUS_PARAMS tdpStatus;
    NV2080_CTRL_PERF_RATED_TDP_CONTROL_PARAMS tdpCtrl;
    NV2080_CTRL_PMGR_MODULE_INFO_PARAMS mod;
    NV2080_CTRL_PERF_GET_LEVEL_INFO_V2_PARAMS level;
} Once;

static void take_once(Rm *rm, Once *o)
{
    memset(o, 0, sizeof *o);
    o->tdpStatusSt = rm_ctrl(rm, rm->hSubdev, NV2080_CTRL_CMD_PERF_RATED_TDP_GET_STATUS, &o->tdpStatus, sizeof o->tdpStatus);
    log_status("NV2080_CTRL_CMD_PERF_RATED_TDP_GET_STATUS", NV2080_CTRL_CMD_PERF_RATED_TDP_GET_STATUS, o->tdpStatusSt);
    o->tdpCtrl.client = NV2080_CTRL_PERF_RATED_TDP_CLIENT_RM;
    o->tdpCtrlSt = rm_ctrl(rm, rm->hSubdev, NV2080_CTRL_CMD_PERF_RATED_TDP_GET_CONTROL, &o->tdpCtrl, sizeof o->tdpCtrl);
    log_status("NV2080_CTRL_CMD_PERF_RATED_TDP_GET_CONTROL", NV2080_CTRL_CMD_PERF_RATED_TDP_GET_CONTROL, o->tdpCtrlSt);
    o->modSt = rm_ctrl(rm, rm->hSubdev, NV2080_CTRL_CMD_PMGR_GET_MODULE_INFO, &o->mod, sizeof o->mod);
    log_status("NV2080_CTRL_CMD_PMGR_GET_MODULE_INFO", NV2080_CTRL_CMD_PMGR_GET_MODULE_INFO, o->modSt);
    // The open headers do not carry the clock domain ids, so probe each single-bit
    // domain on its own and keep the ones the RM accepts.
    static NV2080_CTRL_PERF_GET_LEVEL_INFO_V2_PARAMS one;
    NvU32 n = 0, lastBad = NV_OK;
    for (NvU32 i = 0; i < 32; i++) {
        memset(&one, 0, sizeof one);
        one.level = NV2080_CTRL_PERF_GET_LEVEL_INFO_LEVEL_STANDARD;
        one.perfGetClkInfoList[0].domain = 1u << i;
        one.perfGetClkInfoListSize = 1;
        NvU32 st = rm_ctrl(rm, rm->hSubdev, NV2080_CTRL_CMD_PERF_GET_LEVEL_INFO_V2, &one, sizeof one);
        if (st == NV_OK && n < NV2080_CTRL_PERF_CLK_MAX_DOMAINS) { o->level.perfGetClkInfoList[n++] = one.perfGetClkInfoList[0]; o->level.flags = one.flags; }
        else if (st != NV_OK) lastBad = st;
    }
    o->level.perfGetClkInfoListSize = n;
    o->levelSt = n ? NV_OK : lastBad;
    log_status("NV2080_CTRL_CMD_PERF_GET_LEVEL_INFO_V2 (per-domain probe)", NV2080_CTRL_CMD_PERF_GET_LEVEL_INFO_V2, o->levelSt);
}

// ---------------------------------------------------------------- formatting

static const char *kThrNames[] = {
    "gpu_idle", "app_clock_setting", "sw_power_cap", "hw_slowdown", "sync_boost",
    "sw_thermal_slowdown", "hw_thermal_slowdown", "hw_power_brake", "display_clock_setting",
};

static void throttle_str(NvU32 m, char *buf, size_t n)
{
    buf[0] = 0;
    if (m == NA) { snprintf(buf, n, "n/a"); return; }
    if (m == 0) { snprintf(buf, n, "none"); return; }
    for (unsigned i = 0; i < 32; i++) if (m & (1u << i)) {
        char tmp[40];
        if (i < sizeof kThrNames / sizeof kThrNames[0]) snprintf(tmp, sizeof tmp, "%s", kThrNames[i]);
        else snprintf(tmp, sizeof tmp, "bit%u", i);
        if (buf[0]) strlcat(buf, "|", n);
        strlcat(buf, tmp, n);
    }
}

// pstate: the RUSD field and the control both use the NV2080_CTRL_PERF_PSTATES_Px bit mask.
static int pstate_num(NvU32 v)
{
    if (v == NA || v == 0) return -1;
    for (int i = 0; i < 16; i++) if (v == (1u << i)) return i;
    return -1;
}

// NvTemp is signed 24.8 fixed point.
static double temp_c(NvS32 raw) { return raw / 256.0; }

static void u32(char *b, size_t n, NvU32 v) { if (v == NA) snprintf(b, n, "n/a"); else snprintf(b, n, "%u", v); }
static void ju32(FILE *f, const char *k, NvU32 v, int comma) { if (v == NA) fprintf(f, "\"%s\": null", k); else fprintf(f, "\"%s\": %u", k, v); if (comma) fputs(", ", f); }

static const char *rm_status_name(NvU32 st)
{
    switch (st) {
    case NV_OK: return "NV_OK";
    case NV_ERR_NOT_SUPPORTED: return "NV_ERR_NOT_SUPPORTED";
    case NV_ERR_INVALID_ARGUMENT: return "NV_ERR_INVALID_ARGUMENT";
    case NV_ERR_INVALID_PARAM_STRUCT: return "NV_ERR_INVALID_PARAM_STRUCT";
    case NV_ERR_INSUFFICIENT_PERMISSIONS: return "NV_ERR_INSUFFICIENT_PERMISSIONS";
    case NV_ERR_INVALID_COMMAND: return "NV_ERR_INVALID_COMMAND";
    case NV_ERR_INVALID_STATE: return "NV_ERR_INVALID_STATE";
    case NV_ERR_INVALID_OBJECT_HANDLE: return "NV_ERR_INVALID_OBJECT_HANDLE";
    case NV_ERR_INVALID_CLASS: return "NV_ERR_INVALID_CLASS";
    case NV_ERR_GENERIC: return "NV_ERR_GENERIC";
    case NV_ERR_OBJECT_NOT_FOUND: return "NV_ERR_OBJECT_NOT_FOUND";
    case NV_ERR_NOT_READY: return "NV_ERR_NOT_READY";
    case NV_ERR_STATE_IN_USE: return "NV_ERR_STATE_IN_USE";
    case NV_ERR_INVALID_REQUEST: return "NV_ERR_INVALID_REQUEST";
    case NV_ERR_TIMEOUT: return "NV_ERR_TIMEOUT";
    case NV_ERR_INVALID_LIMIT: return "NV_ERR_INVALID_LIMIT";
    case NV_ERR_INVALID_FLAGS: return "NV_ERR_INVALID_FLAGS";
    case NV_ERR_OPERATING_SYSTEM: return "NV_ERR_OPERATING_SYSTEM";
    case NV_ERR_INVALID_DEVICE: return "NV_ERR_INVALID_DEVICE";
    case NV_ERR_ILLEGAL_ACTION: return "NV_ERR_ILLEGAL_ACTION";
    default: return "";
    }
}

static void print_status_log(FILE *f)
{
    fprintf(f, "RM calls:\n");
    for (int i = 0; i < gLogN; i++)
        fprintf(f, "  %-66s 0x%08x %s\n", gLog[i].name, gLog[i].status, rm_status_name(gLog[i].status));
}

static void print_human(Rm *rm, Once *o, Sample *s)
{
    char b1[32], b2[32], thr[256];
    printf("nvgfx-smi  GPU %04x:%04x at %04x:%02x:%02x  gpu_id 0x%x\n",
           rm->card.pci_info.vendor_id, rm->card.pci_info.device_id,
           rm->card.pci_info.domain, rm->card.pci_info.bus, rm->card.pci_info.slot, rm->card.gpu_id);
    printf("RUSD: alloc 0x%08x %s, map 0x%08x %s%s\n", rm->rusdAllocStatus, rm_status_name(rm->rusdAllocStatus),
           rm->rusdMapStatus, rm_status_name(rm->rusdMapStatus), rm->rusd ? " (mapped)" : " (not mapped)");
    printf("\n");
    int pn = pstate_num(s->rusdPstate), pc = pstate_num(s->pstateCtrl);
    printf("pstate            RUSD %s   control %s\n",
           pn >= 0 ? (snprintf(b1, sizeof b1, "P%d", pn), b1) : "n/a",
           pc >= 0 ? (snprintf(b2, sizeof b2, "P%d", pc), b2) : "n/a");
    u32(b1, sizeof b1, s->gpcMHz); u32(b2, sizeof b2, s->gpcTargetMHz);
    printf("graphics clock    %s MHz (target %s MHz)", b1, b2);
    if (s->pmuGpcKHz != NA && s->pmuGpcKHz) printf("   PMUMON gpc %u MHz active %u MHz", s->pmuGpcKHz / 1000,
                                                 s->pmuActiveGpcKHz == NA ? 0 : s->pmuActiveGpcKHz / 1000);
    printf("\n");
    u32(b1, sizeof b1, s->memMHz); u32(b2, sizeof b2, s->memTargetMHz);
    printf("memory clock      %s MHz (target %s MHz)", b1, b2);
    if (s->pmuMemKHz != NA && s->pmuMemKHz) printf("   PMUMON dram %u MHz", s->pmuMemKHz / 1000);
    printf("\n");
    u32(b1, sizeof b1, s->vidMHz); u32(b2, sizeof b2, s->smMHz);
    printf("video / sm clock  %s / %s MHz\n", b1, b2);
    u32(b1, sizeof b1, s->limReq); u32(b2, sizeof b2, s->limEnf);
    printf("power limit       requested %s mW, enforced %s mW\n", b1, b2);
    u32(b1, sizeof b1, s->instGpu); u32(b2, sizeof b2, s->avgGpu);
    printf("power draw        instant %s mW, average %s mW", b1, b2);
    u32(b1, sizeof b1, s->tgp);
    printf(", TGP %s mW\n", b1);
    if (s->instModule != NA || s->avgModule != NA || s->instCpu != NA)
        printf("                  module inst %u / avg %u mW, memory avg %u mW, cpu inst %u mW\n",
               s->instModule == NA ? 0 : s->instModule, s->avgModule == NA ? 0 : s->avgModule,
               s->avgMem == NA ? 0 : s->avgMem, s->instCpu == NA ? 0 : s->instCpu);
    printf("temperature       ");
    if (s->tempGpuRaw != NAS) printf("GPU %.1f C (RUSD)", temp_c(s->tempGpuRaw)); else printf("RUSD n/a");
    if (s->ctrlTempC != NAS) printf("   control %d C", s->ctrlTempC);
    if (s->tempMemRaw != NAS) printf("   memory %.1f C", temp_c(s->tempMemRaw));
    if (s->tempBoardRaw != NAS) printf("   board %.1f C", temp_c(s->tempBoardRaw));
    printf("\n");
    throttle_str(s->thrMask, thr, sizeof thr);
    printf("throttle reasons  %s", thr);
    if (s->thrMask != NA) printf(" (0x%x)", s->thrMask);
    printf("\n");
    u32(b1, sizeof b1, s->gpuBusy); u32(b2, sizeof b2, s->memBusy);
    printf("utilization       gpu %s, memory %s (raw RUSD units)\n", b1, b2);

    if (o) {
        printf("\n");
        if (o->tdpStatusSt == NV_OK)
            printf("rated TDP status  output action %u vpstate %u, inputs rm %u global %u os %u profile %u\n",
                   o->tdpStatus.output, o->tdpStatus.outputVPstate, o->tdpStatus.inputs[0], o->tdpStatus.inputs[2],
                   o->tdpStatus.inputs[3], o->tdpStatus.inputs[4]);
        if (o->tdpCtrlSt == NV_OK)
            printf("rated TDP control client RM input action %u vpstate %u\n", o->tdpCtrl.input, o->tdpCtrl.vPstateType);
        if (o->modSt == NV_OK)
            printf("PMGR module info  moduleId %u nvswitch %u\n", o->mod.moduleId, o->mod.nvswitchSupport);
        if (o->levelSt == NV_OK) {
            printf("perf level (standard) flags 0x%x:\n", o->level.flags);
            for (NvU32 i = 0; i < o->level.perfGetClkInfoListSize && i < NV2080_CTRL_PERF_CLK_MAX_DOMAINS; i++) {
                NV2080_CTRL_PERF_GET_CLK_INFO *c = &o->level.perfGetClkInfoList[i];
                if (!c->currentFreq && !c->maxFreq && !c->defaultFreq) continue;
                printf("  domain 0x%08x current %u default %u min %u max %u (kHz) flags 0x%x\n",
                       c->domain, c->currentFreq, c->defaultFreq, c->minFreq, c->maxFreq, c->flags);
            }
        }
    }
    printf("\nRUSD timestamps (0 = not populated): clk %llu thr %llu pstate %llu limit %llu temp %llu avg %llu inst %llu tgp %llu util %llu\n",
           (unsigned long long)s->tsClk, (unsigned long long)s->tsThr, (unsigned long long)s->tsPstate,
           (unsigned long long)s->tsLimit, (unsigned long long)s->tsTemp, (unsigned long long)s->tsAvg,
           (unsigned long long)s->tsInst, (unsigned long long)s->tsTgp, (unsigned long long)s->tsUtil);
    print_status_log(stdout);
}

static void print_json(Rm *rm, Once *o, Sample *s)
{
    char thr[256];
    throttle_str(s->thrMask, thr, sizeof thr);
    int pn = pstate_num(s->rusdPstate), pc = pstate_num(s->pstateCtrl);
    printf("{\n  \"time\": %.3f,\n", s->t);
    printf("  \"pci\": \"%04x:%04x\", \"gpu_id\": %u,\n", rm->card.pci_info.vendor_id, rm->card.pci_info.device_id, rm->card.gpu_id);
    printf("  \"rusd\": {\"alloc_status\": %u, \"map_status\": %u, \"mapped\": %s},\n",
           rm->rusdAllocStatus, rm->rusdMapStatus, rm->rusd ? "true" : "false");
    printf("  ");
    if (pn >= 0) printf("\"pstate\": %d, ", pn); else printf("\"pstate\": null, ");
    if (pc >= 0) printf("\"pstate_control\": %d,\n  ", pc); else printf("\"pstate_control\": null,\n  ");
    ju32(stdout, "gpc_mhz", s->gpcMHz, 1); ju32(stdout, "gpc_target_mhz", s->gpcTargetMHz, 1);
    ju32(stdout, "mem_mhz", s->memMHz, 1); ju32(stdout, "mem_target_mhz", s->memTargetMHz, 1);
    ju32(stdout, "video_mhz", s->vidMHz, 1); ju32(stdout, "sm_mhz", s->smMHz, 0);
    printf(",\n  ");
    ju32(stdout, "pmumon_gpc_khz", s->pmuGpcKHz, 1); ju32(stdout, "pmumon_active_gpc_khz", s->pmuActiveGpcKHz, 1);
    ju32(stdout, "pmumon_dram_khz", s->pmuMemKHz, 0);
    printf(",\n  ");
    ju32(stdout, "power_limit_requested_mw", s->limReq, 1); ju32(stdout, "power_limit_enforced_mw", s->limEnf, 1);
    ju32(stdout, "power_inst_mw", s->instGpu, 1); ju32(stdout, "power_avg_mw", s->avgGpu, 1);
    ju32(stdout, "tgp_mw", s->tgp, 1); ju32(stdout, "module_inst_mw", s->instModule, 1);
    ju32(stdout, "module_avg_mw", s->avgModule, 1); ju32(stdout, "memory_avg_mw", s->avgMem, 0);
    printf(",\n  ");
    if (s->tempGpuRaw != NAS) printf("\"temp_c\": %.2f, ", temp_c(s->tempGpuRaw)); else printf("\"temp_c\": null, ");
    if (s->ctrlTempC != NAS) printf("\"temp_c_control\": %d, ", s->ctrlTempC); else printf("\"temp_c_control\": null, ");
    if (s->thrMask != NA) printf("\"throttle_mask\": %u, ", s->thrMask); else printf("\"throttle_mask\": null, ");
    printf("\"throttle\": \"%s\",\n  ", thr);
    ju32(stdout, "gpu_busy", s->gpuBusy, 1); ju32(stdout, "mem_busy", s->memBusy, 0);
    printf(",\n");
    if (o) {
        printf("  \"rated_tdp_status\": ");
        if (o->tdpStatusSt == NV_OK) printf("{\"output\": %u, \"output_vpstate\": %u},\n", o->tdpStatus.output, o->tdpStatus.outputVPstate);
        else printf("null,\n");
        printf("  \"pmgr_module_id\": ");
        if (o->modSt == NV_OK) printf("%u,\n", o->mod.moduleId); else printf("null,\n");
    }
    printf("  \"rm_calls\": [");
    for (int i = 0; i < gLogN; i++)
        printf("%s\n    {\"call\": \"%s\", \"status\": \"0x%08x\"}", i ? "," : "", gLog[i].name, gLog[i].status);
    printf("\n  ]\n}\n");
}

static void csv_header(void)
{
    printf("time,pstate,gpc_mhz,mem_mhz,power_mw_inst,power_mw_avg,limit_req_mw,limit_enf_mw,tgp_mw,temp_c,throttle_mask,throttle\n");
}

static void csv_line(Sample *s, double t0)
{
    char thr[256], b[16][24];
    throttle_str(s->thrMask, thr, sizeof thr);
    int pn = pstate_num(s->rusdPstate);
    if (pn < 0) pn = pstate_num(s->pstateCtrl);
    NvU32 gpc = s->gpcMHz != NA ? s->gpcMHz : (s->pmuGpcKHz != NA && s->pmuGpcKHz ? s->pmuGpcKHz / 1000 : NA);
    NvU32 mem = s->memMHz != NA ? s->memMHz : (s->pmuMemKHz != NA && s->pmuMemKHz ? s->pmuMemKHz / 1000 : NA);
    u32(b[0], 24, gpc); u32(b[1], 24, mem); u32(b[2], 24, s->instGpu); u32(b[3], 24, s->avgGpu);
    u32(b[4], 24, s->limReq); u32(b[5], 24, s->limEnf); u32(b[6], 24, s->tgp);
    if (s->tempGpuRaw != NAS) snprintf(b[7], 24, "%.1f", temp_c(s->tempGpuRaw));
    else if (s->ctrlTempC != NAS) snprintf(b[7], 24, "%d", s->ctrlTempC);
    else snprintf(b[7], 24, "n/a");
    if (s->thrMask != NA) snprintf(b[8], 24, "0x%x", s->thrMask); else snprintf(b[8], 24, "n/a");
    if (pn >= 0) snprintf(b[9], 24, "P%d", pn); else snprintf(b[9], 24, "n/a");
    printf("%.3f,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n", s->t - t0, b[9], b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], thr);
    fflush(stdout);
}

static void on_sig(int sig) { (void)sig; gStop = 1; }

// --ctrl: send one raw control (params given as 32-bit little-endian words) and dump the reply. With --binapi the
// control goes through an NV2081_BINAPI object (NV2082 with --priv), which the kernel forwards to GSP unchecked; that
// is how a client reaches GSP controls the open kernel does not export. Otherwise it goes to the subdevice.
static int raw_ctrl(Rm *rm, NvU32 cmd, const NvU32 *words, int nwords, NvU32 size, int via)
{
    NvU8 buf[4096] = { 0 };
    if (size > sizeof buf) size = sizeof buf;
    if (size < (NvU32)nwords * 4) size = (NvU32)nwords * 4;
    memcpy(buf, words, (size_t)nwords * 4);
    NvHandle h = rm->hSubdev;
    if (via) {
        h = 0xcafe0081;
        NvU32 cls = via == 2 ? 0x2082 : 0x2081, ap = 0;
        NvU32 st = rm_alloc(rm, rm->hSubdev, &h, cls, &ap, sizeof ap);
        printf("alloc %s 0x%08x %s\n", via == 2 ? "NV2082_BINAPI_PRIVILEGED" : "NV2081_BINAPI", st, rm_status_name(st));
        if (st) return 1;
    }
    NvU32 st = rm_ctrl(rm, h, cmd, buf, size);
    printf("control 0x%08x size %u -> 0x%08x %s\n", cmd, size, st, rm_status_name(st));
    for (NvU32 i = 0; i < size; i += 4) {
        NvU32 w; memcpy(&w, buf + i, 4);
        printf("%s%08x", (i % 32) ? " " : (i ? "\n  " : "  "), w);
    }
    printf("\n");
    if (via) rm_free(rm, h);
    return st != 0;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: nvgfx-smi [--once] [--json] [--watch [ms]] [--verbose]\n"
        "  --once        one human-readable report (default)\n"
        "  --json        one report as JSON\n"
        "  --watch [ms]  CSV, one line per interval (default 1000 ms) until Ctrl-C\n"
        "  --verbose     log transport details to stderr\n"
        "  --ctrl CMD [WORD...] [--size N] [--binapi|--priv]\n"
        "                send one raw RM control (hex cmd, hex 32-bit param words) and dump the reply\n");
}

int main(int argc, char **argv)
{
    int mode = 0;   // 0 once, 1 json, 2 watch, 3 ctrl
    unsigned intervalMs = 1000;
    NvU32 ctrlCmd = 0, ctrlWords[256], ctrlSize = 0; int ctrlN = 0, ctrlVia = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ctrl") && i + 1 < argc) {
            mode = 3; ctrlCmd = (NvU32)strtoul(argv[++i], NULL, 16);
            while (i + 1 < argc && argv[i + 1][0] != '-' && ctrlN < 256) ctrlWords[ctrlN++] = (NvU32)strtoul(argv[++i], NULL, 16);
        }
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) ctrlSize = (NvU32)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--binapi")) ctrlVia = 1;
        else if (!strcmp(argv[i], "--priv")) ctrlVia = 2;
        else if (!strcmp(argv[i], "--once")) mode = 0;
        else if (!strcmp(argv[i], "--json")) mode = 1;
        else if (!strcmp(argv[i], "--watch")) {
            mode = 2;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') intervalMs = (unsigned)atoi(argv[++i]);
            if (intervalMs < 20) intervalMs = 20;
        } else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) gVerbose = 1;
        else { usage(); return argv[i][1] == 'h' || !strcmp(argv[i], "--help") ? 0 : 2; }
    }

    Rm rm;
    if (rm_open(&rm)) { print_status_log(stderr); rm_close(&rm); return 1; }
    if (mode == 3) { int r = raw_ctrl(&rm, ctrlCmd, ctrlWords, ctrlN, ctrlSize, ctrlVia); rm_close(&rm); return r; }
    rusd_open(&rm);
    thermal_find_sensor(&rm);

    if (mode == 2) {
        signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
        if (!rm.rusd) fprintf(stderr, "nvgfx-smi: RUSD not mapped (alloc 0x%08x, map 0x%08x); power/limit/throttle columns will be n/a\n",
                              rm.rusdAllocStatus, rm.rusdMapStatus);
        Sample s;
        take_sample(&rm, &s);     // primes PMUMON
        double t0 = now_s();
        csv_header();
        while (!gStop) {
            take_sample(&rm, &s);
            csv_line(&s, t0);
            struct timespec ts = { intervalMs / 1000, (long)(intervalMs % 1000) * 1000000L };
            nanosleep(&ts, NULL);
        }
        fprintf(stderr, "\n");
        print_status_log(stderr);
    } else {
        Once o;
        take_once(&rm, &o);
        Sample s;
        take_sample(&rm, &s);
        // RUSD polling starts when the object is allocated; give it one interval to fill.
        if (rm.rusd && !s.tsLimit && !s.tsClk) { usleep(300 * 1000); take_sample(&rm, &s); }
        else { usleep(150 * 1000); take_sample(&rm, &s); }
        if (mode == 1) print_json(&rm, &o, &s); else print_human(&rm, &o, &s);
    }
    rm_close(&rm);
    return 0;
}
