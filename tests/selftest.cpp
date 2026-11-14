// Self-test for the code paths Yuri's Revenge does not exercise (8-bit palette,
// flip chain, colour keys, overlapping/stretched Blt, GetDC, QueryInterface).
// Calls go through the real ddraw.h C++ interfaces, so this also validates the
// hand-written vtable layouts. Usage: selftest.exe <path to ddraw.dll>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (c) g_pass++;                                                         \
        else { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); }           \
    } while (0)

static const GUID kIID_DD2 = {0xB3A6F3E0, 0x2B43, 0x11CF, {0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56}};
static const GUID kIID_DD7 = {0x15e65ec0, 0x3b9c, 0x11d2, {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}};
static const GUID kIID_Surf7 = {0x06675a80, 0x3b9b, 0x11d2, {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}};

static IDirectDrawSurface *make_surface(IDirectDraw *dd, int w, int h, int bpp)
{
    DDSURFACEDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    d.dwWidth = w;
    d.dwHeight = h;
    d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    d.ddpfPixelFormat.dwFlags = DDPF_RGB | (bpp == 8 ? DDPF_PALETTEINDEXED8 : 0);
    d.ddpfPixelFormat.dwRGBBitCount = bpp;
    IDirectDrawSurface *s = NULL;
    HRESULT hr = dd->CreateSurface(&d, &s, NULL);
    CHECK(hr == DD_OK && s);
    return s;
}

