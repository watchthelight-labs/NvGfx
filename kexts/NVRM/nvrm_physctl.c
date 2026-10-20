/*
 * NvGfx: issue one RM control with kernel privilege, for experiments with the internal PRH/PMGR controls that
 * userland cannot reach (notes/power.md). Called only by NVRM.cpp's NVRM_ESC_PHYS_CTRL, which checks root and the
 * nvrmphysctl boot-arg first. Compiled with RM's own osapi.c command line (rmcc.py) because it needs OBJGPU.
 */
// <wtl> fork: nvgfx | author: watchthelight </wtl>

#include <nv.h>
#include <nv-priv.h>
#include <os/os.h>
#include <osapi.h>
#include "gpu/gpu.h"
#include <core/thread_state.h>
#include <core/locks.h>
#include "rmapi/rmapi.h"
#include "nvrm_physctl_abi.h"

NV_STATUS rm_nvgfx_phys_control(nv_state_t *nv, NvU32 mode, NvU32 hClient, NvU32 hObject, NvU32 cmd, void *pParams,
                                NvU32 paramsSize);

NV_STATUS rm_nvgfx_phys_control(nv_state_t *nv, NvU32 mode, NvU32 hClient, NvU32 hObject, NvU32 cmd, void *pParams,
                                NvU32 paramsSize)
{
    THREAD_STATE_NODE threadState;
    NV_STATUS st;
    void *fp;
    nvidia_stack_t *sp = NULL;
    OBJGPU *pGpu = nv ? NV_GET_NV_PRIV_PGPU(nv) : NULL;

    if (pGpu == NULL || paramsSize > NVRM_PHYSCTL_MAX_PARAMS || (paramsSize && pParams == NULL) || mode > NVRM_PHYSCTL_KERNEL)
        return NV_ERR_INVALID_ARGUMENT;
    if (paramsSize == 0) pParams = NULL;

    NV_ENTER_RM_RUNTIME(sp, fp);
    threadStateInit(&threadState, THREAD_STATE_FLAGS_NONE);
    // Same order as every RM entry: API lock, then the GPU locks (RMAPI_GPU_LOCK_INTERNAL and the GSP RPC path both
    // expect both held).
    if ((st = rmapiLockAcquire(API_LOCK_FLAGS_NONE, RM_LOCK_MODULES_OSAPI)) == NV_OK) {
        if ((st = rmGpuLocksAcquire(GPUS_LOCK_FLAGS_NONE, RM_LOCK_MODULES_OSAPI)) == NV_OK) {
            if (!FULL_GPU_SANITY_CHECK(pGpu)) {
                st = NV_ERR_GPU_NOT_FULL_POWER;
            } else if (mode == NVRM_PHYSCTL_KERNEL) {
                RM_API *pRmApi = rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL);
                st = pRmApi->Control(pRmApi, hClient, hObject, cmd, pParams, paramsSize);
            } else {
                RM_API *pRmApi = GPU_GET_PHYSICAL_RMAPI(pGpu);
                NvHandle h = mode == NVRM_PHYSCTL_PHYS_DEVICE ? pGpu->hInternalDevice
                           : mode == NVRM_PHYSCTL_PHYS_CLIENT ? pGpu->hInternalClient : pGpu->hInternalSubdevice;
                st = pRmApi->Control(pRmApi, pGpu->hInternalClient, h, cmd, pParams, paramsSize);
            }
            rmGpuLocksRelease(GPUS_LOCK_FLAGS_NONE, NULL);
        }
        rmapiLockRelease();
    }
    threadStateFree(&threadState, THREAD_STATE_FLAGS_NONE);
    NV_EXIT_RM_RUNTIME(sp, fp);
    return st;
}
