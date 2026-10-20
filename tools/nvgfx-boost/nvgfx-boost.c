// <wtl> fork: nvgfx | author: watchthelight </wtl>
// nvgfx-boost: holds or drops one NVIDIA PERF_BOOST request for the whole system, so the GPU boost can be switched at
// runtime. NVK asks for "boost to max, infinite" in every Metal client when nvrm610.conf has BOOST=1 (the default), and
// WindowServer never lets go, so the GPU stays in P0. With BOOST=0 in nvrm610.conf no client boosts, and this daemon
// alone decides: it keeps an RM client open with BOOST_TO_MAX while the settings file says gpuboost=1 (or has no
// gpuboost line) and clears it for gpuboost=0. The RM transport is the one nvgfx-smi uses (NVAccel user client, NV_ESC).
//
//   nvgfx-boost [settings-file]    daemon, default /Library/Application Support/LegionFan/settings
//   nvgfx-boost --once on|off      set once and exit (off only clears this process's own request)
//
// It writes "boost 1 rc 0x0" style lines to /Library/Application Support/LegionFan/gpuboost.state for the menu app.
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#include "ctrl/ctrl00de.h"

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
            fprintf(stderr, "nvgfx-boost: IOServiceOpen(NVAccel, 0x%08x) -> 0x%08x, trying NVRM\n", open_type, kr);
    }
    if (kr != KERN_SUCCESS) {
        svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVRM"));
        if (!svc) { fprintf(stderr, "nvgfx-boost: no NVAccel and no NVRM service\n"); return -1; }
        kr = IOServiceOpen(svc, mach_task_self(), type, &c->conn);
        IOObjectRelease(svc);
    }
    if (kr != KERN_SUCCESS) { fprintf(stderr, "nvgfx-boost: IOServiceOpen(type %u) -> 0x%08x\n", type, kr); return -1; }
    // The kext names clients by a posix fd number (MAP_MEMORY passes it), so bind a real one.
    c->fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (c->fd < 0) { IOServiceClose(c->conn); c->conn = 0; return -1; }
    uint64_t in = (uint64_t)c->fd;
    kr = IOConnectCallScalarMethod(c->conn, SEL_BINDFD, &in, 1, NULL, NULL);
    if (kr != KERN_SUCCESS) fprintf(stderr, "nvgfx-boost: bind fd %d -> 0x%08x\n", c->fd, kr);
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
    if (kr != KERN_SUCCESS && gVerbose) fprintf(stderr, "nvgfx-boost: esc 0x%02x (%u bytes) -> kr 0x%08x\n", cmd, size, kr);
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
        fprintf(stderr, "nvgfx-boost: NV_ESC_CARD_INFO -> kr 0x%08x valid %d (driver not up?)\n", kr, rm->card.valid);
        return -1;
    }

    NvU32 st = rm_alloc(rm, 0, &rm->hClient, NV01_ROOT_CLIENT, NULL, 0);
    log_status("alloc NV01_ROOT_CLIENT", NV01_ROOT_CLIENT, st);
    if (st != NV_OK) { fprintf(stderr, "nvgfx-boost: alloc root client -> RM 0x%08x\n", st); return -1; }

    NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS id;
    memset(&id, 0, sizeof id);
    id.gpuId = rm->card.gpu_id;
    st = rm_ctrl(rm, rm->hClient, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &id, sizeof id);
    log_status("NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2", NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, st);
    if (st != NV_OK) { fprintf(stderr, "nvgfx-boost: GPU_GET_ID_INFO_V2 -> RM 0x%08x\n", st); return -1; }

    NV0080_ALLOC_PARAMETERS a80;
    memset(&a80, 0, sizeof a80);
    a80.deviceId = id.deviceInstance;
    a80.hClientShare = rm->hClient;
    rm->hDevice = 0xcaf00001;
    st = rm_alloc(rm, rm->hClient, &rm->hDevice, NV01_DEVICE_0, &a80, sizeof a80);
    log_status("alloc NV01_DEVICE_0", NV01_DEVICE_0, st);
    if (st != NV_OK) { fprintf(stderr, "nvgfx-boost: alloc device -> RM 0x%08x\n", st); return -1; }

    NV2080_ALLOC_PARAMETERS a20;
    memset(&a20, 0, sizeof a20);
    a20.subDeviceId = id.subDeviceInstance;
    rm->hSubdev = 0xcaf00002;
    st = rm_alloc(rm, rm->hDevice, &rm->hSubdev, NV20_SUBDEVICE_0, &a20, sizeof a20);
    log_status("alloc NV20_SUBDEVICE_0", NV20_SUBDEVICE_0, st);
    if (st != NV_OK) { fprintf(stderr, "nvgfx-boost: alloc subdevice -> RM 0x%08x\n", st); return -1; }
    return 0;
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


static int want_boost(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 1;
    char line[256];
    int want = 1;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "gpuboost=", 9)) want = line[9] != '0';
    fclose(f);
    return want;
}

static void write_state(int held, NvU32 rc)
{
    const char *p = "/Library/Application Support/LegionFan/gpuboost.state";
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "boost %d rc 0x%x\n", held, (unsigned)rc);
    fclose(f);
    rename(tmp, p);
}

static NvU32 boost(Rm *rm, int on)
{
    NV2080_CTRL_PERF_BOOST_PARAMS bp;
    memset(&bp, 0, sizeof bp);
    if (on) {
        bp.flags = NV2080_CTRL_PERF_BOOST_FLAGS_CMD_BOOST_TO_MAX | (NV2080_CTRL_PERF_BOOST_FLAGS_CUDA_YES << 4);
        bp.duration = NV2080_CTRL_PERF_BOOST_DURATION_INFINITE;
    } else {
        bp.flags = NV2080_CTRL_PERF_BOOST_FLAGS_CMD_CLEAR;
    }
    return rm_ctrl(rm, rm->hSubdev, NV2080_CTRL_CMD_PERF_BOOST, &bp, sizeof bp);
}

static void on_signal(int s) { (void)s; gStop = 1; }

int main(int argc, char **argv)
{
    Rm rm;
    if (argc == 3 && !strcmp(argv[1], "--once")) {
        if (rm_open(&rm)) return 1;
        NvU32 st = boost(&rm, !strcmp(argv[2], "on"));
        printf("boost %s rc 0x%x\n", argv[2], (unsigned)st);
        if (!strcmp(argv[2], "on")) { signal(SIGINT, on_signal); signal(SIGTERM, on_signal); while (!gStop) pause(); }
        rm_close(&rm);
        return st == NV_OK ? 0 : 2;
    }
    const char *path = argc > 1 ? argv[1] : "/Library/Application Support/LegionFan/settings";
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    int open_ = 0, held = 0;
    NvU32 last = NV_OK;
    while (!gStop) {
        int want = want_boost(path);
        if (want && !held) {
            if (!open_) open_ = rm_open(&rm) == 0;
            if (open_) {
                last = boost(&rm, 1);
                held = last == NV_OK;
                if (!held) { rm_close(&rm); open_ = 0; }
                fprintf(stderr, "nvgfx-boost: boost on rc 0x%x\n", (unsigned)last);
            }
            write_state(held, last);
        } else if (!want && held) {
            last = boost(&rm, 0);
            rm_close(&rm);
            open_ = held = 0;
            fprintf(stderr, "nvgfx-boost: boost off rc 0x%x\n", (unsigned)last);
            write_state(0, last);
        }
        sleep(2);
    }
    if (held) boost(&rm, 0);
    if (open_) rm_close(&rm);
    write_state(0, NV_OK);
    return 0;
}
