// IGPUConsoleRange: lets Apple's IONDRVFramebuffer drive a panel the firmware lit on an Intel iGPU whose boot
// framebuffer lives in stolen memory (Meteor/Arrow Lake: GOP framebuffer = DSM base, outside every PCI BAR).
// IOBootNDRV::fromRegistryEntry only accepts a console that lies inside one of the device's memory ranges, so on these
// parts it never builds its VRAM descriptor and WindowServer's open fails (kIOReturnUnsupported). This driver probes
// first in the IOFramebuffer category, appends the console range to the iGPU's device memory, and declines, so
// IONDRVFramebuffer probes next and finds it. It acts only when the console is the iGPU's own stolen memory.
// <wtl> fork: nvgfx | author: watchthelight </wtl>

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSArray.h>
#include <mach/kmod.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <string.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <sys/sysctl.h>
#include <stdarg.h>


static void wtlSysctlRegister(); static void wtlSysctlUnregister();
extern "C" kern_return_t _start(kmod_info_t *ki, void *d); extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
extern "C" { KMOD_EXPLICIT_DECL(org.watchthelight.IGPUConsoleRange, "0.5", _start, _stop) }
extern "C" kern_return_t _start(kmod_info_t *, void *) { wtlSysctlRegister(); return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *, void *) { wtlSysctlUnregister(); return KERN_SUCCESS; }

// IOLog lines from this kext never reach the unified log, so every line also goes to a ring that sysctl
// debug.wtlacpi_log and the iGPU's "watchthelight-trace" property show, and the last one to NVRAM (survives a power cut)
static char gTrace[4096]; static size_t gTraceLen; static IOLock *gTraceLock; static IOService *gTraceNub;
static void wtlTrace(const char *fmt, ...) __printflike(1, 2);
static void wtlTrace(const char *fmt, ...)
{
    char line[256]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    IOLog("IGPUConsoleRange: %s\n", line); kprintf("IGPUConsoleRange: %s\n", line);
    if (!gTraceLock) gTraceLock = IOLockAlloc();
    if (gTraceLock) IOLockLock(gTraceLock);
    uint64_t ms; absolutetime_to_nanoseconds(mach_absolute_time(), &ms); ms /= 1000000;
    char e[300]; int n = snprintf(e, sizeof e, "%llu.%03llu %s\n", ms / 1000, ms % 1000, line);
    if (n > 0) {
        size_t k = (size_t)n < sizeof e ? (size_t)n : sizeof e - 1;
        if (gTraceLen + k >= sizeof gTrace) { size_t drop = gTraceLen + k - sizeof gTrace + 1024; if (drop > gTraceLen) drop = gTraceLen; memmove(gTrace, gTrace + drop, gTraceLen - drop); gTraceLen -= drop; }
        memcpy(gTrace + gTraceLen, e, k); gTraceLen += k; gTrace[gTraceLen] = 0;
    }
    if (gTraceNub) gTraceNub->setProperty("watchthelight-trace", gTrace);
    if (gTraceLock) IOLockUnlock(gTraceLock);
    if (IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane)) {
        if (OSData *d = OSData::withBytes(line, (unsigned)strnlen(line, sizeof line))) { o->setProperty("wtl-igpurange", d); d->release(); }
        o->release();
    }
}
#define RLOG(fmt, ...) wtlTrace(fmt, ##__VA_ARGS__)

class IGPUConsoleRange : public IOService {
    OSDeclareDefaultStructors(IGPUConsoleRange)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
};
OSDefineMetaClassAndStructors(IGPUConsoleRange, IOService)

static bool nvidiaPresent()
{
    bool nv = false;
    if (OSDictionary *m = IOService::serviceMatching("IOPCIDevice")) {
        if (OSIterator *it = IOService::getMatchingServices(m)) {
            while (OSObject *o = it->getNextObject())
                if (IOPCIDevice *d = OSDynamicCast(IOPCIDevice, o))
                    if (d->configRead16(kIOPCIConfigVendorID) == 0x10de && d->configRead8(kIOPCIConfigClassCode + 2) == 0x03) { nv = true; break; }
            it->release();
        }
        m->release();
    }
    return nv;
}

