// IDirectDrawSurface (v1..v7, one superset vtable). Pixels live in a top-down
// DIB section in system memory, so Lock returns a real pointer and GetDC is a
// plain memory DC. Blt/BltFast are CPU rectangle copies.
#include "qdd.h"
#include "blit.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const GUID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kIID_Surf1 = {0x6C14DB81, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
static const GUID kIID_Surf2 = {0x57805885, 0x6eec, 0x11cf, {0x94, 0x41, 0xa8, 0x23, 0x03, 0xc1, 0x0e, 0x27}};
static const GUID kIID_Surf3 = {0xDA044E00, 0x69B2, 0x11D0, {0xA1, 0xD5, 0x00, 0xAA, 0x00, 0xB8, 0xDF, 0xBB}};
static const GUID kIID_Surf4 = {0x0B2B8630, 0xAD35, 0x11D0, {0x8E, 0xA6, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B}};
static const GUID kIID_Surf7 = {0x06675a80, 0x3b9b, 0x11d2, {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}};

void fill_pixel_format(int bpp, DDPIXELFORMAT *pf)
{
    memset(pf, 0, sizeof(*pf));
    pf->dwSize = sizeof(DDPIXELFORMAT);
    pf->dwFlags = DDPF_RGB;
    pf->dwRGBBitCount = bpp;
    if (bpp == 8) {
        pf->dwFlags |= DDPF_PALETTEINDEXED8;
    } else if (bpp == 16) {
        pf->dwRBitMask = 0xF800;
        pf->dwGBitMask = 0x07E0;
        pf->dwBBitMask = 0x001F;
    } else {
        pf->dwRBitMask = 0xFF0000;
        pf->dwGBitMask = 0x00FF00;
        pf->dwBBitMask = 0x0000FF;
    }
}

void surface_fill_desc(Surface *s, DDSURFACEDESC2 *d, DWORD size)
{
    memset(d, 0, size);
    d->dwSize = size;
    d->dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    d->dwWidth = s->w;
    d->dwHeight = s->h;
    d->lPitch = s->pitch;
    d->ddsCaps.dwCaps = s->caps;
    fill_pixel_format(s->bpp, &d->ddpfPixelFormat);
    if (s->ckFlags & DDCKEY_SRCBLT) {
        d->dwFlags |= DDSD_CKSRCBLT;
        d->ddckCKSrcBlt = s->ckSrc;
    }
    if (s->ckFlags & DDCKEY_DESTBLT) {
        d->dwFlags |= DDSD_CKDESTBLT;
        d->ddckCKDestBlt = s->ckDst;
    }
    if (s->attached && (s->caps & DDSCAPS_PRIMARYSURFACE)) {
        d->dwFlags |= DDSD_BACKBUFFERCOUNT;
        d->dwBackBufferCount = 1;
    }
}

// --- allocation ---------------------------------------------------------------

struct BMI256 {
    BITMAPINFOHEADER h;
    DWORD colors[256];
};

static const BYTE GUARD_BYTE = 0xFD;   // guard_lines sentinel

volatile LONG g_blitMode = BM_FASTSTRETCH;   // set by config_load, switched by perfab

// blitstats (Q152 debug, see below)
static CRITICAL_SECTION s_bsCs;
static bool s_bsInit;
static LONG s_surfIds;

// a top-down DIB section of rows x w at bpp (16 bpp: RGB565 bit fields, 8 bpp: grey palette until SetPalette)
static HBITMAP make_dib(int w, int rows, int bpp, void **bits)
{
    BMI256 bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.h.biSize = sizeof(BITMAPINFOHEADER);
    bmi.h.biWidth = w;
    bmi.h.biHeight = -rows;   // top-down
    bmi.h.biPlanes = 1;
    bmi.h.biBitCount = (WORD)bpp;
    if (bpp == 16) {
        bmi.h.biCompression = BI_BITFIELDS;
        bmi.colors[0] = 0xF800;
        bmi.colors[1] = 0x07E0;
        bmi.colors[2] = 0x001F;
    } else {
        bmi.h.biCompression = BI_RGB;
        if (bpp == 8) {
            bmi.h.biClrUsed = 256;
            for (int i = 0; i < 256; i++)
                bmi.colors[i] = (DWORD)(i * 0x010101);
        }
    }
    *bits = NULL;
    return CreateDIBSection(NULL, (BITMAPINFO *)&bmi, DIB_RGB_COLORS, bits, NULL, 0);
}

// writewatch (Q149): the primary's pixels are a VirtualAlloc block with MEM_WRITE_WATCH, so the renderer can ask
// Windows which 4 KB pages were written since its last copy (GetWriteWatch) instead of comparing the whole
// picture - QEC writes the primary without Lock, after Unlock, through a pointer it kept. Same layout as the DIB
// section (pitch, guard rows); GetDC hands out a separate DIB that is copied in and out (see S_GetDC).
static bool alloc_pixels_ww(Surface *s)
{
    int pitch = ((s->w * s->bpp / 8) + 3) & ~3;
    size_t size = (size_t)pitch * (s->h + (g_cfg.guardLines ? 2 : 0));
    BYTE *base = (BYTE *)VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (!base) {
        log_event("writewatch: VirtualAlloc(MEM_WRITE_WATCH, %Iu bytes) failed (%lu) - primary as a DIB section, "
                  "changes found by comparing (pollprimary)", size, GetLastError());
        return false;
    }
    // does write watching work here (it needs Windows support for the flag in this process)? One byte written
    // must be listed as exactly one page; otherwise the DIB section and the comparison are used
    base[size / 2] = 1;
    void *pg[4];
    ULONG_PTR n = 4;
    ULONG gran = 0;
    UINT r = GetWriteWatch(WRITE_WATCH_FLAG_RESET, base, size, pg, &n, &gran);
    base[size / 2] = 0;
    if (r != 0 || n != 1 || (BYTE *)pg[0] > base + size / 2 || base + size / 2 >= (BYTE *)pg[0] + gran) {
        log_event("writewatch: self-check failed (GetWriteWatch %u, %Iu page(s), granularity %lu) - primary as a DIB "
                  "section, changes found by comparing (pollprimary)", r, n, gran);
        VirtualFree(base, 0, MEM_RELEASE);
        return false;
    }
    s->wwBase = base;
    s->wwSize = size;
    s->pitch = pitch;
    s->bits = base + (g_cfg.guardLines ? pitch : 0);   // committed memory is zero already
    if (g_cfg.guardLines) {
        memset(s->bits - pitch, GUARD_BYTE, pitch);
        memset(s->bits + (size_t)pitch * s->h, GUARD_BYTE, pitch);
    }
    ResetWriteWatch(base, size);   // the renderer starts with a complete copy anyway
    log_event("writewatch: primary %dx%dx%d pixels in write-watched memory (%Iu KB at %p)", s->w, s->h, s->bpp,
              size / 1024, base);
    return true;
}

static bool alloc_pixels(Surface *s, bool writeWatch)
{
    if (writeWatch && alloc_pixels_ww(s))
        return true;
    // guard_lines: the DIB has a sentinel row above and below the surface; Lock hands out the rows in
    // between, GetDC draws there through a 1-row viewport offset (see S_GetDC)
    void *bits = NULL;
    s->dib = make_dib(s->w, s->h + (g_cfg.guardLines ? 2 : 0), s->bpp, &bits);
    if (!s->dib)
        return false;
    s->pitch = ((s->w * s->bpp / 8) + 3) & ~3;
    s->bits = (BYTE *)bits + (g_cfg.guardLines ? s->pitch : 0);
    memset(s->bits, 0, (size_t)s->pitch * s->h);
    if (g_cfg.guardLines) {
        memset(s->bits - s->pitch, GUARD_BYTE, s->pitch);
        memset(s->bits + (size_t)s->pitch * s->h, GUARD_BYTE, s->pitch);
    }
    return true;
}

