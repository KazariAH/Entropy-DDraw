// DllMain, exported entry points and the IDirectDraw / 2 / 4 / 7 object.
#include "qdd.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// IIDs are defined here (instead of linking dxguid.lib).
#include <initguid.h>
DEFINE_GUID(QIID_IDirectDraw, 0x6C14DB80, 0xA733, 0x11CE, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60);
DEFINE_GUID(QIID_IDirectDraw2, 0xB3A6F3E0, 0x2B43, 0x11CF, 0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56);
DEFINE_GUID(QIID_IDirectDraw4, 0x9c59509a, 0x39bd, 0x11d1, 0x8c, 0x4a, 0x00, 0xc0, 0x4f, 0xd9, 0x30, 0xc5);
DEFINE_GUID(QIID_IDirectDraw7, 0x15e65ec0, 0x3b9c, 0x11d2, 0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b);
DEFINE_GUID(QIID_IUnknown, 0x00000000, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);

CRITICAL_SECTION g_lock;
CRITICAL_SECTION g_pixLock;
DDraw *g_dd;
Surface *volatile g_lastLocked;
static HMODULE s_self;

// ---------------------------------------------------------------------------
// interface pointer <-> object. vtbl1 at +0, vtbl2 at +4, vtbl4 at +8
// ---------------------------------------------------------------------------
template <int O> static DDraw *D(void *p) { return (DDraw *)((char *)p - O * sizeof(void *)); }

DDraw *dd_from_iface(void *iface)
{
    // Used by surfaces created through any interface version; all of them store
    // the object pointer directly, so this is only needed for GetDDInterface.
    return (DDraw *)iface;
}

static void mode_dims(DDraw *dd, int *w, int *h, int *bpp)
{
    if (dd->modeSet) {
        *w = dd->modeW; *h = dd->modeH; *bpp = dd->modeBpp;
    } else {
        *w = GetSystemMetrics(SM_CXSCREEN);
        *h = GetSystemMetrics(SM_CYSCREEN);
        *bpp = g_cfg.defaultBpp;
    }
}

static void describe_iid(REFIID r, char *buf, size_t n)
{
    _snprintf_s(buf, n, _TRUNCATE, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", r.Data1, r.Data2, r.Data3,
                r.Data4[0], r.Data4[1], r.Data4[2], r.Data4[3], r.Data4[4], r.Data4[5], r.Data4[6], r.Data4[7]);
}

// ---------------------------------------------------------------------------
// implementations
// ---------------------------------------------------------------------------
static HRESULT dd_QueryInterface(DDraw *dd, REFIID riid, void **out)
{
    COUNT(DD_QueryInterface);
    if (!out)
        return E_POINTER;
    if (IsEqualGUID(riid, QIID_IUnknown) || IsEqualGUID(riid, QIID_IDirectDraw)) {
        *out = &dd->vtbl1;
    } else if (IsEqualGUID(riid, QIID_IDirectDraw2)) {
        *out = &dd->vtbl2;
    } else if (IsEqualGUID(riid, QIID_IDirectDraw4) || IsEqualGUID(riid, QIID_IDirectDraw7)) {
        *out = &dd->vtbl4;
    } else {
        char b[64];
        describe_iid(riid, b, sizeof(b));
        log_event("IDirectDraw::QueryInterface %s -> E_NOINTERFACE", b);
        *out = NULL;
        return E_NOINTERFACE;
    }
    InterlockedIncrement(&dd->ref);
    return S_OK;
}

static ULONG dd_AddRef(DDraw *dd)
{
    COUNT(DD_AddRef);
    return InterlockedIncrement(&dd->ref);
}

static ULONG dd_Release(DDraw *dd)
{
    COUNT(DD_Release);
    LONG r = InterlockedDecrement(&dd->ref);
    if (r == 0) {
        log_event("IDirectDraw released");
        render_stop();
        window_restore_mode(dd);
        window_detach();
        lock_enter(&g_lock, LK_G);
        if (g_dd == dd)
            g_dd = NULL;
        LeaveCriticalSection(&g_lock);
        log_flush();
        // The object itself is intentionally leaked (a few bytes): surfaces the
        // game forgot to release still point at it.
    }
    return r;
}

static HRESULT dd_Compact(DDraw *) { COUNT(DD_Compact); return DD_OK; }

static HRESULT dd_CreateClipper(DDraw *, DWORD flags, LPDIRECTDRAWCLIPPER *out, IUnknown *)
{
    COUNT(DD_CreateClipper);
    if (!out)
        return DDERR_INVALIDPARAMS;
    log_event("CreateClipper flags=0x%lX", flags);
    *out = (LPDIRECTDRAWCLIPPER)clipper_create();
    return *out ? DD_OK : DDERR_OUTOFMEMORY;
}

