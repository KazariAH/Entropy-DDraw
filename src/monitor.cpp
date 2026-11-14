// Monitors and DPI. Every placement decision of the window code goes through this list: the monitors
// EnumDisplayMonitors reports, plus - with the debug key `monitortest=WxH` - a simulated one right of
// the desktop (a code path test on a single-monitor machine; Windows itself knows nothing about it).
// All APIs newer than Windows 7 are resolved at run time (no import of shcore.dll).
#include "qdd.h"
#include <string.h>
#include <stdio.h>

typedef UINT(WINAPI *PFN_GetDpiForWindow)(HWND);
typedef UINT(WINAPI *PFN_GetDpiForSystem)(void);
typedef HRESULT(WINAPI *PFN_GetDpiForMonitor)(HMONITOR, int, UINT *, UINT *);
typedef BOOL(WINAPI *PFN_AdjustForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);
typedef HANDLE(WINAPI *PFN_GetThreadCtx)(void);
typedef int(WINAPI *PFN_AwarenessFromCtx)(HANDLE);

static PFN_GetDpiForWindow p_GetDpiForWindow;
static PFN_GetDpiForSystem p_GetDpiForSystem;
static PFN_GetDpiForMonitor p_GetDpiForMonitor;
static PFN_AdjustForDpi p_AdjustForDpi;
static bool s_resolved;

static void resolve()
{
    if (s_resolved)
        return;
    s_resolved = true;
    HMODULE u32 = GetModuleHandleA("user32.dll");
    p_GetDpiForWindow = (PFN_GetDpiForWindow)GetProcAddress(u32, "GetDpiForWindow");
    p_GetDpiForSystem = (PFN_GetDpiForSystem)GetProcAddress(u32, "GetDpiForSystem");
    p_AdjustForDpi = (PFN_AdjustForDpi)GetProcAddress(u32, "AdjustWindowRectExForDpi");
}

static UINT system_dpi()
{
    resolve();
    if (p_GetDpiForSystem)
        return p_GetDpiForSystem();
    HDC dc = GetDC(NULL);
    UINT d = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(NULL, dc);
    return d;
}

UINT dpi_of_window(HWND h)
{
    resolve();
    UINT d = p_GetDpiForWindow && h ? p_GetDpiForWindow(h) : 0;
    return d ? d : system_dpi();
}

BOOL adjust_window_rect(RECT *r, DWORD style, DWORD ex, UINT dpi)
{
    resolve();
    // a per-monitor aware process gets the frame of the system DPI from AdjustWindowRectEx, which is
    // wrong on a monitor with another scale (the client would come out a few pixels off)
    if (p_AdjustForDpi && dpi)
        return p_AdjustForDpi(r, style, FALSE, ex, dpi);
    return AdjustWindowRectEx(r, style, FALSE, ex);
}

// --- monitor list --------------------------------------------------------------------------

struct EnumCtx { MonInfo *out; int n, cap; };

static void fill(HMONITOR m, MonInfo *mi)
{
    MONITORINFOEXA i;
    memset(&i, 0, sizeof(i));
    i.cbSize = sizeof(i);
    memset(mi, 0, sizeof(*mi));
    mi->h = m;
    if (GetMonitorInfoA(m, &i)) {
        mi->rc = i.rcMonitor;
        mi->work = i.rcWork;
        mi->primary = (i.dwFlags & MONITORINFOF_PRIMARY) != 0;
        strcpy_s(mi->dev, i.szDevice);
    }
    static bool shcoreTried;   // never from DllMain (loader lock): the first monitor query comes later
    if (!shcoreTried) {
        shcoreTried = true;
        HMODULE shcore = LoadLibraryA("shcore.dll");   // Windows 8.1+
        if (shcore)
            p_GetDpiForMonitor = (PFN_GetDpiForMonitor)GetProcAddress(shcore, "GetDpiForMonitor");
    }
    UINT x = 0, y = 0;
    if (p_GetDpiForMonitor && SUCCEEDED(p_GetDpiForMonitor(m, 0 /*MDT_EFFECTIVE_DPI*/, &x, &y)))
        mi->dpi = x;
}

