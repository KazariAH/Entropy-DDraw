// IDirectDrawPalette and IDirectDrawClipper.
#include "qdd.h"
#include <string.h>
#include <stdlib.h>

static const GUID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kIID_Palette = {0x6C14DB84, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
static const GUID kIID_Clipper = {0x6C14DB85, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};

// --- palette ------------------------------------------------------------------------

static HRESULT __stdcall P_QueryInterface(Palette *p, REFIID riid, void **out)
{
    COUNT(P_QueryInterface);
    if (!out)
        return E_POINTER;
    if (IsEqualGUID(riid, kIID_IUnknown) || IsEqualGUID(riid, kIID_Palette)) {
        InterlockedIncrement(&p->ref);
        *out = p;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

void palette_addref(Palette *p) { InterlockedIncrement(&p->ref); }

void palette_release(Palette *p)
{
    if (InterlockedDecrement(&p->ref) == 0) {
        // make sure the render thread no longer looks at it
        lock_enter(&g_lock, LK_G);
        if (g_dd && g_dd->primary && g_dd->primary->pal == p)
            g_dd->primary->pal = NULL;
        LeaveCriticalSection(&g_lock);
        free(p);
    }
}

static ULONG __stdcall P_AddRef(Palette *p)
{
    COUNT(P_AddRef);
    return InterlockedIncrement(&p->ref);
}

static ULONG __stdcall P_Release(Palette *p)
{
    COUNT(P_Release);
    LONG r = p->ref - 1;
    palette_release(p);
    return r < 0 ? 0 : r;
}

static HRESULT __stdcall P_GetCaps(Palette *p, LPDWORD caps)
{
    COUNT(P_GetCaps);
    if (!caps)
        return DDERR_INVALIDPARAMS;
    *caps = p->caps;
    return DD_OK;
}

static HRESULT __stdcall P_GetEntries(Palette *p, DWORD, DWORD start, DWORD count, LPPALETTEENTRY out)
{
    COUNT(P_GetEntries);
    if (!out || start >= 256)
        return DDERR_INVALIDPARAMS;
    if (start + count > 256)
        count = 256 - start;
    memcpy(out, &p->ents[start], count * sizeof(PALETTEENTRY));
    return DD_OK;
}

static HRESULT __stdcall P_Initialize(Palette *, LPDIRECTDRAW, DWORD, LPPALETTEENTRY)
{
    COUNT(P_Initialize);
    return DDERR_ALREADYINITIALIZED;
}

static HRESULT __stdcall P_SetEntries(Palette *p, DWORD, DWORD start, DWORD count, LPPALETTEENTRY in)
{
    COUNT(P_SetEntries);
    if (!in || start >= 256)
        return DDERR_INVALIDPARAMS;
    if (start + count > 256)
        count = 256 - start;
    lock_enter(&g_lock, LK_G);
    memcpy(&p->ents[start], in, count * sizeof(PALETTEENTRY));
    LeaveCriticalSection(&g_lock);
    render_palette_changed(p);
    return DD_OK;
}

static const void *const s_palVtbl[] = {
    (const void *)&P_QueryInterface, (const void *)&P_AddRef,     (const void *)&P_Release,
    (const void *)&P_GetCaps,        (const void *)&P_GetEntries, (const void *)&P_Initialize,
    (const void *)&P_SetEntries,
};

Palette *palette_create(DWORD flags, const PALETTEENTRY *ents)
{
    Palette *p = (Palette *)calloc(1, sizeof(Palette));
    if (!p)
        return NULL;
    p->vtbl = s_palVtbl;
    p->ref = 1;
    p->caps = flags | DDPCAPS_8BIT;
    if (ents) {
        int n = (flags & DDPCAPS_4BIT) ? 16 : (flags & DDPCAPS_2BIT) ? 4 : (flags & DDPCAPS_1BIT) ? 2 : 256;
        memcpy(p->ents, ents, n * sizeof(PALETTEENTRY));
    }
    return p;
}

// --- clipper ------------------------------------------------------------------------

void clipper_addref(Clipper *c) { InterlockedIncrement(&c->ref); }

void clipper_release(Clipper *c)
{
    if (InterlockedDecrement(&c->ref) == 0)
        free(c);
}

static HRESULT __stdcall C_QueryInterface(Clipper *c, REFIID riid, void **out)
{
    COUNT(C_QueryInterface);
    if (!out)
        return E_POINTER;
    if (IsEqualGUID(riid, kIID_IUnknown) || IsEqualGUID(riid, kIID_Clipper)) {
        InterlockedIncrement(&c->ref);
        *out = c;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG __stdcall C_AddRef(Clipper *c)
{
    COUNT(C_AddRef);
    return InterlockedIncrement(&c->ref);
}

static ULONG __stdcall C_Release(Clipper *c)
{
    COUNT(C_Release);
    LONG r = c->ref - 1;
    clipper_release(c);
    return r < 0 ? 0 : r;
}

// One rectangle: the window's client area in screen coordinates (or the whole
// primary when no window is attached).
static HRESULT __stdcall C_GetClipList(Clipper *c, LPRECT, LPRGNDATA data, LPDWORD size)
{
    COUNT(C_GetClipList);
    if (!size)
        return DDERR_INVALIDPARAMS;
    DWORD need = sizeof(RGNDATAHEADER) + sizeof(RECT);
    if (!data) {
        *size = need;
        return DD_OK;
    }
    if (*size < need)
        return DDERR_REGIONTOOSMALL;
    RECT r;
    if (c->hwnd && GetClientRect(c->hwnd, &r)) {
        MapWindowPoints(c->hwnd, NULL, (POINT *)&r, 2);
    } else {
        SetRect(&r, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    }
    memset(&data->rdh, 0, sizeof(data->rdh));
    data->rdh.dwSize = sizeof(RGNDATAHEADER);
    data->rdh.iType = RDH_RECTANGLES;
    data->rdh.nCount = 1;
    data->rdh.nRgnSize = sizeof(RECT);
    data->rdh.rcBound = r;
    memcpy(data->Buffer, &r, sizeof(RECT));
    return DD_OK;
}

static HRESULT __stdcall C_GetHWnd(Clipper *c, HWND *out)
{
    COUNT(C_GetHWnd);
    if (!out)
        return DDERR_INVALIDPARAMS;
    *out = c->hwnd;
    return DD_OK;
}

static HRESULT __stdcall C_Initialize(Clipper *, LPDIRECTDRAW, DWORD) { COUNT(C_Initialize); return DD_OK; }

static HRESULT __stdcall C_IsClipListChanged(Clipper *, BOOL *changed)
{
    COUNT(C_IsClipListChanged);
    if (changed) *changed = FALSE;
    return DD_OK;
}

static HRESULT __stdcall C_SetClipList(Clipper *, LPRGNDATA, DWORD) { COUNT(C_SetClipList); return DD_OK; }

static HRESULT __stdcall C_SetHWnd(Clipper *c, DWORD, HWND hwnd)
{
    COUNT(C_SetHWnd);
    log_event("Clipper::SetHWnd %p", hwnd);
    c->hwnd = hwnd;
    return DD_OK;
}

static const void *const s_clipVtbl[] = {
    (const void *)&C_QueryInterface, (const void *)&C_AddRef,     (const void *)&C_Release,
    (const void *)&C_GetClipList,    (const void *)&C_GetHWnd,    (const void *)&C_Initialize,
    (const void *)&C_IsClipListChanged, (const void *)&C_SetClipList, (const void *)&C_SetHWnd,
};

Clipper *clipper_create()
{
    Clipper *c = (Clipper *)calloc(1, sizeof(Clipper));
    if (!c)
        return NULL;
    c->vtbl = s_clipVtbl;
    c->ref = 1;
    return c;
}