static BYTE *lock(IDirectDrawSurface *s, LONG *pitch)
{
    DDSURFACEDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    if (s->Lock(NULL, &d, DDLOCK_WAIT, NULL) != DD_OK)
        return NULL;
    *pitch = d.lPitch;
    return (BYTE *)d.lpSurface;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "ddraw.dll";
    HMODULE m = LoadLibraryA(path);
    if (!m) { printf("cannot load %s\n", path); return 2; }
    typedef HRESULT(WINAPI * PFN_Create)(GUID *, LPDIRECTDRAW *, IUnknown *);
    typedef HRESULT(WINAPI * PFN_CreateEx)(GUID *, LPVOID *, REFIID, IUnknown *);
    PFN_Create create = (PFN_Create)GetProcAddress(m, "DirectDrawCreate");
    PFN_CreateEx createEx = (PFN_CreateEx)GetProcAddress(m, "DirectDrawCreateEx");
    CHECK(create && createEx);
    if (!create) return 2;

    HWND wnd = CreateWindowA("STATIC", "qdd selftest", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, NULL, NULL, NULL, NULL);
    IDirectDraw *dd = NULL;
    CHECK(create(NULL, &dd, NULL) == DD_OK && dd);
    CHECK(dd->SetCooperativeLevel(wnd, DDSCL_NORMAL) == DD_OK);

    // --- QueryInterface across versions ---
    IDirectDraw2 *dd2 = NULL;
    CHECK(dd->QueryInterface(kIID_DD2, (void **)&dd2) == S_OK && dd2);
    DDSURFACEDESC mode;
    memset(&mode, 0, sizeof(mode));
    mode.dwSize = sizeof(mode);
    CHECK(dd2->GetDisplayMode(&mode) == DD_OK && mode.ddpfPixelFormat.dwRGBBitCount == 16);
    DWORD total = 0, freeMem = 0;
    DDSCAPS caps = {DDSCAPS_VIDEOMEMORY};
    CHECK(dd2->GetAvailableVidMem(&caps, &total, &freeMem) == DD_OK && total > 0);
    dd2->Release();
    IDirectDraw7 *dd7 = NULL;
    CHECK(createEx(NULL, (void **)&dd7, kIID_DD7, NULL) == DD_OK && dd7);
    DDSURFACEDESC2 mode2;
    memset(&mode2, 0, sizeof(mode2));
    mode2.dwSize = sizeof(mode2);
    CHECK(dd7->GetDisplayMode(&mode2) == DD_OK && mode2.dwWidth > 0);
    dd7->Release();

    // --- 16-bit: colour fill, overlapping blt, source colour key, stretch ---
    IDirectDrawSurface *a = make_surface(dd, 64, 64, 16);
    IDirectDrawSurface *b = make_surface(dd, 64, 64, 16);
    DDBLTFX fx;
    memset(&fx, 0, sizeof(fx));
    fx.dwSize = sizeof(fx);
    fx.dwFillColor = 0x1234;
    CHECK(a->Blt(NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx) == DD_OK);
    LONG pitch;
    BYTE *p = lock(a, &pitch);
    CHECK(p && ((WORD *)p)[0] == 0x1234 && ((WORD *)(p + 63 * pitch))[63] == 0x1234);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++)
            ((WORD *)(p + y * pitch))[x] = (WORD)(y * 64 + x);
    a->Unlock(NULL);
    // overlapping move down/right by (3,5) within the same surface
    RECT src = {0, 0, 40, 40}, dst = {3, 5, 43, 45};
    CHECK(a->Blt(&dst, a, &src, DDBLT_WAIT, NULL) == DD_OK);
    p = lock(a, &pitch);
    bool ok = true;
    for (int y = 0; y < 40 && ok; y++)
        for (int x = 0; x < 40 && ok; x++)
            ok = ((WORD *)(p + (y + 5) * pitch))[x + 3] == (WORD)(y * 64 + x);
    CHECK(ok);
    a->Unlock(NULL);
    // colour-keyed BltFast: key 0x0000 in source must leave destination untouched
    fx.dwFillColor = 0x7777;
    b->Blt(NULL, NULL, NULL, DDBLT_COLORFILL, &fx);
    DDCOLORKEY key = {0x0000, 0x0000};
    CHECK(a->SetColorKey(DDCKEY_SRCBLT, &key) == DD_OK);
    p = lock(a, &pitch);
    ((WORD *)p)[0] = 0x0000;
    ((WORD *)p)[1] = 0xABCD;
    a->Unlock(NULL);
    RECT r2 = {0, 0, 2, 1};
    CHECK(b->BltFast(10, 10, a, &r2, DDBLTFAST_SRCCOLORKEY | DDBLTFAST_WAIT) == DD_OK);
    p = lock(b, &pitch);
    CHECK(((WORD *)(p + 10 * pitch))[10] == 0x7777 && ((WORD *)(p + 10 * pitch))[11] == 0xABCD);
    b->Unlock(NULL);
    DDCOLORKEY got = {0, 0};
    CHECK(a->GetColorKey(DDCKEY_SRCBLT, &got) == DD_OK && got.dwColorSpaceLowValue == 0);
    // destination colour key: only pixels equal to 0x7777 get written
    p = lock(b, &pitch);
    ((WORD *)(p + 20 * pitch))[20] = 0x1111;
    b->Unlock(NULL);
    DDCOLORKEY dkey = {0x7777, 0x7777};
    CHECK(b->SetColorKey(DDCKEY_DESTBLT, &dkey) == DD_OK);
    fx.dwFillColor = 0x2222;
    IDirectDrawSurface *c = make_surface(dd, 4, 4, 16);
    c->Blt(NULL, NULL, NULL, DDBLT_COLORFILL, &fx);
    RECT d3 = {19, 20, 23, 24};
    CHECK(b->Blt(&d3, c, NULL, DDBLT_KEYDEST, NULL) == DD_OK);
    p = lock(b, &pitch);
    CHECK(((WORD *)(p + 20 * pitch))[20] == 0x1111 && ((WORD *)(p + 20 * pitch))[19] == 0x2222);
    b->Unlock(NULL);
    // stretch 4x4 -> 8x8 (nearest)
    p = lock(c, &pitch);
    ((WORD *)p)[3] = 0x0F0F;
    c->Unlock(NULL);
    RECT d4 = {0, 0, 8, 8};
    CHECK(b->Blt(&d4, c, NULL, DDBLT_WAIT, NULL) == DD_OK);
    p = lock(b, &pitch);
    CHECK(((WORD *)p)[6] == 0x0F0F && ((WORD *)p)[7] == 0x0F0F && ((WORD *)(p + pitch))[7] == 0x0F0F &&
          ((WORD *)p)[5] == 0x2222);
    b->Unlock(NULL);
    // Q150: near-1:1 stretches like QEC's 2560 <-> 2543 (faststretch: column table + memcpy runs), clipped at the
    // destination edge, with a source colour key, and inside one surface - every pixel against the formula
    // ((x - dst.left) * srcW) / dstW of the per-pixel version
    {
        IDirectDrawSurface *ss = make_surface(dd, 300, 40, 16), *sd = make_surface(dd, 320, 48, 16);
        LONG sp = 0, dp = 0;
        BYTE *sb = lock(ss, &sp);
        if (sb) {
            for (int y = 0; y < 40; y++)
                for (int x = 0; x < 300; x++) ((WORD *)(sb + y * sp))[x] = (WORD)(x * 7 + y * 977 + 1);
            ss->Unlock(NULL);
        }
        static WORD before[48][320];
        struct Case { RECT d; bool key; } cs[4] = {{{3, 2, 286, 42}, false}, {{0, 0, 317, 47}, false},
                                                   {{40, 5, 360, 45}, false}, {{1, 1, 284, 39}, true}};
        for (int k = 0; k < 4; k++) {
            BYTE *db = lock(sd, &dp);
            if (db) {
                for (int y = 0; y < 48; y++)
                    for (int x = 0; x < 320; x++) before[y][x] = ((WORD *)(db + y * dp))[x] = (WORD)(0xF000 + x + y);
                sd->Unlock(NULL);
            }
            DDBLTFX kfx;
            memset(&kfx, 0, sizeof(kfx));
            kfx.dwSize = sizeof(kfx);
            WORD keyv = (WORD)(5 * 7 + 3 * 977 + 1);   // the source pixel (5, 3)
            kfx.ddckSrcColorkey.dwColorSpaceLowValue = kfx.ddckSrcColorkey.dwColorSpaceHighValue = keyv;
            RECT dr = cs[k].d;
            CHECK(sd->Blt(&dr, ss, NULL, DDBLT_WAIT | (cs[k].key ? DDBLT_KEYSRCOVERRIDE : 0), cs[k].key ? &kfx : NULL) == DD_OK);
            db = lock(sd, &dp);
            bool same = db != NULL;
            int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
            for (int y = 0; y < 48 && same; y++)
                for (int x = 0; x < 320 && same; x++) {
                    WORD want = before[y][x];
                    if (x >= dr.left && x < dr.right && y >= dr.top && y < dr.bottom) {
                        int sx = (int)(((long long)(x - dr.left) * 300) / dw), sy = (int)(((long long)(y - dr.top) * 40) / dh);
                        WORD v = (WORD)(sx * 7 + sy * 977 + 1);
                        if (!(cs[k].key && v == keyv)) want = v;
                    }
                    if (((WORD *)(db + y * dp))[x] != want) same = false;
                }
            if (db) sd->Unlock(NULL);
            CHECK(same);
        }
        // inside one surface: 300 -> 283 columns of the same rows (source snapshot first)
        RECT srcR = {0, 0, 300, 20}, dstR = {0, 20, 283, 40};
        BYTE *sb2 = lock(ss, &sp);
        static WORD orig[40][300];
        if (sb2) {
            for (int y = 0; y < 40; y++) memcpy(orig[y], sb2 + y * sp, 600);
            ss->Unlock(NULL);
        }
        CHECK(ss->Blt(&dstR, ss, &srcR, DDBLT_WAIT, NULL) == DD_OK);
        sb2 = lock(ss, &sp);
        bool ok2 = sb2 != NULL;
        for (int y = 20; y < 40 && ok2; y++)
            for (int x = 0; x < 300 && ok2; x++) {
                WORD want = x < 283 ? orig[(y - 20) * 20 / 20][(int)(((long long)x * 300) / 283)] : orig[y][x];
                if (((WORD *)(sb2 + y * sp))[x] != want) ok2 = false;
            }
        if (sb2) ss->Unlock(NULL);
        CHECK(ok2);
        ss->Release();
        sd->Release();
    }
    // clipping: destination partly outside must not crash and must clip
    RECT d5 = {-10, -10, 30, 30};
    CHECK(b->Blt(&d5, a, &src, DDBLT_WAIT, NULL) == DD_OK);
    RECT d6 = {50, 50, 90, 90};
    CHECK(b->Blt(&d6, a, &src, DDBLT_WAIT, NULL) == DD_OK);

    // --- GetDC/ReleaseDC writes through to the pixels ---
    HDC dc = NULL;
    CHECK(b->GetDC(&dc) == DD_OK && dc);
    RECT fr = {0, 0, 64, 64};
    HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
    FillRect(dc, &fr, white);
    CHECK(b->ReleaseDC(dc) == DD_OK);
    p = lock(b, &pitch);
    CHECK(((WORD *)(p + 32 * pitch))[32] == 0xFFFF);
    b->Unlock(NULL);

    // --- 8-bit surface + palette ---
    PALETTEENTRY pe[256];
    for (int i = 0; i < 256; i++) {
        pe[i].peRed = (BYTE)i;
        pe[i].peGreen = (BYTE)(255 - i);
        pe[i].peBlue = 0;
        pe[i].peFlags = 0;
    }
    IDirectDrawPalette *pal = NULL;
    CHECK(dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &pal, NULL) == DD_OK && pal);
    IDirectDrawSurface *s8 = make_surface(dd, 16, 16, 8);
    CHECK(s8->SetPalette(pal) == DD_OK);
    DDPIXELFORMAT pf;
    memset(&pf, 0, sizeof(pf));
    pf.dwSize = sizeof(pf);
    CHECK(s8->GetPixelFormat(&pf) == DD_OK && (pf.dwFlags & DDPF_PALETTEINDEXED8) && pf.dwRGBBitCount == 8);
    PALETTEENTRY back[4];
    CHECK(pal->GetEntries(0, 10, 4, back) == DD_OK && back[0].peRed == 10 && back[3].peGreen == 255 - 13);
    PALETTEENTRY one = {1, 2, 3, 0};
    CHECK(pal->SetEntries(0, 5, 1, &one) == DD_OK);
    CHECK(pal->GetEntries(0, 5, 1, back) == DD_OK && back[0].peBlue == 3);
    fx.dwFillColor = 200;
    CHECK(s8->Blt(NULL, NULL, NULL, DDBLT_COLORFILL, &fx) == DD_OK);
    p = lock(s8, &pitch);
    CHECK(p && p[0] == 200 && p[15 * pitch + 15] == 200);
    s8->Unlock(NULL);
    IDirectDrawPalette *pal2 = NULL;
    CHECK(s8->GetPalette(&pal2) == DD_OK && pal2 == pal);
    if (pal2) pal2->Release();

    // --- primary + flip chain ---
    DDSURFACEDESC pd;
    memset(&pd, 0, sizeof(pd));
    pd.dwSize = sizeof(pd);
    pd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    pd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    pd.dwBackBufferCount = 1;
    IDirectDrawSurface *prim = NULL;
    CHECK(dd->CreateSurface(&pd, &prim, NULL) == DD_OK && prim);
    DDSCAPS bc = {DDSCAPS_BACKBUFFER};
    IDirectDrawSurface *bb = NULL;
    CHECK(prim->GetAttachedSurface(&bc, &bb) == DD_OK && bb);
    fx.dwFillColor = 0x00F0;
    bb->Blt(NULL, NULL, NULL, DDBLT_COLORFILL, &fx);
    fx.dwFillColor = 0x0F00;
    prim->Blt(NULL, NULL, NULL, DDBLT_COLORFILL, &fx);
    CHECK(prim->Flip(NULL, DDFLIP_WAIT) == DD_OK);
    p = lock(prim, &pitch);
    CHECK(p && ((WORD *)p)[0] == 0x00F0);
    prim->Unlock(NULL);
    // flipclear (second run of build.ps1 -Test, QDD_FLIPCLEAR=1 and flipclear=true in the ini): the new back
    // buffer starts black after Flip, Restore clears; otherwise both keep their pixels
    bool flipClear = GetEnvironmentVariableA("QDD_FLIPCLEAR", NULL, 0) > 0;
    p = lock(bb, &pitch);
    CHECK(p && ((WORD *)p)[0] == (flipClear ? 0x0000 : 0x0F00));
    bb->Unlock(NULL);
    CHECK(prim->IsLost() == DD_OK);
    CHECK(prim->Restore() == DD_OK);
    p = lock(prim, &pitch);
    CHECK(p && ((WORD *)p)[0] == (flipClear ? 0x0000 : 0x00F0));
    prim->Unlock(NULL);
    IDirectDrawSurface7 *prim7 = NULL;
    CHECK(prim->QueryInterface(kIID_Surf7, (void **)&prim7) == S_OK && prim7);
    if (prim7) prim7->Release();
    DDSURFACEDESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    CHECK(prim->GetSurfaceDesc(&sd) == DD_OK && (sd.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE));

    // --- Q149 writewatch: a primary without a flip chain lives in write-watched memory (no DIB section) ---
    // Lock / write after Unlock through the kept pointer (as QEC does) / GetDC drawing into a copy that is copied
    // in at GetDC and back at ReleaseDC; the render thread lists the written pages (checked in ddraw.log below)
    {
        DDSURFACEDESC wd;
        memset(&wd, 0, sizeof(wd));
        wd.dwSize = sizeof(wd);
        wd.dwFlags = DDSD_CAPS;
        wd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
        IDirectDrawSurface *wp = NULL;
        CHECK(dd->CreateSurface(&wd, &wp, NULL) == DD_OK && wp);
        if (wp) {
            LONG wpitch = 0;
            BYTE *w0 = lock(wp, &wpitch);
            CHECK(w0 && wpitch > 0 && ((WORD *)w0)[0] == 0);   // starts black
            if (w0) {
                ((WORD *)w0)[0] = 0x1234;
                wp->Unlock(NULL);
                ((WORD *)(w0 + wpitch * 3))[2] = 0x4321;   // after Unlock, through the old pointer
                if (GetEnvironmentVariableA("QDD_GUARD", NULL, 0) > 0)
                    CHECK(w0[-1] == 0xFD && w0[-wpitch] == 0xFD);   // guard row in front of the write-watched rows
            }
            Sleep(150);   // a few render ticks see the written pages
            HDC wdc = NULL;
            CHECK(wp->GetDC(&wdc) == DD_OK && wdc);
            if (wdc) {
                COLORREF c0 = GetPixel(wdc, 0, 0);   // 0x1234 copied in: r 2, g 17, b 20 (5/6/5 bits)
                CHECK(c0 != CLR_INVALID && c0 != 0);
                SetPixelV(wdc, 1, 0, RGB(255, 255, 255));
                wp->ReleaseDC(wdc);
            }
            BYTE *w1 = lock(wp, &wpitch);
            CHECK(w1 && ((WORD *)w1)[0] == 0x1234 && ((WORD *)w1)[1] == 0xFFFF && ((WORD *)(w1 + wpitch * 3))[2] == 0x4321);
            if (w1) wp->Unlock(NULL);
            fx.dwFillColor = 0x07E0;
            CHECK(wp->Blt(NULL, NULL, NULL, DDBLT_COLORFILL, &fx) == DD_OK);
            w1 = lock(wp, &wpitch);
            CHECK(w1 && ((WORD *)w1)[7] == 0x07E0);
            if (w1) wp->Unlock(NULL);
            Sleep(100);
            wp->Release();
        }
    }

    // --- clipper ---
    IDirectDrawClipper *clip = NULL;
    CHECK(dd->CreateClipper(0, &clip, NULL) == DD_OK && clip);
    CHECK(clip->SetHWnd(0, wnd) == DD_OK);
    DWORD sz = 0;
    CHECK(clip->GetClipList(NULL, NULL, &sz) == DD_OK && sz > 0);
    CHECK(prim->SetClipper(clip) == DD_OK);

    // --- caps / modes ---
    DDCAPS hw;
    memset(&hw, 0, sizeof(hw));
    hw.dwSize = sizeof(hw);
    CHECK(dd->GetCaps(&hw, NULL) == DD_OK && (hw.dwCaps & DDCAPS_BLT));
    struct Ctx { int n; } ctx = {0};
    CHECK(dd->EnumDisplayModes(0, NULL, &ctx, [](LPDDSURFACEDESC, LPVOID c) -> HRESULT {
        ((Ctx *)c)->n++;
        return DDENUMRET_OK;
    }) == DD_OK && ctx.n > 0);

    // EnumDisplayModes reports the real modes of the monitor: every size / refresh rate must be one
    // EnumDisplaySettings offers, the desktop's current mode must be there, each in 8, 16 and 32 bpp;
    // without DDEDM_REFRESHRATES every size once with refresh rate 0. The third run of build.ps1 -Test
    // (QDD_GUARD=1) has resolutions=2560x1600,1920x1080,1280x1024,640x480 and max_resolutions=2 in the ini.
    {
        struct Mode { DWORD w, h, hz, bpp; };
        struct List { Mode m[3000]; int n; };
        static List rates, plain;
        auto collect = [](LPDDSURFACEDESC d, LPVOID c) -> HRESULT {
            List *l = (List *)c;
            if (l->n < 3000) {
                Mode &m = l->m[l->n++];
                m.w = d->dwWidth; m.h = d->dwHeight; m.hz = d->dwRefreshRate; m.bpp = d->ddpfPixelFormat.dwRGBBitCount;
            }
            return DDENUMRET_OK;
        };
        CHECK(dd->EnumDisplayModes(DDEDM_REFRESHRATES, NULL, &rates, collect) == DD_OK && rates.n > 0);
        CHECK(dd->EnumDisplayModes(0, NULL, &plain, collect) == DD_OK && plain.n > 0);
        DEVMODEA cur;
        memset(&cur, 0, sizeof(cur));
        cur.dmSize = sizeof(cur);
        EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &cur);
        bool allReal = true, curFound = false, zeroRates = true, plainUnique = true;
        int per[3] = {0, 0, 0};
        for (int i = 0; i < rates.n; i++) {
            const Mode &md = rates.m[i];
            bool real = false;
            DEVMODEA dm;
            memset(&dm, 0, sizeof(dm));
            dm.dmSize = sizeof(dm);
            for (DWORD k = 0; !real && EnumDisplaySettingsA(NULL, k, &dm); k++)
                real = dm.dmPelsWidth == md.w && dm.dmPelsHeight == md.h && (dm.dmDisplayFrequency == md.hz || (md.hz == 0 && dm.dmDisplayFrequency <= 1));
            if (!real) { allReal = false; printf("  not a real mode: %lux%lu@%lu\n", md.w, md.h, md.hz); }
            if (md.w == cur.dmPelsWidth && md.h == cur.dmPelsHeight && md.hz == cur.dmDisplayFrequency) curFound = true;
            per[md.bpp == 8 ? 0 : md.bpp == 16 ? 1 : 2]++;
        }
        for (int i = 0; i < plain.n; i++) {
            if (plain.m[i].hz != 0) zeroRates = false;
            for (int j = 0; j < i; j++)
                if (plain.m[j].w == plain.m[i].w && plain.m[j].h == plain.m[i].h && plain.m[j].bpp == plain.m[i].bpp)
                    plainUnique = false;
        }
        printf("  EnumDisplayModes: %d modes with refresh rates (%d per bpp), %d sizes without\n", rates.n, per[2],
               plain.n / 3);
        CHECK(allReal);
        CHECK(per[0] == per[1] && per[1] == per[2] && per[0] * 3 == rates.n);
        CHECK(zeroRates && plainUnique && plain.n <= rates.n);
        bool limited = GetEnvironmentVariableA("QDD_GUARD", NULL, 0) > 0;
        if (limited) {
            // the 2 largest of the listed sizes the monitor really has
            static const DWORD listed[4][2] = {{2560, 1600}, {1920, 1080}, {1280, 1024}, {640, 480}};
            int expect = 0;
            for (int s = 0; s < 4; s++) {
                DEVMODEA dm;
                memset(&dm, 0, sizeof(dm));
                dm.dmSize = sizeof(dm);
                bool has = false;
                for (DWORD k = 0; !has && EnumDisplaySettingsA(NULL, k, &dm); k++)
                    has = dm.dmPelsWidth == listed[s][0] && dm.dmPelsHeight == listed[s][1];
                if (has) expect++;
            }
            if (expect > 2) expect = 2;
            bool onlyListed = true;
            for (int i = 0; i < plain.n; i++) {
                bool l = false;
                for (int s = 0; s < 4; s++) l = l || (plain.m[i].w == listed[s][0] && plain.m[i].h == listed[s][1]);
                onlyListed = onlyListed && l;
            }
            printf("  resolutions / max_resolutions: %d sizes reported, %d expected\n", plain.n / 3, expect);
            CHECK(onlyListed && plain.n == expect * 3);
            if (plain.n >= 6) CHECK(plain.m[0].w * plain.m[0].h < plain.m[1].w * plain.m[1].h);   // ascending
        } else {
            CHECK(curFound);
        }
    }

    // guard_lines (third run): a write one row past the end / one byte before the start lands in a guard
    // row, the next Lock reports and repairs it; GetDC drawing stays inside the surface
    if (GetEnvironmentVariableA("QDD_GUARD", NULL, 0) > 0) {
        IDirectDrawSurface *g = make_surface(dd, 64, 32, 16);
        LONG gp = 0;
        BYTE *q = lock(g, &gp);
        CHECK(q && gp == 128);
        if (q) {
            CHECK(q[-1] == 0xFD && q[gp * 32] == 0xFD && q[gp * 32 + gp - 1] == 0xFD);   // the guard rows
            memset(q + gp * 32, 0x55, 6);   // the bug: 3 pixels into row 32 of a 32-row surface
            q[-1] = 0x66;                   // and one byte before row 0
            g->Unlock(NULL);
            q = lock(g, &gp);
            CHECK(q && q[gp * 32] == 0xFD && q[gp * 32 + 5] == 0xFD && q[-1] == 0xFD);   // repaired
            g->Unlock(NULL);
        }
        HDC gdc = NULL;
        CHECK(g->GetDC(&gdc) == DD_OK && gdc);
        if (gdc) {
            SetPixelV(gdc, 0, 0, RGB(255, 255, 255));    // the surface's first pixel
            SetPixelV(gdc, 0, 32, RGB(255, 255, 255));   // just below it: clipped away
            SetPixelV(gdc, 5, -1, RGB(255, 255, 255));   // just above: clipped away
            g->ReleaseDC(gdc);
        }
        q = lock(g, &gp);
        CHECK(q && ((WORD *)q)[0] == 0xFFFF && q[gp * 32] == 0xFD && q[-gp + 10] == 0xFD);
        g->Unlock(NULL);
        g->Release();
    }

    bb->Release();
    clip->Release();
    prim->Release();
    s8->Release();
    pal->Release();
    a->Release();
    b->Release();
    c->Release();
    dd->Release();
    DestroyWindow(wnd);
    if (GetEnvironmentVariableA("QDD_GUARD", NULL, 0) > 0) {
        // the alarm lines are in ddraw.log (written when the last DirectDraw object went away)
        char logPath[MAX_PATH];
        GetModuleFileNameA(m, logPath, MAX_PATH);
        char *sl = strrchr(logPath, '\\');
        if (sl) strcpy_s(sl + 1, MAX_PATH - (sl + 1 - logPath), "ddraw.log");
        static char text[1 << 20];
        FILE *f = NULL;
        size_t logLen = 0;
        if (fopen_s(&f, logPath, "rb") == 0 && f) {
            logLen = fread(text, 1, sizeof(text) - 1, f);
            fclose(f);
        }
        text[logLen] = 0;
        int alarms = 0;
        for (const char *t = text; (t = strstr(t, "guard_lines ALARM")) != NULL; t++) alarms++;
        printf("  ddraw.log: %d guard_lines alarm line(s)\n", alarms);
        CHECK(alarms == 2 && strstr(text, "AFTER the last row") && strstr(text, "BEFORE the first row"));
        // Q149: the chainless primary was write-watched, GetDC went through the copy, GetWriteWatch worked
        CHECK(strstr(text, "pixels in write-watched memory") && strstr(text, "GDI draws into a copy") &&
              !strstr(text, "GetWriteWatch failed") && !strstr(text, "VirtualAlloc(MEM_WRITE_WATCH") &&
              !strstr(text, "writewatch: self-check failed"));
    }
    printf("selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