static IOReturn rescanRp01()
{
    IOReturn pr = kIOReturnNotFound;
    if (OSDictionary *m = IOService::serviceMatching("IOPCIDevice")) {
        if (OSIterator *it = IOService::getMatchingServices(m)) {
            while (OSObject *o = it->getNextObject()) {
                IOPCIDevice *d = OSDynamicCast(IOPCIDevice, o);
                OSString *ap = d ? OSDynamicCast(OSString, d->getProperty("acpi-path")) : nullptr;
                if (!ap || !strnstr(ap->getCStringNoCopy(), "/PC00@0/RP01@60000", ap->getLength() + 1)) continue;
                pr = d->requestProbe(kIOPCIProbeOptionNeedsScan | kIOPCIProbeOptionDone);
                RLOG("rescan of %s (link status 0x%x) -> 0x%x", d->getName(), (unsigned)d->configRead16(0x52), (unsigned)pr);
                break;
            }
            it->release();
        }
        m->release();
    }
    return pr;
}

// "<IOACPIPlane path> <method or name> [integer args]": evaluates it and logs the result
static void acpiEval(char *cmd)
{
    char *tok[10]; int nt = 0;
    for (char *p = cmd; *p && nt < 10; ) {
        while (*p == ' ' || *p == '\n') *p++ = 0;
        if (!*p) break;
        tok[nt++] = p;
        while (*p && *p != ' ' && *p != '\n') p++;
    }
    if (nt == 1 && !strncmp(tok[0], "rescan", 7)) { rescanRp01(); return; }
    if (nt == 1 && !strncmp(tok[0], "nv", 3)) { RLOG("NVIDIA GPU on PCI: %s", nvidiaPresent() ? "yes" : "no"); return; }
    if (nt < 2) { RLOG("usage: <IOACPIPlane path> <name> [ints] | rescan | nv"); return; }
    char path[200]; snprintf(path, sizeof path, "IOACPIPlane:%s", tok[0]);
    IOACPIPlatformDevice *dev = OSDynamicCast(IOACPIPlatformDevice, IORegistryEntry::fromPath(path));
    if (!dev) { RLOG("%s: no such ACPI device", tok[0]); return; }
    OSObject *args[8] = {}; IOItemCount na = 0;
    for (int i = 2; i < nt && na < 8; i++) args[na++] = OSNumber::withNumber(strtoq(tok[i], nullptr, 0), 64);
    OSObject *res = nullptr;
    IOReturn r = dev->evaluateObject(tok[1], &res, na ? args : nullptr, na);
    char rv[96] = "none";
    if (OSNumber *n = OSDynamicCast(OSNumber, res)) snprintf(rv, sizeof rv, "0x%llx", n->unsigned64BitValue());
    else if (OSString *s = OSDynamicCast(OSString, res)) snprintf(rv, sizeof rv, "\"%s\"", s->getCStringNoCopy());
    else if (OSData *d = OSDynamicCast(OSData, res)) {
        int o = snprintf(rv, sizeof rv, "buf[%u]", d->getLength());
        const uint8_t *b = (const uint8_t *)d->getBytesNoCopy();
        for (unsigned i = 0; b && i < d->getLength() && i < 24 && o < (int)sizeof rv - 4; i++) o += snprintf(rv + o, sizeof rv - o, " %02x", b[i]);
    } else if (res) snprintf(rv, sizeof rv, "%s", res->getMetaClass()->getClassName());
    RLOG("%s %s(%u args) -> 0x%x result %s", tok[0], tok[1], (unsigned)na, (unsigned)r, rv);
    OSSafeReleaseNULL(res);
    for (IOItemCount i = 0; i < na; i++) OSSafeReleaseNULL(args[i]);
    dev->release();
}

static int wtlacpi_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
    char cmd[256] = {};
    int err = sysctl_handle_string(nullptr, cmd, sizeof cmd, req);
    if (err || !req->newptr) return err;
    acpiEval(cmd);
    return 0;
}
static int wtlacpi_log_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
    return SYSCTL_OUT(req, gTrace, gTraceLen + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, wtlacpi, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, wtlacpi_sysctl, "A",
            "evaluate an ACPI object: <IOACPIPlane path> <name> [ints] | rescan | nv");
SYSCTL_PROC(_debug, OID_AUTO, wtlacpi_log, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, wtlacpi_log_sysctl, "A",
            "IGPUConsoleRange trace");