static HRESULT dd_CreatePalette(DDraw *, DWORD flags, LPPALETTEENTRY ents, LPDIRECTDRAWPALETTE *out, IUnknown *)
{
    COUNT(DD_CreatePalette);
    if (!out)
        return DDERR_INVALIDPARAMS;
    log_event("CreatePalette flags=0x%lX", flags);
    *out = (LPDIRECTDRAWPALETTE)palette_create(flags, ents);
    return *out ? DD_OK : DDERR_OUTOFMEMORY;
}

static HRESULT dd_CreateSurface(DDraw *dd, DDSURFACEDESC2 *desc, void **out, IUnknown *)
{
    COUNT(DD_CreateSurface);
    if (!desc || !out)
        return DDERR_INVALIDPARAMS;
    DDSURFACEDESC2 d;
    memset(&d, 0, sizeof(d));
    DWORD sz = desc->dwSize;
    if (sz != sizeof(DDSURFACEDESC) && sz != sizeof(DDSURFACEDESC2))
        return DDERR_INVALIDPARAMS;
    memcpy(&d, desc, sz);
    Surface *s = NULL;
    HRESULT hr = surface_create(dd, &d, &s);
    *out = s;
    return hr;
}

static HRESULT dd_DuplicateSurface(DDraw *, void *, void **out)
{
    COUNT(DD_DuplicateSurface);
    if (out) *out = NULL;
    return DDERR_UNSUPPORTED;   // has an out-pointer: must not fake success
}

struct ModeEntry { int w, h, hz; };

// "2560x1440, 1920x1080" contains w x h?
static bool size_listed(const char *list, int w, int h)
{
    for (const char *p = list; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL) {
        int lw, lh;
        if (sscanf_s(p, " %dx%d", &lw, &lh) == 2 && lw == w && lh == h)
            return true;
    }
    return false;
}

static int cmp_mode(const void *a, const void *b)
{
    const ModeEntry *x = (const ModeEntry *)a, *y = (const ModeEntry *)b;
    if (x->w != y->w) return x->w - y->w;
    if (x->h != y->h) return x->h - y->h;
    return x->hz - y->hz;
}

// The real display modes of the monitor the game window is on (the primary before there is a window):
// every size and refresh rate the driver offers. The colour depth is not taken from the driver - every
// mode is emulated in 8, 16 and 32 bpp. resolutions= keeps only the listed sizes, max_resolutions= drops
// sizes above WxH or keeps only the N largest sizes.
static int collect_modes(ModeEntry *list, int cap, char *dev, size_t devCap, int *total)
{
    MonInfo m;
    if (g_dd && g_dd->hwnd)
        monitor_of_window(g_dd->hwnd, &m);
    else
        monitor_primary(&m);
    if (m.fake)
        monitor_primary(&m);
    strcpy_s(dev, devCap, m.dev);
    int n = 0;
    *total = 0;
    DEVMODEA dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsA(dev, i, &dm); i++) {
        int w = dm.dmPelsWidth, h = dm.dmPelsHeight, hz = dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 0;
        bool dup = false;
        for (int j = 0; j < n && !dup; j++)
            dup = list[j].w == w && list[j].h == h && list[j].hz == hz;
        if (!dup && n < cap) {
            list[n].w = w;
            list[n].h = h;
            list[n].hz = hz;
            n++;
        }
    }
    *total = n;
    int kept = 0;
    for (int i = 0; i < n; i++) {
        const ModeEntry e = list[i];
        if (g_cfg.resolutions[0] && !size_listed(g_cfg.resolutions, e.w, e.h))
            continue;
        if (g_cfg.maxResW && (e.w > g_cfg.maxResW || e.h > g_cfg.maxResH))
            continue;
        list[kept++] = e;
    }
    n = kept;
    qsort(list, n, sizeof(ModeEntry), cmp_mode);
    if (g_cfg.maxResolutions > 0) {
        int sizes = 0;
        for (int i = 0; i < n; i++)
            if (i == 0 || list[i].w != list[i - 1].w || list[i].h != list[i - 1].h) sizes++;
        int drop = sizes - g_cfg.maxResolutions, first = 0;
        for (; drop > 0 && first < n; first++)   // the smallest sizes go
            if (first + 1 == n || list[first + 1].w != list[first].w || list[first + 1].h != list[first].h) drop--;
        if (first) {
            memmove(list, list + first, (n - first) * sizeof(ModeEntry));
            n -= first;
        }
    }
    return n;
}

