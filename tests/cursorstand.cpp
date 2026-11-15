// Q178 stand-in for a game menu: DirectDrawCreate + SetCooperativeLevel(NORMAL) of ddraw.dll (argv[1], its ddraw.ini
// next to it), then a whole-primary Blt 3 times a second for argv[2] seconds (a loading screen / menu that copies the
// full screen now and then; argv[3] = the interval, 333 ms by default) - and never a ShowCursor call of its own (its count stays 0: it wants the system pointer).
// The pointer is put over the window. Pair with cursortest=-N (a forced hide every second) and cursorprobe=true; the
// verdict is in ddraw.log ("cursor:" lines) and the CURSOR_SHOWING samples printed here.
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <stdlib.h>

static void pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    do {
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        Sleep(5);
    } while ((LONG)(GetTickCount() - end) < 0);
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    int seconds = atoi(argv[2]), interval = argc > 3 ? atoi(argv[3]) : 333;
    if (interval < 10) interval = 10;
    int perSecond = 1000 / interval;
    SetProcessDPIAware();
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "QddCursorStand";
    RegisterClassA(&wc);
    HWND h = CreateWindowA("QddCursorStand", "Q178 cursor stand-in", WS_POPUP | WS_VISIBLE, 200, 150, 800, 600, NULL,
                           NULL, wc.hInstance, NULL);
    SetForegroundWindow(h);
    SetCursorPos(600, 450);

    HMODULE dd = LoadLibraryA(argv[1]);
    typedef HRESULT(WINAPI * PFN_Create)(GUID *, LPDIRECTDRAW *, IUnknown *);
    PFN_Create create = dd ? (PFN_Create)GetProcAddress(dd, "DirectDrawCreate") : NULL;
    LPDIRECTDRAW ddraw = NULL;
    if (!create || FAILED(create(NULL, &ddraw, NULL))) {
        printf("DirectDrawCreate failed\n");
        return 3;
    }
    ddraw->SetCooperativeLevel(h, DDSCL_NORMAL);
    DDSURFACEDESC sd = {};
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    LPDIRECTDRAWSURFACE primary = NULL, back = NULL;
    if (FAILED(ddraw->CreateSurface(&sd, &primary, NULL))) {
        printf("primary failed\n");
        return 4;
    }
    DDSURFACEDESC pd = {};
    pd.dwSize = sizeof(pd);
    primary->GetSurfaceDesc(&pd);
    DDSURFACEDESC bd = {};
    bd.dwSize = sizeof(bd);
    bd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    bd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    bd.dwWidth = pd.dwWidth;
    bd.dwHeight = pd.dwHeight;
    if (FAILED(ddraw->CreateSurface(&bd, &back, NULL))) {
        printf("offscreen failed\n");
        return 5;
    }
    printf("primary %lux%lu\n", pd.dwWidth, pd.dwHeight);

    int showing = 0, samples = 0, copies = 0;
    for (int t = 0; t < seconds * perSecond; t++) {
        DDBLTFX fx = {};
        fx.dwSize = sizeof(fx);
        fx.dwFillColor = (DWORD)(t * 0x1234567);
        back->Blt(NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        primary->Blt(NULL, back, NULL, DDBLT_WAIT, NULL);   // the whole screen: a "frame" to the renderer
        copies++;
        pump((DWORD)interval);
        CURSORINFO ci;
        ci.cbSize = sizeof(ci);
        if (t % perSecond == perSecond - 1 && GetCursorInfo(&ci)) {
            samples++;
            if (ci.flags & CURSOR_SHOWING) showing++;
            printf("%2d s: system pointer %s\n", t / perSecond + 1, (ci.flags & CURSOR_SHOWING) ? "showing" : "HIDDEN");
        }
    }
    printf("%d full copies; system pointer showing in %d of %d samples\n", copies, showing, samples);
    back->Release();
    primary->Release();
    ddraw->Release();
    DestroyWindow(h);
    return 0;
}
