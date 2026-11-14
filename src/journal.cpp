// Q157: activity journal (see journal.h)
#include "qdd.h"
#include "journal.h"
#include <stdio.h>
#include <string.h>

struct JEntry {
    LONGLONG t;
    DWORD tid;
    LONG kind;
    ULONG_PTR a, b, c;
};
static const LONG J_CAP = 8192;   // power of two
static JEntry s_ring[J_CAP];
static volatile LONG s_next;
static volatile LONG s_counts[JC_COUNT];

void jr(int kind, ULONG_PTR a, ULONG_PTR b, ULONG_PTR c)
{
    LONG i = InterlockedIncrement(&s_next) - 1;
    JEntry &e = s_ring[i & (J_CAP - 1)];
    e.kind = -1;   // being written
    e.t = perf_now();
    e.tid = GetCurrentThreadId();
    e.a = a;
    e.b = b;
    e.c = c;
    e.kind = kind;
}

void jr_count(int counter)
{
    InterlockedIncrement(&s_counts[counter]);
}

void jr_tick()
{
    static LONGLONG last;
    LONGLONG now = perf_now();
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    if (now - last < f.QuadPart / 10)
        return;
    last = now;
    LONG l = InterlockedExchange(&s_counts[JC_LOCK], 0), b = InterlockedExchange(&s_counts[JC_BLT], 0),
         c = InterlockedExchange(&s_counts[JC_CURSOR], 0);
    if (l || b || c)
        jr(J_COUNTS, l, b, c);
}

static const char *msg_name(UINT m)
{
    switch (m) {
    case 0x0000: return "WM_NULL";
    case 0x0002: return "WM_DESTROY";
    case 0x0003: return "WM_MOVE";
    case 0x0005: return "WM_SIZE";
    case 0x0006: return "WM_ACTIVATE";
    case 0x0007: return "WM_SETFOCUS";
    case 0x0008: return "WM_KILLFOCUS";
    case 0x000B: return "WM_SETREDRAW";
    case 0x000D: return "WM_GETTEXT";
    case 0x000F: return "WM_PAINT";
    case 0x0010: return "WM_CLOSE";
    case 0x0014: return "WM_ERASEBKGND";
    case 0x0018: return "WM_SHOWWINDOW";
    case 0x001C: return "WM_ACTIVATEAPP";
    case 0x0020: return "WM_SETCURSOR";
    case 0x0021: return "WM_MOUSEACTIVATE";
    case 0x0046: return "WM_WINDOWPOSCHANGING";
    case 0x0047: return "WM_WINDOWPOSCHANGED";
    case 0x0084: return "WM_NCHITTEST";
    case 0x0086: return "WM_NCACTIVATE";
    case 0x0100: return "WM_KEYDOWN";
    case 0x0101: return "WM_KEYUP";
    case 0x0102: return "WM_CHAR";
    case 0x0104: return "WM_SYSKEYDOWN";
    case 0x0105: return "WM_SYSKEYUP";
    case 0x0110: return "WM_INITDIALOG";
    case 0x0111: return "WM_COMMAND";
    case 0x0112: return "WM_SYSCOMMAND";
    case 0x0113: return "WM_TIMER";
    case 0x0133: return "WM_CTLCOLOREDIT";
    case 0x0136: return "WM_CTLCOLORDLG";
    case 0x0138: return "WM_CTLCOLORSTATIC";
    case 0x0200: return "WM_MOUSEMOVE";
    case 0x0201: return "WM_LBUTTONDOWN";
    case 0x0202: return "WM_LBUTTONUP";
    case 0x0204: return "WM_RBUTTONDOWN";
    case 0x0205: return "WM_RBUTTONUP";
    case 0x0215: return "WM_CAPTURECHANGED";
    case 0x0281: return "WM_IME_SETCONTEXT";
    case 0x0282: return "WM_IME_NOTIFY";
    case 0x02A3: return "WM_MOUSELEAVE";
    default: return NULL;
    }
}