static HRESULT enum_modes(DWORD descSize, DWORD flags, DDSURFACEDESC2 *filter, void *ctx, LPDDENUMMODESCALLBACK2 cb)
{
    COUNT(DD_EnumDisplayModes);
    if (!cb)
        return DDERR_INVALIDPARAMS;
    static ModeEntry list[512];
    char dev[32];
    int total;
    int n = collect_modes(list, 512, dev, sizeof(dev), &total);
    bool rates = (flags & DDEDM_REFRESHRATES) != 0;
    static LONG calls;
    LONG call = InterlockedIncrement(&calls);
    if (call <= 3) {
        int sizes = 0;
        for (int i = 0; i < n; i++)
            if (i == 0 || list[i].w != list[i - 1].w || list[i].h != list[i - 1].h) sizes++;
        char mr[32] = "0";
        if (g_cfg.maxResW) _snprintf_s(mr, sizeof(mr), _TRUNCATE, "%dx%d", g_cfg.maxResW, g_cfg.maxResH);
        else if (g_cfg.maxResolutions) _snprintf_s(mr, sizeof(mr), _TRUNCATE, "%d", g_cfg.maxResolutions);
        log_event("EnumDisplayModes #%ld flags=0x%lX%s filter=%s: %s offers %d modes; after resolutions=\"%s\" "
                  "max_resolutions=%s: %d sizes / %d modes, each in 8, 16 and 32 bpp", call, flags,
                  rates ? " (REFRESHRATES)" : "", filter ? "yes" : "none", dev, total, g_cfg.resolutions,
                  mr, sizes, n);
        // the list itself: "640x480@60,75 800x600@60 ...", in lines of up to ~400 characters
        char line[440] = "";
        for (int i = 0; i < n; i++) {
            char one[32];
            bool newSize = i == 0 || list[i].w != list[i - 1].w || list[i].h != list[i - 1].h;
            if (newSize)
                _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%dx%d@%d", line[0] ? " " : "", list[i].w, list[i].h, list[i].hz);
            else
                _snprintf_s(one, sizeof(one), _TRUNCATE, ",%d", list[i].hz);
            if (strlen(line) + strlen(one) > 400) {
                log_event("  modes: %s", line);
                line[0] = 0;
                if (!newSize) _snprintf_s(one, sizeof(one), _TRUNCATE, "%dx%d@%d", list[i].w, list[i].h, list[i].hz);
                else if (one[0] == ' ') memmove(one, one + 1, strlen(one));
            }
            strcat_s(line, one);
        }
        if (line[0])
            log_event("  modes: %s", line);
    }
    static const int bpps[3] = {8, 16, 32};
    int reported = 0;
    for (int b = 0; b < 3; b++) {
        for (int i = 0; i < n; i++) {
            // without DDEDM_REFRESHRATES every size once, refresh rate 0 (= the default), as DirectDraw does
            if (!rates && i > 0 && list[i].w == list[i - 1].w && list[i].h == list[i - 1].h)
                continue;
            int hz = rates ? list[i].hz : 0;
            if (filter) {
                if ((filter->dwFlags & DDSD_WIDTH) && filter->dwWidth != (DWORD)list[i].w) continue;
                if ((filter->dwFlags & DDSD_HEIGHT) && filter->dwHeight != (DWORD)list[i].h) continue;
                if ((filter->dwFlags & DDSD_PIXELFORMAT) &&
                    filter->ddpfPixelFormat.dwRGBBitCount != (DWORD)bpps[b]) continue;
                if ((filter->dwFlags & DDSD_REFRESHRATE) && filter->dwRefreshRate && rates &&
                    filter->dwRefreshRate != (DWORD)hz) continue;
            }
            DDSURFACEDESC2 d;
            memset(&d, 0, sizeof(d));
            d.dwSize = descSize;
            d.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
            d.dwWidth = list[i].w;
            d.dwHeight = list[i].h;
            d.lPitch = list[i].w * bpps[b] / 8;
            d.dwRefreshRate = hz;
            fill_pixel_format(bpps[b], &d.ddpfPixelFormat);
            reported++;
            if (cb(&d, ctx) == DDENUMRET_CANCEL) {
                if (call <= 3)
                    log_event("EnumDisplayModes #%ld: cancelled by the game after %d modes", call, reported);
                return DD_OK;
            }
        }
    }
    if (call <= 3)
        log_event("EnumDisplayModes #%ld: reported %d modes", call, reported);
    return DD_OK;
}

static HRESULT dd_EnumSurfaces(DDraw *, DWORD, void *, void *, void *) { COUNT(DD_EnumSurfaces); return DD_OK; }

static HRESULT dd_FlipToGDISurface(DDraw *)
{
    COUNT(DD_FlipToGDISurface);
    if (g_counts[M_DD_FlipToGDISurface] <= 3)
        log_event("FlipToGDISurface (no-op)");
    return DD_OK;
}

static void fill_caps(DDCAPS *c)
{
    DWORD sz = c->dwSize;
    if (sz == 0 || sz > sizeof(DDCAPS))
        sz = sizeof(DDCAPS);
    DDCAPS t;
    memset(&t, 0, sizeof(t));
    t.dwSize = sz;
    t.dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY | DDCAPS_CANBLTSYSMEM |
               DDCAPS_PALETTE | DDCAPS_CANCLIP;
    t.dwCaps2 = DDCAPS2_WIDESURFACES | DDCAPS2_NOPAGELOCKREQUIRED;
    t.dwCKeyCaps = DDCKEYCAPS_SRCBLT | DDCKEYCAPS_DESTBLT;
    t.dwPalCaps = DDPCAPS_8BIT | DDPCAPS_PRIMARYSURFACE;
    t.dwVidMemTotal = 256 * 1024 * 1024;
    t.dwVidMemFree = 256 * 1024 * 1024;
    t.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_FLIP | DDSCAPS_BACKBUFFER |
                       DDSCAPS_VIDEOMEMORY | DDSCAPS_SYSTEMMEMORY | DDSCAPS_PALETTE | DDSCAPS_COMPLEX;
    memcpy(c, &t, sz);
}

