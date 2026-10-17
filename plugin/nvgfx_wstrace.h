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
static int nvgfx_wstrace_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_NVGFX_WSTRACE"); on = e && e[0] == '1' && getprogname() && !strcmp(getprogname(), "WindowServer"); }
    return on;
}
__attribute__((format(printf, 1, 2))) static void nvgfx_wstrace(const char *fmt, ...)
{
    static _Atomic unsigned n;
    if (!nvgfx_wstrace_on() || atomic_fetch_add(&n, 1) >= 12000) return;
    char b[700]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    syslog(LOG_ERR, "NvGfx wstrace %s", b);
}
// N-94: the same line with its own counter and cap, so one chatty topic (samplers) cannot use up the shared budget.
__attribute__((format(printf, 3, 4))) static void nvgfx_wstrace_n(_Atomic unsigned *ctr, unsigned cap, const char *fmt, ...)
{
    if (!nvgfx_wstrace_on() || atomic_fetch_add(ctr, 1) >= cap) return;
    char b[700]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    syslog(LOG_ERR, "NvGfx wstrace %s", b);
}
#endif