// guard_lines: a write before the first or after the last row of a surface (a game blitting or copying
// one row too far, a wrong pitch) lands in a sentinel row instead of other memory. Checked when the
// surface is locked, unlocked and released; a damaged row is reported once and repaired.
static volatile LONG s_guardHits;

void guard_check(Surface *s, const char *when)
{
    if (!g_cfg.guardLines || !s || !s->bits)
        return;
    for (int which = 0; which < 2; which++) {
        BYTE *row = which == 0 ? s->bits - s->pitch : s->bits + (size_t)s->pitch * s->h;
        int changed = 0, first = -1, last = -1;
        for (int i = 0; i < s->pitch; i++)
            if (row[i] != GUARD_BYTE) {
                changed++;
                if (first < 0) first = i;
                last = i;
            }
        if (!changed)
            continue;
        LONG n = InterlockedIncrement(&s_guardHits);
        if (n <= 50 || n % 1000 == 0)
            log_event("guard_lines ALARM #%ld: surface %p (%dx%dx%d, pitch %d%s) - the guard row %s it was "
                      "overwritten: %d byte(s) changed, bytes %d..%d (x %d..%d), first value 0x%02X; found at %s",
                      n, s, s->w, s->h, s->bpp, s->pitch, (s->caps & DDSCAPS_PRIMARYSURFACE) ? ", primary" : "",
                      which == 0 ? "BEFORE the first row" : "AFTER the last row", changed, first, last,
                      first / (s->bpp / 8 ? s->bpp / 8 : 1), last / (s->bpp / 8 ? s->bpp / 8 : 1), row[first], when);
        memset(row, GUARD_BYTE, s->pitch);
    }
}

static Surface *new_surface(DDraw *dd, int w, int h, int bpp, DWORD caps, bool writeWatch = false)
{
    Surface *s = (Surface *)calloc(1, sizeof(Surface));
    if (!s)
        return NULL;
    s->vtbl = g_surfaceVtbl;
    s->ref = 1;
    s->dd = dd;
    s->w = w;
    s->h = h;
    s->bpp = bpp;
    s->caps = caps;
    s->id = (int)InterlockedIncrement(&s_surfIds);
    if (g_cfg.blitStats && !s_bsInit) {
        InitializeCriticalSection(&s_bsCs);
        s_bsInit = true;
    }
    if (!alloc_pixels(s, writeWatch)) {
        free(s);
        return NULL;
    }
    return s;
}

static void free_surface(Surface *s)
{
    jr(J_FREE, s->id, (ULONG_PTR)s->w << 16 | s->h);
    lock_enter(&g_lock, LK_G);   // the debug dump reads g_lastLocked under g_lock
    if (g_lastLocked == s)
        g_lastLocked = NULL;
    LeaveCriticalSection(&g_lock);
    // the render thread may still be copying these pixels (it took g_pixLock before the surface stopped being
    // the primary): wait until it is done
    lock_enter(&g_pixLock, LK_PIX);
    LeaveCriticalSection(&g_pixLock);
    if (s->memdc) {
        SelectObject(s->memdc, s->oldbmp);
        DeleteDC(s->memdc);
    }
    guard_check(s, "Release");
    if (s->dib)
        DeleteObject(s->dib);
    if (s->wwBase)
        VirtualFree(s->wwBase, 0, MEM_RELEASE);
    if (s->pal)
        palette_release(s->pal);
    if (s->clip)
        clipper_release(s->clip);
    free(s);
}

static ULONG surf_release(Surface *s);

// flipclear: black pixels, and the next frame uploads the whole surface
void surface_clear(Surface *s)
{
    if (s && s->bits && s->pitch > 0 && s->h > 0)
        memset(s->bits, 0, (size_t)s->pitch * s->h);
}

HRESULT surface_create(DDraw *dd, const DDSURFACEDESC2 *d, Surface **out)
{
    *out = NULL;
    DWORD caps = (d->dwFlags & DDSD_CAPS) ? d->ddsCaps.dwCaps : 0;
    int modeBpp = dd->modeSet ? dd->modeBpp : g_cfg.defaultBpp;
    int w, h, bpp;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        if (dd->modeSet && ((dd->coop & DDSCL_EXCLUSIVE) || dd->normalEmu)) {
            w = dd->modeW;
            h = dd->modeH;
        } else {
            // Windowed (DDSCL_NORMAL): the primary represents the desktop and
            // the game blits to its client rectangle in screen coordinates.
            w = GetSystemMetrics(SM_CXSCREEN);
            h = GetSystemMetrics(SM_CYSCREEN);
        }
        bpp = modeBpp;
    } else {
        if (!(d->dwFlags & DDSD_WIDTH) || !(d->dwFlags & DDSD_HEIGHT) || !d->dwWidth || !d->dwHeight) {
            log_event("CreateSurface: offscreen without size (flags=0x%lX caps=0x%lX) -> INVALIDPARAMS", d->dwFlags,
                      caps);
            return DDERR_INVALIDPARAMS;
        }
        w = d->dwWidth;
        h = d->dwHeight;
        bpp = modeBpp;
        if (d->dwFlags & DDSD_PIXELFORMAT) {
            const DDPIXELFORMAT &pf = d->ddpfPixelFormat;
            if (pf.dwFlags & DDPF_PALETTEINDEXED8) bpp = 8;
            else if (pf.dwRGBBitCount == 8 || pf.dwRGBBitCount == 16 || pf.dwRGBBitCount == 32)
                bpp = pf.dwRGBBitCount;
            else {
                log_event("CreateSurface: unsupported pixel format bits=%lu flags=0x%lX", pf.dwRGBBitCount, pf.dwFlags);
                return DDERR_INVALIDPIXELFORMAT;
            }
        }
    }
    if (caps & DDSCAPS_ZBUFFER) {
        log_event("CreateSurface: Z buffer requested -> UNSUPPORTED");
        return DDERR_UNSUPPORTED;
    }

    DWORD outCaps = caps;
    if (!(outCaps & (DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY)))
        outCaps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
    if (caps & DDSCAPS_PRIMARYSURFACE)
        outCaps |= DDSCAPS_VISIBLE | ((caps & DDSCAPS_FLIP) ? DDSCAPS_FRONTBUFFER : 0);

    // writewatch: a primary without a flip chain (Flip swaps pixel blocks between the chain's surfaces)
    DWORD backCount = (d->dwFlags & DDSD_BACKBUFFERCOUNT) ? d->dwBackBufferCount : 0;
    bool ww = g_cfg.writeWatch && (caps & DDSCAPS_PRIMARYSURFACE) && !((caps & DDSCAPS_FLIP) && backCount);
    Surface *s = new_surface(dd, w, h, bpp, outCaps, ww);
    if (s) jr(J_CREATE, s->id, (ULONG_PTR)w << 16 | h, outCaps);
    if (!s)
        return DDERR_OUTOFMEMORY;
    if (d->dwFlags & DDSD_CKSRCBLT) {
        s->ckFlags |= DDCKEY_SRCBLT;
        s->ckSrc = d->ddckCKSrcBlt;
    }
    if (d->dwFlags & DDSD_CKDESTBLT) {
        s->ckFlags |= DDCKEY_DESTBLT;
        s->ckDst = d->ddckCKDestBlt;
    }

    if ((caps & DDSCAPS_FLIP) && backCount) {
        Surface *prev = s;
        for (DWORD i = 0; i < backCount; i++) {
            DWORD bc = (outCaps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_VISIBLE | DDSCAPS_FRONTBUFFER)) |
                       (i == 0 ? DDSCAPS_BACKBUFFER : 0);
            Surface *b = new_surface(dd, w, h, bpp, bc);
            if (!b) {
                surf_release(s);
                return DDERR_OUTOFMEMORY;
            }
            b->chainOwner = s;
            prev->attached = b;
            prev = b;
        }
    }

    static LONG logged;
    if (InterlockedIncrement(&logged) <= 400)
        log_event("CreateSurface %dx%dx%d caps=0x%08lX flags=0x%08lX backbuffers=%lu -> %p%s", w, h, bpp, caps,
                  d->dwFlags, backCount, s, (caps & DDSCAPS_PRIMARYSURFACE) ? " [PRIMARY]" : "");

    if (caps & DDSCAPS_PRIMARYSURFACE) {
        lock_enter(&g_lock, LK_G);
        dd->primary = s;
        LeaveCriticalSection(&g_lock);
        render_full_dirty();
    }
    *out = s;
    return DD_OK;
}

