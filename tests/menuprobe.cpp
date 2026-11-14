// Q143 remove_menu probe: a window with a menu bar goes through DirectDrawCreate + SetCooperativeLevel(NORMAL)
// of ddraw.dll (argv[1]); the window is captured before and after (argv[2]_before.bmp / _after.bmp).
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>

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

static void pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    while (GetTickCount() < end) {
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        Sleep(10);
    }
}

static void report(HWND h, const char *when)
{
    RECT w, c;
    GetWindowRect(h, &w);
    GetClientRect(h, &c);
    printf("%s: menu %p, window %ldx%ld, client %ldx%ld\n", when, (void *)GetMenu(h), w.right - w.left, w.bottom - w.top,
           c.right, c.bottom);
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    SetProcessDPIAware();
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hbrBackground = (HBRUSH)GetStockObject(GRAY_BRUSH);
    wc.lpszClassName = "QddMenuProbe";
    RegisterClassA(&wc);
    HMENU bar = CreateMenu(), file = CreatePopupMenu();
    AppendMenuA(file, MF_STRING, 1, "Exit");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)file, "File");
    AppendMenuA(bar, MF_STRING, 2, "Options");
    AppendMenuA(bar, MF_STRING, 3, "Help");
    RECT r = {0, 0, 640, 480};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);
    HWND h = CreateWindowA("QddMenuProbe", "remove_menu probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                           r.right - r.left, r.bottom - r.top, NULL, bar, wc.hInstance, NULL);
    pump(500);
    report(h, "before");
    char path[MAX_PATH];
    sprintf_s(path, "%s_before.bmp", argv[2]);
    save_bmp(h, path);

    HMODULE dd = LoadLibraryA(argv[1]);
    typedef HRESULT(WINAPI * PFN_Create)(GUID *, LPDIRECTDRAW *, IUnknown *);
    PFN_Create create = dd ? (PFN_Create)GetProcAddress(dd, "DirectDrawCreate") : NULL;
    LPDIRECTDRAW ddraw = NULL;
    if (!create || FAILED(create(NULL, &ddraw, NULL))) {
        printf("DirectDrawCreate failed\n");
        return 3;
    }
    ddraw->SetCooperativeLevel(h, DDSCL_NORMAL);
    pump(500);
    report(h, "after SetCooperativeLevel");
    sprintf_s(path, "%s_after.bmp", argv[2]);
    save_bmp(h, path);
    ddraw->Release();
    DestroyWindow(h);
    return 0;
}