static void wtlSysctlRegister() { sysctl_register_oid(&sysctl__debug_wtlacpi); sysctl_register_oid(&sysctl__debug_wtlacpi_log); }
static void wtlSysctlUnregister() { sysctl_unregister_oid(&sysctl__debug_wtlacpi); sysctl_unregister_oid(&sysctl__debug_wtlacpi_log); }

static IOPCIDevice *rp01()
{
    IOPCIDevice *found = nullptr;
    if (OSDictionary *m = IOService::serviceMatching("IOPCIDevice")) {
        if (OSIterator *it = IOService::getMatchingServices(m)) {
            while (OSObject *o = it->getNextObject()) {
                IOPCIDevice *d = OSDynamicCast(IOPCIDevice, o);
                OSString *ap = d ? OSDynamicCast(OSString, d->getProperty("acpi-path")) : nullptr;
                if (ap && strnstr(ap->getCStringNoCopy(), "/PC00@0/RP01@60000", ap->getLength() + 1)) { found = d; d->retain(); break; }
            }
            it->release();
        }
        m->release();
    }
    return found;
}

// nvfbigpu=1: power the dGPU up from probe, before anything has used the iGPU slot. A driver that starts on the iGPU
// and is terminated later marks it for a function reset on the next match, and that reset of the scanning-out iGPU cuts
// the laptop's power (I-093), so the slot is decided once here and never handed back.
// The sequence is the firmware's own (research/hybrid-graphics.md): the EC re-attach request, then RP01 Link Disable
// cleared *before* PXP._ON (whose GC6O/L23D retrain cannot work with it set, which is what N-203 and N-207 did), then
// a link check; without a link the GPU goes back off through PXP._OFF, never left powered without one.
static bool attachFromProbe()
{
    if (nvidiaPresent()) { RLOG("NVIDIA GPU already on PCI: no attach needed"); return true; }
    IOService *ec = IOService::waitForMatchingService(IOService::nameMatching("EC0"), 20ull * kSecondScale);
    RLOG("EC0 %s", ec ? "matched" : "not matched after 20 s");
    OSSafeReleaseNULL(ec);
    IOACPIPlatformDevice *ecd = OSDynamicCast(IOACPIPlatformDevice, IORegistryEntry::fromPath("IOACPIPlane:/_SB/PC00/LPCB@1f0000/EC0"));
    IOACPIPlatformDevice *gpu = OSDynamicCast(IOACPIPlatformDevice, IORegistryEntry::fromPath("IOACPIPlane:/_SB/PC00/RP01@60000/PXSX@0"));
    IOPCIDevice *rp = rp01();
    bool nv = false;
    if (!ecd || !gpu || !rp) { RLOG("attach: missing EC0 %p, PXSX %p or RP01 %p", ecd, gpu, rp); goto out; }
    {
        OSObject *a[2] = { OSNumber::withNumber(4ull, 32), OSNumber::withNumber(0ull, 32) };
        UInt32 hpff = 0xff;
        ecd->evaluateObject("WEJF", nullptr, a, 2);
        OSSafeReleaseNULL(a[0]); OSSafeReleaseNULL(a[1]);
        OSObject *b[1] = { OSNumber::withNumber(2ull, 32) };
        OSObject *res = nullptr;
        if (ecd->evaluateObject("REJF", &res, b, 1) == kIOReturnSuccess)
            if (OSNumber *n = OSDynamicCast(OSNumber, res)) hpff = n->unsigned32BitValue();
        OSSafeReleaseNULL(res); OSSafeReleaseNULL(b[0]);
        RLOG("attach: WEJF(4,0) done, HPFF %u", (unsigned)hpff);
        if (hpff != 0) { RLOG("attach: HPFF still set, no power change"); goto out; }
        uint16_t lc = rp->configRead16(0x50);
        rp->configWrite16(0x50, (uint16_t)(lc & ~0x10));
        RLOG("attach: RP01 link disable cleared (linkctl 0x%04x -> 0x%04x)", lc, rp->configRead16(0x50));
        IOReturn r = gpu->evaluateObject("\\_SB.PC00.RP01.PXP._ON");
        bool link = false;
        for (int i = 0; i < 20 && !(link = (rp->configRead16(0x52) & 0x2000) != 0); i++) IOSleep(100);
        RLOG("attach: PXP._ON -> 0x%x, data link %s (linkstat 0x%04x)", (unsigned)r, link ? "up" : "DOWN", rp->configRead16(0x52));
        if (!link) {
            r = gpu->evaluateObject("\\_SB.PC00.RP01.PXP._OFF");
            RLOG("attach: no link, PXP._OFF -> 0x%x", (unsigned)r);
            goto out;
        }
    }
    rescanRp01();
    for (int i = 0; i < 50 && !nvidiaPresent(); i++) IOSleep(200);
    nv = nvidiaPresent();
    RLOG("NVIDIA GPU %s after the attach", nv ? "present" : "absent");
out:
    OSSafeReleaseNULL(ecd); OSSafeReleaseNULL(gpu); OSSafeReleaseNULL(rp);
    return nv;
}