static HRESULT dd_GetCaps(DDraw *, DDCAPS *hw, DDCAPS *hel)
{
    COUNT(DD_GetCaps);
    if (hw) fill_caps(hw);
    if (hel) fill_caps(hel);
    return DD_OK;
}

static HRESULT dd_GetDisplayMode(DDraw *dd, DDSURFACEDESC2 *d)
{
    COUNT(DD_GetDisplayMode);
    if (!d || (d->dwSize != sizeof(DDSURFACEDESC) && d->dwSize != sizeof(DDSURFACEDESC2)))
        return DDERR_INVALIDPARAMS;
    DWORD sz = d->dwSize;
    memset(d, 0, sz);
    d->dwSize = sz;
    int w, h, bpp;
    mode_dims(dd, &w, &h, &bpp);
    d->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    d->dwWidth = w;
    d->dwHeight = h;
    d->lPitch = w * bpp / 8;
    d->dwRefreshRate = 60;
    fill_pixel_format(bpp, &d->ddpfPixelFormat);
    return DD_OK;
}

static HRESULT dd_GetFourCCCodes(DDraw *, DWORD *num, DWORD *)
{
    COUNT(DD_GetFourCCCodes);
    if (num) *num = 0;
    return DD_OK;
}

static HRESULT dd_GetGDISurface(DDraw *dd, void **out)
{
    COUNT(DD_GetGDISurface);
    if (!out)
        return DDERR_INVALIDPARAMS;
    lock_enter(&g_lock, LK_G);
    Surface *p = dd->primary;
    if (p) InterlockedIncrement(&p->ref);
    LeaveCriticalSection(&g_lock);
    *out = p;
    return p ? DD_OK : DDERR_NOTFOUND;
}

static HRESULT dd_GetMonitorFrequency(DDraw *, DWORD *f)
{
    COUNT(DD_GetMonitorFrequency);
    if (f) *f = 60;
    return DD_OK;
}

static HRESULT dd_GetScanLine(DDraw *, DWORD *l)
{
    COUNT(DD_GetScanLine);
    if (l) *l = 0;
    return DDERR_UNSUPPORTED;
}

static HRESULT dd_GetVerticalBlankStatus(DDraw *, BOOL *b)
{
    COUNT(DD_GetVerticalBlankStatus);
    if (b) *b = TRUE;
    return DD_OK;
}

static HRESULT dd_Initialize(DDraw *, GUID *) { COUNT(DD_Initialize); return DDERR_ALREADYINITIALIZED; }

static HRESULT dd_RestoreDisplayMode(DDraw *dd)
{
    COUNT(DD_RestoreDisplayMode);
    jr(J_API, JA_RESTOREMODE);
    log_event("RestoreDisplayMode");
    window_restore_mode(dd);
    return DD_OK;
}

static HRESULT dd_SetCooperativeLevel(DDraw *dd, HWND hwnd, DWORD flags)
{
    COUNT(DD_SetCooperativeLevel);
    jr(J_API, JA_SETCOOP, 0, flags);
    char cls[64] = "";
    if (hwnd) GetClassNameA(hwnd, cls, sizeof(cls));
    log_event("SetCooperativeLevel hwnd=%p (class \"%s\") flags=0x%lX%s%s%s", hwnd, cls, flags,
              (flags & DDSCL_EXCLUSIVE) ? " EXCLUSIVE" : "", (flags & DDSCL_FULLSCREEN) ? " FULLSCREEN" : "",
              (flags & DDSCL_NORMAL) ? " NORMAL" : "");
    dd->coop = flags;
    if (hwnd && hwnd != dd->hwnd) {
        dd->hwnd = hwnd;
        window_attach(hwnd);
    }
    // DDSCL_NORMAL with an ordinary (captioned or smaller than the screen) game window: instead of a
    // desktop-sized primary the game draws at its client's screen position, treat the client area as
    // the screen. The primary gets the client size and every coordinate API reports client-relative
    // values, so the image no longer depends on where the window is, parts off the desktop are not
    // lost and the window can be dragged freely.
    if (flags & DDSCL_EXCLUSIVE)
        dd->normalEmu = false;
    if (g_cfg.normalEmu && g_cfg.hookMouse && hwnd && !(flags & (DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN)) && !dd->modeSet) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        bool popup = (GetWindowLongA(hwnd, GWL_STYLE) & WS_POPUP) != 0;
        if (rc.right > 0 && rc.bottom > 0 &&
            (!popup || rc.right < GetSystemMetrics(SM_CXSCREEN) || rc.bottom < GetSystemMetrics(SM_CYSCREEN))) {
            lock_enter(&g_lock, LK_G);
            dd->modeW = rc.right;
            dd->modeH = rc.bottom;
            dd->modeBpp = g_cfg.defaultBpp;
            dd->modeSet = true;
            dd->normalEmu = true;
            LeaveCriticalSection(&g_lock);
            log_event("normal mode emulation: client %ldx%ld is the game screen (primary %ldx%ldx%d)", rc.right,
                      rc.bottom, rc.right, rc.bottom, g_cfg.defaultBpp);
            window_normal_emu(dd);
        }
    }
    if (dd->modeSet && (flags & DDSCL_EXCLUSIVE))
        window_apply_mode(dd);
    if (hwnd)
        render_start(hwnd);
    return DD_OK;
}