// --- IUnknown -------------------------------------------------------------------

static HRESULT __stdcall S_QueryInterface(Surface *s, REFIID riid, void **out)
{
    COUNT(S_QueryInterface);
    if (!out)
        return E_POINTER;
    if (IsEqualGUID(riid, kIID_IUnknown) || IsEqualGUID(riid, kIID_Surf1) || IsEqualGUID(riid, kIID_Surf2) ||
        IsEqualGUID(riid, kIID_Surf3) || IsEqualGUID(riid, kIID_Surf4) || IsEqualGUID(riid, kIID_Surf7)) {
        InterlockedIncrement(&s->ref);
        *out = s;
        return S_OK;
    }
    log_event("IDirectDrawSurface::QueryInterface {%08lX-...} -> E_NOINTERFACE", riid.Data1);
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG __stdcall S_AddRef(Surface *s)
{
    COUNT(S_AddRef);
    return InterlockedIncrement(&s->ref);
}

static ULONG surf_release(Surface *s)
{
    jr(J_API, JA_RELEASE, s->id, (ULONG_PTR)s->ref - 1);
    LONG r = InterlockedDecrement(&s->ref);
    if (r > 0)
        return r;
    lock_enter(&g_lock, LK_G);
    if (s->dd && s->dd->primary == s)
        s->dd->primary = NULL;
    LeaveCriticalSection(&g_lock);
    // the chain owner holds one reference on each back buffer
    Surface *b = s->attached;
    s->attached = NULL;
    if (b && b->chainOwner == s) {
        // release the whole owned chain
        while (b) {
            Surface *next = b->attached;
            b->attached = NULL;
            b->chainOwner = NULL;
            surf_release(b);
            b = (next && next->chainOwner == s) ? next : NULL;
        }
    } else if (b) {
        surf_release(b);   // added through AddAttachedSurface
    }
    free_surface(s);
    return 0;
}

static ULONG __stdcall S_Release(Surface *s)
{
    COUNT(S_Release);
    return surf_release(s);
}

// --- blitting ---------------------------------------------------------------------

static inline bool key_match(DWORD v, const DDCOLORKEY &k)
{
    return v >= k.dwColorSpaceLowValue && v <= k.dwColorSpaceHighValue;
}

enum KeyMode { KEY_NONE, KEY_SRC, KEY_DST };

template <typename T>
static void copy_rect(Surface *d, const RECT &dr, Surface *s, const RECT &sr, KeyMode km, const DDCOLORKEY &key)
{
    int w = dr.right - dr.left, h = dr.bottom - dr.top;
    bool same = (d == s);
    if (!same && km == KEY_NONE) {
        // two surfaces: the whole rectangle at once (one block when the rows are contiguous, QEC's full-screen
        // copies); blitcopy=sse2nt streams copies larger than the caches past them
        size_t rowBytes = (size_t)w * sizeof(T);
        int method = (g_blitMode & BM_NTCOPY) ? blit_pick(BLIT_SSE2_NT, rowBytes * h, 1u << 20) : BLIT_CRT;
        blit_rows(method, d->bits + (size_t)dr.top * d->pitch + (size_t)dr.left * sizeof(T), d->pitch,
                  s->bits + (size_t)sr.top * s->pitch + (size_t)sr.left * sizeof(T), s->pitch, rowBytes, h);
        return;
    }
    bool upward = same && dr.top > sr.top;   // copy bottom-up when moving down
    T *tmp = NULL;
    if (same && km != KEY_NONE)
        tmp = (T *)malloc(w * sizeof(T));
    for (int i = 0; i < h; i++) {
        int row = upward ? h - 1 - i : i;
        T *dp = (T *)(d->bits + (size_t)(dr.top + row) * d->pitch) + dr.left;
        const T *sp = (const T *)(s->bits + (size_t)(sr.top + row) * s->pitch) + sr.left;
        if (km == KEY_NONE) {
            memmove(dp, sp, w * sizeof(T));
            continue;
        }
        if (tmp) {
            memcpy(tmp, sp, w * sizeof(T));
            sp = tmp;
        }
        if (km == KEY_SRC) {
            for (int x = 0; x < w; x++)
                if (!key_match(sp[x], key)) dp[x] = sp[x];
        } else {
            for (int x = 0; x < w; x++)
                if (key_match(dp[x], key)) dp[x] = sp[x];
        }
    }
    free(tmp);
}

// nearest-neighbour stretch; dr is already clipped to the destination, dr0/sr
// are the original rectangles that define the mapping.
// Q150 (faststretch): the source column of every destination column is computed once per blit (stretch_map, same
// formula); without colour key, runs of consecutive columns are copied with memcpy - QEC stretches 2560 <-> 2543
// wide several times a second, which took ~9 ms each with a 64-bit division per pixel (now ~1 ms or less).
template <typename T>
static void stretch_rect(Surface *d, const RECT &dr, const RECT &dr0, Surface *s, const RECT &sr, KeyMode km,
                         const DDCOLORKEY &key)
{
    int dw0 = dr0.right - dr0.left, dh0 = dr0.bottom - dr0.top;
    int sw0 = sr.right - sr.left, sh0 = sr.bottom - sr.top;
    // snapshot the source region when blitting within one surface
    BYTE *srcBits = s->bits;
    int srcPitch = s->pitch, sx0 = sr.left, sy0 = sr.top;
    BYTE *copy = NULL;
    if (d == s) {
        srcPitch = sw0 * sizeof(T);
        copy = (BYTE *)malloc((size_t)srcPitch * sh0);
        if (!copy)
            return;
        for (int y = 0; y < sh0; y++)
            memcpy(copy + (size_t)y * srcPitch, s->bits + (size_t)(sr.top + y) * s->pitch + sr.left * sizeof(T),
                   srcPitch);
        srcBits = copy;
        sx0 = sy0 = 0;
    }
    int w = dr.right - dr.left;
    int *map = (g_blitMode & BM_FASTSTRETCH) ? (int *)malloc(sizeof(int) * w) : NULL;
    StretchRun *runs = NULL;
    int nRuns = -1;
    if (map) {
        stretch_map(map, w, dr.left - dr0.left, sw0, dw0);
        if (km == KEY_NONE && (runs = (StretchRun *)malloc(sizeof(StretchRun) * (w / 8 + 1))) != NULL)
            nRuns = stretch_runs(map, w, runs, w / 8 + 1);   // worth it while runs average 8+ pixels
    }
    for (int y = dr.top; y < dr.bottom; y++) {
        int sy = (int)(((long long)(y - dr0.top) * sh0) / dh0);
        T *dp = (T *)(d->bits + (size_t)y * d->pitch);
        const T *sp = (const T *)(srcBits + (size_t)(sy0 + sy) * srcPitch) + sx0;
        if (nRuns > 0) {
            T *row = dp + dr.left;
            for (int r = 0; r < nRuns; r++)
                memcpy(row + runs[r].dst, sp + runs[r].src, runs[r].len * sizeof(T));
            continue;
        }
        if (map) {
            T *row = dp + dr.left;
            for (int i = 0; i < w; i++) {
                T v = sp[map[i]];
                if (km == KEY_SRC && key_match(v, key)) continue;
                if (km == KEY_DST && !key_match(row[i], key)) continue;
                row[i] = v;
            }
            continue;
        }
        for (int x = dr.left; x < dr.right; x++) {
            int sx = (int)(((long long)(x - dr0.left) * sw0) / dw0);
            T v = sp[sx];
            if (km == KEY_SRC && key_match(v, key)) continue;
            if (km == KEY_DST && !key_match(dp[x], key)) continue;
            dp[x] = v;
        }
    }
    free(runs);
    free(map);
    free(copy);
}

template <typename T> static void fill_rect(Surface *d, const RECT &r, DWORD color)
{
    T c = (T)color;
    for (int y = r.top; y < r.bottom; y++) {
        T *p = (T *)(d->bits + (size_t)y * d->pitch);
        for (int x = r.left; x < r.right; x++)
            p[x] = c;
    }
}

static bool intersect_bounds(RECT &r, int w, int h)
{
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > w) r.right = w;
    if (r.bottom > h) r.bottom = h;
    return r.left < r.right && r.top < r.bottom;
}

