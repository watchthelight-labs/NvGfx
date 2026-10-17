// NvGfx: NVMTL_NVGFX_WSTRACE=1 logs what WindowServer asks for (samplers, pipelines by function name, small render
// targets) to syslog as "NvGfx wstrace", capped, to find how CoreAnimation draws its backdrop blur (blocky Dock blur,
// N-75). Off by default; other processes never log.
#ifndef NVGFX_WSTRACE_H
#define NVGFX_WSTRACE_H
#include <syslog.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>
// N-97: NVMTL_NVGFX_WSTRACE=1 arms the trace; it logs only while /Library/GPUBundles/nvmtl/wstrace.on exists (checked once a
// second), so the budget is spent on the moment under study (a badge update) instead of the login storm. "2" logs from
// the start as before.
static _Atomic unsigned nvgfx_arm_gen;
static int nvgfx_wstrace_on(void)
{
    static int armed = -1, always = 0;
    static _Atomic int live; static _Atomic long checked;
    if (armed < 0) { const char *e = getenv("NVMTL_NVGFX_WSTRACE");
#if NVMTL_RELEASE
        const int here = getprogname() && !strcmp(getprogname(), "WindowServer");   // the release build traces WindowServer only
#else
        const int here = 1;   // a debug build traces whichever process loaded it (in-process CARenderer tests)
#endif
        armed = e && (e[0] == '1' || e[0] == '2') && here; always = e && e[0] == '2'; }
    if (!armed) return 0;
    if (always) return 1;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    if (ts.tv_sec != atomic_load(&checked)) { atomic_store(&checked, ts.tv_sec);
        const int now = access("/Library/GPUBundles/nvmtl/wstrace.on", F_OK) == 0;
        if (now && !atomic_load(&live)) atomic_fetch_add(&nvgfx_arm_gen, 1);   // N-116: a new arming starts every budget over
        atomic_store(&live, now); }
    return atomic_load(&live);
}
// N-116: every trace budget belongs to one arming of the flag. NVGFX_FRESH(counter) zeroes the counter when the flag has
// been raised again since the counter last counted, so a capture that was killed or ran early cannot starve the next one.
#define NVGFX_FRESH(ctr) do { static _Atomic unsigned gen_; unsigned g_ = atomic_load(&nvgfx_arm_gen); if (atomic_load(&gen_) != g_) { atomic_store(&gen_, g_); atomic_store(&(ctr), 0); } } while (0)
__attribute__((format(printf, 1, 2))) static void nvgfx_wstrace(const char *fmt, ...)
{
    static _Atomic unsigned n;
    if (!nvgfx_wstrace_on()) return;
    NVGFX_FRESH(n);
    if (atomic_fetch_add(&n, 1) >= 60000) return;
    char b[700]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    syslog(LOG_ERR, "NvGfx wstrace %s", b);
}
// N-94: the same line with its own counter and cap, so one chatty topic (samplers) cannot use up the shared budget.
__attribute__((format(printf, 3, 4))) static void nvgfx_wstrace_n(_Atomic unsigned *ctr, unsigned cap, const char *fmt, ...)
{
    if (!nvgfx_wstrace_on()) return;
    { static _Atomic unsigned gen_; unsigned g_ = atomic_load(&nvgfx_arm_gen); if (atomic_load(&gen_) != g_) { atomic_store(&gen_, g_); atomic_store(ctr, 0); } }
    if (atomic_fetch_add(ctr, 1) >= cap) return;
    char b[700]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    syslog(LOG_ERR, "NvGfx wstrace %s", b);
}
#endif