static const char *const s_kind[J_KINDS] = {"api", "create", "free", "subproc", "msghook", "hook", "present",
                                            "childset", "childpaint", "force", "logthread", "counts", "note"};
static const char *const s_api[JA_COUNT] = {"Release", "Flip", "Blt->primary", "GetDC", "ReleaseDC", "SetPalette",
                                            "Restore", "SetCooperativeLevel", "SetDisplayMode", "RestoreDisplayMode",
                                            "SetClipper"};
static const char *const s_hook[JH_COUNT] = {"ClipCursor", "SetCursorPos", "SetWindowPos", "MoveWindow",
                                             "WindowFromPoint", "ShowWindow"};

void journal_dump(const char *why, double seconds)
{
    LONG end = s_next;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    LONGLONG now = perf_now(), from = now - (LONGLONG)(seconds * f.QuadPart);
    char path[MAX_PATH];
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&journal_dump, &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    if (slash) slash[1] = 0;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char name[96];
    _snprintf_s(name, sizeof(name), _TRUNCATE, "ddraw_journal_%04u%02u%02u_%02u%02u%02u_%03u_%s.txt", st.wYear, st.wMonth,
                st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, why);
    strcat_s(path, name);
    FILE *fp = NULL;
    if (fopen_s(&fp, path, "w") || !fp)
        return;
    DWORD game = g_gameThread, render = render_thread_id();
    fprintf(fp, "journal %s: last %.1f s, threads: game %lu, render %lu, this %lu; t = ms before the dump\n", why, seconds,
            game, render, GetCurrentThreadId());
    LONG first = end - J_CAP > 0 ? end - J_CAP : 0;
    for (LONG i = first; i < end; i++) {
        const JEntry &e = s_ring[i & (J_CAP - 1)];
        if (e.kind < 0 || e.kind >= J_KINDS || e.t < from)
            continue;
        double ms = (double)(now - e.t) * 1000.0 / (double)f.QuadPart;
        const char *who = e.tid == game ? "game  " : e.tid == render ? "render" : "other ";
        char d[200];
        switch (e.kind) {
        case J_API:
            _snprintf_s(d, sizeof(d), _TRUNCATE, "%s surface #%Iu %Ix", e.a < JA_COUNT ? s_api[e.a] : "?", e.b, e.c);
            break;
        case J_CREATE:
            _snprintf_s(d, sizeof(d), _TRUNCATE, "surface #%Iu %Iux%Iu caps 0x%Ix", e.a, e.b >> 16, e.b & 0xFFFF, e.c);
            break;
        case J_SUBMSG:
        case J_HOOKMSG: {
            const char *n = msg_name((UINT)e.b);
            if (n) _snprintf_s(d, sizeof(d), _TRUNCATE, "hwnd %Ix %s wParam %Ix", e.a, n, e.c);
            else _snprintf_s(d, sizeof(d), _TRUNCATE, "hwnd %Ix msg 0x%04Ix wParam %Ix", e.a, e.b, e.c);
            break;
        }
        case J_HOOKCALL:
            _snprintf_s(d, sizeof(d), _TRUNCATE, "%s %Ix %Ix", e.a < JH_COUNT ? s_hook[e.a] : "?", e.b, e.c);
            break;
        case J_COUNTS:
            _snprintf_s(d, sizeof(d), _TRUNCATE, "last 100 ms: Lock %Iu, Blt %Iu, cursor/coordinate hooks %Iu", e.a, e.b, e.c);
            break;
        case J_NOTE:
            _snprintf_s(d, sizeof(d), _TRUNCATE, "%s", (const char *)e.a);
            break;
        default:
            _snprintf_s(d, sizeof(d), _TRUNCATE, "%Ix %Ix %Ix", e.a, e.b, e.c);
        }
        fprintf(fp, "%9.1f %s %-10s %s\n", -ms, who, s_kind[e.kind], d);
    }
    fclose(fp);
    log_event("journal: %s -> %s", why, name);
}
