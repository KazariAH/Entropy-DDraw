// Q144 EnumDisplayModes probe: a "video settings" window like a game's, whose resolution list is filled
// from IDirectDraw7::EnumDisplayModes(DDEDM_REFRESHRATES) of ddraw.dll (argv[1]). The window is saved as
// argv[2] (BMP) and the list is printed. QEC never calls EnumDisplayModes, so this stands in for a game's
// settings screen.
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>

static const GUID kIID_DD7 = {0x15e65ec0, 0x3b9c, 0x11d2, {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}};

static void save_bmp(HWND h, const char *path)
{
    RECT r;
    GetWindowRect(h, &r);
    int w = r.right - r.left, hh = r.bottom - r.top;
    HDC sdc = GetDC(NULL), mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = hh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(mdc, bmp);
    PrintWindow(h, mdc, 2 /* PW_RENDERFULLCONTENT */);
    GdiFlush();
    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
    fh.bfSize = fh.bfOffBits + w * hh * 4;
    FILE *f = NULL;
    if (fopen_s(&f, path, "wb") == 0 && f) {
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f);
        fwrite(bits, 4, (size_t)w * hh, f);
        fclose(f);
    }
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(NULL, sdc);
}

struct Ctx { HWND list; int n; };

static HRESULT WINAPI on_mode(LPDDSURFACEDESC2 d, LPVOID p)
{
    Ctx *c = (Ctx *)p;
    char s[64];
    sprintf_s(s, "%lu x %lu   %lu Hz   %lu bpp", d->dwWidth, d->dwHeight, d->dwRefreshRate,
              d->ddpfPixelFormat.dwRGBBitCount);
    if (d->ddpfPixelFormat.dwRGBBitCount == 32)   // the settings screen of a 32-bit game
        SendMessageA(c->list, LB_ADDSTRING, 0, (LPARAM)s);
    printf("%s\n", s);
    c->n++;
    return DDENUMRET_OK;
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    HMODULE m = LoadLibraryA(argv[1]);
    if (!m) { printf("cannot load %s\n", argv[1]); return 2; }
    typedef HRESULT(WINAPI * PFN_CreateEx)(GUID *, LPVOID *, REFIID, IUnknown *);
    PFN_CreateEx createEx = (PFN_CreateEx)GetProcAddress(m, "DirectDrawCreateEx");
    HWND wnd = CreateWindowA("#32770", "Video settings (EnumDisplayModes)", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                             100, 100, 520, 700, NULL, NULL, NULL, NULL);
    HFONT font = CreateFontA(-22, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    HWND label = CreateWindowA("STATIC", "Resolution (32 bpp modes reported by ddraw.dll):", WS_CHILD | WS_VISIBLE, 16, 12, 480, 30,
                               wnd, NULL, NULL, NULL);
    HWND list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL, 16, 48, 470, 580, wnd, NULL,
                                NULL, NULL);
    SendMessageA(label, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(list, WM_SETFONT, (WPARAM)font, TRUE);
    IDirectDraw7 *dd = NULL;
    if (!createEx || createEx(NULL, (void **)&dd, kIID_DD7, NULL) != DD_OK || !dd) { printf("DirectDrawCreateEx failed\n"); return 3; }
    Ctx c = {list, 0};
    HRESULT hr = dd->EnumDisplayModes(DDEDM_REFRESHRATES, NULL, &c, on_mode);
    printf("EnumDisplayModes hr=0x%08lX, %d modes\n", (unsigned long)hr, c.n);
    DWORD end = GetTickCount() + 1500;
    MSG msg;
    while (GetTickCount() < end) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        Sleep(20);
    }
    save_bmp(wnd, argv[2]);
    dd->Release();
    DestroyWindow(wnd);
    return 0;
}