static HRESULT set_mode(DDraw *dd, DWORD w, DWORD h, DWORD bpp, DWORD refresh, DWORD flags)
{
    COUNT(DD_SetDisplayMode);
    jr(J_API, JA_SETMODE, 0, (ULONG_PTR)w << 16 | h);
    log_event("SetDisplayMode %lux%lux%lu refresh=%lu flags=0x%lX (coop=0x%lX)", w, h, bpp, refresh, flags, dd->coop);
    if (w == 0 || h == 0 || (bpp != 8 && bpp != 16 && bpp != 32)) {
        log_event("SetDisplayMode: unsupported bpp/size -> DDERR_INVALIDMODE");
        return DDERR_INVALIDMODE;
    }
    lock_enter(&g_lock, LK_G);
    dd->modeW = w;
    dd->modeH = h;
    dd->modeBpp = bpp;
    dd->modeSet = true;
    if (g_cfg.flipClear && dd->primary) {   // a new mode starts black, no picture of the old one left over
        surface_clear(dd->primary);
        for (Surface *b = dd->primary->attached; b && b != dd->primary && b->chainOwner == dd->primary; b = b->attached)
            surface_clear(b);
        log_event("flipclear: primary and back buffers cleared (SetDisplayMode)");
    }
    LeaveCriticalSection(&g_lock);
    if (dd->coop & DDSCL_EXCLUSIVE)
        window_apply_mode(dd);
    render_full_dirty();
    return DD_OK;
}

static HRESULT dd_SetDisplayMode1(DDraw *dd, DWORD w, DWORD h, DWORD bpp) { return set_mode(dd, w, h, bpp, 0, 0); }
static HRESULT dd_SetDisplayMode2(DDraw *dd, DWORD w, DWORD h, DWORD bpp, DWORD r, DWORD f)
{
    return set_mode(dd, w, h, bpp, r, f);
}

static HRESULT dd_WaitForVerticalBlank(DDraw *, DWORD flags, HANDLE)
{
    COUNT(DD_WaitForVerticalBlank);
    if (flags & DDWAITVB_BLOCKBEGINEVENT)
        return DDERR_UNSUPPORTED;
    // Emulate a 60 Hz blank: sleep until the next 16.67 ms boundary.
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG period = freq.QuadPart / 60;
    LONGLONG remain = period - (now.QuadPart % period);
    DWORD ms = (DWORD)(remain * 1000 / freq.QuadPart);
    if (ms) Sleep(ms);
    return DD_OK;
}

static HRESULT dd_GetAvailableVidMem(DDraw *, DDSCAPS2 *, DWORD *total, DWORD *freeMem)
{
    COUNT(DD_GetAvailableVidMem);
    if (total) *total = 256 * 1024 * 1024;
    if (freeMem) *freeMem = 256 * 1024 * 1024;
    return DD_OK;
}

static HRESULT dd_GetSurfaceFromDC(DDraw *, HDC, void **out)
{
    COUNT(DD_GetSurfaceFromDC);
    if (out) *out = NULL;
    return DDERR_NOTFOUND;
}

static HRESULT dd_RestoreAllSurfaces(DDraw *) { COUNT(DD_RestoreAllSurfaces); return DD_OK; }
static HRESULT dd_TestCooperativeLevel(DDraw *) { COUNT(DD_TestCooperativeLevel); return DD_OK; }

static HRESULT dd_GetDeviceIdentifier(DDraw *, DDDEVICEIDENTIFIER *id, DWORD)
{
    COUNT(DD_GetDeviceIdentifier);
    if (!id)
        return DDERR_INVALIDPARAMS;
    memset(id, 0, sizeof(*id));   // the smaller (v4) struct: safe for both versions
    strcpy_s(id->szDriver, "quantumddraw");
    strcpy_s(id->szDescription, "QuantumDDraw OpenGL presenter");
    return DD_OK;
}

static HRESULT dd_StartModeTest(DDraw *, LPSIZE, DWORD, DWORD) { COUNT(DD_StartModeTest); return DDERR_UNSUPPORTED; }
static HRESULT dd_EvaluateMode(DDraw *, DWORD, DWORD *) { COUNT(DD_EvaluateMode); return DDERR_UNSUPPORTED; }