// --- Q152 blitstats (debug) ------------------------------------------------------------------------------------
// Which surfaces the full-size copies (1M+ pixels) go between, and - after a full copy onto the primary - how long
// until the game next touches that source or the primary through our API, and how. Answers whether the source
// could stand in for the front buffer (zero copy) and how much time an asynchronous copy would have.
struct BsPair { int src, dst, sw, sh, dw, dh; bool dprim; long n; };
static BsPair s_bsPairs[12];
static int s_bsPairN;
static Surface *s_bsSrc;          // source of the last full copy onto the primary
static LONGLONG s_bsT;            // when it ended
static bool s_bsWaitSrc, s_bsWaitPrim;
enum { BS_BUCKETS = 6 };
static const double s_bsEdges[BS_BUCKETS - 1] = {0.1, 1, 5, 20, 100};   // ms
static long s_bsSrcGap[BS_BUCKETS], s_bsPrimGap[BS_BUCKETS];
static long s_bsSrcHow[3], s_bsPrimHow[4];   // source: Lock / GetDC / Blt into; primary: Lock / GetDC / full copy / other Blt

static void bs_bucket(long *h, LONGLONG ticks)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    double ms = (double)ticks * 1000.0 / (double)f.QuadPart;
    int b = 0;
    while (b < BS_BUCKETS - 1 && ms >= s_bsEdges[b]) b++;
    h[b]++;
}

// how: 0 Lock, 1 GetDC, 2 Blt into it (full copy for the primary), 3 other Blt into the primary
static void bs_touch(Surface *x, int how)
{
    if (!s_bsInit || !x)
        return;
    EnterCriticalSection(&s_bsCs);
    LONGLONG dt = perf_now() - s_bsT;
    if (s_bsWaitSrc && x == s_bsSrc) {
        s_bsWaitSrc = false;
        bs_bucket(s_bsSrcGap, dt);
        s_bsSrcHow[how > 2 ? 2 : how]++;
    }
    if (s_bsWaitPrim && x->dd && x->dd->primary == x) {
        s_bsWaitPrim = false;
        bs_bucket(s_bsPrimGap, dt);
        s_bsPrimHow[how]++;
    }
    LeaveCriticalSection(&s_bsCs);
}

static void bs_copy(Surface *s, Surface *d)
{
    if (!s_bsInit)
        return;
    bool prim = d->dd && d->dd->primary == d;
    bs_touch(d, 2);
    EnterCriticalSection(&s_bsCs);
    int i = 0;
    while (i < s_bsPairN && !(s_bsPairs[i].src == s->id && s_bsPairs[i].dst == d->id)) i++;
    if (i == s_bsPairN && s_bsPairN < 12) {
        BsPair p = {s->id, d->id, s->w, s->h, d->w, d->h, prim, 0};
        s_bsPairs[s_bsPairN++] = p;
    }
    if (i < s_bsPairN) s_bsPairs[i].n++;
    if (prim) {
        s_bsSrc = s;
        s_bsT = perf_now();
        s_bsWaitSrc = s_bsWaitPrim = true;
    }
    LeaveCriticalSection(&s_bsCs);
}

void blitstats_flush(double seconds)
{
    if (!s_bsInit)
        return;
    char line[900];
    int n = 0;
    EnterCriticalSection(&s_bsCs);
    n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "blitstats %.1fs: full copies", seconds);
    for (int i = 0; i < s_bsPairN && n > 0; i++)
        n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " #%d %dx%d -> #%d %dx%d%s %.1f/s;", s_bsPairs[i].src,
                         s_bsPairs[i].sw, s_bsPairs[i].sh, s_bsPairs[i].dst, s_bsPairs[i].dw, s_bsPairs[i].dh,
                         s_bsPairs[i].dprim ? " (PRIMARY)" : "", s_bsPairs[i].n / seconds);
    if (n > 0)
        n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE,
                         " | after a full copy to the primary, its source is next touched after <0.1/<1/<5/<20/<100/>=100 ms: "
                         "%ld/%ld/%ld/%ld/%ld/%ld (Lock %ld, GetDC %ld, Blt into %ld); the primary: %ld/%ld/%ld/%ld/%ld/%ld "
                         "(Lock %ld, GetDC %ld, next full copy %ld, other Blt %ld)",
                         s_bsSrcGap[0], s_bsSrcGap[1], s_bsSrcGap[2], s_bsSrcGap[3], s_bsSrcGap[4], s_bsSrcGap[5],
                         s_bsSrcHow[0], s_bsSrcHow[1], s_bsSrcHow[2], s_bsPrimGap[0], s_bsPrimGap[1], s_bsPrimGap[2],
                         s_bsPrimGap[3], s_bsPrimGap[4], s_bsPrimGap[5], s_bsPrimHow[0], s_bsPrimHow[1], s_bsPrimHow[2],
                         s_bsPrimHow[3]);
    for (int i = 0; i < s_bsPairN; i++) s_bsPairs[i].n = 0;
    memset(s_bsSrcGap, 0, sizeof(s_bsSrcGap));
    memset(s_bsPrimGap, 0, sizeof(s_bsPrimGap));
    memset(s_bsSrcHow, 0, sizeof(s_bsSrcHow));
    memset(s_bsPrimHow, 0, sizeof(s_bsPrimHow));
    LeaveCriticalSection(&s_bsCs);
    log_event("%s", line);
}

// Q150: the first distinct stretches the game makes (source surface / rect -> destination rect), for the log
static void stretch_note(Surface *s, const RECT &sr, const RECT &dr0, KeyMode km)
{
    static LONG seen;
    static RECT lastS, lastD;
    if (seen >= 12 || (EqualRect(&lastS, &sr) && EqualRect(&lastD, &dr0)))
        return;
    lastS = sr;
    lastD = dr0;
    InterlockedIncrement(&seen);
    log_event("Blt stretch: source %dx%d rect (%ld,%ld)-(%ld,%ld) -> destination (%ld,%ld)-(%ld,%ld)%s, faststretch %s", s->w,
              s->h, sr.left, sr.top, sr.right, sr.bottom, dr0.left, dr0.top, dr0.right, dr0.bottom,
              km == KEY_NONE ? "" : " with colour key", (g_blitMode & BM_FASTSTRETCH) ? "on" : "off");
}