static BOOL CALLBACK enum_proc(HMONITOR m, HDC, LPRECT, LPARAM l)
{
    EnumCtx *c = (EnumCtx *)l;
    if (c->n < c->cap)
        fill(m, &c->out[c->n++]);
    return TRUE;
}

static void fake_monitor(MonInfo *list, int n, MonInfo *mi)
{
    // right of the rightmost real monitor, top aligned with the primary
    long right = 0;
    for (int i = 0; i < n; i++)
        if (!list[i].fake && list[i].rc.right > right) right = list[i].rc.right;
    memset(mi, 0, sizeof(*mi));
    mi->h = QDD_FAKE_MONITOR;
    SetRect(&mi->rc, right, 0, right + g_cfg.monitorTestW, g_cfg.monitorTestH);
    mi->work = mi->rc;
    mi->work.bottom -= 48;   // a taskbar, so work area and monitor differ like on a real one
    strcpy_s(mi->dev, "\\\\.\\QDDTEST1");
    mi->fake = true;
    mi->dpi = 96;
}

int monitors_list(MonInfo *out, int cap)
{
    EnumCtx c = {out, 0, cap - 1};
    EnumDisplayMonitors(NULL, NULL, enum_proc, (LPARAM)&c);
    if (g_cfg.monitorTestW && c.n < cap)
        fake_monitor(out, c.n, &out[c.n++]);
    return c.n;
}

static long area(const RECT &a, const RECT &b)
{
    RECT i;
    return IntersectRect(&i, &a, &b) ? (i.right - i.left) * (i.bottom - i.top) : 0;
}

static long long dist2(const RECT &a, const RECT &b)
{
    long dx = a.right < b.left ? b.left - a.right : b.right < a.left ? a.left - b.right : 0;
    long dy = a.bottom < b.top ? b.top - a.bottom : b.bottom < a.top ? a.top - b.bottom : 0;
    return (long long)dx * dx + (long long)dy * dy;
}

void monitor_of_rect(const RECT &r, MonInfo *out)
{
    MonInfo list[16];
    int n = monitors_list(list, 16);
    int best = -1;
    long bestArea = 0;
    for (int i = 0; i < n; i++) {
        long a = area(r, list[i].rc);
        if (a > bestArea) { bestArea = a; best = i; }
    }
    if (best < 0) {
        long long bestD = -1;
        for (int i = 0; i < n; i++) {
            long long d = dist2(r, list[i].rc);
            if (bestD < 0 || d < bestD) { bestD = d; best = i; }
        }
    }
    if (best >= 0)
        *out = list[best];
    else
        fill(MonitorFromRect(&r, MONITOR_DEFAULTTOPRIMARY), out);
}

void monitor_of_window(HWND h, MonInfo *out)
{
    RECT r;
    if (!h || !GetWindowRect(h, &r) || IsIconic(h)) {
        fill(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), out);
        return;
    }
    monitor_of_rect(r, out);
}

bool monitor_by_device(const char *dev, MonInfo *out)
{
    MonInfo list[16];
    int n = monitors_list(list, 16);
    for (int i = 0; i < n; i++)
        if (_stricmp(list[i].dev, dev) == 0) {
            *out = list[i];
            return true;
        }
    return false;
}

void monitor_primary(MonInfo *out)
{
    const POINT origin = {0, 0};
    fill(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), out);
}

void monitors_log(const char *why)
{
    MonInfo list[16];
    int n = monitors_list(list, 16);
    log_event("monitors (%s): %d", why, n);
    for (int i = 0; i < n; i++) {
        const MonInfo &m = list[i];
        log_event("  monitor %d: handle %p %s (%ld,%ld)-(%ld,%ld) %ldx%ld, work area (%ld,%ld)-(%ld,%ld), DPI %u (%u%%)%s%s",
                  i + 1, m.h, m.dev, m.rc.left, m.rc.top, m.rc.right, m.rc.bottom, m.rc.right - m.rc.left,
                  m.rc.bottom - m.rc.top, m.work.left, m.work.top, m.work.right, m.work.bottom, m.dpi, m.dpi * 100 / 96,
                  m.primary ? " primary" : "", m.fake ? " SIMULATED (monitortest)" : "");
    }
}