// ---------------------------------------------------------------------------
// stdcall thunks per interface offset
// ---------------------------------------------------------------------------
#define T0(R, n) template <int O> static R __stdcall t_##n(void *p) { return dd_##n(D<O>(p)); }
#define T1(R, n, A) template <int O> static R __stdcall t_##n(void *p, A a) { return dd_##n(D<O>(p), a); }
#define T2(R, n, A, B) template <int O> static R __stdcall t_##n(void *p, A a, B b) { return dd_##n(D<O>(p), a, b); }
#define T3(R, n, A, B, C) \
    template <int O> static R __stdcall t_##n(void *p, A a, B b, C c) { return dd_##n(D<O>(p), a, b, c); }
#define T4(R, n, A, B, C, E) \
    template <int O> static R __stdcall t_##n(void *p, A a, B b, C c, E e) { return dd_##n(D<O>(p), a, b, c, e); }
#define T5(R, n, A, B, C, E, F) \
    template <int O> static R __stdcall t_##n(void *p, A a, B b, C c, E e, F f) { return dd_##n(D<O>(p), a, b, c, e, f); }

T2(HRESULT, QueryInterface, REFIID, void **)
T0(ULONG, AddRef)
T0(ULONG, Release)
T0(HRESULT, Compact)
T3(HRESULT, CreateClipper, DWORD, LPDIRECTDRAWCLIPPER *, IUnknown *)
T4(HRESULT, CreatePalette, DWORD, LPPALETTEENTRY, LPDIRECTDRAWPALETTE *, IUnknown *)
T3(HRESULT, CreateSurface, DDSURFACEDESC2 *, void **, IUnknown *)
T2(HRESULT, DuplicateSurface, void *, void **)
T4(HRESULT, EnumSurfaces, DWORD, void *, void *, void *)
T0(HRESULT, FlipToGDISurface)
T2(HRESULT, GetCaps, DDCAPS *, DDCAPS *)
T1(HRESULT, GetDisplayMode, DDSURFACEDESC2 *)
T2(HRESULT, GetFourCCCodes, DWORD *, DWORD *)
T1(HRESULT, GetGDISurface, void **)
T1(HRESULT, GetMonitorFrequency, DWORD *)
T1(HRESULT, GetScanLine, DWORD *)
T1(HRESULT, GetVerticalBlankStatus, BOOL *)
T1(HRESULT, Initialize, GUID *)
T0(HRESULT, RestoreDisplayMode)
T2(HRESULT, SetCooperativeLevel, HWND, DWORD)
T3(HRESULT, SetDisplayMode1, DWORD, DWORD, DWORD)
T5(HRESULT, SetDisplayMode2, DWORD, DWORD, DWORD, DWORD, DWORD)
T2(HRESULT, WaitForVerticalBlank, DWORD, HANDLE)
T3(HRESULT, GetAvailableVidMem, DDSCAPS2 *, DWORD *, DWORD *)
T2(HRESULT, GetSurfaceFromDC, HDC, void **)
T0(HRESULT, RestoreAllSurfaces)
T0(HRESULT, TestCooperativeLevel)
T2(HRESULT, GetDeviceIdentifier, DDDEVICEIDENTIFIER *, DWORD)
T3(HRESULT, StartModeTest, LPSIZE, DWORD, DWORD)
T2(HRESULT, EvaluateMode, DWORD, DWORD *)

static HRESULT __stdcall t_EnumModes1(void *, DWORD flags, DDSURFACEDESC2 *f, void *ctx, LPDDENUMMODESCALLBACK2 cb)
{
    return enum_modes(sizeof(DDSURFACEDESC), flags, f, ctx, cb);
}
static HRESULT __stdcall t_EnumModes4(void *, DWORD flags, DDSURFACEDESC2 *f, void *ctx, LPDDENUMMODESCALLBACK2 cb)
{
    return enum_modes(sizeof(DDSURFACEDESC2), flags, f, ctx, cb);
}

#define VCOMMON_HEAD(O, ENUM)                                                                                      \
    (const void *)&t_QueryInterface<O>, (const void *)&t_AddRef<O>, (const void *)&t_Release<O>,                    \
        (const void *)&t_Compact<O>, (const void *)&t_CreateClipper<O>, (const void *)&t_CreatePalette<O>,          \
        (const void *)&t_CreateSurface<O>, (const void *)&t_DuplicateSurface<O>, (const void *)&ENUM,               \
        (const void *)&t_EnumSurfaces<O>, (const void *)&t_FlipToGDISurface<O>, (const void *)&t_GetCaps<O>,        \
        (const void *)&t_GetDisplayMode<O>, (const void *)&t_GetFourCCCodes<O>, (const void *)&t_GetGDISurface<O>,  \
        (const void *)&t_GetMonitorFrequency<O>, (const void *)&t_GetScanLine<O>,                                   \
        (const void *)&t_GetVerticalBlankStatus<O>, (const void *)&t_Initialize<O>,                                 \
        (const void *)&t_RestoreDisplayMode<O>, (const void *)&t_SetCooperativeLevel<O>