static HRESULT do_blt(Surface *d, const RECT *pdr, Surface *s, const RECT *psr, DWORD flags, const DDBLTFX *fx,
                      KeyMode km, DDCOLORKEY key)
{
    jr_count(JC_BLT);
    if (d->dd && d->dd->primary == d)
        jr(J_API, JA_BLT_PRIMARY, d->id, s ? (ULONG_PTR)s->id : 0);
    RECT dr;
    if (pdr) dr = *pdr;
    else SetRect(&dr, 0, 0, d->w, d->h);
    if (dr.right <= dr.left || dr.bottom <= dr.top)
        return DD_OK;
    if (g_cfg.trace && d->dd && d->dd->primary == d && !(flags & DDBLT_COLORFILL)) {
        // debug: every distinct destination / source pair of blits to the primary (first 40)
        static RECT lastD, lastS;
        static int logged;
        RECT sr0 = psr ? *psr : RECT{0, 0, s ? s->w : 0, s ? s->h : 0};
        if (logged < 40 && (!EqualRect(&dr, &lastD) || !EqualRect(&sr0, &lastS))) {
            logged++;
            lastD = dr;
            lastS = sr0;
            log_event("trace Blt->primary dst (%ld,%ld)-(%ld,%ld) src (%ld,%ld)-(%ld,%ld) flags 0x%lX", dr.left, dr.top,
                      dr.right, dr.bottom, sr0.left, sr0.top, sr0.right, sr0.bottom, flags);
        }
    }

    if (flags & DDBLT_COLORFILL) {
        if (!fx)
            return DDERR_INVALIDPARAMS;
        log_hist(H_COLORFILL, dr.right - dr.left, dr.bottom - dr.top);
        if (!intersect_bounds(dr, d->w, d->h))
            return DD_OK;
        LONGLONG tf = perf_now();
        switch (d->bpp) {
        case 8: fill_rect<BYTE>(d, dr, fx->dwFillColor); break;
        case 16: fill_rect<WORD>(d, dr, fx->dwFillColor); break;
        default: fill_rect<DWORD>(d, dr, fx->dwFillColor); break;
        }
        perf_call(GC_FILL, perf_now() - tf);
        if (s_bsInit) bs_touch(d, 3);
        render_dirty(d, &dr);
        return DD_OK;
    }
    if (!s) {
        if ((flags & DDBLT_ROP) && fx && fx->dwROP == BLACKNESS) {
            if (intersect_bounds(dr, d->w, d->h)) {
                switch (d->bpp) {
                case 8: fill_rect<BYTE>(d, dr, 0); break;
                case 16: fill_rect<WORD>(d, dr, 0); break;
                default: fill_rect<DWORD>(d, dr, 0); break;
                }
                render_dirty(d, &dr);
            }
        }
        return DD_OK;
    }
    if (s->bpp != d->bpp) {
        static LONG warned;
        if (InterlockedIncrement(&warned) <= 10)
            log_event("Blt: format mismatch src %dbpp -> dst %dbpp ignored", s->bpp, d->bpp);
        return DD_OK;
    }
    RECT sr;
    if (psr) sr = *psr;
    else SetRect(&sr, 0, 0, s->w, s->h);
    if (sr.right <= sr.left || sr.bottom <= sr.top)
        return DD_OK;

    int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    int sw = sr.right - sr.left, sh = sr.bottom - sr.top;
    if (dw == sw && dh == sh) {
        // clip both rects together
        if (dr.left < 0) { sr.left -= dr.left; dr.left = 0; }
        if (dr.top < 0) { sr.top -= dr.top; dr.top = 0; }
        if (sr.left < 0) { dr.left -= sr.left; sr.left = 0; }
        if (sr.top < 0) { dr.top -= sr.top; sr.top = 0; }
        if (dr.right > d->w) { sr.right -= dr.right - d->w; dr.right = d->w; }
        if (dr.bottom > d->h) { sr.bottom -= dr.bottom - d->h; dr.bottom = d->h; }
        if (sr.right > s->w) { dr.right -= sr.right - s->w; sr.right = s->w; }
        if (sr.bottom > s->h) { dr.bottom -= sr.bottom - s->h; sr.bottom = s->h; }
        if (dr.right <= dr.left || dr.bottom <= dr.top)
            return DD_OK;
        LONGLONG tc = perf_now();
        switch (d->bpp) {
        case 8: copy_rect<BYTE>(d, dr, s, sr, km, key); break;
        case 16: copy_rect<WORD>(d, dr, s, sr, km, key); break;
        default: copy_rect<DWORD>(d, dr, s, sr, km, key); break;
        }
        bool big = (LONGLONG)(dr.right - dr.left) * (dr.bottom - dr.top) >= 1000000;   // QEC: full-screen copies
        if (big) g_gameCpu = (LONG)GetCurrentProcessorNumber();   // rendercluster follows this
        perf_call(big ? GC_COPYBIG : GC_COPY, perf_now() - tc);
        if (s_bsInit) {
            if (big) bs_copy(s, d);
            else bs_touch(d, 3);
        }
        // Q165: another surface copied onto the whole primary = the game finished a frame (QEC: #8 -> primary).
        // Q173: or onto all of its width from row 0 and at least 90 % of its height - a tester's QEC at 1920x1080
        // copied its frames as 1920x1048 (the bottom strip drawn on its own), and framesync never saw a frame.
        if (s != d && dr.left == 0 && dr.top == 0 && dr.right == d->w && (LONGLONG)dr.bottom * 10 >= (LONGLONG)d->h * 9 &&
            d->dd && d->dd->primary == d) {
            render_frame(d, dr.bottom == d->h);
            return DD_OK;
        }
    } else {
        // the source rectangle must lie inside the source surface
        RECT sc = sr;
        if (!intersect_bounds(sc, s->w, s->h) || !EqualRect(&sc, &sr))
            return DDERR_INVALIDRECT;
        RECT dr0 = dr;
        if (!intersect_bounds(dr, d->w, d->h))
            return DD_OK;
        stretch_note(s, sr, dr0, km);
        LONGLONG ts = perf_now();
        switch (d->bpp) {
        case 8: stretch_rect<BYTE>(d, dr, dr0, s, sr, km, key); break;
        case 16: stretch_rect<WORD>(d, dr, dr0, s, sr, km, key); break;
        default: stretch_rect<DWORD>(d, dr, dr0, s, sr, km, key); break;
        }
        perf_call(GC_STRETCH, perf_now() - ts);
        if (s_bsInit) bs_touch(d, 3);
    }
    render_dirty(d, &dr);
    return DD_OK;
}

static HRESULT __stdcall S_Blt(Surface *d, LPRECT dr, Surface *s, LPRECT sr, DWORD flags, LPDDBLTFX fx)
{
    COUNT(S_Blt);
    PerfCall pc(GC_BLT);
    window_pump();
    log_bltflags(flags);
    if (dr) log_hist(H_BLT, dr->right - dr->left, dr->bottom - dr->top);
    else log_hist(H_BLT, d->w, d->h);
    KeyMode km = KEY_NONE;
    DDCOLORKEY key = {0, 0};
    if ((flags & DDBLT_KEYSRCOVERRIDE) && fx) { km = KEY_SRC; key = fx->ddckSrcColorkey; }
    else if ((flags & DDBLT_KEYSRC) && s && (s->ckFlags & DDCKEY_SRCBLT)) { km = KEY_SRC; key = s->ckSrc; }
    else if ((flags & DDBLT_KEYDESTOVERRIDE) && fx) { km = KEY_DST; key = fx->ddckDestColorkey; }
    else if ((flags & DDBLT_KEYDEST) && (d->ckFlags & DDCKEY_DESTBLT)) { km = KEY_DST; key = d->ckDst; }
    return do_blt(d, dr, s, sr, flags, fx, km, key);
}