IOService *IGPUConsoleRange::probe(IOService *provider, SInt32 *score)
{
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci || pci->getFunctionNumber() != 0) return nullptr;
    gTraceNub = pci;
    char b[4];
    if (PE_parse_boot_argn("-igpurangeoff", b, sizeof b)) { RLOG("off by boot-arg"); return nullptr; }

    PE_Video v; bzero(&v, sizeof v);
    if (!getPlatform() || getPlatform()->getConsoleInfo(&v) != kIOReturnSuccess || !v.v_baseAddr || v.v_depth != 32) return nullptr;
    const uint64_t base = (uint64_t)v.v_baseAddr & ~3ull, bytes = (uint64_t)v.v_rowBytes * v.v_height;
    if (!bytes) return nullptr;

    for (IOItemCount i = 0; i < pci->getDeviceMemoryCount(); i++)
        if (IODeviceMemory *m = pci->getDeviceMemoryWithIndex(i)) {
            const uint64_t pa = m->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
            if (base >= pa && base < pa + m->getLength()) { RLOG("console 0x%llx already in range %u", base, (unsigned)i); return nullptr; }
        }

    // DSM base from MMIO (config 0xC0 reads 0 on display 14); BAR0 is mapped only for this one read
    if (!(pci->configRead16(kIOPCIConfigCommand) & kIOPCICommandMemorySpace)) { RLOG("memory decode off, not touching it"); return nullptr; }
    IOMemoryMap *mm = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!mm || mm->getLength() < 0x108108) { OSSafeReleaseNULL(mm); return nullptr; }
    volatile uint32_t *r = (volatile uint32_t *)mm->getVirtualAddress();
    const uint64_t dsm = (((uint64_t)r[0x1080C4 / 4] << 32) | r[0x1080C0 / 4]) & ~0xFFFFFull;
    mm->release();
    if (!dsm || base < dsm || base - dsm >= 0x10000000ull) {
        RLOG("console 0x%llx is not this GPU's stolen memory (DSM 0x%llx), leaving the device alone", base, dsm);
        return nullptr;
    }

    OSArray *old = pci->getDeviceMemory();
    OSArray *arr = old ? OSArray::withArray(old, old->getCount() + 1) : OSArray::withCapacity(1);
    IODeviceMemory *dm = IODeviceMemory::withRange((IOPhysicalAddress)base, (IOPhysicalLength)round_page_64(bytes));
    if (!arr || !dm) { OSSafeReleaseNULL(arr); OSSafeReleaseNULL(dm); return nullptr; }
    arr->setObject(dm); dm->release();
    pci->setDeviceMemory(arr); arr->release();
    pci->setProperty("watchthelight-console-range", (unsigned long long)base, 64);
    RLOG("console 0x%llx (%lux%lu rowBytes %lu) is DSM+0x%llx: added as device memory %u for IONDRVFramebuffer",
         base, v.v_width, v.v_height, v.v_rowBytes, base - dsm, (unsigned)pci->getDeviceMemoryCount() - 1);
    // nvfbigpu=1: NVRMFB's proxy head drives this panel, so IONDRV must not start a second framebuffer on the same
    // memory; holding the category blocks it. Without an NVIDIA GPU, decline and let IONDRVFramebuffer probe next.
    int proxy = 0;
    if (PE_parse_boot_argn("nvfbigpu", &proxy, sizeof proxy) && proxy && attachFromProbe()) {
        RLOG("holding the iGPU's framebuffer slot for the NVIDIA proxy head");
        return this;
    }
    return nullptr;
}