static const void *const s_vtbl1[] = {
    VCOMMON_HEAD(0, t_EnumModes1),
    (const void *)&t_SetDisplayMode1<0>,
    (const void *)&t_WaitForVerticalBlank<0>,
};

static const void *const s_vtbl2[] = {
    VCOMMON_HEAD(1, t_EnumModes1),
    (const void *)&t_SetDisplayMode2<1>,
    (const void *)&t_WaitForVerticalBlank<1>,
    (const void *)&t_GetAvailableVidMem<1>,
};

static const void *const s_vtbl4[] = {
    VCOMMON_HEAD(2, t_EnumModes4),
    (const void *)&t_SetDisplayMode2<2>,
    (const void *)&t_WaitForVerticalBlank<2>,
    (const void *)&t_GetAvailableVidMem<2>,
    (const void *)&t_GetSurfaceFromDC<2>,
    (const void *)&t_RestoreAllSurfaces<2>,
    (const void *)&t_TestCooperativeLevel<2>,
    (const void *)&t_GetDeviceIdentifier<2>,
    (const void *)&t_StartModeTest<2>,
    (const void *)&t_EvaluateMode<2>,
};

// ---------------------------------------------------------------------------
// exports
// ---------------------------------------------------------------------------
static DDraw *create_dd()
{
    log_start_thread();
    hooks_install();
    static bool filterChecked;
    if (!filterChecked) {
        filterChecked = true;
        crash_reassert();
    }
    DDraw *dd = (DDraw *)calloc(1, sizeof(DDraw));
    if (!dd)
        return NULL;
    dd->vtbl1 = s_vtbl1;
    dd->vtbl2 = s_vtbl2;
    dd->vtbl4 = s_vtbl4;
    dd->ref = 1;
    lock_enter(&g_lock, LK_G);
    g_dd = dd;
    LeaveCriticalSection(&g_lock);
    return dd;
}

extern "C" HRESULT WINAPI DirectDrawCreate(GUID *guid, LPDIRECTDRAW *out, IUnknown *outer)
{
    COUNT(DirectDrawCreate);
    log_event("DirectDrawCreate guid=%p outer=%p", guid, outer);
    if (!out)
        return DDERR_INVALIDPARAMS;
    DDraw *dd = create_dd();
    *out = dd ? (LPDIRECTDRAW)&dd->vtbl1 : NULL;
    return dd ? DD_OK : DDERR_OUTOFMEMORY;
}

extern "C" HRESULT WINAPI DirectDrawCreateEx(GUID *guid, void **out, REFIID iid, IUnknown *outer)
{
    COUNT(DirectDrawCreateEx);
    log_event("DirectDrawCreateEx guid=%p outer=%p", guid, outer);
    if (!out)
        return DDERR_INVALIDPARAMS;
    if (!IsEqualGUID(iid, QIID_IDirectDraw7))
        return DDERR_INVALIDPARAMS;
    DDraw *dd = create_dd();
    *out = dd ? (void *)&dd->vtbl4 : NULL;
    return dd ? DD_OK : DDERR_OUTOFMEMORY;
}

extern "C" HRESULT WINAPI DirectDrawCreateClipper(DWORD flags, LPDIRECTDRAWCLIPPER *out, IUnknown *)
{
    COUNT(DirectDrawCreateClipper);
    log_event("DirectDrawCreateClipper flags=0x%lX", flags);
    if (!out)
        return DDERR_INVALIDPARAMS;
    *out = (LPDIRECTDRAWCLIPPER)clipper_create();
    return *out ? DD_OK : DDERR_OUTOFMEMORY;
}

extern "C" HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA cb, LPVOID ctx)
{
    COUNT(DirectDrawEnumerateA);
    if (cb) cb(NULL, (LPSTR) "Primary Display Driver", (LPSTR) "display", ctx);
    return DD_OK;
}

extern "C" HRESULT WINAPI DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA cb, LPVOID ctx, DWORD)
{
    COUNT(DirectDrawEnumerateExA);
    if (cb) cb(NULL, (LPSTR) "Primary Display Driver", (LPSTR) "display", ctx, NULL);
    return DD_OK;
}

// The W variants are unsupported by Microsoft's ddraw.dll as well.
extern "C" HRESULT WINAPI DirectDrawEnumerateW(LPDDENUMCALLBACKW, LPVOID)
{
    COUNT(DirectDrawEnumerateW);
    return DDERR_UNSUPPORTED;
}