static HRESULT __stdcall S_BltFast(Surface *d, DWORD x, DWORD y, Surface *s, LPRECT psr, DWORD flags)
{
    COUNT(S_BltFast);
    PerfCall pc(GC_BLTFAST);
    window_pump();
    log_bltflags(flags);
    if (!s)
        return DDERR_INVALIDPARAMS;
    RECT sr;
    if (psr) sr = *psr;
    else SetRect(&sr, 0, 0, s->w, s->h);
    log_hist(H_BLTFAST, sr.right - sr.left, sr.bottom - sr.top);
    RECT dr = {(LONG)x, (LONG)y, (LONG)x + (sr.right - sr.left), (LONG)y + (sr.bottom - sr.top)};
    KeyMode km = KEY_NONE;
    DDCOLORKEY key = {0, 0};
    if ((flags & DDBLTFAST_SRCCOLORKEY) && (s->ckFlags & DDCKEY_SRCBLT)) { km = KEY_SRC; key = s->ckSrc; }
    else if ((flags & DDBLTFAST_DESTCOLORKEY) && (d->ckFlags & DDCKEY_DESTBLT)) { km = KEY_DST; key = d->ckDst; }
    return do_blt(d, &dr, s, &sr, 0, NULL, km, key);
}

static HRESULT __stdcall S_BltBatch(Surface *, LPDDBLTBATCH, DWORD, DWORD) { COUNT(S_BltBatch); return DD_OK; }

// --- attachment / flipping ------------------------------------------------------------

static HRESULT __stdcall S_AddAttachedSurface(Surface *s, Surface *a)
{
    COUNT(S_AddAttachedSurface);
    if (!a)
        return DDERR_INVALIDPARAMS;
    log_event("AddAttachedSurface %p <- %p (caps 0x%lX)", s, a, a->caps);
    if (a->caps & DDSCAPS_ZBUFFER)
        return DD_OK;
    if (s->attached)
        return DDERR_SURFACEALREADYATTACHED;
    InterlockedIncrement(&a->ref);
    s->attached = a;
    return DD_OK;
}

static HRESULT __stdcall S_AddOverlayDirtyRect(Surface *, LPRECT) { COUNT(S_AddOverlayDirtyRect); return DD_OK; }

static HRESULT __stdcall S_DeleteAttachedSurface(Surface *s, DWORD, Surface *a)
{
    COUNT(S_DeleteAttachedSurface);
    if (!s->attached || (a && a != s->attached))
        return DDERR_SURFACENOTATTACHED;
    Surface *b = s->attached;
    s->attached = NULL;
    b->chainOwner = NULL;
    surf_release(b);
    return DD_OK;
}

static HRESULT __stdcall S_EnumAttachedSurfaces(Surface *s, LPVOID ctx, LPDDENUMSURFACESCALLBACK7 cb)
{
    COUNT(S_EnumAttachedSurfaces);
    if (!cb)
        return DDERR_INVALIDPARAMS;
    if (s->attached) {
        DDSURFACEDESC2 d;
        surface_fill_desc(s->attached, &d, sizeof(DDSURFACEDESC2));
        InterlockedIncrement(&s->attached->ref);
        cb((LPDIRECTDRAWSURFACE7)s->attached, &d, ctx);
    }
    return DD_OK;
}

static HRESULT __stdcall S_EnumOverlayZOrders(Surface *, DWORD, LPVOID, LPDDENUMSURFACESCALLBACK7)
{
    COUNT(S_EnumOverlayZOrders);
    return DD_OK;
}

static void swap_pixels(Surface *a, Surface *b)
{
    BYTE *bits = a->bits; a->bits = b->bits; b->bits = bits;
    HBITMAP dib = a->dib; a->dib = b->dib; b->dib = dib;
    HDC dc = a->memdc; a->memdc = b->memdc; b->memdc = dc;
    HGDIOBJ ob = a->oldbmp; a->oldbmp = b->oldbmp; b->oldbmp = ob;
    int p = a->pitch; a->pitch = b->pitch; b->pitch = p;
    BYTE *wb = a->wwBase; a->wwBase = b->wwBase; b->wwBase = wb;
    size_t ws = a->wwSize; a->wwSize = b->wwSize; b->wwSize = ws;
    BYTE *gb = a->gdiBits; a->gdiBits = b->gdiBits; b->gdiBits = gb;
}

static HRESULT __stdcall S_Flip(Surface *s, Surface *target, DWORD flags)
{
    COUNT(S_Flip);
    PerfCall pc(GC_FLIP);
    jr(J_API, JA_FLIP, s->id);
    window_pump();
    (void)flags;
    if (s->attached && (s->caps & DDSCAPS_PRIMARYSURFACE)) {
        lock_enter(&g_lock, LK_G);
        lock_enter(&g_pixLock, LK_PIX);   // the render thread may be copying the front buffer's pixels
        if (target && target != s->attached) {
            swap_pixels(s, target);
        } else {
            // rotate the chain: front <- back1 <- back2 ... <- old front
            Surface *cur = s;
            while (cur->attached && cur->attached != s && cur->attached->chainOwner == s) {
                swap_pixels(cur, cur->attached);
                cur = cur->attached;
            }
        }
        // flipclear: the back buffer the game draws next starts black - with real hardware its content
        // after a flip is undefined, and stale pixels of an older frame would otherwise show up again
        if (g_cfg.flipClear) {
            Surface *b = target && target != s->attached ? target : s->attached;
            surface_clear(b);
            static LONG logged;
            if (logged++ < 3)
                log_event("flipclear: back buffer %p cleared after Flip", b);
        }
        LeaveCriticalSection(&g_pixLock);
        LeaveCriticalSection(&g_lock);
    }
    render_full_dirty();
    return DD_OK;
}

static HRESULT __stdcall S_GetAttachedSurface(Surface *s, LPDDSCAPS2 caps, Surface **out)
{
    COUNT(S_GetAttachedSurface);
    if (!out || !caps)
        return DDERR_INVALIDPARAMS;
    *out = NULL;
    Surface *a = s->attached;
    if (a && ((a->caps & caps->dwCaps) == caps->dwCaps || (caps->dwCaps & (DDSCAPS_BACKBUFFER | DDSCAPS_FLIP)))) {
        InterlockedIncrement(&a->ref);
        *out = a;
        return DD_OK;
    }
    log_event("GetAttachedSurface caps=0x%lX on %p -> NOTFOUND", caps->dwCaps, s);
    return DDERR_NOTFOUND;
}

static HRESULT __stdcall S_GetBltStatus(Surface *, DWORD) { COUNT(S_GetBltStatus); return DD_OK; }

static HRESULT __stdcall S_GetCaps(Surface *s, LPDDSCAPS2 caps)
{
    COUNT(S_GetCaps);
    if (!caps)
        return DDERR_INVALIDPARAMS;
    caps->dwCaps = s->caps;   // only the first DWORD: DDSCAPS and DDSCAPS2 differ in size
    return DD_OK;
}

static HRESULT __stdcall S_GetClipper(Surface *s, Clipper **out)
{
    COUNT(S_GetClipper);
    if (!out)
        return DDERR_INVALIDPARAMS;
    *out = s->clip;
    if (!s->clip)
        return DDERR_NOCLIPPERATTACHED;
    clipper_addref(s->clip);
    return DD_OK;
}

static HRESULT __stdcall S_GetColorKey(Surface *s, DWORD flags, LPDDCOLORKEY key)
{
    COUNT(S_GetColorKey);
    if (!key)
        return DDERR_INVALIDPARAMS;
    if ((flags & DDCKEY_SRCBLT) && (s->ckFlags & DDCKEY_SRCBLT)) { *key = s->ckSrc; return DD_OK; }
    if ((flags & DDCKEY_DESTBLT) && (s->ckFlags & DDCKEY_DESTBLT)) { *key = s->ckDst; return DD_OK; }
    return DDERR_NOCOLORKEY;
}