// --- DPI awareness ---------------------------------------------------------------------------

// The game window is created before DirectDrawCreate, so the awareness is declared from DllMain.
// permonitor: per-monitor v2 (the window gets WM_DPICHANGED and real pixels on every monitor), then
// per-monitor v1, then the older APIs; system: one DPI for all monitors; off: nothing is declared
// (Windows scales the window as a bitmap - blurry above 100%).
void dpi_init()
{
    typedef BOOL(WINAPI * PFN_SetCtx)(HANDLE);
    typedef HRESULT(WINAPI * PFN_SetAw)(int);
    typedef BOOL(WINAPI * PFN_SetAware)(void);
    static const char *const names[] = {"off", "system", "permonitor"};
    HMODULE u32 = GetModuleHandleA("user32.dll");
    const char *how = "nothing declared";
    if (g_cfg.dpi != DPI_OFF) {
        PFN_SetCtx setCtx = (PFN_SetCtx)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        bool done = false;
        if (setCtx) {   // Windows 10 1703+
            if (g_cfg.dpi == DPI_PERMONITOR && setCtx((HANDLE)(LONG_PTR)-4)) { how = "SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)"; done = true; }
            else if (g_cfg.dpi == DPI_PERMONITOR && setCtx((HANDLE)(LONG_PTR)-3)) { how = "SetProcessDpiAwarenessContext(PER_MONITOR_AWARE)"; done = true; }
            else if (g_cfg.dpi == DPI_SYSTEM && setCtx((HANDLE)(LONG_PTR)-2)) { how = "SetProcessDpiAwarenessContext(SYSTEM_AWARE)"; done = true; }
            else how = "SetProcessDpiAwarenessContext failed (already declared by a manifest?)";
        }
        HMODULE shcore = done ? NULL : LoadLibraryA("shcore.dll");   // Windows 8.1+
        PFN_SetAw setAw = shcore ? (PFN_SetAw)GetProcAddress(shcore, "SetProcessDpiAwareness") : NULL;
        if (!done && setAw && SUCCEEDED(setAw(g_cfg.dpi == DPI_PERMONITOR ? 2 : 1))) {
            how = g_cfg.dpi == DPI_PERMONITOR ? "SetProcessDpiAwareness(PER_MONITOR)" : "SetProcessDpiAwareness(SYSTEM)";
            done = true;
        }
        PFN_SetAware setAware = done ? NULL : (PFN_SetAware)GetProcAddress(u32, "SetProcessDPIAware");
        if (setAware && setAware()) {   // Vista / 7
            how = "SetProcessDPIAware (system)";
            done = true;
        }
    }
    char now[128];
    dpi_describe(now, sizeof(now));
    log_event("DPI: dpi=%s -> %s; in effect: %s", names[g_cfg.dpi], how, now);
}

void dpi_describe(char *buf, size_t cap)
{
    HMODULE u32 = GetModuleHandleA("user32.dll");
    PFN_GetThreadCtx getCtx = (PFN_GetThreadCtx)GetProcAddress(u32, "GetThreadDpiAwarenessContext");
    PFN_AwarenessFromCtx fromCtx = (PFN_AwarenessFromCtx)GetProcAddress(u32, "GetAwarenessFromDpiAwarenessContext");
    typedef BOOL(WINAPI * PFN_AreEqual)(HANDLE, HANDLE);
    PFN_AreEqual equal = (PFN_AreEqual)GetProcAddress(u32, "AreDpiAwarenessContextsEqual");
    const char *aw = "unknown (Windows before 10 1607)";
    if (getCtx && fromCtx) {
        HANDLE c = getCtx();
        int a = fromCtx(c);
        aw = a == 0 ? "unaware" : a == 1 ? "system aware" : a == 2 ? "per-monitor aware" : "?";
        if (a == 2 && equal && equal(c, (HANDLE)(LONG_PTR)-4))
            aw = "per-monitor aware v2";
    }
    _snprintf_s(buf, cap, _TRUNCATE, "%s, system DPI %u (%u%%)", aw, system_dpi(), system_dpi() * 100 / 96);
}
