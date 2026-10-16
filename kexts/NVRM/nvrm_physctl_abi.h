/*
 * NvGfx: root-only RM control escape (notes/power.md). Shared by NVRM.cpp, nvrm_physctl.c and tools/nvgfx-smi.
 */
#pragma once
#define NVRM_ESC_PHYS_CTRL       0xE4u
#define NVRM_PHYSCTL_VERSION     1u
#define NVRM_PHYSCTL_MAX_PARAMS  4096u

/* mode */
#define NVRM_PHYSCTL_PHYS_SUBDEV 0u   /* GPU_GET_PHYSICAL_RMAPI(pGpu)->Control(hInternalClient, hInternalSubdevice) */
#define NVRM_PHYSCTL_PHYS_DEVICE 1u   /* ... hInternalClient, hInternalDevice */
#define NVRM_PHYSCTL_PHYS_CLIENT 2u   /* ... hInternalClient, hInternalClient */
#define NVRM_PHYSCTL_KERNEL      3u   /* rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL)->Control(hClient, hObject): a CPU-RM
                                         object the caller owns, issued as an internal (kernel) call */

/* status values the kext sets itself (anything else is an NV_STATUS from RM) */
#define NVRM_PHYSCTL_ST_OFF         0x80000001u   /* boot-arg nvrmphysctl=1 not set */
#define NVRM_PHYSCTL_ST_BAD_VERSION 0x80000002u
#define NVRM_PHYSCTL_ST_BAD_SIZE    0x80000003u

struct NVRMPhysCtrlEsc {
    unsigned int version;      /* in: NVRM_PHYSCTL_VERSION */
    unsigned int cmd;          /* in: RM control command */
    unsigned int paramsSize;   /* in: bytes of params used, <= NVRM_PHYSCTL_MAX_PARAMS (0 allowed: NULL params) */
    unsigned int status;       /* out: NV_STATUS or NVRM_PHYSCTL_ST_* */
    unsigned int mode;         /* in: NVRM_PHYSCTL_* */
    unsigned int hClient;      /* in: NVRM_PHYSCTL_KERNEL only */
    unsigned int hObject;      /* in: NVRM_PHYSCTL_KERNEL only */
    unsigned int reserved;
    unsigned char params[NVRM_PHYSCTL_MAX_PARAMS];   /* in/out */
};