static void sync_dib_palette(Surface *s)
{
    if (s->bpp != 8 || !s->memdc)
        return;
    Palette *p = s->pal;
    if (!p && s->dd && s->dd->primary)
        p = s->dd->primary->pal;
    if (!p)
        return;
    RGBQUAD q[256];
    for (int i = 0; i < 256; i++) {
        q[i].rgbRed = p->ents[i].peRed;
        q[i].rgbGreen = p->ents[i].peGreen;
        q[i].rgbBlue = p->ents[i].peBlue;
        q[i].rgbReserved = 0;
    }
    SetDIBColorTable(s->memdc, 0, 256, q);
}

static HRESULT __stdcall S_GetDC(Surface *s, HDC *out)
{
    COUNT(S_GetDC);
    jr(J_API, JA_GETDC, s->id);
    PerfCall pc(GC_GETDC);
    if (s_bsInit) bs_touch(s, 1);
    if (!out)
        return DDERR_INVALIDPARAMS;
    if (s->wwBase && !s->dib) {
        // writewatch: the pixels are no DIB section - GDI draws into a DIB of the same layout (no guard rows)
        // that holds a copy between GetDC and ReleaseDC
        void *bits = NULL;
        s->dib = make_dib(s->w, s->h, s->bpp, &bits);
        if (!s->dib)
            return DDERR_GENERIC;
        s->gdiBits = (BYTE *)bits;
        log_event("writewatch: GetDC on the primary %p - GDI draws into a copy (copied back at ReleaseDC)", s);
    }
    if (!s->memdc) {
        s->memdc = CreateCompatibleDC(NULL);
        if (!s->memdc)
            return DDERR_GENERIC;
        s->oldbmp = SelectObject(s->memdc, s->dib);
        if (g_cfg.guardLines && !s->wwBase) {
            // the DIB starts with the upper guard row: GDI drawing is moved one row down and clipped to the
            // surface, so (0,0) is the surface's first pixel and GDI never touches a guard row
            SetViewportOrgEx(s->memdc, 0, 1, NULL);
            IntersectClipRect(s->memdc, 0, 0, s->w, s->h);
        }
    }
    if (s->wwBase && s->gdiBits)
        memcpy(s->gdiBits, s->bits, (size_t)s->pitch * s->h);
    sync_dib_palette(s);
    *out = s->memdc;
    return DD_OK;
}

static HRESULT __stdcall S_ReleaseDC(Surface *s, HDC)
{
    COUNT(S_ReleaseDC);
    jr(J_API, JA_RELEASEDC, s->id);
    PerfCall pc(GC_GETDC);
    if (s_bsInit) bs_touch(s, 1);
    GdiFlush();
    if (s->wwBase && s->gdiBits)
        memcpy(s->bits, s->gdiBits, (size_t)s->pitch * s->h);
    render_dirty(s, NULL);
    return DD_OK;
}

static HRESULT __stdcall S_GetFlipStatus(Surface *, DWORD) { COUNT(S_GetFlipStatus); return DD_OK; }

static HRESULT __stdcall S_GetOverlayPosition(Surface *, LPLONG, LPLONG)
{
    COUNT(S_GetOverlayPosition);
    return DDERR_NOTAOVERLAYSURFACE;
}

static HRESULT __stdcall S_GetPalette(Surface *s, Palette **out)
{
    COUNT(S_GetPalette);
    if (!out)
        return DDERR_INVALIDPARAMS;
    *out = s->pal;
    if (!s->pal)
        return DDERR_NOPALETTEATTACHED;
    palette_addref(s->pal);
    return DD_OK;
}

static HRESULT __stdcall S_GetPixelFormat(Surface *s, LPDDPIXELFORMAT pf)
{
    COUNT(S_GetPixelFormat);
    if (!pf)
        return DDERR_INVALIDPARAMS;
    fill_pixel_format(s->bpp, pf);
    return DD_OK;
}

static HRESULT __stdcall S_GetSurfaceDesc(Surface *s, LPDDSURFACEDESC2 d)
{
    COUNT(S_GetSurfaceDesc);
    if (!d || (d->dwSize != sizeof(DDSURFACEDESC) && d->dwSize != sizeof(DDSURFACEDESC2)))
        return DDERR_INVALIDPARAMS;
    surface_fill_desc(s, d, d->dwSize);
    return DD_OK;
}

static HRESULT __stdcall S_Initialize(Surface *, LPDIRECTDRAW, LPDDSURFACEDESC2)
{
    COUNT(S_Initialize);
    return DDERR_ALREADYINITIALIZED;
}

static HRESULT __stdcall S_IsLost(Surface *) { COUNT(S_IsLost); return DD_OK; }

static HRESULT __stdcall S_Lock(Surface *s, LPRECT r, LPDDSURFACEDESC2 d, DWORD, HANDLE)
{
    COUNT(S_Lock);
    PerfCall pc(GC_LOCK);
    jr_count(JC_LOCK);
    if (s_bsInit) bs_touch(s, 0);
    window_pump();
    guard_check(s, "Lock");
    if (!d || (d->dwSize != sizeof(DDSURFACEDESC) && d->dwSize != sizeof(DDSURFACEDESC2)))
        return DDERR_INVALIDPARAMS;
    RECT lr;
    if (r) {
        lr = *r;
        if (!intersect_bounds(lr, s->w, s->h))
            return DDERR_INVALIDRECT;
    } else {
        SetRect(&lr, 0, 0, s->w, s->h);
    }
    log_hist((s->dd && s->dd->primary == s) ? H_LOCK_PRIMARY : H_LOCK, lr.right - lr.left, lr.bottom - lr.top);
    surface_fill_desc(s, d, d->dwSize);
    d->dwFlags |= DDSD_LPSURFACE;
    d->lpSurface = s->bits + (size_t)lr.top * s->pitch + lr.left * (s->bpp / 8);
    s->lockRect = lr;
    s->locked = true;
    if (!(s->dd && s->dd->primary == s))
        g_lastLocked = s;
    return DD_OK;
}

// Surfaces live in system memory and keep their pixels across focus and display mode changes, so they
// are never lost (IsLost = DD_OK) and Restore has nothing to bring back. Restore still re-uploads the
// whole picture; with flipclear it also clears the surface, like a real restore of video memory.
static HRESULT __stdcall S_Restore(Surface *s)
{
    COUNT(S_Restore);
    jr(J_API, JA_RESTORE, s->id);
    if (g_cfg.flipClear) {
        lock_enter(&g_lock, LK_G);
        surface_clear(s);
        LeaveCriticalSection(&g_lock);
    }
    static LONG logged;
    if (logged++ < 5)
        log_event("Restore %p (%dx%d)%s", s, s->w, s->h, g_cfg.flipClear ? ": cleared (flipclear)" : "");
    render_full_dirty();
    return DD_OK;
}

static HRESULT __stdcall S_SetClipper(Surface *s, Clipper *c)
{
    COUNT(S_SetClipper);
    jr(J_API, JA_CLIPPER, s->id);
    if (c) clipper_addref(c);
    if (s->clip) clipper_release(s->clip);
    s->clip = c;
    return DD_OK;
}

static HRESULT __stdcall S_SetColorKey(Surface *s, DWORD flags, LPDDCOLORKEY key)
{
    COUNT(S_SetColorKey);
    DWORD which = flags & (DDCKEY_SRCBLT | DDCKEY_DESTBLT);
    if (!which) {
        static LONG warned;
        if (InterlockedIncrement(&warned) <= 5)
            log_event("SetColorKey: unsupported flags 0x%lX ignored", flags);
        return DD_OK;
    }
    if (!key) {
        s->ckFlags &= ~which;
        return DD_OK;
    }
    DDCOLORKEY k = *key;
    if (!(flags & DDCKEY_COLORSPACE))
        k.dwColorSpaceHighValue = k.dwColorSpaceLowValue;
    s->ckFlags |= which;
    if (which & DDCKEY_SRCBLT) s->ckSrc = k;
    if (which & DDCKEY_DESTBLT) s->ckDst = k;
    return DD_OK;
}