extern "C" HRESULT WINAPI DirectDrawEnumerateExW(LPDDENUMCALLBACKEXW, LPVOID, DWORD)
{
    COUNT(DirectDrawEnumerateExW);
    return DDERR_UNSUPPORTED;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        s_self = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_lock);
        InitializeCriticalSection(&g_pixLock);
        config_load(inst);
        log_init(inst);
        crash_init();
        OSVERSIONINFOA vi;
        memset(&vi, 0, sizeof(vi));
        vi.dwOSVersionInfoSize = sizeof(vi);
#pragma warning(suppress : 4996)
        GetVersionExA(&vi);
        log_event("QuantumDDraw loaded (build " __DATE__ " " __TIME__ "), Windows %lu.%lu.%lu (manifest-limited)",
                  vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
        log_event("config: windowed=%d windowscale=%d scale=%d linear=%d vsync=%d maxfps=%d hookmouse=%d dpi=%d "
                  "resizable=%d noactivateapp=%d bpp=%d pollprimary=%d dump=%d pbo=%d trace=%d writewatch=%d presentquiet=%d "
                  "presentcap=%d perfab=%d presentsync=%d faststretch=%d blitcopy=%s fusedupload=%d rendercluster=%d",
                  g_cfg.windowed, g_cfg.windowScale, g_cfg.scale, g_cfg.linear, g_cfg.vsync, g_cfg.maxfps,
                  g_cfg.hookMouse, g_cfg.dpi, g_cfg.resizable, g_cfg.noActivateApp, g_cfg.defaultBpp, g_cfg.pollPrimary, g_cfg.dump, g_cfg.pbo, g_cfg.trace,
                  g_cfg.writeWatch, g_cfg.presentQuiet, g_cfg.presentCap, g_cfg.perfAB, g_cfg.presentSync, g_cfg.fastStretch,
                  g_cfg.ntCopy ? "sse2nt" : "memmove", g_cfg.fusedUpload, g_cfg.renderCluster);
        log_event("config: border=%d fullscreen=%d refresh_rate=%d keytogglefullscreen=0x%02X keyunlockcursor=0x%02X "
                  "savesettings=%d adjmouse=%.2f center_cursor_fix=%d mouseprobe=%d",
                  g_cfg.border, g_cfg.fullscreen, g_cfg.refreshRate, g_cfg.keyToggleFullscreen, g_cfg.keyUnlockCursor,
                  g_cfg.saveSettings, g_cfg.adjMouse, g_cfg.centerCursorFix, g_cfg.mouseProbe);
        log_event("config: letterboxoffset=%d fix_alt_key_stuck=%d fix_not_responding=%d game_handles_close=%d "
                  "flipclear=%d fixchilds=%d childpaint=%d childrepaint=%d keepclipchildren=%d lockcursor=%d",
                  g_cfg.letterboxOffset, g_cfg.fixAltKeyStuck, g_cfg.fixNotResponding, g_cfg.gameHandlesClose,
                  g_cfg.flipClear, g_cfg.fixChilds, g_cfg.childPaint, g_cfg.childRepaint, g_cfg.keepClipChildren, g_cfg.lockCursor);
        static const char *const rendererNames[] = {"auto", "opengl", "gdi"}, *const dumpNames[] = {"false", "true", "full"};
        log_event("config: renderer=%s forceglfail=%d crashdump=%s crashtest=%d center_window=%d remove_menu=%d "
                  "keyscreenshot=0x%02X",
                  rendererNames[g_cfg.renderer], g_cfg.forceGlFail, dumpNames[g_cfg.crashDump], g_cfg.crashTest,
                  g_cfg.centerWindow, g_cfg.removeMenu, g_cfg.keyScreenshot);
        log_event("config: keynextmonitor=0x%02X monitortest=%dx%d resolutions=\"%s\" max_resolutions=%d/%dx%d "
                  "guard_lines=%d guardtest=%d maxgameticks=%d stalltest=%d",
                  g_cfg.keyNextMonitor, g_cfg.monitorTestW, g_cfg.monitorTestH, g_cfg.resolutions, g_cfg.maxResolutions,
                  g_cfg.maxResW, g_cfg.maxResH, g_cfg.guardLines, g_cfg.guardTest, g_cfg.maxGameTicks, g_cfg.stallTest);
        log_event("config: framesync=%d framefallback=%d renderpriority=%d emptypresents=%d pacestats=%d", g_cfg.frameSync,
                  g_cfg.frameFallback, g_cfg.renderPriority, g_cfg.emptyPresents, g_cfg.paceStats);
        log_event("config: client=%s%s", g_cfg.ownIni ? "Entropy-DDraw" : "(none: not our ddraw.ini)",
                  g_cfg.foreignNoActivate ? " - its noactivateapp=false ignored, kept 1" : "");
        // The game window is created before DirectDrawCreate, so DPI awareness
        // has to be decided here (all newer APIs are resolved dynamically).
        dpi_init();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_dd && g_dd->realModeChanged)
            ChangeDisplaySettingsA(NULL, 0);
        clock_report(true);
        log_event("process detach (%s)", reserved ? "process exit" : "FreeLibrary");
        log_shutdown();
    }
    return TRUE;
}