static HRESULT __stdcall S_SetOverlayPosition(Surface *, LONG, LONG) { COUNT(S_SetOverlayPosition); return DD_OK; }

static HRESULT __stdcall S_SetPalette(Surface *s, Palette *p)
{
    COUNT(S_SetPalette);
    jr(J_API, JA_SETPALETTE, s->id, (ULONG_PTR)p);
    if (p) palette_addref(p);
    Palette *old = s->pal;
    lock_enter(&g_lock, LK_G);
    s->pal = p;
    LeaveCriticalSection(&g_lock);
    if (old) palette_release(old);
    if (s->dd && s->dd->primary == s)
        render_palette_changed(p);
    return DD_OK;
}

static HRESULT __stdcall S_Unlock(Surface *s, LPRECT)
{
    COUNT(S_Unlock);
    PerfCall pc(GC_UNLOCK);
    guard_check(s, "Unlock");
    if (s->locked) {
        s->locked = false;
        // Q173: a Lock of the whole primary says nothing about what changed (QEC redraws its cursor that way at its
        // loop rate, ~150/s on a tester's machine); marking all of it dirty uploaded a full frame each time. The
        // render thread compares the primary with its shadow copy instead and uploads only the rows that changed.
        if (g_cfg.pollPrimary && s->dd && s->dd->primary == s && s->lockRect.left <= 0 && s->lockRect.top <= 0 &&
            s->lockRect.right >= s->w && s->lockRect.bottom >= s->h)
            render_scan(s);
        else
            render_dirty(s, &s->lockRect);
    }
    // guardtest (debug): at the 300th, 600th and 900th Unlock, write past the surface like a game bug
    // would - 4 bytes after the last row, 2 bytes before the first row, a whole row after the last one.
    // The next Lock / Unlock / Release of that surface has to report it.
    static LONG unlocks;
    LONG u = InterlockedIncrement(&unlocks);
    if (g_cfg.guardTest && g_cfg.guardLines && (u == 300 || u == 600 || u == 900)) {
        BYTE *end = s->bits + (size_t)s->pitch * s->h;
        if (u == 300) memset(end, 0x11, 4);
        else if (u == 600) memset(s->bits - 2, 0x22, 2);
        else memset(end, 0x33, s->pitch);
        log_event("guardtest: Unlock #%ld of surface %p (%dx%dx%d) - wrote %d byte(s) %s", u, s, s->w, s->h, s->bpp,
                  u == 300 ? 4 : u == 600 ? 2 : s->pitch, u == 600 ? "before the first row" : "after the last row");
    }
    return DD_OK;
}

static HRESULT __stdcall S_UpdateOverlay(Surface *, LPRECT, Surface *, LPRECT, DWORD, LPDDOVERLAYFX)
{
    COUNT(S_UpdateOverlay);
    return DD_OK;
}
static HRESULT __stdcall S_UpdateOverlayDisplay(Surface *, DWORD) { COUNT(S_UpdateOverlayDisplay); return DD_OK; }
static HRESULT __stdcall S_UpdateOverlayZOrder(Surface *, DWORD, Surface *) { COUNT(S_UpdateOverlayZOrder); return DD_OK; }

static HRESULT __stdcall S_GetDDInterface(Surface *s, LPVOID *out)
{
    COUNT(S_GetDDInterface);
    if (!out)
        return DDERR_INVALIDPARAMS;
    *out = &s->dd->vtbl1;
    InterlockedIncrement(&s->dd->ref);
    return DD_OK;
}

static HRESULT __stdcall S_PageLock(Surface *, DWORD) { COUNT(S_PageLock); return DD_OK; }
static HRESULT __stdcall S_PageUnlock(Surface *, DWORD) { COUNT(S_PageUnlock); return DD_OK; }

static HRESULT __stdcall S_SetSurfaceDesc(Surface *, LPDDSURFACEDESC2, DWORD)
{
    COUNT(S_SetSurfaceDesc);
    log_event("SetSurfaceDesc (client memory) -> UNSUPPORTED");
    return DDERR_UNSUPPORTED;
}

static HRESULT __stdcall S_SetPrivateData(Surface *, REFGUID, LPVOID, DWORD, DWORD) { COUNT(S_SetPrivateData); return DD_OK; }
static HRESULT __stdcall S_GetPrivateData(Surface *, REFGUID, LPVOID, LPDWORD)
{
    COUNT(S_GetPrivateData);
    return DDERR_NOTFOUND;
}
static HRESULT __stdcall S_FreePrivateData(Surface *, REFGUID) { COUNT(S_FreePrivateData); return DD_OK; }
static HRESULT __stdcall S_GetUniquenessValue(Surface *, LPDWORD v)
{
    COUNT(S_GetUniquenessValue);
    if (v) *v = 1;
    return DD_OK;
}
static HRESULT __stdcall S_ChangeUniquenessValue(Surface *) { COUNT(S_ChangeUniquenessValue); return DD_OK; }
static HRESULT __stdcall S_SetPriority(Surface *, DWORD) { COUNT(S_SetPriority); return DD_OK; }
static HRESULT __stdcall S_GetPriority(Surface *, LPDWORD v)
{
    COUNT(S_GetPriority);
    if (v) *v = 0;
    return DD_OK;
}
static HRESULT __stdcall S_SetLOD(Surface *, DWORD) { COUNT(S_SetLOD); return DD_OK; }
static HRESULT __stdcall S_GetLOD(Surface *, LPDWORD v)
{
    COUNT(S_GetLOD);
    if (v) *v = 0;
    return DD_OK;
}

// IDirectDrawSurface7 layout; v1..v4 are strict prefixes of it.
const void *const g_surfaceVtbl[] = {
    (const void *)&S_QueryInterface, (const void *)&S_AddRef, (const void *)&S_Release,
    (const void *)&S_AddAttachedSurface, (const void *)&S_AddOverlayDirtyRect, (const void *)&S_Blt,
    (const void *)&S_BltBatch, (const void *)&S_BltFast, (const void *)&S_DeleteAttachedSurface,
    (const void *)&S_EnumAttachedSurfaces, (const void *)&S_EnumOverlayZOrders, (const void *)&S_Flip,
    (const void *)&S_GetAttachedSurface, (const void *)&S_GetBltStatus, (const void *)&S_GetCaps,
    (const void *)&S_GetClipper, (const void *)&S_GetColorKey, (const void *)&S_GetDC,
    (const void *)&S_GetFlipStatus, (const void *)&S_GetOverlayPosition, (const void *)&S_GetPalette,
    (const void *)&S_GetPixelFormat, (const void *)&S_GetSurfaceDesc, (const void *)&S_Initialize,
    (const void *)&S_IsLost, (const void *)&S_Lock, (const void *)&S_ReleaseDC, (const void *)&S_Restore,
    (const void *)&S_SetClipper, (const void *)&S_SetColorKey, (const void *)&S_SetOverlayPosition,
    (const void *)&S_SetPalette, (const void *)&S_Unlock, (const void *)&S_UpdateOverlay,
    (const void *)&S_UpdateOverlayDisplay, (const void *)&S_UpdateOverlayZOrder,
    // v2
    (const void *)&S_GetDDInterface, (const void *)&S_PageLock, (const void *)&S_PageUnlock,
    // v3
    (const void *)&S_SetSurfaceDesc,
    // v4
    (const void *)&S_SetPrivateData, (const void *)&S_GetPrivateData, (const void *)&S_FreePrivateData,
    (const void *)&S_GetUniquenessValue, (const void *)&S_ChangeUniquenessValue,
    // v7
    (const void *)&S_SetPriority, (const void *)&S_GetPriority, (const void *)&S_SetLOD, (const void *)&S_GetLOD,
};
