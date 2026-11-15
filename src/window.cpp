// Window handling: subclassing, windowed emulation of exclusive fullscreen,
// optional real mode switch, DPI awareness and cursor-API remapping.
#include "qdd.h"
#include <tlhelp32.h>
#include <string.h>
#include <stdio.h>
#include <intrin.h>
#include <stdlib.h>

static HWND s_hwnd;
static WNDPROC s_oldProc;
static bool s_unicode;
static bool s_hadClipChildren;   // WS_CLIPCHILDREN removed by window_attach, restored by window_detach
static bool s_gameClipChildren;  // Q170: the game's own window had WS_CLIPCHILDREN (whether it was kept or removed)
static volatile bool s_emu;        // exclusive fullscreen emulated in a window
static volatile bool s_active = true;
static int s_appliedW, s_appliedH;
static bool s_needReapply;

// Presentation of an emulated game screen: a window (framed, or borderless with `border=false`), a
// borderless window covering the monitor, or the monitor really switched to the game's resolution.
enum Presentation { PR_WINDOWED, PR_FULLSCREEN, PR_EXCLUSIVE };
static const char *const s_prName[] = {"windowed", "borderless fullscreen", "exclusive fullscreen"};
static int s_present = PR_WINDOWED;
static bool s_presentInit;          // presentation managed by us (emulation started)
static bool s_switching;            // inside present_set
static bool s_keepWinClient;        // present_set: s_winClient was set by the caller (not taken from the window)
static bool s_applied;              // the window has a presentation of ours (its rectangle may be remembered)
static RECT s_winClient;            // client area (screen coordinates) of the windowed state
static LONG s_framedStyle, s_framedEx;
static char s_modeDevice[32];       // display whose mode we changed
static LONG s_modeSwitches;
static bool s_cursorFree;           // Ctrl+Tab: the user released the cursor clip
static UINT s_hotkeyMsg;            // private message posted by the hotkey filter
static LONG s_childrenVisible;      // fixchilds: visible descendants of the game window (render thread count)
static HHOOK s_msgHook;
static bool s_sizeMove;              // Windows runs the move / size loop
static bool s_adjOn;                 // adjmouse: relative mode running (s_adjX/Y = game position)
static double s_adjX, s_adjY;
static POINT s_adjReal;
static bool s_clipActive;             // apply_clip confined the cursor (locked)
// lockcursor (Q147): mouse buttons the game has seen go down (bit 0 L, 1 R, 2 M), on which window, and the last
// mouse position there (game coordinates, as the game got it) - for the drag clip and for button-ups the game
// would otherwise never see
static volatile LONG s_btnMask;
static HWND s_btnHwnd;
static LPARAM s_btnLparam;
static volatile bool s_dragClip;      // lockcursor=1: a button is held after a press in the picture
static volatile bool s_lockSticky;    // lockcursor=2: locked since a click in the picture (like cnc-ddraw)
static DWORD s_guiThread;             // thread of the game window (its message queue)
static volatile DWORD s_lastPull;     // tick: the game thread last took a message out of its queue

// Q156: a window's text without WM_GETTEXT. GetWindowText sends WM_GETTEXT to a window of this process - from our
// render / log threads that is a cross-thread SendMessage the game thread dispatches wherever it next pumps
// messages (in the middle of loading a save game, say). InternalGetWindowText reads the stored text directly.
static void window_text(HWND h, char *out, int cap)
{
    out[0] = 0;
    wchar_t w[128] = L"";
    if (h && IsWindow(h) && InternalGetWindowText(h, w, 128) > 0)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
}
static volatile DWORD s_lastCall;     // tick: the game thread last called one of our drawing methods
static bool s_altSeen;                // the game got an Alt key-down and no key-up yet
static int s_clicksLogged;            // modifier state logged for the first clicks after a focus change
static volatile LONG s_closing;       // the close button was pressed (game_handles_close=false)

bool emu_active() { return s_emu && s_hwnd && g_dd && g_dd->modeSet; }
// fixchilds + childpaint: the game window keeps WS_CLIPCHILDREN (see window_attach)
static bool keep_clip_children() { return g_cfg.keepClipChildren && g_cfg.fixChilds && g_cfg.childPaint; }
static void clip_on_message(UINT m);
static int repaint_children(HWND top);
static void lockcursor_track(const MSG *m);
static void release_stuck_buttons(const char *why);
static void lockcursor_watch();
// a window of this process (the game's own dialogs, message boxes, ...)
static bool own_window(HWND h)
{
    DWORD pid = 0;
    return h && GetWindowThreadProcessId(h, &pid) && pid == GetCurrentProcessId();
}
static void apply_clip();
static bool game_related(HWND h);
static void present_set(int to, const char *why);
static void remember_windowed();
static void display_restore(DDraw *dd, const char *why);
static LRESULT borderless_hit(LPARAM l);
static void adj_reset();
static bool adj_update(POINT *game);
static void monitor_next(const char *why);
static void monitor_check(const char *why);
static void adjust_for(RECT *r, LONG style, LONG ex);
static bool make_visible(RECT *wr);
static bool borderless_windowed() { return s_presentInit && s_present == PR_WINDOWED && !g_cfg.border; }

// --- coordinate mapping (emulated fullscreen) -----------------------------------------------

static bool game_viewport(RECT *vp, POINT *origin)
{
    DDraw *dd = g_dd;
    if (!dd || !s_hwnd)
        return false;
    RECT cr;
    if (!GetClientRect(s_hwnd, &cr))
        return false;
    compute_viewport(cr.right, cr.bottom, dd->modeW, dd->modeH, vp);
    if (origin) {
        origin->x = origin->y = 0;
        ClientToScreen(s_hwnd, origin);
    }
    return vp->right > vp->left && vp->bottom > vp->top;
}

static void client_to_game(POINT *p, const RECT &vp)
{
    DDraw *dd = g_dd;
    int vw = vp.right - vp.left, vh = vp.bottom - vp.top;
    long x = (long)((long long)(p->x - vp.left) * dd->modeW / vw);
    long y = (long)((long long)(p->y - vp.top) * dd->modeH / vh);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= dd->modeW) x = dd->modeW - 1;
    if (y >= dd->modeH) y = dd->modeH - 1;
    p->x = x;
    p->y = y;
}

void emu_screen_to_game(POINT *p)
{
    RECT vp;
    POINT o;
    if (!game_viewport(&vp, &o))
        return;
    p->x -= o.x;
    p->y -= o.y;
    client_to_game(p, vp);
}

void emu_game_to_screen(POINT *p)
{
    RECT vp;
    POINT o;
    if (!game_viewport(&vp, &o))
        return;
    DDraw *dd = g_dd;
    p->x = o.x + vp.left + (long)((long long)p->x * (vp.right - vp.left) / dd->modeW);
    p->y = o.y + vp.top + (long)((long long)p->y * (vp.bottom - vp.top) / dd->modeH);
}

// --- window procedure --------------------------------------------------------------------

static LRESULT call_old(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return s_unicode ? CallWindowProcW(s_oldProc, h, m, w, l) : CallWindowProcA(s_oldProc, h, m, w, l);
}

// Q174 cursorprobe: WM_SETCURSOR the game window got (client area / elsewhere) and how often the game answered TRUE
static volatile LONG s_cpMsg, s_cpMsgClient, s_cpMsgTrue;
static volatile LONG s_cpShow, s_cpSet, s_cpSetNull, s_cpClass;   // the game's ShowCursor / SetCursor / class cursor calls
static volatile LONG s_cpCounter = 0x7FFFFFFF;   // the display counter the last ShowCursor of the game returned
static volatile LONG s_cursorTestDue;   // cursortest: set by the render thread, done once on the game thread
// Q174 keepcursor: the game's own ShowCursor calls (net), the counter before them, and how far we raised the counter
static volatile LONG s_gameShowDelta;
static LONG s_cursorBase = 0x7FFFFFFF;           // the counter of the game thread minus the game's own calls (window_attach)
static LONG s_cursorRaised;                      // ShowCursor(TRUE) calls of ours not yet undone (game thread only)
static volatile LONG s_cursorCheckDue;           // the render thread found the pointer hidden while it should show

typedef int(WINAPI *PFN_ShowCursor)(BOOL);
static PFN_ShowCursor r_ShowCursor;   // keepcursor / cursorprobe: the real ShowCursor behind our hook of the game's import

// the display counter of this thread, unchanged
static int cursor_counter()
{
    int c = ShowCursor(TRUE) - 1;
    ShowCursor(FALSE);
    return c;
}

// Q174/Q178: does the game want the system pointer now? Decided by the game's own ShowCursor calls: QEC hides the system
// pointer itself (ShowCursor(FALSE) twice) when it switches to the pointer it draws into the picture (a mission) and
// shows it again for the briefing / menus. Frame copies are no sign of that - loading screens and menus copy the whole
// screen now and then too (a tester's menu at 3/s handed the pointer to the game, and OBS left it hidden). Frames decide
// only when the game's calls were never tracked (no baseline, or our ShowCursor hook is not in place).
// why: counter, frames and the rule that decided, for the log.
static bool cursor_wanted(char *why, int n)
{
    double age = render_frame_age_ms();
    char frames[48];
    if (age < 1e11) _snprintf_s(frames, sizeof(frames), _TRUNCATE, "last frame copy %.0f ms ago", age);
    else strcpy_s(frames, "no frame copy yet");
    LONG base = s_cursorBase;
    if (base != 0x7FFFFFFF && r_ShowCursor) {
        LONG intent = base + s_gameShowDelta;
        bool want = intent >= 0;
        _snprintf_s(why, n, _TRUNCATE, "rule: the game's own ShowCursor count %ld (%s); %s (not used)", intent,
                    want ? "it shows the system pointer" : "it hid the system pointer, it draws its own", frames);
        return want;
    }
    bool want = !(age < 300.0);
    _snprintf_s(why, n, _TRUNCATE, "rule: frames (the game's ShowCursor count was not tracked) - %s, %s", frames,
                want ? "no frames: the system pointer is wanted" : "frames: the game draws its own pointer");
    return want;
}

// Q174 keepcursor (game thread): a capture tool or another injected component can hide the system pointer of the game
// thread (display counter below zero, or a NULL cursor) - cnc-ddraw owns the pointer and kept it, we left it to the game
// and it stayed hidden. When the game wants the system pointer: our own ShowCursor(TRUE) up to 0 (remembered) and the
// class cursor set (an arrow if there is none). When it does not: what we raised is given back, nothing else is touched.
// setShape = answer of WM_SETCURSOR (true = the cursor was set, the message is done).
static bool cursor_keep(HWND h, bool setShape)
{
    if (!g_cfg.keepCursor)
        return false;
    char why[200];
    bool want = cursor_wanted(why, sizeof(why));
    static int lastState = -1;
    static LONG logged;
    if (!want) {
        if (s_cursorRaised > 0) {
            for (LONG i = 0; i < s_cursorRaised; i++) ShowCursor(FALSE);
            log_event("cursor: %ld raise(s) of ours given back - %s (counter now %d)", s_cursorRaised, why, cursor_counter());
            s_cursorRaised = 0;
        }
        if (lastState != 0) {
            lastState = 0;
            log_event("cursor: system pointer left to the game - %s", why);
        }
        return false;
    }
    int raised = 0, before = cursor_counter();
    while (before + raised < 0 && raised < 64) {
        ShowCursor(TRUE);
        raised++;
    }
    s_cursorRaised += raised;
    HCURSOR cls = (HCURSOR)GetClassLongPtrA(h, GCLP_HCURSOR);
    HCURSOR shape = cls ? cls : LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    HCURSOR was = NULL;
    if (setShape || raised)
        was = SetCursor(shape);
    if (lastState != 1) {
        lastState = 1;
        log_event("cursor: system pointer kept visible over the game - %s", why);
    }
    if ((raised || (setShape && !was)) && InterlockedIncrement(&logged) <= 50)
        log_event("cursor: pointer was hidden from outside - counter %d%s, %s; raised by %d (ours outstanding %ld), cursor set "
                  "to %s %p", before, before < 0 ? " (below 0)" : "", was ? "a cursor was set" : "no cursor was set (NULL)",
                  raised, s_cursorRaised, cls ? "the class cursor" : "the arrow", shape);
    return setShape;
}

static void release_stuck_keys(const char *why);

// game_handles_close=false: the close button ends the game. The game's own WM_CLOSE handling runs
// first; if one of its dialogs shows up in response (a question, a save screen) nothing else happens,
// the user answers it. Otherwise the game thread gets WM_QUIT (an orderly end of its message loop) and
// if the process is still there 3 s after the click it is ended (display mode restored, state saved).
static HWND s_dlgBefore[64];
static int s_dlgBeforeN;

struct DlgList { HWND *h; int n, max; bool children; };

static BOOL CALLBACK collect_dialog(HWND h, LPARAM l)
{
    DlgList *d = (DlgList *)l;
    char cls[16] = "";
    if (d->children && (GetAncestor(h, GA_PARENT) != s_hwnd || !GetClassNameA(h, cls, sizeof(cls)) || strcmp(cls, "#32770")))
        return TRUE;
    if (h != s_hwnd && IsWindow(h) && IsWindowVisible(h) && d->n < d->max)
        d->h[d->n++] = h;
    return TRUE;
}

// visible dialogs of the game: top-level windows of its thread and #32770 children of the main window
static int game_dialogs(HWND *list, int max)
{
    DlgList d = {list, 0, max, false};
    EnumThreadWindows(s_guiThread, collect_dialog, (LPARAM)&d);
    d.children = true;
    EnumChildWindows(s_hwnd, collect_dialog, (LPARAM)&d);
    return d.n;
}

static DWORD WINAPI close_thread(LPVOID)
{
    Sleep(400);
    HWND now[64];
    int n = game_dialogs(now, 64);
    for (int i = 0; i < n; i++) {
        bool known = false;
        for (int j = 0; j < s_dlgBeforeN && !known; j++)
            known = now[i] == s_dlgBefore[j];
        if (!known) {
            char text[64] = "";
            window_text(now[i], text, sizeof(text));
            log_event("close: the game answered with a dialog (%p \"%s\") - left to the user, nothing ended", now[i], text);
            InterlockedExchange(&s_closing, 0);
            return 0;
        }
    }
    log_event("close: no dialog from the game - WM_QUIT posted to its thread");
    PostThreadMessageA(s_guiThread, WM_QUIT, 0, 0);
    Sleep(2600);
    log_event("close: game still running 3 s after the click - ending the process (exit code 0)");
    if (g_dd)
        display_restore(g_dd, "close button");
    window_save_state();
    log_flush();
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

static void close_requested()
{
    if (InterlockedExchange(&s_closing, 1))
        return;
    log_event("close: close button / Alt+F4 / WM_CLOSE, the game's own handling ran (%d dialog(s) visible before); "
              "watching that it ends", s_dlgBeforeN);
    HANDLE t = CreateThread(NULL, 0, close_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
}

static LRESULT CALLBACK sub_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    jr(J_SUBMSG, (ULONG_PTR)h, m, w);
    if (s_cursorTestDue && InterlockedExchange(&s_cursorTestDue, 0)) {
        // cursortest (debug): what an injected component hiding the cursor does, on the game's own thread
        int c = ShowCursor(FALSE);
        HCURSOR was = SetCursor(NULL);
        log_event("cursortest: ShowCursor(FALSE) -> counter %d, SetCursor(NULL) (was %p) on the game thread %lu", c, was,
                  GetCurrentThreadId());
    }
    if (s_cursorCheckDue && InterlockedExchange(&s_cursorCheckDue, 0))
        cursor_keep(h, false);
    if (m == WM_SETCURSOR) {
        if (g_cfg.cursorProbe) {
            InterlockedIncrement(&s_cpMsg);
            if (LOWORD(l) == HTCLIENT) InterlockedIncrement(&s_cpMsgClient);
        }
        // Q174: over the game picture (the window or one of its dialogs) the pointer is ours to keep visible
        if (LOWORD(l) == HTCLIENT && ((HWND)w == h || IsChild(h, (HWND)w)) && cursor_keep(h, true))
            return TRUE;
        LRESULT r = call_old(h, m, w, l);
        if (r && g_cfg.cursorProbe) InterlockedIncrement(&s_cpMsgTrue);
        return r;
    }
    if (m == s_hotkeyMsg && s_hotkeyMsg) {
        if (w == 1) {
            int to = s_present != PR_WINDOWED ? PR_WINDOWED : g_cfg.windowed ? PR_FULLSCREEN : PR_EXCLUSIVE;
            log_event("hotkey Alt+0x%02X: %s -> %s", g_cfg.keyToggleFullscreen, s_prName[s_present], s_prName[to]);
            present_set(to, "Alt+Enter");
        } else if (w == 2) {
            s_cursorFree = !s_cursorFree;
            if (s_cursorFree)
                s_lockSticky = s_dragClip = false;   // lockcursor too, until Ctrl+<key> again
            adj_reset();
            apply_clip();
            RECT c;
            GetClipCursor(&c);
            log_event("hotkey Ctrl+0x%02X: cursor %s, clip now (%ld,%ld)-(%ld,%ld)", g_cfg.keyUnlockCursor,
                      s_cursorFree ? "unlocked" : "locked", c.left, c.top, c.right, c.bottom);
        } else if (w == 3) {
            log_event("hotkey 0x%02X: screenshot requested", g_cfg.keyScreenshot);
            render_screenshot();
        } else if (w == 4) {
            log_event("hotkey Ctrl+0x%02X: next monitor", g_cfg.keyNextMonitor);
            monitor_next("hotkey");
        }
        return 0;
    }
    if (g_cfg.fixChilds && s_childrenVisible &&
        (m == WM_MOVE || m == WM_SIZE || (m == WM_ACTIVATE && LOWORD(w) != WA_INACTIVE)))
        repaint_children(h);   // dialogs up: they paint again too
    // noactivateapp (default true, Q147): outside a real display mode the game is never told that another
    // APPLICATION took the focus - YR pauses its game loop on deactivation, which stops a multiplayer client while
    // another window is in front. Focus moves inside the game (main window <-> its own dialogs) are passed on
    // unchanged: hiding those broke the briefing's buttons. WM_ACTIVATEAPP 0 is handled below.
    if (g_cfg.noActivateApp && !(g_dd && g_dd->realModeChanged) &&
        ((m == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE && !own_window((HWND)l)) ||
         (m == WM_KILLFOCUS && !own_window((HWND)w)))) {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 10)
            log_event("noactivateapp: %s (to another application) not passed to the game",
                      m == WM_ACTIVATE ? "WM_ACTIVATE(inactive)" : "WM_KILLFOCUS");
        return s_unicode ? DefWindowProcW(h, m, w, l) : DefWindowProcA(h, m, w, l);
    }
    switch (m) {
    case 0x02E0: {   // WM_DPICHANGED (per-monitor aware): the window moved to a monitor with another scale
        RECT *sug = (RECT *)l;
        UINT dpi = LOWORD(w);
        RECT cr, wr;
        GetClientRect(h, &cr);
        GetWindowRect(h, &wr);
        log_event("WM_DPICHANGED: DPI %u (%u%%), suggested window (%ld,%ld)-(%ld,%ld), window now (%ld,%ld)-(%ld,%ld) "
                  "client %ldx%ld%s", dpi, dpi * 100 / 96, sug ? sug->left : 0, sug ? sug->top : 0, sug ? sug->right : 0,
                  sug ? sug->bottom : 0, wr.left, wr.top, wr.right, wr.bottom, cr.right, cr.bottom,
                  s_presentInit ? "" : " (not ours: left to the game)");
        if (!s_presentInit)
            break;
        if (s_present != PR_WINDOWED || s_switching) {
            // fullscreen covers its monitor in pixels whatever the scale; nothing to resize
            render_force();
            return 0;
        }
        // Windowed: the picture keeps its pixel size (sharp, 1:1 with the game where it was 1:1); only the
        // frame changes with the new DPI. Placed at the suggested position (under the cursor while dragging).
        RECT c = {0, 0, cr.right, cr.bottom};
        if (g_cfg.border)
            adjust_window_rect(&c, GetWindowLongA(h, GWL_STYLE), GetWindowLongA(h, GWL_EXSTYLE), dpi);
        int x = sug ? sug->left : wr.left, y = sug ? sug->top : wr.top;
        s_switching = true;
        SetWindowPos(h, NULL, x, y, c.right - c.left, c.bottom - c.top, SWP_NOZORDER | SWP_NOACTIVATE);
        s_switching = false;
        GetClientRect(h, &cr);
        log_event("WM_DPICHANGED: client kept at %ldx%ld (frame for DPI %u)", cr.right, cr.bottom, dpi);
        remember_windowed();
        render_force();
        monitor_check("DPI change");
        return 0;
    }
    case WM_DISPLAYCHANGE:
        // a monitor added / removed / changed resolution, or the primary changed: fullscreen is fitted to the
        // window's monitor again, a window outside every work area is brought back
        log_event("WM_DISPLAYCHANGE %ux%u %ubpp%s", (unsigned)LOWORD(l), (unsigned)HIWORD(l), (unsigned)w,
                  s_switching ? " (our own mode switch)" : "");
        if (!s_switching && s_presentInit && g_dd && !g_dd->realModeChanged) {
            monitors_log("display change");
            if (s_present == PR_FULLSCREEN) {
                present_set(PR_FULLSCREEN, "display change");
            } else if (s_present == PR_WINDOWED) {
                RECT wr;
                GetWindowRect(h, &wr);
                if (make_visible(&wr)) {
                    SetWindowPos(h, NULL, wr.left, wr.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    log_event("display change: window was outside every work area -> (%ld,%ld)", wr.left, wr.top);
                }
            }
            monitor_check("display change");
        }
        break;
    case WM_MOVE:
    case WM_ENTERSIZEMOVE:
    case WM_EXITSIZEMOVE:
        clip_on_message(m);
        if (m != WM_ENTERSIZEMOVE && !s_sizeMove) {
            remember_windowed();
            window_save_state();
        }
        render_force();
        // the emulated screen does not move: the game sees its client at (0,0)
        if (m == WM_MOVE && emu_active())
            l = 0;
        break;
    case WM_SIZE:
        if (!s_sizeMove) {
            remember_windowed();
            window_save_state();
        }
        render_force();
        // ... nor change size: fullscreen / window switches and resizing only scale the picture, the game
        // keeps seeing its own screen (as GetClientRect reports it)
        if (emu_active() && w != SIZE_MINIMIZED)
            l = MAKELPARAM(g_dd->modeW, g_dd->modeH);
        break;
    case WM_STYLECHANGING:
    case WM_STYLECHANGED:
    case WM_WINDOWPOSCHANGED:
        if (s_switching)   // our own presentation change is none of the game's business
            return s_unicode ? DefWindowProcW(h, m, w, l) : DefWindowProcA(h, m, w, l);
        break;
    case WM_PAINT:
    case WM_SHOWWINDOW:
        render_force();   // re-upload everything and present, whatever the dirty tracking says
        break;
    case WM_NCCALCSIZE:
        if (w && borderless_windowed())
            return 0;   // borderless: the whole window is client area, the sizing frame stays invisible
        break;
    case WM_MOVING:
        // QEC's procedure clamps the dragged rectangle to the screen (window wider than the desktop =>
        // it can hardly be moved). While emulating, the image does not depend on the window position,
        // so the game is not consulted.
        if (emu_active())
            return TRUE;
        // fall through
    case WM_WINDOWPOSCHANGING: {
        if (s_switching)   // our own presentation change: the game's procedure must not move it elsewhere
            return s_unicode ? DefWindowProcW(h, m, w, l) : DefWindowProcA(h, m, w, l);
        // diagnostics: does the game's own window procedure restrict where the window may go?
        RECT before = {0, 0, 0, 0};
        if (m == WM_MOVING && l) before = *(RECT *)l;
        if (m == WM_WINDOWPOSCHANGING && l) SetRect(&before, ((WINDOWPOS *)l)->x, ((WINDOWPOS *)l)->y, 0, 0);
        LRESULT r = call_old(h, m, w, l);
        RECT after = before;
        if (m == WM_MOVING && l) after = *(RECT *)l;
        if (m == WM_WINDOWPOSCHANGING && l) SetRect(&after, ((WINDOWPOS *)l)->x, ((WINDOWPOS *)l)->y, 0, 0);
        static int logged;
        if ((before.left != after.left || before.top != after.top) && logged++ < 20)
            log_event("%s: game window procedure changed the position (%ld,%ld) -> (%ld,%ld)",
                      m == WM_MOVING ? "WM_MOVING" : "WM_WINDOWPOSCHANGING", before.left, before.top, after.left, after.top);
        return r;
    }
    case WM_NCHITTEST: {
        if (borderless_windowed() && !IsZoomed(h)) {
            LRESULT ht = borderless_hit(l);
            if (ht != HTCLIENT)
                return ht;
            return call_old(h, m, w, l);
        }
        // Window dragging belongs to Windows, not to the game: outside the client area use the
        // default hit test (caption, borders), whatever the game's procedure answers.
        LRESULT game = call_old(h, m, w, l);
        if (!emu_active())
            return game;
        LRESULT def = s_unicode ? DefWindowProcW(h, m, w, l) : DefWindowProcA(h, m, w, l);
        if (def != HTCLIENT && def != game) {
            static int logged;
            if (logged++ < 5)
                log_event("WM_NCHITTEST: game answered %ld, default %ld - using the default", (long)game, (long)def);
            return def;
        }
        return game;
    }
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (emu_active() && (w == HTCAPTION || (w >= HTLEFT && w <= HTBOTTOMRIGHT))) {
            static int logged;
            if (logged++ < 5)
                log_event("WM_NCLBUTTON%s hit %ld: handled by DefWindowProc (window move / size)",
                          m == WM_NCLBUTTONDOWN ? "DOWN" : "DBLCLK", (long)w);
            return s_unicode ? DefWindowProcW(h, m, w, l) : DefWindowProcA(h, m, w, l);
        }
        break;
    case WM_SYSCOMMAND:
        if ((w & 0xFFF0) == SC_KEYMENU && l == 0 && !GetMenu(h)) {
            // A lone Alt (or F10) released: DefWindowProc would enter the menu loop of a window without a
            // menu bar - invisible, it swallows the next key (Esc after Alt+Tab did not open the game menu).
            // Alt+Space (l = ' ') still opens the system menu.
            static int logged;
            if (logged++ < 10)
                log_event("WM_SYSCOMMAND SC_KEYMENU from a lone Alt: menu loop not entered (the window has no menu)");
            return 0;
        }
        if ((w & 0xFFF0) == SC_CLOSE && !g_cfg.gameHandlesClose) {
            // QEC ends itself from its SC_CLOSE handling: the game goes first, we only make sure it ends
            log_event("WM_SYSCOMMAND SC_CLOSE");
            if (!s_closing)
                s_dlgBeforeN = game_dialogs(s_dlgBefore, 64);
            LRESULT r = call_old(h, m, w, l);
            close_requested();
            return r;
        }
        if (emu_active() &&((w & 0xFFF0) == SC_MOVE || (w & 0xFFF0) == SC_SIZE)) {
            static int logged;
            if (logged++ < 5)
                log_event("WM_SYSCOMMAND 0x%lX: handled by DefWindowProc", (unsigned long)(w & 0xFFF0));
            return s_unicode ? DefWindowProcW(h, m, w, l) : DefWindowProcA(h, m, w, l);
        }
        break;
    case WM_ERASEBKGND:
        return 1;   // OpenGL covers the whole client area
    case WM_ACTIVATEAPP:
        s_active = w != 0;
        log_event("WM_ACTIVATEAPP %d", (int)(w != 0));
        if (!w) {
            release_stuck_buttons("focus lost");
            if (s_emu)
                ClipCursor(NULL);
            adj_reset();
            if (g_dd && g_dd->realModeChanged && s_present == PR_EXCLUSIVE) {
                // Alt-Tab out of a real display mode: the desktop gets its own mode back
                display_restore(g_dd, "focus lost");
                s_needReapply = true;
                ShowWindow(h, SW_MINIMIZE);
            }
        } else if (s_needReapply && g_dd) {
            s_needReapply = false;
            present_set(PR_EXCLUSIVE, "focus regained");
        }
        if (w)
            clip_on_message(m);   // re-apply the game's clip at the current window position
        release_stuck_keys(w ? "focus regained" : "focus lost");
        render_force();
        if (!w && g_cfg.noActivateApp && !(g_dd && g_dd->realModeChanged)) {
            log_event("noactivateapp: WM_ACTIVATEAPP 0 not passed to the game (it keeps running in the background)");
            return 0;   // keep the game running while the window is in the background
        }
        break;
    case WM_CLOSE: {
        if (g_cfg.gameHandlesClose) {
            log_event("WM_CLOSE: left to the game (game_handles_close=true)");
            break;
        }
        if (!s_closing)
            s_dlgBeforeN = game_dialogs(s_dlgBefore, 64);
        LRESULT r = call_old(h, m, w, l);
        close_requested();
        return r;
    }
    case WM_DESTROY:
        log_event("WM_DESTROY on game window");
        window_save_state();
        render_stop();
        if (g_dd)
            display_restore(g_dd, "window destroyed");
        break;
    default:
        if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL && m != 0x020E /*WM_MOUSEHWHEEL*/ &&
            emu_active()) {
            RECT vp;
            POINT g;
            if (adj_update(&g)) {
                l = MAKELPARAM(g.x, g.y);   // adjmouse: the same position GetCursorPos reports
            } else if (game_viewport(&vp, NULL)) {
                POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
                client_to_game(&p, vp);
                l = MAKELPARAM(p.x, p.y);
            }
        }
        break;
    }
    return call_old(h, m, w, l);
}

// Alt+<keytogglefullscreen> and Ctrl+<keyunlockcursor> are taken out of the queue before the game
// (or one of its child dialogs) sees them and handled by sub_proc through a private message.
static bool s_altUsed;       // Alt was part of a hotkey (its key-up must not open the system menu)
static DWORD s_firedAt;      // tick / message time of the last fullscreen toggle
static HHOOK s_kbHook;

static bool hotkey_filter(MSG *m)
{
    if (!s_presentInit || !m->hwnd || !game_related(m->hwnd))
        return false;
    int fk = g_cfg.keyToggleFullscreen, uk = g_cfg.keyUnlockCursor;
    bool alt = (m->lParam & (1L << 29)) != 0, repeat = (m->lParam & (1L << 30)) != 0;
    switch (m->message) {
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        if (s_altUsed && m->message == WM_SYSKEYUP && m->wParam == VK_MENU) {
            // DefWindowProc would see a lone Alt press (the hotkey key was taken away) and open the
            // system menu loop, which swallows the following keys. The game still gets the key-up.
            s_altUsed = false;
            m->message = WM_KEYUP;
            return false;
        }
        if (!fk || (int)m->wParam != fk || !alt)
            return false;
        if (m->message == WM_SYSKEYUP) {
            // the key-up always reaches the game: in a mission QEC may have seen the key-down itself
            // and would otherwise keep the key "held" (Esc stops opening the menu)
            m->message = WM_KEYUP;
            return false;
        }
        if (!repeat) {
            PostMessageA(s_hwnd, s_hotkeyMsg, 1, 0);
            s_firedAt = m->time;
        }
        s_altUsed = true;
        return true;
    case WM_SYSCHAR:
        if (fk != VK_RETURN || m->wParam != '\r')
            return false;
        // In a mission QEC takes the Enter key-down out of the queue without the hook seeing it; only
        // the WM_SYSCHAR translated from it arrives. It triggers the toggle unless the key-down did.
        if (alt && !repeat && m->time - s_firedAt > 100) {
            PostMessageA(s_hwnd, s_hotkeyMsg, 1, 0);
            s_firedAt = m->time;
            s_altUsed = true;
        }
        return true;   // or DefWindowProc beeps
    case WM_KEYDOWN:
    case WM_KEYUP:
        if (!uk || (int)m->wParam != uk || GetKeyState(VK_CONTROL) >= 0)
            return false;
        if (m->message == WM_KEYDOWN && !repeat)
            PostMessageA(s_hwnd, s_hotkeyMsg, 2, 0);
        return m->message == WM_KEYDOWN;   // key-ups pass, the game never keeps a key "held"
    case WM_CHAR:
        return uk == VK_TAB && m->wParam == '\t' && GetKeyState(VK_CONTROL) < 0;
    case WM_LBUTTONDOWN:
        // borderless window: QEC's child dialogs cover the client area, so the main window's hit test
        // is not asked there. A click in the drag / resize zones starts Windows' move / size loop.
        if (borderless_windowed() && !IsZoomed(s_hwnd)) {
            LRESULT ht = borderless_hit(MAKELPARAM(m->pt.x, m->pt.y));
            if (ht != HTCLIENT) {
                ReleaseCapture();
                PostMessageA(s_hwnd, WM_NCLBUTTONDOWN, ht, MAKELPARAM(m->pt.x, m->pt.y));
                return true;
            }
        }
        return false;
    }
    return false;
}

// In a mission the Enter key-down never shows up in the message hook (QEC or one of its extensions
// takes it earlier), so the game would see Alt+Enter as Enter. Our WH_KEYBOARD hook is installed
// later, so it runs first and discards the hotkey key-downs - and their key-ups, so the game sees
// neither half of the key.
static LRESULT CALLBACK kb_hook(int code, WPARAM vk, LPARAM l)
{
    if (code >= 0 && s_presentInit && s_hwnd && GetForegroundWindow() == s_hwnd) {
        bool up = (l & 0x80000000L) != 0, alt = (l & (1L << 29)) != 0, repeat = (l & (1L << 30)) != 0;
        int fk = g_cfg.keyToggleFullscreen, uk = g_cfg.keyUnlockCursor;
        int which = 0;
        // a discarded key-down: its key-up (seen as peek and/or removal within a moment) is discarded too
        static bool taken[256];
        static DWORD upSeen[256];
        if (vk < 256 && up && taken[vk]) {
            DWORD now = GetTickCount();
            if (!upSeen[vk])
                upSeen[vk] = now ? now : 1;
            if (now - upSeen[vk] < 300)
                return 1;
            taken[vk] = false;
        }
        if (vk < 256 && !up)
            taken[vk] = false;
        if (up)
            return CallNextHookEx(s_kbHook, code, vk, l);
        if (!up && fk && (int)vk == fk && alt)
            which = 1;
        else if (!up && uk && (int)vk == uk && GetKeyState(VK_CONTROL) < 0)
            which = 2;
        else if (!up && g_cfg.keyNextMonitor && (int)vk == g_cfg.keyNextMonitor && GetKeyState(VK_CONTROL) < 0)
            which = 4;
        else if (!up && g_cfg.keyScreenshot && (int)vk == g_cfg.keyScreenshot && !alt)
            which = 3;
        if (which) {
            if (vk < 256) {
                taken[vk] = true;
                upSeen[vk] = 0;
            }
            // the same key-down may be seen several times (HC_NOREMOVE peeks, HC_ACTION); one action per press
            static DWORD lastAt[5];
            DWORD now = GetTickCount();
            if (!repeat && now - lastAt[which] > 150) {
                lastAt[which] = now;
                PostMessageA(s_hwnd, s_hotkeyMsg, which, 0);
                if (which == 1) {
                    s_firedAt = now;
                    s_altUsed = true;
                }
            }
            return 1;
        }
    }
    return CallNextHookEx(s_kbHook, code, vk, l);
}

// letterboxoffset: QEC's #32770 child dialogs and their buttons are real windows laid out 1:1 in the
// main window's client area, i.e. at their game coordinates. The picture, though, sits in the
// viewport (below a black bar, or scaled), so Windows hands a click to the child under the real cursor
// - not the one drawn there (2560x1440 in a 2560x1600 fullscreen: every button 80 px off). The child
// layout is left alone (the game reads it back and paints the dialogs from it); instead every mouse
// message is re-addressed: the real point is mapped to game coordinates like GetCursorPos does, and
// the message goes to the child laid out there (or to the window holding the capture), in that
// child's client coordinates.
static HWND layout_child_at(POINT g)
{
    HWND cur = s_hwnd;
    POINT p = g;   // in cur's client coordinates
    for (int depth = 0; depth < 16; depth++) {
        HWND c = ChildWindowFromPointEx(cur, p, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
        if (!c || c == cur)
            break;
        char cls[16];
        if (!GetClassNameA(c, cls, sizeof(cls)))   // gone meanwhile
            break;
        if (!_stricmp(cls, "Static") && !(GetWindowLongA(c, GWL_STYLE) & SS_NOTIFY))
            break;   // labels let the mouse through to their dialog, as with WindowFromPoint
        MapWindowPoints(cur, c, &p, 1);
        cur = c;
    }
    return cur;
}

static bool letterbox_mouse(MSG *m)
{
    UINT msg = m->message;
    if (!g_cfg.letterboxOffset || msg < WM_MOUSEFIRST || msg > WM_MOUSELAST || msg == WM_MOUSEWHEEL ||
        msg == 0x020E /*WM_MOUSEHWHEEL*/ || !m->hwnd || !emu_active() || !game_related(m->hwnd))
        return false;
    RECT vp;
    POINT o;
    if (!game_viewport(&vp, &o))
        return false;
    if (vp.left == 0 && vp.top == 0 && vp.right == g_dd->modeW && vp.bottom == g_dd->modeH)
        return false;   // picture 1:1 at the client origin: the layout already matches
    POINT c = {m->pt.x - o.x, m->pt.y - o.y}, g;
    if (!adj_update(&g)) {
        g = c;
        if (g.x < vp.left) g.x = vp.left;
        if (g.y < vp.top) g.y = vp.top;
        if (g.x >= vp.right) g.x = vp.right - 1;
        if (g.y >= vp.bottom) g.y = vp.bottom - 1;
        client_to_game(&g, vp);
    }
    HWND cap = GetCapture(), to = cap && game_related(cap) ? cap : layout_child_at(g);
    POINT p = g;
    if (to == s_hwnd)
        p = c;   // sub_proc maps the main window's client coordinates itself
    else
        MapWindowPoints(s_hwnd, to, &p, 1);
    static LONG logged;
    bool press = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_RBUTTONUP;
    if (press && logged++ < 200) {
        char cls[32] = "", text[96] = "";
        wchar_t wtext[48] = L"";
        GetClassNameA(to, cls, sizeof(cls));
        InternalGetWindowText(to, wtext, 48);
        WideCharToMultiByte(CP_UTF8, 0, wtext, -1, text, sizeof(text), NULL, NULL);
        POINT before = {(short)LOWORD(m->lParam), (short)HIWORD(m->lParam)};
        log_event("letterboxoffset: msg 0x%04X real (%ld,%ld) -> game (%ld,%ld), viewport (%ld,%ld)-(%ld,%ld): "
                  "hwnd %p (%ld,%ld) -> %p \"%s\" %s (%ld,%ld)%s", msg, m->pt.x, m->pt.y, g.x, g.y, vp.left, vp.top,
                  vp.right, vp.bottom, m->hwnd, before.x, before.y, to, text, cls, p.x, p.y,
                  to == cap ? " [capture]" : "");
    }
    m->hwnd = to;
    m->lParam = MAKELPARAM(p.x, p.y);
    return true;
}

// Modifier keys the game thread still believes held although they are up (the key-up went to another
// window while the game was in the background - Alt+Tab): the game gets the key-ups it missed, and with
// fix_alt_key_stuck the thread key state (GetKeyState) is corrected too. Without the fix only Alt is
// released, and only when the game saw it go down.
static void release_stuck_keys(const char *why)
{
    if (!s_hwnd)
        return;
    BYTE ks[256];
    if (!GetKeyboardState(ks))
        return;
    static const int vks[] = {VK_MENU, VK_LMENU, VK_RMENU, VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_SHIFT, VK_LSHIFT, VK_RSHIFT};
    static const char *const names[] = {"Alt", "LAlt", "RAlt", "Ctrl", "LCtrl", "RCtrl", "Shift", "LShift", "RShift"};
    char held[96] = "";
    bool fix = false;
    for (int i = 0; i < 9; i++) {
        if ((ks[vks[i]] & 0x80) && GetAsyncKeyState(vks[i]) >= 0) {
            strcat_s(held, " ");
            strcat_s(held, names[i]);
            ks[vks[i]] &= ~0x80;
            fix = true;
        }
    }
    HWND to = GetFocus();
    if (!to || !game_related(to))
        to = s_hwnd;
    bool altUp = GetAsyncKeyState(VK_MENU) >= 0;
    int posted = 0;
    if (g_cfg.fixAltKeyStuck) {
        if (fix)
            SetKeyboardState(ks);
        static const int generic[] = {VK_MENU, VK_CONTROL, VK_SHIFT};
        for (int i = 0; i < 3; i++) {
            bool stuck = strstr(held, i == 0 ? "Alt" : i == 1 ? "Ctrl" : "Shift") != NULL || (i == 0 && s_altSeen);
            if (stuck && GetAsyncKeyState(generic[i]) >= 0) {
                UINT sc = MapVirtualKeyA(generic[i], 0);
                PostMessageA(to, WM_KEYUP, generic[i], (LPARAM)(0xC0000001UL | (sc << 16)));
                posted++;
            }
        }
        if (altUp) s_altSeen = false;
    } else if (s_altSeen && altUp) {
        // a plain key-up: WM_SYSKEYUP of a lone Alt would start DefWindowProc's menu loop
        PostMessageA(to, WM_KEYUP, VK_MENU, (LPARAM)(0xC0000001UL | (MapVirtualKeyA(VK_MENU, 0) << 16)));
        s_altSeen = false;
        posted++;
    }
    s_clicksLogged = 0;
    log_event("keys (%s): thread state held:%s%s, Alt physically %s, %d key-up(s) posted%s", why, held[0] ? held : " none",
              fix && g_cfg.fixAltKeyStuck ? " -> cleared" : "", altUp ? "up" : "down", posted,
              g_cfg.fixAltKeyStuck ? "" : " (fix_alt_key_stuck=false: Alt only)");
}

// fix_not_responding: Windows marks a window "Not Responding" when its thread takes no message for
// 5 s. While the game thread is busy in a long loop of drawing calls it is kept responsive from here:
// at most once a second it peeks at its input (default) or, with the fix, dispatches one message.
void window_pump()
{
    if (!s_hwnd || GetCurrentThreadId() != s_guiThread)
        return;
    DWORD now = GetTickCount();
    s_lastCall = now;
    // stalltest (debug): 20, 40 and 60 s after the first call the game thread is held here, like a machine
    // that froze for a moment (maxgameticks tests)
    static DWORD firstCall;
    static int stalls;
    if (!firstCall)
        firstCall = now;
    if (g_cfg.stallTest > 0 && stalls < 3 && now - firstCall >= 20000u * (stalls + 1)) {
        stalls++;
        log_event("stalltest #%d: game thread held for %d ms", stalls, g_cfg.stallTest);
        Sleep(g_cfg.stallTest);
        now = s_lastCall = GetTickCount();
        log_event("stalltest #%d: game thread released", stalls);
    }
    if (now - s_lastPull < 1000)
        return;
    s_lastPull = now;
    MSG msg;
    if (g_cfg.fixNotResponding) {
        if (PeekMessageA(&msg, s_hwnd, 0, 0, PM_REMOVE)) {
            static LONG n;
            if (n++ < 20)
                log_event("fix_not_responding: game thread busy for 1 s, dispatched message 0x%04X", msg.message);
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    } else {
        PeekMessageA(&msg, s_hwnd, 0, 0, PM_NOREMOVE | PM_QS_INPUT);
    }
}

// render thread, about once a second: a game thread that stops calling us for more than 2 s while it
// is busy - it does not answer a WM_NULL within 250 ms either (a game idle in its message loop, e.g.
// behind the Esc menu, does) - is a long frame; windows Windows considers hung are logged as well.
// Q174 keepcursor (render thread, every tick, at most every 50 ms): a pointer hidden while the mouse rests over the game,
// when the game wants the system pointer, is put back on the game thread (WM_SETCURSOR only comes when the mouse moves)
void window_cursor_tick()
{
    HWND h = s_hwnd;
    if (!g_cfg.keepCursor || !h || !s_oldProc)
        return;
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 50)
        return;
    last = now;
    if (GetForegroundWindow() != h || IsIconic(h))
        return;
    CURSORINFO k;
    k.cbSize = sizeof(k);
    POINT p;
    char why[200];
    if (GetCursorInfo(&k) && !(k.flags & CURSOR_SHOWING) && GetCursorPos(&p)) {
        HWND under = WindowFromPoint(p);
        if (under && GetAncestor(under, GA_ROOT) == h && cursor_wanted(why, sizeof(why)) && !s_cursorCheckDue) {
            InterlockedExchange(&s_cursorCheckDue, 1);   // done on the game thread (the counter is the game thread's)
            PostMessageA(h, WM_NULL, 0, 0);
        }
    }
}

// Q174: the OS cursor state once a second (cursorprobe), and the cursortest trigger
static void cursor_watch(HWND h)
{
    static DWORD start;
    if (!start) start = GetTickCount();
    // cursortest=N: once after N s; cursortest=-N: every second from N s on (a component that keeps hiding the pointer)
    int at = g_cfg.cursorTest < 0 ? -g_cfg.cursorTest : g_cfg.cursorTest;
    if (at > 0 && h && GetTickCount() - start >= (DWORD)at * 1000) {
        static bool done;
        if (!done || g_cfg.cursorTest < 0) {
            done = true;
            InterlockedExchange(&s_cursorTestDue, 1);
            PostMessageA(h, WM_NULL, 0, 0);   // the next message of the game thread runs it
        }
    }
    if (!g_cfg.cursorProbe || !h)
        return;
    CURSORINFO ci;
    ci.cbSize = sizeof(ci);
    BOOL ok = GetCursorInfo(&ci);
    POINT p;
    GetCursorPos(&p);
    HWND under = WindowFromPoint(p);
    HWND top = under ? GetAncestor(under, GA_ROOT) : NULL;
    log_event("cursorprobe: %s flags=0x%lX (%s) hCursor=%p at (%ld,%ld) over %s | class cursor %p | foreground %s | "
              "WM_SETCURSOR so far %ld (client %ld, game TRUE %ld) | game ShowCursor %ld (counter %ld) SetCursor %ld (NULL %ld) "
              "SetClassLong(GCL_HCURSOR) %ld",
              ok ? "GetCursorInfo" : "GetCursorInfo FAILED", ci.flags,
              (ci.flags & CURSOR_SHOWING) ? "showing" : (ci.flags & 2 /* CURSOR_SUPPRESSED */) ? "suppressed" : "HIDDEN",
              ci.hCursor, p.x, p.y, top == h ? "the game window" : under ? "another window" : "nothing",
              (void *)GetClassLongPtrA(h, GCLP_HCURSOR), GetForegroundWindow() == h ? "game" : "other", s_cpMsg,
              s_cpMsgClient, s_cpMsgTrue, s_cpShow, s_cpCounter, s_cpSet, s_cpSetNull, s_cpClass);
}

void window_watch()
{
    lockcursor_watch();
    HWND h = s_hwnd;
    cursor_watch(h);
    if (!h || !s_lastCall)
        return;
    DWORD now = GetTickCount(), gap = now - s_lastCall;
    static DWORD longSince, hungSince;
    static LONG longCount, hungCount;
    if (gap > 2000 && !longSince && s_active && !IsIconic(h) &&
        now - s_lastPull > 1000) {   // Q156: no message pulled for a second (nothing sent to the game: see window_hang_check)
        longSince = s_lastCall;
        log_event("long frame #%ld: game thread busy, no drawing call for %lu ms%s", ++longCount, gap,
                  s_sizeMove ? " (window move / size loop)" : "");
    } else if (gap <= 2000 && longSince) {
        log_event("long frame #%ld ended after %lu ms", longCount, s_lastCall - longSince);
        longSince = 0;
    }
    bool hung = IsHungAppWindow(h) != FALSE;
    if (hung && !hungSince) {
        hungSince = now ? now : 1;
        log_event("NOT RESPONDING #%ld: Windows reports the game window as hung", ++hungCount);
    } else if (!hung && hungSince) {
        log_event("not responding #%ld ended after %lu ms", hungCount, now - hungSince);
        hungSince = 0;
    }
}

// fixchilds: QEC shows its briefing, Esc menu, save / load and loading screens as #32770 child dialogs over
// the client area and draws their pictures into the primary surface only while handling WM_PAINT. Nothing
// invalidates a freshly created dialog that sits on our OpenGL window, so without help those screens never
// appear (the buttons work, the picture stays on the last game frame). Every 50 ms the visible descendants of
// the game window are compared with the last set; on a change the direct children are invalidated at once,
// after 250 ms and after 1 s (a dialog filled in after creation). RDW_INVALIDATE only: the WM_PAINT is
// generated in the game thread's own message loop, nothing is sent across threads from here.
struct ChildSet { HWND h[64]; int n; };

static BOOL CALLBACK collect_visible(HWND h, LPARAM l)
{
    ChildSet *s = (ChildSet *)l;
    if (IsWindow(h) && IsWindowVisible(h) && s->n < 64)
        s->h[s->n++] = h;
    return TRUE;
}

// childrepaint (default 0): with WS_CLIPCHILDREN kept QEC paints its dialogs and their controls itself, in its own
// order; any invalidation from outside starts an extra paint pass in which the briefing text box paints alone,
// clipped to its rectangle, over the text the game had drawn (Q146) - so by default nothing is invalidated.
// For tests only: childrepaint=1 marks the dialogs (direct children) invalid the way the game does it itself
// (InvalidateRect, no erase) - measured: still wipes the briefing text; childrepaint=2 invalidates dialogs and all
// their controls (the first Q146 build).
static int repaint_children(HWND top)
{
    if (!g_cfg.childRepaint)
        return 0;
    int n = 0;
    for (HWND c = GetWindow(top, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        if (!IsWindow(c) || !IsWindowVisible(c))   // Q156: re-validated right before use
            continue;
        if (g_cfg.childRepaint >= 2)
            RedrawWindow(c, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
        else
            InvalidateRect(c, NULL, FALSE);
        n++;
    }
    return n;
}

void window_children_tick()
{
    HWND h = s_hwnd;
    if (!g_cfg.fixChilds || !h)
        return;
    static DWORD last;
    static ChildSet prev;
    static ChildSched sched;
    static LONG changes;
    DWORD now = GetTickCount();
    if (now - last < 50)
        return;
    last = now;
    ChildSet cur;
    cur.n = 0;
    EnumChildWindows(h, collect_visible, (LPARAM)&cur);
    bool changed = cur.n != prev.n || memcmp(cur.h, prev.h, cur.n * sizeof(HWND)) != 0;
    if (changed) {
        if (++changes <= 40) {
            char text[64] = "";
            if (cur.n)
                window_text(cur.h[cur.n - 1], text, sizeof(text));
            log_event("fixchilds: %d visible child window(s) (was %d)%s%s%s", cur.n, prev.n, cur.n ? ", last \"" : "",
                      text, cur.n ? "\"" : "");
        }
        prev = cur;
        InterlockedExchange(&s_childrenVisible, cur.n);
        jr(J_CHILDSET, cur.n, cur.n ? (ULONG_PTR)cur.h[0] : 0);
    }
    int act = child_sched_step(&sched, now, changed, cur.n);
    if (act & CS_REPAINT) {
        int n = repaint_children(h);
        if (changes <= 40)
            log_event("fixchilds: %d child window(s) invalidated (%lu ms after the change)", n, now - sched.changedAt);
    }
    if (act & CS_PRESENT)
        render_force();
}

// render thread, after a present: visible direct children of the game window that get the picture painted
// into them (childpaint), with their position in our client area
int window_child_targets(ChildTarget *t, int max)
{
    HWND h = s_hwnd;
    if (!h || !s_childrenVisible)
        return 0;
    int n = 0;
    for (HWND c = GetWindow(h, GW_CHILD); c && n < max; c = GetWindow(c, GW_HWNDNEXT)) {
        if (!IsWindow(c) || !IsWindowVisible(c))   // Q156: re-validated right before use
            continue;
        RECT cr;
        if (!GetClientRect(c, &cr))
            continue;
        char cls[32] = "";
        if (!GetClassNameA(c, cls, sizeof(cls)))   // gone meanwhile
            continue;
        if (!child_paint_eligible(cls, cr.right, cr.bottom, GetWindowLongA(c, GWL_EXSTYLE)))
            continue;
        POINT o = {0, 0};
        MapWindowPoints(c, h, &o, 1);   // our own import: the real function
        t[n].hwnd = c;
        t[n].x = o.x;
        t[n].y = o.y;
        t[n].w = cr.right;
        t[n].hh = cr.bottom;
        n++;
    }
    return n;
}

// log thread, every second (Q147): the game thread has made no drawing call for 6 s AND its window does not
// answer a WM_NULL within 300 ms => hung. True once per episode (re-armed when the game draws again).
bool window_hang_check(char *why, int cap)
{
    HWND h = s_hwnd;
    static bool reported;
    if (!h || !s_lastCall)
        return false;
    DWORD gap = GetTickCount() - s_lastCall;
    if (gap < 6000) {
        reported = false;
        return false;
    }
    if (reported || IsIconic(h))
        return false;
    // Q156: our threads never send messages to the game's windows (a WM_NULL from here is dispatched by the game
    // thread wherever it next pumps messages - suspected in the save-game load crashes). The message hook notes every
    // message the game thread takes; none for a second = not in its message loop.
    if (GetTickCount() - s_lastPull < 1000 || !IsHungAppWindow(h))   // Windows' own verdict too (no message sent):
        return false;   // the game thread still takes messages (a long load at start-up is no hang)
    reported = true;
    _snprintf_s(why, cap, _TRUNCATE, "game thread hung: no drawing call for %lu ms, no message taken for %lu ms (IsHungAppWindow=%d)",
                gap, GetTickCount() - s_lastPull, (int)(IsHungAppWindow(h) != FALSE));
    return true;
}

static LRESULT CALLBACK msg_hook(int code, WPARAM w, LPARAM l)
{
    if (code == HC_ACTION && w == PM_REMOVE && l)
        jr(J_HOOKMSG, (ULONG_PTR)((MSG *)l)->hwnd, ((MSG *)l)->message, ((MSG *)l)->wParam);
    if (code == HC_ACTION && w == PM_REMOVE)
        s_lastPull = GetTickCount();
    if (code == HC_ACTION && w == PM_REMOVE && l) {
        MSG *m = (MSG *)l;
        if (m->wParam == VK_MENU && (m->message == WM_SYSKEYDOWN || m->message == WM_KEYDOWN))
            s_altSeen = true;
        else if (m->wParam == VK_MENU && (m->message == WM_SYSKEYUP || m->message == WM_KEYUP))
            s_altSeen = false;
        if (m->message == WM_LBUTTONDOWN && s_clicksLogged < 10 && game_related(m->hwnd)) {
            s_clicksLogged++;
            log_event("click: modifiers as the game sees them: GetKeyState Alt=%d Ctrl=%d Shift=%d, MK flags 0x%lX",
                      GetKeyState(VK_MENU) < 0, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_SHIFT) < 0,
                      (unsigned long)m->wParam);
        }
        letterbox_mouse(m);
        lockcursor_track(m);
    }
    if (g_cfg.mouseProbe && code == HC_ACTION && l && ((MSG *)l)->message >= WM_KEYFIRST &&
        ((MSG *)l)->message <= WM_SYSDEADCHAR) {
        static LONG n;
        const MSG *m = (const MSG *)l;
        if (n++ < 200)
            log_event("keyprobe %s msg 0x%04X vk 0x%02lX lParam 0x%08lX hwnd %p, thread Alt %d Ctrl %d, physical Alt %d%s",
                      w == PM_REMOVE ? "remove" : "peek", m->message, (unsigned long)m->wParam, (unsigned long)m->lParam,
                      m->hwnd, GetKeyState(VK_MENU) < 0, GetKeyState(VK_CONTROL) < 0, GetAsyncKeyState(VK_MENU) < 0,
                      m->hwnd && game_related(m->hwnd) ? "" : " (not the game's)");
    }
    if (code == HC_ACTION && w == PM_REMOVE && l && hotkey_filter((MSG *)l))
        ((MSG *)l)->message = WM_NULL;
    return CallNextHookEx(s_msgHook, code, w, l);
}

void window_attach(HWND hwnd)
{
    if (s_hwnd == hwnd)
        return;
    window_detach();
    s_hwnd = hwnd;
    s_unicode = IsWindowUnicode(hwnd) != FALSE;
    s_oldProc = (WNDPROC)(s_unicode ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)sub_proc)
                                    : SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)sub_proc));
    LONG style = GetWindowLongA(hwnd, GWL_STYLE), ex = GetWindowLongA(hwnd, GWL_EXSTYLE);
    RECT wr;
    GetWindowRect(hwnd, &wr);
    log_event("window_attach hwnd=%p unicode=%d style=0x%08lX exstyle=0x%08lX rect=(%ld,%ld)-(%ld,%ld)", hwnd,
              (int)s_unicode, style, ex, wr.left, wr.top, wr.right, wr.bottom);
    // YR-style games keep visible #32770 child dialogs over the whole client area and draw their
    // contents into the primary themselves. With WS_CLIPCHILDREN the GL output is clipped away under
    // them. With fixchilds + childpaint the picture is painted into those children, so the style stays -
    // QEC needs it: without it the briefing text is drawn clipped to its text box (Q146). Otherwise
    // (old behaviour) the style is removed so that the GL output covers the children.
    s_hadClipChildren = (style & WS_CLIPCHILDREN) != 0;
    s_gameClipChildren = s_hadClipChildren;
    {   // Q174 keepcursor: the display counter of the game thread before the game's own ShowCursor calls
        int c = cursor_counter();
        s_cursorBase = c - s_gameShowDelta;
        HCURSOR cls = (HCURSOR)GetClassLongPtrA(hwnd, GCLP_HCURSOR);
        log_event("cursor: game thread %lu, display counter %d (the game's own ShowCursor so far %+ld), class cursor %p%s - "
                  "keepcursor %s", GetCurrentThreadId(), c, s_gameShowDelta, cls, cls ? "" : " (none: the arrow is used)",
                  g_cfg.keepCursor ? "on" : "off");
    }
    if (s_hadClipChildren && keep_clip_children()) {
        log_event("window_attach: WS_CLIPCHILDREN kept (the dialogs get the picture painted into them)");
        s_hadClipChildren = false;
    } else if (s_hadClipChildren) {
        SetWindowLongA(hwnd, GWL_STYLE, style & ~WS_CLIPCHILDREN);
        log_event("window_attach: WS_CLIPCHILDREN removed (child windows would hide the GL output)");
    }
    if (g_cfg.removeMenu) {
        HMENU menu = GetMenu(hwnd);
        if (menu) {
            // the client area keeps its size (it is the game screen of a DDSCL_NORMAL game)
            RECT c;
            GetClientRect(hwnd, &c);
            SetMenu(hwnd, NULL);
            AdjustWindowRectEx(&c, GetWindowLongA(hwnd, GWL_STYLE), FALSE, GetWindowLongA(hwnd, GWL_EXSTYLE));
            SetWindowPos(hwnd, NULL, 0, 0, c.right - c.left, c.bottom - c.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            log_event("remove_menu: menu bar %p taken off the window, client size kept", menu);
        } else {
            log_event("remove_menu: the window has no menu bar");
        }
    }
    // hotkeys are read from the thread's message queue, so they also work while a child dialog has focus;
    // the same hook re-addresses mouse messages (letterboxoffset) and tracks the modifier keys
    s_guiThread = GetWindowThreadProcessId(hwnd, NULL);
    g_gameThread = s_guiThread;
    if (!s_msgHook) {
        s_hotkeyMsg = RegisterWindowMessageA("QuantumDDraw.Hotkey");
        s_msgHook = SetWindowsHookExA(WH_GETMESSAGE, msg_hook, NULL, s_guiThread);
        if (g_cfg.keyToggleFullscreen || g_cfg.keyUnlockCursor || g_cfg.keyScreenshot || g_cfg.keyNextMonitor)
            s_kbHook = SetWindowsHookExA(WH_KEYBOARD, kb_hook, NULL, s_guiThread);
        log_event("hotkeys: Alt+0x%02X toggles fullscreen, Ctrl+0x%02X unlocks the cursor, 0x%02X saves a screenshot, "
                  "Ctrl+0x%02X next monitor (hook %s)",
                  g_cfg.keyToggleFullscreen, g_cfg.keyUnlockCursor, g_cfg.keyScreenshot, g_cfg.keyNextMonitor,
                  s_msgHook ? "installed" : "FAILED");
    }
    MonInfo m;
    monitor_of_window(hwnd, &m);
    char aw[128];
    dpi_describe(aw, sizeof(aw));
    monitors_log("window attach");
    log_event("window_attach: on monitor %s %p, window DPI %u (%u%%), monitor DPI %u; process %s", m.dev, m.h,
              dpi_of_window(hwnd), dpi_of_window(hwnd) * 100 / 96, m.dpi, aw);
}

void window_detach()
{
    if (s_hwnd && s_oldProc && IsWindow(s_hwnd)) {
        LONG_PTR cur = GetWindowLongPtrA(s_hwnd, GWLP_WNDPROC);
        if (cur == (LONG_PTR)sub_proc) {
            if (s_unicode) SetWindowLongPtrW(s_hwnd, GWLP_WNDPROC, (LONG_PTR)s_oldProc);
            else SetWindowLongPtrA(s_hwnd, GWLP_WNDPROC, (LONG_PTR)s_oldProc);
        }
        if (s_hadClipChildren)
            SetWindowLongA(s_hwnd, GWL_STYLE, GetWindowLongA(s_hwnd, GWL_STYLE) | WS_CLIPCHILDREN);
    }
    if (s_msgHook) {
        UnhookWindowsHookEx(s_msgHook);
        if (s_kbHook) UnhookWindowsHookEx(s_kbHook);
        s_kbHook = NULL;
        s_msgHook = NULL;
    }
    s_hadClipChildren = false;
    s_hwnd = NULL;
    s_oldProc = NULL;
    s_emu = false;
    s_presentInit = false;
    s_applied = false;
    s_present = PR_WINDOWED;
}

// --- display mode / presentation -------------------------------------------------------------

static void describe_mode(const char *dev, char *buf, size_t n)
{
    DEVMODEA dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(dev, ENUM_CURRENT_SETTINGS, &dm))
        _snprintf_s(buf, n, _TRUNCATE, "%lux%lu@%luHz", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
    else
        strcpy_s(buf, n, "?");
}

// The monitor the game window is on (most overlap; see monitor.cpp) - fullscreen, the real mode switch
// and the windowed size all follow the window, not the primary monitor. s_forceMon: the next
// presentation change goes to that monitor instead (keynextmonitor).
static MonInfo s_forceMon;
static bool s_forceMonSet;

static void window_monitor(MonInfo *m)
{
    if (s_forceMonSet)
        *m = s_forceMon;
    else
        monitor_of_window(s_hwnd, m);
}

// window frame for a client rectangle: per-monitor aware, the frame has the size of the monitor's DPI
static void adjust_for(RECT *r, LONG style, LONG ex)
{
    UINT dpi = 0;
    if (g_cfg.dpi == DPI_PERMONITOR) {
        MonInfo m;
        monitor_of_rect(*r, &m);
        dpi = m.fake ? dpi_of_window(s_hwnd) : m.dpi;
    }
    adjust_window_rect(r, style, ex, dpi);
}

static bool display_change(DDraw *dd)
{
    char before[48], after[48];
    MonInfo m;
    window_monitor(&m);
    const char *dev = m.dev;
    if (m.fake) {
        log_event("display mode: %s is the simulated monitor (monitortest) -> borderless fullscreen instead", dev);
        return false;
    }
    describe_mode(dev, before, sizeof(before));
    DEVMODEA dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = dd->modeW;
    dm.dmPelsHeight = dd->modeH;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;   // the desktop colour depth is kept, GL converts
    if (g_cfg.refreshRate > 0) {
        dm.dmDisplayFrequency = g_cfg.refreshRate;
        dm.dmFields |= DM_DISPLAYFREQUENCY;
    }
    LONG r = ChangeDisplaySettingsExA(dev, &dm, NULL, CDS_FULLSCREEN, NULL);
    if (r != DISP_CHANGE_SUCCESSFUL && (dm.dmFields & DM_DISPLAYFREQUENCY)) {
        log_event("ChangeDisplaySettingsEx %s %dx%d@%dHz failed (%ld), trying the default refresh rate", dev, dd->modeW,
                  dd->modeH, g_cfg.refreshRate, r);
        dm.dmFields &= ~DM_DISPLAYFREQUENCY;
        r = ChangeDisplaySettingsExA(dev, &dm, NULL, CDS_FULLSCREEN, NULL);
    }
    if (r != DISP_CHANGE_SUCCESSFUL) {
        log_event("ChangeDisplaySettingsEx %s %dx%d failed (%ld) -> borderless fullscreen instead", dev, dd->modeW,
                  dd->modeH, r);
        return false;
    }
    strcpy_s(s_modeDevice, dev);
    dd->realModeChanged = true;
    describe_mode(dev, after, sizeof(after));
    log_event("display mode %s: %s -> %s (switch #%ld)", dev, before, after, ++s_modeSwitches);
    return true;
}

static void display_restore(DDraw *dd, const char *why)
{
    if (!dd->realModeChanged)
        return;
    const char *dev = s_modeDevice[0] ? s_modeDevice : NULL;
    ChangeDisplaySettingsExA(dev, NULL, NULL, 0, NULL);
    dd->realModeChanged = false;
    char now[48];
    describe_mode(dev, now, sizeof(now));
    log_event("display mode restored (%s): %s now %s", why, dev ? dev : "", now);
}

// windowed state: the game's own frame, or borderless (popup; the sizing frame is kept but turned
// into client area by WM_NCCALCSIZE, borderless_hit provides drag and resize zones)
static LONG windowed_style()
{
    if (g_cfg.border)
        return s_framedStyle | WS_VISIBLE;
    LONG st = (s_framedStyle & ~(WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZEBOX)) | WS_POPUP | WS_VISIBLE;
    return g_cfg.resizable ? st | WS_THICKFRAME : st;
}

static LONG fullscreen_style()
{
    return (s_framedStyle & ~(WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZEBOX)) | WS_POPUP | WS_VISIBLE;
}

static LRESULT borderless_hit(LPARAM l)
{
    POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
    RECT wr;
    GetWindowRect(s_hwnd, &wr);
    const int edge = 6, corner = 16;
    bool left = p.x < wr.left + edge, right = p.x >= wr.right - edge;
    bool top = p.y < wr.top + edge, bottom = p.y >= wr.bottom - edge;
    if (g_cfg.resizable) {
        bool nearL = p.x < wr.left + corner, nearR = p.x >= wr.right - corner;
        bool nearT = p.y < wr.top + corner, nearB = p.y >= wr.bottom - corner;
        if ((top && nearL) || (left && nearT)) return HTTOPLEFT;
        if ((top && nearR) || (right && nearT)) return HTTOPRIGHT;
        if ((bottom && nearL) || (left && nearB)) return HTBOTTOMLEFT;
        if ((bottom && nearR) || (right && nearB)) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (bottom) return HTBOTTOM;
    }
    return top ? HTCAPTION : HTCLIENT;   // the top strip moves the window
}

static void remember_windowed()
{
    if (!s_presentInit || !s_applied || s_present != PR_WINDOWED || s_switching || IsIconic(s_hwnd) || IsZoomed(s_hwnd))
        return;
    RECT c;
    if (!GetClientRect(s_hwnd, &c) || c.right <= 0 || c.bottom <= 0)
        return;
    MapWindowPoints(s_hwnd, NULL, (POINT *)&c, 2);
    s_winClient = c;
}

// The window must be reachable: its top strip (where it is grabbed) has to overlap the work area of
// a monitor by at least 64x16 pixels, otherwise it is pushed onto the nearest monitor. Covers a
// remembered position on a monitor that is gone or has a different resolution now.
static bool make_visible(RECT *wr)
{
    RECT strip = {wr->left, wr->top, wr->right, wr->top + 32}, is;
    MonInfo mi;
    monitor_of_rect(strip, &mi);
    if (IntersectRect(&is, &strip, &mi.work) && is.right - is.left >= 64 && is.bottom - is.top >= 16)
        return false;
    const RECT &a = mi.work;
    long w = wr->right - wr->left, h = wr->bottom - wr->top, x = wr->left, y = wr->top;
    if (x + w > a.right) x = a.right - w;
    if (x < a.left) x = a.left;
    if (y + h > a.bottom) y = a.bottom - h;
    if (y < a.top) y = a.top;
    OffsetRect(wr, x - wr->left, y - wr->top);
    return true;
}

// --- savesettings: quantumddraw.state next to the DLL ---------------------------------------

static void state_path(char *path)
{
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&state_path, &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    char *name = slash ? slash + 1 : path;
    strcpy_s(name, MAX_PATH - (name - path), "quantumddraw.state");
}

static long state_long(const char *key, const char *path, bool *ok)
{
    char v[32];
    GetPrivateProfileStringA("window", key, "", v, sizeof(v), path);
    if (!v[0]) *ok = false;
    return atol(v);   // negative positions are valid (monitors left of / above the primary)
}

// The position is remembered twice: absolute (left / top) and relative to the monitor it was on
// (monitor = device name, rel_left / rel_top from that monitor's top-left corner). The relative one wins:
// it stays right when the primary monitor changes (all virtual-screen coordinates shift) or the monitor
// moved in the layout. A monitor that is gone: the same relative position on the primary monitor.
// make_visible then pulls the window into a work area if it still ends up outside.
static bool state_load(RECT *client, bool *fullscreen)
{
    char path[MAX_PATH], dev[32];
    state_path(path);
    bool ok = true;
    long x = state_long("left", path, &ok), y = state_long("top", path, &ok);
    long w = state_long("width", path, &ok), h = state_long("height", path, &ok);
    long fs = state_long("fullscreen", path, &ok);
    if (!ok || w <= 0 || h <= 0)
        return false;
    GetPrivateProfileStringA("window", "monitor", "", dev, sizeof(dev), path);
    bool relOk = dev[0] != 0;
    long rx = state_long("rel_left", path, &relOk), ry = state_long("rel_top", path, &relOk);
    if (relOk) {
        MonInfo m;
        bool found = monitor_by_device(dev, &m);
        if (!found)
            monitor_primary(&m);
        long nx = m.rc.left + rx, ny = m.rc.top + ry;
        log_event("savesettings: remembered on %s at (%ld,%ld) relative -> %s %s (%ld,%ld)-(%ld,%ld): client at (%ld,%ld)%s",
                  dev, rx, ry, found ? "found as" : "GONE, using the primary monitor", m.dev, m.rc.left, m.rc.top,
                  m.rc.right, m.rc.bottom, nx, ny, nx != x || ny != y ? " (the absolute position saved differs)" : "");
        x = nx;
        y = ny;
    }
    SetRect(client, x, y, x + w, y + h);
    *fullscreen = fs != 0;
    return true;
}

void window_save_state()
{
    if (!g_cfg.saveSettings || !s_presentInit || !s_hwnd)
        return;
    remember_windowed();
    static RECT saved;
    static int savedFs = -1;
    int fs = s_present == PR_FULLSCREEN;
    if (savedFs == fs && EqualRect(&saved, &s_winClient))
        return;
    char path[MAX_PATH], v[32];
    state_path(path);
    MonInfo m;
    monitor_of_rect(s_winClient, &m);
    const long vals[7] = {s_winClient.left, s_winClient.top, s_winClient.right - s_winClient.left,
                          s_winClient.bottom - s_winClient.top, (long)fs, s_winClient.left - m.rc.left,
                          s_winClient.top - m.rc.top};
    const char *const keys[7] = {"left", "top", "width", "height", "fullscreen", "rel_left", "rel_top"};
    for (int i = 0; i < 7; i++) {
        _snprintf_s(v, sizeof(v), _TRUNCATE, "%ld", vals[i]);
        WritePrivateProfileStringA("window", keys[i], v, path);
    }
    WritePrivateProfileStringA("window", "monitor", m.dev, path);
    saved = s_winClient;
    savedFs = fs;
    log_event("savesettings: client (%ld,%ld) %ldx%ld, fullscreen=%d, monitor %s relative (%ld,%ld) -> %s", vals[0],
              vals[1], vals[2], vals[3], fs, m.dev, vals[5], vals[6], path);
}

// --- switching -----------------------------------------------------------------------------

static void center_cursor(const char *why);

static void present_set(int to, const char *why)
{
    DDraw *dd = g_dd;
    if (!s_hwnd || !dd || !dd->modeSet || !s_presentInit)
        return;
    int from = s_present;
    if (!s_keepWinClient)
        remember_windowed();
    s_switching = true;
    if (IsIconic(s_hwnd))
        ShowWindow(s_hwnd, SW_RESTORE);
    if (from == PR_EXCLUSIVE && to != PR_EXCLUSIVE)
        display_restore(dd, why);
    if (to == PR_EXCLUSIVE && !display_change(dd))
        to = PR_FULLSCREEN;

    RECT r;
    LONG style, ex;
    if (to == PR_WINDOWED) {
        style = windowed_style();
        ex = s_framedEx;
        r = s_winClient;
        if (g_cfg.border)
            adjust_for(&r, style, ex);
        long borderL = s_winClient.left - r.left, clientW = s_winClient.right - s_winClient.left;
        make_visible(&r);
        // a window wider than the monitor (QEC: client = the screen width): its frame goes off-screen, not 3 columns
        // of the picture (Q155, like cnc-ddraw)
        MonInfo fm;
        monitor_of_rect(r, &fm);
        long dx = fit_client_dx(r.left, r.right, r.left + borderL, r.left + borderL + clientW, fm.work.left, fm.work.right);
        if (dx) {
            OffsetRect(&r, dx, 0);
            log_event("presentation: window %ld px wider than the work area of %s - frame moved off-screen by %ld px, the "
                      "client (%ld wide) is fully visible", (r.right - r.left) - (fm.work.right - fm.work.left), fm.dev, dx,
                      clientW);
        }
    } else {
        style = fullscreen_style();
        ex = s_framedEx & ~(WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE);
        MonInfo m;
        window_monitor(&m);   // after a mode switch: the monitor at its new size
        if (s_forceMonSet && !m.fake)
            monitor_by_device(m.dev, &m);
        r = m.rc;
    }
    s_forceMonSet = false;
    s_present = to;   // before SetWindowPos: WM_NCCALCSIZE / WM_NCHITTEST look at it
    SetWindowLongA(s_hwnd, GWL_STYLE, style);
    SetWindowLongA(s_hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(s_hwnd, to == PR_WINDOWED ? HWND_NOTOPMOST : HWND_TOP, r.left, r.top, r.right - r.left,
                 r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    s_switching = false;
    s_applied = true;

    RECT cr, wr;
    GetClientRect(s_hwnd, &cr);
    GetWindowRect(s_hwnd, &wr);
    MonInfo on;
    monitor_of_window(s_hwnd, &on);
    log_event("presentation %s -> %s (%s): window (%ld,%ld)-(%ld,%ld), client %ldx%ld, game %dx%d, monitor %s %p "
              "(%ld,%ld)-(%ld,%ld), window DPI %u", s_prName[from], s_prName[to], why, wr.left, wr.top, wr.right,
              wr.bottom, cr.right, cr.bottom, dd->modeW, dd->modeH, on.dev, on.h, on.rc.left, on.rc.top, on.rc.right,
              on.rc.bottom, dpi_of_window(s_hwnd));
    render_rebuild(why);
    apply_clip();
    center_cursor(why);
    window_save_state();
    monitor_check(why);   // which monitor, where the picture is, game <-> screen round trip
}

// Called once the emulated game screen exists: framedStyle / client describe the windowed state
// (client in screen coordinates). force = the window has to be restyled even if nothing changes.
static void present_init(LONG framedStyle, LONG framedEx, const RECT &client, bool force)
{
    static bool stateRead;
    s_framedStyle = framedStyle & ~(WS_VISIBLE | WS_MINIMIZE | WS_MAXIMIZE | (keep_clip_children() ? 0 : WS_CLIPCHILDREN));
    s_framedEx = framedEx;
    if (!s_presentInit)
        s_winClient = client;
    s_presentInit = true;
    bool fs = g_cfg.fullscreen;
    RECT saved;
    bool savedFs;
    if (!stateRead && g_cfg.saveSettings && state_load(&saved, &savedFs)) {
        if (!(windowed_style() & WS_THICKFRAME)) {   // fixed size window: only the position is restored
            saved.right = saved.left + (client.right - client.left);
            saved.bottom = saved.top + (client.bottom - client.top);
        }
        log_event("savesettings: restoring client (%ld,%ld) %ldx%ld, fullscreen=%d", saved.left, saved.top,
                  saved.right - saved.left, saved.bottom - saved.top, (int)savedFs);
        s_winClient = saved;
        fs = savedFs;
    }
    if (!stateRead && g_cfg.centerWindow) {
        // the window (frame included) in the middle of the work area of the monitor the game created it
        // on (Windows puts a new window on the monitor of the process' start / the primary), size unchanged
        MonInfo mi;
        monitor_of_rect(client, &mi);
        RECT w = s_winClient;
        if (g_cfg.border)
            adjust_for(&w, windowed_style(), s_framedEx);
        const RECT &wa = mi.work;
        int x = wa.left + ((wa.right - wa.left) - (w.right - w.left)) / 2;
        int y = wa.top + ((wa.bottom - wa.top) - (w.bottom - w.top)) / 2;
        if (x < wa.left) x = wa.left;
        if (y < wa.top) y = wa.top;
        OffsetRect(&s_winClient, x - w.left, y - w.top);
        log_event("center_window: client %ldx%ld -> (%ld,%ld) on monitor %s (work area (%ld,%ld)-(%ld,%ld))",
                  s_winClient.right - s_winClient.left, s_winClient.bottom - s_winClient.top, s_winClient.left,
                  s_winClient.top, mi.dev, wa.left, wa.top, wa.right, wa.bottom);
    }
    stateRead = true;
    int to = !g_cfg.windowed ? PR_EXCLUSIVE : fs ? PR_FULLSCREEN : PR_WINDOWED;
    if (force || to != PR_WINDOWED || !g_cfg.border || !EqualRect(&s_winClient, &client)) {
        present_set(to, "start");
    } else {
        s_applied = true;
        // QEC creates its window at (0,0) frame included: with a client as wide as the screen the frame pushes the
        // last columns of the picture past the right edge. Move it (no restyle) so the frame is off-screen instead.
        RECT wr;
        GetWindowRect(s_hwnd, &wr);
        MonInfo fm;
        monitor_of_rect(wr, &fm);
        long dx = fit_client_dx(wr.left, wr.right, client.left, client.right, fm.work.left, fm.work.right);
        if (dx) {
            SetWindowPos(s_hwnd, NULL, wr.left + dx, wr.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            remember_windowed();   // s_winClient = the client where it is now
            log_event("presentation: windowed, window left as the game created it but moved %ld px: its frame is "
                      "off-screen, the client (%ld,%ld)-(%ld,%ld) fully on %s", dx, s_winClient.left, s_winClient.top,
                      s_winClient.right, s_winClient.bottom, fm.dev);
        } else {
            log_event("presentation: windowed, window left as the game created it");
        }
        center_cursor("start");
    }
}

void window_apply_mode(DDraw *dd)
{
    if (!s_hwnd || !dd->modeSet)
        return;
    if (s_appliedW == dd->modeW && s_appliedH == dd->modeH)
        return;
    s_appliedW = dd->modeW;
    s_appliedH = dd->modeH;

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    if (g_cfg.resizable)
        style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    // Q170: the style is built anew here, so the game's WS_CLIPCHILDREN was lost for an exclusive-mode game (QEC with
    // Video.Windowed=False). Without it QEC's dialogs (briefing, Esc menu) never got their WM_PAINT after a real display
    // mode switch and were never drawn into the primary: a black screen. Kept as window_attach keeps it.
    if (s_gameClipChildren && keep_clip_children())
        style |= WS_CLIPCHILDREN;
    DWORD ex = (DWORD)GetWindowLongA(s_hwnd, GWL_EXSTYLE) & ~(WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
    ex |= WS_EX_APPWINDOW;

    MonInfo mon;
    window_monitor(&mon);
    const RECT wa = mon.work;
    UINT dpi = g_cfg.dpi == DPI_PERMONITOR ? (mon.fake ? dpi_of_window(s_hwnd) : mon.dpi) : 0;
    int scale = g_cfg.windowScale;
    if (scale <= 0) {
        scale = 1;
        for (int s = 2; s <= 8; s++) {
            RECT r = {0, 0, dd->modeW * s, dd->modeH * s};
            adjust_window_rect(&r, style, ex, dpi);
            if (r.right - r.left > wa.right - wa.left || r.bottom - r.top > wa.bottom - wa.top)
                break;
            scale = s;
        }
    }
    RECT r = {0, 0, dd->modeW * scale, dd->modeH * scale};
    adjust_window_rect(&r, style, ex, dpi);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int x = wa.left + ((wa.right - wa.left) - ww) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - wh) / 2;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;
    RECT client = {x - r.left, y - r.top, x - r.left + dd->modeW * scale, y - r.top + dd->modeH * scale};
    s_emu = g_cfg.hookMouse;
    log_event("emulated mode %dx%d: client scale %d, window %dx%d at (%d,%d) on monitor %s (work area (%ld,%ld)-(%ld,%ld), "
              "DPI %u)", dd->modeW, dd->modeH, scale, ww, wh, x, y, mon.dev, wa.left, wa.top, wa.right, wa.bottom, mon.dpi);
    s_presentInit = false;   // a new game resolution: the windowed rectangle is computed again
    s_applied = false;
    present_init((LONG)style, (LONG)ex, client, true);
    render_force();
}

void window_restore_mode(DDraw *dd)
{
    display_restore(dd, "RestoreDisplayMode / release");
    s_appliedW = s_appliedH = 0;
    ClipCursor(NULL);
}

// --- multiple monitors ------------------------------------------------------------------------

// after a move to another monitor: where the picture is and how game coordinates map to the screen
// (corners and centre game -> screen -> game; a round trip must give the same point back)
static void monitor_check(const char *why)
{
    RECT vp, cr;
    POINT o;
    if (!emu_active() || !game_viewport(&vp, &o))
        return;
    GetClientRect(s_hwnd, &cr);
    MonInfo m;
    monitor_of_window(s_hwnd, &m);
    const POINT pts[5] = {{0, 0}, {g_dd->modeW - 1, 0}, {0, g_dd->modeH - 1}, {g_dd->modeW - 1, g_dd->modeH - 1},
                          {g_dd->modeW / 2, g_dd->modeH / 2}};
    char buf[400] = "";
    int worst = 0;
    for (int i = 0; i < 5; i++) {
        POINT s = pts[i], back;
        emu_game_to_screen(&s);
        back = s;
        emu_screen_to_game(&back);
        int err = abs(back.x - pts[i].x) > abs(back.y - pts[i].y) ? abs(back.x - pts[i].x) : abs(back.y - pts[i].y);
        if (err > worst) worst = err;
        char one[80];
        _snprintf_s(one, sizeof(one), _TRUNCATE, " (%ld,%ld)->(%ld,%ld)->(%ld,%ld)", pts[i].x, pts[i].y, s.x, s.y, back.x, back.y);
        strcat_s(buf, one);
    }
    bool inside = o.x >= m.rc.left && o.y >= m.rc.top && o.x + cr.right <= m.rc.right && o.y + cr.bottom <= m.rc.bottom;
    log_event("monitor check (%s): monitor %s handle %p rect (%ld,%ld)-(%ld,%ld) work (%ld,%ld)-(%ld,%ld) DPI %u%s; client "
              "%ldx%ld at (%ld,%ld)%s, picture (%ld,%ld)-(%ld,%ld) = scale %.3f; game->screen->game:%s; worst round-trip "
              "error %d px", why, m.dev, m.h, m.rc.left, m.rc.top, m.rc.right, m.rc.bottom, m.work.left, m.work.top,
              m.work.right, m.work.bottom, m.dpi, m.fake ? " (SIMULATED)" : "", cr.right, cr.bottom, o.x, o.y,
              inside ? " inside the monitor" : " NOT inside the monitor", vp.left, vp.top, vp.right, vp.bottom,
              (double)(vp.right - vp.left) / g_dd->modeW, buf, worst);
}

// keynextmonitor: the game goes to the next monitor (enumeration order, wrapping). Windowed: the window
// keeps its position relative to the monitor's work area (pulled inside if it does not fit); fullscreen
// covers the next monitor; a real mode switch is undone on the old monitor and done on the new one.
static void monitor_next(const char *why)
{
    if (!s_presentInit || !g_dd || !g_dd->modeSet)
        return;
    MonInfo list[16], cur;
    int n = monitors_list(list, 16);
    monitor_of_window(s_hwnd, &cur);
    int at = 0;
    for (int i = 0; i < n; i++)
        if (list[i].h == cur.h) at = i;
    const MonInfo &to = list[(at + 1) % n];
    log_event("next monitor (%s): %d monitor(s), from #%d %s %p to #%d %s %p (%ld,%ld)-(%ld,%ld), work (%ld,%ld)-(%ld,%ld), "
              "DPI %u%s", why, n, at + 1, cur.dev, cur.h, (at + 1) % n + 1, to.dev, to.h, to.rc.left, to.rc.top,
              to.rc.right, to.rc.bottom, to.work.left, to.work.top, to.work.right, to.work.bottom, to.dpi,
              to.fake ? " SIMULATED" : "");
    remember_windowed();
    OffsetRect(&s_winClient, to.work.left - cur.work.left, to.work.top - cur.work.top);
    RECT w = s_winClient;
    if (g_cfg.border)
        adjust_window_rect(&w, windowed_style(), s_framedEx, 0);
    long dx = 0, dy = 0;   // pull the whole window into the new work area where it fits
    if (w.right > to.work.right) dx = to.work.right - w.right;
    if (w.left + dx < to.work.left) dx = to.work.left - w.left;
    if (w.bottom > to.work.bottom) dy = to.work.bottom - w.bottom;
    if (w.top + dy < to.work.top) dy = to.work.top - w.top;
    OffsetRect(&s_winClient, dx, dy);
    s_forceMon = to;
    s_forceMonSet = true;
    s_keepWinClient = true;
    int pr = s_present;
    if (pr == PR_EXCLUSIVE)
        display_restore(g_dd, "next monitor");
    present_set(pr, "next monitor");
    s_keepWinClient = false;
    s_forceMonSet = false;
}

// --- cursor API remapping through IAT patching ------------------------------------------------

typedef BOOL(WINAPI *PFN_GetCursorPos)(LPPOINT);
typedef BOOL(WINAPI *PFN_SetCursorPos)(int, int);
typedef BOOL(WINAPI *PFN_ClipCursor)(const RECT *);
typedef BOOL(WINAPI *PFN_PtConv)(HWND, LPPOINT);
typedef BOOL(WINAPI *PFN_GetRect)(HWND, LPRECT);

static PFN_GetCursorPos r_GetCursorPos;
static PFN_SetCursorPos r_SetCursorPos;
static PFN_ClipCursor r_ClipCursor;
static PFN_PtConv r_ScreenToClient, r_ClientToScreen;
static PFN_GetRect r_GetClientRect, r_GetWindowRect;

static POINT client_origin();

static BOOL WINAPI h_GetCursorPos(LPPOINT p)
{
    jr_count(JC_CURSOR);
    BOOL ok = r_GetCursorPos(p);
    if (!ok || !p || !emu_active())
        return ok;
    // A fullscreen game never sees the cursor leave the screen. On the window frame / caption (where
    // the cursor may go so the window can be dragged) the position is clamped to the nearest edge,
    // which keeps edge scrolling working. Outside the window (in the background, not clipped) the
    // last position inside is reported, otherwise clamping would trigger edge scrolling.
    static POINT last = {-1, -1};
    RECT vp, wr;
    POINT o, real = *p;
    if (!game_viewport(&vp, &o))
        return ok;
    POINT c = {p->x - o.x, p->y - o.y}, cl = c;
    bool onWindow = r_GetWindowRect(s_hwnd, &wr) && PtInRect(&wr, *p);
    POINT g;
    if (adj_update(&g)) {
        last = g;
    } else if (s_active && (PtInRect(&vp, c) || onWindow)) {
        if (c.x < vp.left) c.x = vp.left;
        if (c.y < vp.top) c.y = vp.top;
        if (c.x >= vp.right) c.x = vp.right - 1;
        if (c.y >= vp.bottom) c.y = vp.bottom - 1;
        client_to_game(&c, vp);
        last = c;
    } else if (last.x < 0) {
        last.x = g_dd->modeW / 2;
        last.y = g_dd->modeH / 2;
    }
    *p = last;
    if (g_cfg.mouseProbe) {
        // debug: one line per new real position, for checking the mapping from outside
        static POINT logged = {-99999, -99999};
        static LONG n;
        if ((real.x != logged.x || real.y != logged.y) && n < 3000) {
            n++;
            logged = real;
            log_event("mouseprobe real (%ld,%ld) client (%ld,%ld) viewport (%ld,%ld)-(%ld,%ld) -> game (%ld,%ld)%s", real.x,
                      real.y, cl.x, cl.y, vp.left, vp.top, vp.right, vp.bottom,
                      last.x, last.y, s_adjOn ? " adjmouse" : "");
        }
    }
    return ok;
}

static BOOL WINAPI h_SetCursorPos(int x, int y)
{
    jr(J_HOOKCALL, JH_SETCURSORPOS, x, y);
    adj_reset();
    if (emu_active()) {
        POINT p = {x, y};
        emu_game_to_screen(&p);
        return r_SetCursorPos(p.x, p.y);
    }
    return r_SetCursorPos(x, y);
}

// The game's ClipCursor rectangle (game coordinates) is kept and re-applied from the live window
// position after every move, so the clip never stays behind at a stale screen position. It is
// widened by the caption bar so the window can still be grabbed and dragged, and released while
// Windows runs the move / size loop.
static bool s_gameClip;
static RECT s_gameClipRect;

static void apply_clip()
{
    if (!r_ClipCursor)
        return;
    // Locked cursor: the game's clip, and in fullscreen the game picture (the cursor stays off the
    // black bars and other monitors). Ctrl+<keyunlockcursor> releases it.
    bool full = s_presentInit && s_present != PR_WINDOWED;
    bool lockPic = s_dragClip || s_lockSticky;   // lockcursor: the picture, while dragging / after a click
    if (!emu_active() || (!s_gameClip && !full && !lockPic) || s_cursorFree || !s_active || s_sizeMove || IsIconic(s_hwnd)) {
        s_clipActive = false;
        r_ClipCursor(NULL);
        return;
    }
    s_clipActive = true;
    RECT gc = s_gameClipRect;
    if (!s_gameClip)
        SetRect(&gc, 0, 0, g_dd->modeW, g_dd->modeH);
    POINT a = {gc.left, gc.top}, b = {gc.right, gc.bottom};
    emu_game_to_screen(&a);
    emu_game_to_screen(&b);
    RECT sr = {a.x, a.y, b.x, b.y};
    RECT wr;
    if (s_gameClip && !lockPic && (GetWindowLongA(s_hwnd, GWL_STYLE) & WS_CAPTION) == WS_CAPTION &&
        r_GetWindowRect(s_hwnd, &wr) && gc.top <= 0)
        sr.top = wr.top;   // caption reachable (not while a drag is confined to the picture)
    r_ClipCursor(&sr);
}

// lockcursor (Q147), game thread (message hook): a press inside the game's picture with lockcursor=1 confines
// the cursor to the picture until every button is up again - a box selection dragged towards a corner can
// no longer end outside the window (on the taskbar's "show desktop" corner, another application), where the
// game would never get the button-up. lockcursor=2 keeps it confined after the first click, like cnc-ddraw's
// mouse lock (released by focus loss or Ctrl+<keyunlockcursor>).
static void lockcursor_track(const MSG *m)
{
    int bit = 0;
    bool down = false;
    switch (m->message) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: bit = 1; down = true; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: bit = 2; down = true; break;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: bit = 4; down = true; break;
    case WM_LBUTTONUP: bit = 1; break;
    case WM_RBUTTONUP: bit = 2; break;
    case WM_MBUTTONUP: bit = 4; break;
    case WM_MOUSEMOVE:
        if (s_btnMask && m->hwnd == s_btnHwnd)
            s_btnLparam = m->lParam;
        return;
    default:
        return;
    }
    if (!m->hwnd || !game_related(m->hwnd))
        return;
    if (down) {
        InterlockedOr(&s_btnMask, bit);
        s_btnHwnd = m->hwnd;
        s_btnLparam = m->lParam;
        if (g_cfg.lockCursor && emu_active() && !s_cursorFree && s_active) {
            if (g_cfg.lockCursor >= 2)
                s_lockSticky = true;
            else
                s_dragClip = true;
            apply_clip();
        }
        return;
    }
    InterlockedAnd(&s_btnMask, ~bit);
    if (!(m->wParam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))) {
        InterlockedExchange(&s_btnMask, 0);
        if (s_dragClip) {
            s_dragClip = false;
            apply_clip();
        }
    }
}

// Focus lost (game thread): buttons the game saw go down but that are physically up got their button-up
// somewhere else (another window took the focus mid-drag) - the game gets it now, at the last position it saw.
// With noactivateapp the game is never told about the focus loss, so without this it would stay in the drag.
static void release_stuck_buttons(const char *why)
{
    s_dragClip = false;
    s_lockSticky = false;
    LONG mask = InterlockedExchange(&s_btnMask, 0);
    if (!mask || !s_btnHwnd || !IsWindow(s_btnHwnd))
        return;
    static const struct { LONG bit; int vk; UINT up; const char *name; } b[] = {
        {1, VK_LBUTTON, WM_LBUTTONUP, "left"}, {2, VK_RBUTTON, WM_RBUTTONUP, "right"}, {4, VK_MBUTTON, WM_MBUTTONUP, "middle"}};
    for (const auto &x : b) {
        if (!(mask & x.bit) || GetAsyncKeyState(x.vk) < 0)
            continue;   // not pressed for the game, or still physically held
        PostMessageA(s_btnHwnd, x.up, 0, s_btnLparam);
        log_event("lockcursor: %s button-up posted to %p at game (%d,%d) - the real one went elsewhere (%s)", x.name,
                  s_btnHwnd, (int)(short)LOWORD(s_btnLparam), (int)(short)HIWORD(s_btnLparam), why);
    }
}

// render thread, about once a second: a confined drag whose button-up the game never got (all buttons
// physically up for two checks in a row) releases the cursor anyway
static void lockcursor_watch()
{
    static int upTicks;
    if (!s_dragClip) {
        upTicks = 0;
        return;
    }
    bool any = GetAsyncKeyState(VK_LBUTTON) < 0 || GetAsyncKeyState(VK_RBUTTON) < 0 || GetAsyncKeyState(VK_MBUTTON) < 0;
    if (any) {
        upTicks = 0;
        return;
    }
    if (++upTicks >= 2) {
        upTicks = 0;
        s_dragClip = false;
        if (r_ClipCursor && !s_lockSticky && !(s_presentInit && s_present != PR_WINDOWED) && !s_gameClip)
            r_ClipCursor(NULL);
        log_event("lockcursor: drag clip released by the watchdog (no button physically held)");
    }
}

static BOOL WINAPI h_ClipCursor(const RECT *r)
{
    jr(J_HOOKCALL, JH_CLIPCURSOR, r ? (ULONG_PTR)(r->left << 16 | (r->top & 0xFFFF)) : 0, r ? (ULONG_PTR)(r->right << 16 | (r->bottom & 0xFFFF)) : 0);
    if (emu_active()) {
        s_gameClip = r != NULL;
        if (r) s_gameClipRect = *r;
        apply_clip();
        return TRUE;
    }
    return r_ClipCursor(r);
}

static void clip_on_message(UINT m)
{
    if (m == WM_ENTERSIZEMOVE) {
        s_sizeMove = true;
        log_event("WM_ENTERSIZEMOVE (cursor clip released while the window is moved)");
    } else if (m == WM_EXITSIZEMOVE) {
        s_sizeMove = false;
        RECT wr;
        GetWindowRect(s_hwnd, &wr);
        log_event("WM_EXITSIZEMOVE: window now at (%ld,%ld)-(%ld,%ld)", wr.left, wr.top, wr.right, wr.bottom);
    }
    if (emu_active())
        apply_clip();
}

void window_normal_emu(DDraw *dd)
{
    if (!s_hwnd)
        return;
    s_appliedW = dd->modeW;
    s_appliedH = dd->modeH;
    s_emu = g_cfg.hookMouse;
    RECT c;
    GetClientRect(s_hwnd, &c);
    MapWindowPoints(s_hwnd, NULL, (POINT *)&c, 2);
    present_init(GetWindowLongA(s_hwnd, GWL_STYLE), GetWindowLongA(s_hwnd, GWL_EXSTYLE), c, false);
    render_force();
}

// center_cursor_fix: after the game screen appears or changes presentation the cursor is put in its
// middle, so it does not start in a corner (edge scrolling) or outside the window. Only while the
// game is in the foreground - the cursor belongs to whoever uses the desktop otherwise.
static void center_cursor(const char *why)
{
    if (!g_cfg.centerCursorFix || !emu_active() || IsIconic(s_hwnd) || GetForegroundWindow() != s_hwnd)
        return;
    POINT c = {g_dd->modeW / 2, g_dd->modeH / 2};
    emu_game_to_screen(&c);
    SetCursorPos(c.x, c.y);
    adj_reset();
    log_event("center_cursor_fix (%s): cursor -> screen (%ld,%ld) = game (%d,%d)", why, c.x, c.y, g_dd->modeW / 2,
              g_dd->modeH / 2);
}

// adjmouse: while the cursor is locked to the game, its movement is scaled. The game position is
// kept as a fraction, every real movement is added times the factor, and the real cursor is put
// back onto the game position - so the next movement starts there and GetCursorPos and the mouse
// messages always agree (no jitter). Off (1.0), unlocked, inactive or during a window drag the
// position is mapped directly.

static void adj_reset() { s_adjOn = false; }

static bool adj_update(POINT *game)
{
    if (g_cfg.adjMouse == 1.0f || !emu_active() || !s_clipActive || !s_active || s_sizeMove || IsIconic(s_hwnd) ||
        !r_GetCursorPos || !r_SetCursorPos) {
        s_adjOn = false;
        return false;
    }
    RECT vp;
    POINT o, real;
    if (!game_viewport(&vp, &o) || !r_GetCursorPos(&real))
        return false;
    DDraw *dd = g_dd;
    if (!s_adjOn) {
        POINT c = {real.x - o.x, real.y - o.y};
        if (!PtInRect(&vp, c))
            return false;   // starts once the cursor is over the game picture
        client_to_game(&c, vp);
        s_adjX = c.x + 0.5;
        s_adjY = c.y + 0.5;
        s_adjReal = real;
        s_adjOn = true;
    } else if (real.x != s_adjReal.x || real.y != s_adjReal.y) {
        double f = g_cfg.adjMouse;
        s_adjX += (real.x - s_adjReal.x) * f * dd->modeW / (vp.right - vp.left);
        s_adjY += (real.y - s_adjReal.y) * f * dd->modeH / (vp.bottom - vp.top);
        if (s_adjX < 0) s_adjX = 0;
        if (s_adjY < 0) s_adjY = 0;
        if (s_adjX >= dd->modeW) s_adjX = dd->modeW - 0.001;
        if (s_adjY >= dd->modeH) s_adjY = dd->modeH - 0.001;
        POINT back = {(long)s_adjX, (long)s_adjY};
        emu_game_to_screen(&back);
        if (back.x != real.x || back.y != real.y) {
            r_SetCursorPos(back.x, back.y);
            r_GetCursorPos(&real);   // the clip may have moved it
        }
        s_adjReal = real;
    }
    game->x = (long)s_adjX;
    game->y = (long)s_adjY;
    return true;
}

typedef int(WINAPI *PFN_GetSystemMetrics)(int);
typedef BOOL(WINAPI *PFN_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
typedef BOOL(WINAPI *PFN_MoveWindow)(HWND, int, int, int, int, BOOL);
static PFN_GetSystemMetrics r_GetSystemMetrics;
static PFN_SetWindowPos r_SetWindowPos;
static PFN_MoveWindow r_MoveWindow;

static int WINAPI h_GetSystemMetrics(int i)
{
    if ((i == SM_CXSCREEN || i == SM_CYSCREEN) && emu_active())
        return i == SM_CXSCREEN ? g_dd->modeW : g_dd->modeH;
    return r_GetSystemMetrics(i);
}

// game position of a top-level popup -> screen: inside the picture (letterboxoffset), or 1:1 from the
// client origin
static void popup_to_screen(int *x, int *y)
{
    POINT p = {*x, *y};
    if (g_cfg.letterboxOffset) {
        emu_game_to_screen(&p);
    } else {
        POINT o = client_origin();
        p.x += o.x;
        p.y += o.y;
    }
    *x = p.x;
    *y = p.y;
}

// The emulated screen does not move: the game must not place its main window (the user does),
// and its top-level popups are positioned in game coordinates.
static BOOL WINAPI h_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT f)
{
    jr(J_HOOKCALL, JH_SETWINDOWPOS, (ULONG_PTR)h, f);
    if (emu_active() && h) {
        if (h == s_hwnd) {
            if ((f & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE)) {
                static int logged;
                if (logged++ < 10)
                    log_event("SetWindowPos on the game window (%d,%d %dx%d flags 0x%X): move/size ignored", x, y, cx,
                              cy, f);
                f |= SWP_NOMOVE | SWP_NOSIZE;
            }
        } else if (!(f & SWP_NOMOVE) && !(GetWindowLongA(h, GWL_STYLE) & WS_CHILD)) {
            popup_to_screen(&x, &y);
        }
    }
    return r_SetWindowPos(h, after, x, y, cx, cy, f);
}

static BOOL WINAPI h_MoveWindow(HWND h, int x, int y, int cx, int cy, BOOL repaint)
{
    jr(J_HOOKCALL, JH_MOVEWINDOW, (ULONG_PTR)h);
    if (emu_active() && h) {
        if (h == s_hwnd) {
            static int logged;
            if (logged++ < 10)
                log_event("MoveWindow on the game window (%d,%d %dx%d): ignored", x, y, cx, cy);
            return TRUE;
        }
        if (!(GetWindowLongA(h, GWL_STYLE) & WS_CHILD))
            popup_to_screen(&x, &y);
    }
    return r_MoveWindow(h, x, y, cx, cy, repaint);
}

// While fullscreen is emulated the game believes its window is the whole
// screen: "game screen" coordinates = real screen - client origin of the main
// window. This applies to the main window and to its child dialogs (YR's
// in-game menus are #32770 children that the game draws into the primary at
// their screen position).
static bool game_related(HWND h)
{
    return h && s_hwnd && (h == s_hwnd || IsChild(s_hwnd, h));
}

static POINT client_origin()
{
    POINT o = {0, 0};
    r_ClientToScreen(s_hwnd, &o);
    return o;
}

// trace=true: geometry questions the game asks about windows other than its main window (first 80)
static void trace_geom(const char *fn, HWND h, const RECT *r, const POINT *p, void *ret)
{
    static LONG n;
    static struct { const char *fn; HWND h; void *ret; RECT v; } seen[512];
    if (!g_cfg.trace || h == s_hwnd || n >= 512)
        return;
    RECT v = {0, 0, 0, 0};
    if (r) v = *r;
    else if (p) SetRect(&v, p->x, p->y, 0, 0);
    for (LONG i = 0; i < n; i++)   // each (call, window, caller, result) once
        if (seen[i].fn == fn && seen[i].h == h && seen[i].ret == ret && EqualRect(&seen[i].v, &v))
            return;
    seen[n].fn = fn;
    seen[n].h = h;
    seen[n].ret = ret;
    seen[n].v = v;
    n++;
    char cls[32] = "", text[32] = "";
    if (h) {
        GetClassNameA(h, cls, sizeof(cls));
        window_text(h, text, sizeof(text));
    }
    char mod[MAX_PATH] = "?";
    HMODULE m = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ret, &m))
        GetModuleFileNameA(m, mod, sizeof(mod));
    const char *base = strrchr(mod, '\\');
    if (r)
        log_event("trace geom %s(%p %s \"%s\") -> (%ld,%ld)-(%ld,%ld) from %s+0x%lX", fn, h, cls, text, r->left, r->top, r->right,
                  r->bottom, base ? base + 1 : mod, (unsigned long)((BYTE *)ret - (BYTE *)m));
    else if (p)
        log_event("trace geom %s(%p %s \"%s\") -> (%ld,%ld) from %s+0x%lX", fn, h, cls, text, p->x, p->y,
                  base ? base + 1 : mod, (unsigned long)((BYTE *)ret - (BYTE *)m));
}

static BOOL WINAPI h_ScreenToClient(HWND h, LPPOINT p)
{
    jr_count(JC_CURSOR);
    if (p && emu_active() && game_related(h)) {
        POINT o = client_origin();
        p->x += o.x;
        p->y += o.y;
    }
    BOOL ok = r_ScreenToClient(h, p);
    trace_geom("ScreenToClient", h, NULL, p, _ReturnAddress());
    return ok;
}

static BOOL WINAPI h_ClientToScreen(HWND h, LPPOINT p)
{
    jr_count(JC_CURSOR);
    BOOL ok = r_ClientToScreen(h, p);
    if (ok && p && emu_active() && game_related(h)) {
        POINT o = client_origin();
        p->x -= o.x;
        p->y -= o.y;
    }
    trace_geom("ClientToScreen", h, NULL, p, _ReturnAddress());
    return ok;
}

static BOOL WINAPI h_GetClientRect(HWND h, LPRECT r)
{
    jr_count(JC_CURSOR);
    if (h == s_hwnd && r && emu_active()) {
        SetRect(r, 0, 0, g_dd->modeW, g_dd->modeH);
        return TRUE;
    }
    BOOL ok = r_GetClientRect(h, r);
    trace_geom("GetClientRect", h, r, NULL, _ReturnAddress());
    return ok;
}

static BOOL WINAPI h_GetWindowRect(HWND h, LPRECT r)
{
    jr_count(JC_CURSOR);
    if (r && emu_active() && game_related(h)) {
        if (h == s_hwnd) {
            SetRect(r, 0, 0, g_dd->modeW, g_dd->modeH);
            return TRUE;
        }
        BOOL ok = r_GetWindowRect(h, r);
        POINT o = client_origin();
        OffsetRect(r, -o.x, -o.y);
        trace_geom("GetWindowRect", h, r, NULL, _ReturnAddress());
        return ok;
    }
    BOOL ok = r_GetWindowRect(h, r);
    trace_geom("GetWindowRect", h, r, NULL, _ReturnAddress());
    return ok;
}

typedef int(WINAPI *PFN_MapWindowPoints)(HWND, HWND, LPPOINT, UINT);
typedef HWND(WINAPI *PFN_WindowFromPoint)(POINT);
static PFN_MapWindowPoints r_MapWindowPoints;
static PFN_WindowFromPoint r_WindowFromPoint;

static int WINAPI h_MapWindowPoints(HWND from, HWND to, LPPOINT pts, UINT n)
{
    jr_count(JC_CURSOR);
    if (!pts || !emu_active())
        return r_MapWindowPoints(from, to, pts, n);
    POINT o = client_origin();
    if (!from && game_related(to))
        for (UINT i = 0; i < n; i++) { pts[i].x += o.x; pts[i].y += o.y; }
    int r = r_MapWindowPoints(from, to, pts, n);
    if (!to && game_related(from))
        for (UINT i = 0; i < n; i++) { pts[i].x -= o.x; pts[i].y -= o.y; }
    if (n >= 2) { RECT rr = {pts[0].x, pts[0].y, pts[1].x, pts[1].y}; trace_geom(to ? "MapWindowPoints(to)" : "MapWindowPoints(from)", to ? to : from, &rr, NULL, _ReturnAddress()); }
    else if (n == 1) trace_geom(to ? "MapWindowPoints(to)" : "MapWindowPoints(from)", to ? to : from, NULL, pts, _ReturnAddress());
    return r;
}

static HWND WINAPI h_WindowFromPoint(POINT p)
{
    jr(J_HOOKCALL, JH_WINDOWFROMPOINT, p.x, p.y);
    if (emu_active()) {
        POINT o = client_origin();
        p.x += o.x;
        p.y += o.y;
    }
    return r_WindowFromPoint(p);
}

// --- trace (debug key "trace"): who draws through GDI / dialogs --------------------------------

typedef HDC(WINAPI *PFN_GetDC)(HWND);
typedef HDC(WINAPI *PFN_BeginPaint)(HWND, LPPAINTSTRUCT);
typedef INT_PTR(WINAPI *PFN_DialogBoxParamA)(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);
typedef INT_PTR(WINAPI *PFN_DialogBoxIndirectParamA)(HINSTANCE, LPCDLGTEMPLATEA, HWND, DLGPROC, LPARAM);
typedef HWND(WINAPI *PFN_CreateDialogParamA)(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);
typedef HWND(WINAPI *PFN_CreateDialogIndirectParamA)(HINSTANCE, LPCDLGTEMPLATEA, HWND, DLGPROC, LPARAM);
typedef BOOL(WINAPI *PFN_BitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);
typedef BOOL(WINAPI *PFN_StretchBlt)(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
typedef int(WINAPI *PFN_StretchDIBits)(HDC, int, int, int, int, int, int, int, int, const void *, const BITMAPINFO *,
                                        UINT, DWORD);
typedef int(WINAPI *PFN_SetDIBitsToDevice)(HDC, int, int, DWORD, DWORD, int, int, UINT, UINT, const void *,
                                            const BITMAPINFO *, UINT);

static PFN_GetDC r_GetDC, r_GetWindowDC;
static PFN_BeginPaint r_BeginPaint;
static PFN_DialogBoxParamA r_DialogBoxParamA;
static PFN_DialogBoxIndirectParamA r_DialogBoxIndirectParamA;
static PFN_CreateDialogParamA r_CreateDialogParamA;
static PFN_CreateDialogIndirectParamA r_CreateDialogIndirectParamA;
static PFN_BitBlt r_BitBlt;
static PFN_StretchBlt r_StretchBlt;
static PFN_StretchDIBits r_StretchDIBits;
static PFN_SetDIBitsToDevice r_SetDIBitsToDevice;

enum TraceId { T_GETDC, T_GETWINDOWDC, T_BEGINPAINT, T_DIALOGBOX, T_CREATEDIALOG, T_BITBLT, T_STRETCHBLT,
               T_STRETCHDIBITS, T_SETDIBITS, T_UPDATE, T_TEXTOUT, T_COUNT };
static volatile LONG s_traceCount[T_COUNT];

static void caller_module(void *ret, char *buf, int n)
{
    HMODULE m = NULL;
    buf[0] = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)ret, &m) && m) {
        char path[MAX_PATH];
        GetModuleFileNameA(m, path, MAX_PATH);
        const char *s = strrchr(path, '\\');
        _snprintf_s(buf, n, _TRUNCATE, "%s+0x%lX", s ? s + 1 : path, (unsigned long)((BYTE *)ret - (BYTE *)m));
    }
}

static void describe_hwnd(HWND h, char *buf, int n)
{
    char cls[64] = "";
    if (h) GetClassNameA(h, cls, sizeof(cls));
    RECT r = {0, 0, 0, 0};
    if (h) GetWindowRect(h, &r);
    _snprintf_s(buf, n, _TRUNCATE, "%p \"%s\" (%ld,%ld)-(%ld,%ld)%s%s", h, cls, r.left, r.top, r.right, r.bottom,
                h && h == s_hwnd ? " [game]" : (h && s_hwnd && IsChild(s_hwnd, h) ? " [child]" : ""),
                h && !IsWindowVisible(h) ? " hidden" : "");
}

// first 24 calls of each kind are logged, then every 500th
static bool trace_hit(int id)
{
    LONG c = InterlockedIncrement(&s_traceCount[id]);
    return c <= 24 || c % 500 == 0;
}

static void trace_dc(const char *what, int id, HDC dc, void *ret, const char *extra)
{
    if (!trace_hit(id))
        return;
    char mod[96], wnd[160] = "memory DC";
    caller_module(ret, mod, sizeof(mod));
    HWND w = dc ? WindowFromDC(dc) : NULL;
    if (w) describe_hwnd(w, wnd, sizeof(wnd));
    log_event("trace #%ld %s %s -> %s from %s", s_traceCount[id], what, extra, wnd, mod);
}

static HDC WINAPI h_GetDC(HWND h)
{
    if (trace_hit(T_GETDC)) {
        char mod[96], wnd[160];
        caller_module(_ReturnAddress(), mod, sizeof(mod));
        describe_hwnd(h, wnd, sizeof(wnd));
        log_event("trace #%ld GetDC(%s) from %s", s_traceCount[T_GETDC], wnd, mod);
    }
    return r_GetDC(h);
}

static HDC WINAPI h_GetWindowDC(HWND h)
{
    if (trace_hit(T_GETWINDOWDC)) {
        char mod[96], wnd[160];
        caller_module(_ReturnAddress(), mod, sizeof(mod));
        describe_hwnd(h, wnd, sizeof(wnd));
        log_event("trace #%ld GetWindowDC(%s) from %s", s_traceCount[T_GETWINDOWDC], wnd, mod);
    }
    return r_GetWindowDC(h);
}

// --- Q174 cursorprobe: what the game does with the OS cursor -------------------------------------------------
typedef HCURSOR(WINAPI *PFN_SetCursor)(HCURSOR);
typedef DWORD(WINAPI *PFN_SetClassLongA)(HWND, int, LONG);
static PFN_SetCursor r_SetCursor;
static PFN_SetClassLongA r_SetClassLongA;

static int WINAPI h_ShowCursor(BOOL show)
{
    int r = r_ShowCursor(show);
    InterlockedExchangeAdd(&s_gameShowDelta, show ? 1 : -1);   // keepcursor: what the game itself wants
    LONG n = InterlockedIncrement(&s_cpShow);
    InterlockedExchange(&s_cpCounter, r);
    if (g_cfg.cursorProbe && n <= 40) {
        char mod[96];
        caller_module(_ReturnAddress(), mod, sizeof(mod));
        log_event("cursorprobe: ShowCursor(%s) -> counter %d (thread %lu) from %s", show ? "TRUE" : "FALSE", r,
                  GetCurrentThreadId(), mod);
    }
    return r;
}

static HCURSOR WINAPI h_SetCursor(HCURSOR c)
{
    HCURSOR old = r_SetCursor(c);
    LONG n = InterlockedIncrement(&s_cpSet);
    if (!c) InterlockedIncrement(&s_cpSetNull);
    if (n <= 40) {
        char mod[96];
        caller_module(_ReturnAddress(), mod, sizeof(mod));
        log_event("cursorprobe: SetCursor(%p) (was %p, thread %lu) from %s", c, old, GetCurrentThreadId(), mod);
    }
    return old;
}

static DWORD WINAPI h_SetClassLongA(HWND h, int i, LONG v)
{
    DWORD r = r_SetClassLongA(h, i, v);
    if (i == GCL_HCURSOR && InterlockedIncrement(&s_cpClass) <= 40) {
        char mod[96];
        caller_module(_ReturnAddress(), mod, sizeof(mod));
        log_event("cursorprobe: SetClassLong(%p, GCL_HCURSOR, %08lX) (was %08lX) from %s", h, (DWORD)v, r, mod);
    }
    return r;
}

static HDC WINAPI h_BeginPaint(HWND h, LPPAINTSTRUCT ps)
{
    HDC dc = r_BeginPaint(h, ps);
    if (trace_hit(T_BEGINPAINT)) {
        char mod[96], wnd[160];
        caller_module(_ReturnAddress(), mod, sizeof(mod));
        describe_hwnd(h, wnd, sizeof(wnd));
        log_event("trace #%ld BeginPaint(%s) rc=(%ld,%ld)-(%ld,%ld) from %s", s_traceCount[T_BEGINPAINT], wnd,
                  ps ? ps->rcPaint.left : 0, ps ? ps->rcPaint.top : 0, ps ? ps->rcPaint.right : 0,
                  ps ? ps->rcPaint.bottom : 0, mod);
    }
    return dc;
}

// trace=true, Q146: the paint bookkeeping of the game's own controls (which region it believes needs drawing)
typedef BOOL(WINAPI *PFN_GetUpdateRect)(HWND, LPRECT, BOOL);
typedef BOOL(WINAPI *PFN_ValidateRect)(HWND, const RECT *);
typedef BOOL(WINAPI *PFN_InvalidateRect)(HWND, const RECT *, BOOL);
typedef BOOL(WINAPI *PFN_RedrawWindow)(HWND, const RECT *, HRGN, UINT);
typedef BOOL(WINAPI *PFN_UpdateWindow)(HWND);
typedef BOOL(WINAPI *PFN_LockWindowUpdate)(HWND);
typedef BOOL(WINAPI *PFN_TextOutA)(HDC, int, int, LPCSTR, int);
static PFN_GetUpdateRect r_GetUpdateRect;
static PFN_ValidateRect r_ValidateRect;
static PFN_InvalidateRect r_InvalidateRect;
static PFN_RedrawWindow r_RedrawWindow;
static PFN_UpdateWindow r_UpdateWindow;
static PFN_LockWindowUpdate r_LockWindowUpdate;
static PFN_TextOutA r_TextOutA;

static bool trace_hit_n(int id, LONG n)
{
    return InterlockedIncrement(&s_traceCount[id]) <= n;
}

static void trace_update(const char *what, HWND h, const RECT *r, const char *extra, void *ret)
{
    if (!trace_hit_n(T_UPDATE, 400))
        return;
    char mod[96], wnd[160];
    caller_module(ret, mod, sizeof(mod));
    describe_hwnd(h, wnd, sizeof(wnd));
    if (r)
        log_event("trace #%ld %s(%s) rect (%ld,%ld)-(%ld,%ld)%s from %s", s_traceCount[T_UPDATE], what, wnd, r->left, r->top,
                  r->right, r->bottom, extra, mod);
    else
        log_event("trace #%ld %s(%s) rect NULL%s from %s", s_traceCount[T_UPDATE], what, wnd, extra, mod);
}

static BOOL WINAPI h_GetUpdateRect(HWND h, LPRECT r, BOOL erase)
{
    BOOL ok = r_GetUpdateRect(h, r, erase);
    trace_update(ok ? "GetUpdateRect=TRUE" : "GetUpdateRect=FALSE", h, r, "", _ReturnAddress());
    return ok;
}

static BOOL WINAPI h_ValidateRect(HWND h, const RECT *r)
{
    trace_update("ValidateRect", h, r, "", _ReturnAddress());
    return r_ValidateRect(h, r);
}

static BOOL WINAPI h_InvalidateRect(HWND h, const RECT *r, BOOL erase)
{
    trace_update("InvalidateRect", h, r, erase ? " erase" : "", _ReturnAddress());
    return r_InvalidateRect(h, r, erase);
}

static BOOL WINAPI h_RedrawWindow(HWND h, const RECT *r, HRGN rgn, UINT f)
{
    char e[48];
    _snprintf_s(e, sizeof(e), _TRUNCATE, " rgn %p flags 0x%X", rgn, f);
    trace_update("RedrawWindow", h, r, e, _ReturnAddress());
    return r_RedrawWindow(h, r, rgn, f);
}

static BOOL WINAPI h_UpdateWindow(HWND h)
{
    trace_update("UpdateWindow", h, NULL, "", _ReturnAddress());
    return r_UpdateWindow(h);
}

static BOOL WINAPI h_LockWindowUpdate(HWND h)
{
    trace_update("LockWindowUpdate", h, NULL, "", _ReturnAddress());
    return r_LockWindowUpdate(h);
}

static BOOL WINAPI h_TextOutA(HDC dc, int x, int y, LPCSTR s, int n)
{
    if (trace_hit_n(T_TEXTOUT, 200)) {
        char mod[96], wnd[160] = "memory DC";
        caller_module(_ReturnAddress(), mod, sizeof(mod));
        HWND w = WindowFromDC(dc);
        if (w) describe_hwnd(w, wnd, sizeof(wnd));
        RECT clip = {0, 0, 0, 0};
        int ck = GetClipBox(dc, &clip);
        log_event("trace #%ld TextOutA(%d,%d, %d chars) -> %s, clip box (%ld,%ld)-(%ld,%ld) type %d from %s",
                  s_traceCount[T_TEXTOUT], x, y, n, wnd, clip.left, clip.top, clip.right, clip.bottom, ck, mod);
    }
    return r_TextOutA(dc, x, y, s, n);
}

static void trace_dialog(const char *what, int id, HWND parent, LPCSTR tmpl, void *ret)
{
    if (!trace_hit(id))
        return;
    char mod[96], wnd[160];
    caller_module(ret, mod, sizeof(mod));
    describe_hwnd(parent, wnd, sizeof(wnd));
    log_event("trace #%ld %s template=%p parent=%s from %s", s_traceCount[id], what, tmpl, wnd, mod);
}

static INT_PTR WINAPI h_DialogBoxParamA(HINSTANCE i, LPCSTR t, HWND p, DLGPROC f, LPARAM l)
{
    trace_dialog("DialogBoxParamA", T_DIALOGBOX, p, t, _ReturnAddress());
    INT_PTR r = r_DialogBoxParamA(i, t, p, f, l);
    log_event("trace DialogBoxParamA template=%p returned %ld", t, (long)r);
    return r;
}

static INT_PTR WINAPI h_DialogBoxIndirectParamA(HINSTANCE i, LPCDLGTEMPLATEA t, HWND p, DLGPROC f, LPARAM l)
{
    trace_dialog("DialogBoxIndirectParamA", T_DIALOGBOX, p, (LPCSTR)t, _ReturnAddress());
    INT_PTR r = r_DialogBoxIndirectParamA(i, t, p, f, l);
    log_event("trace DialogBoxIndirectParamA template=%p returned %ld", t, (long)r);
    return r;
}

static HWND WINAPI h_CreateDialogParamA(HINSTANCE i, LPCSTR t, HWND p, DLGPROC f, LPARAM l)
{
    trace_dialog("CreateDialogParamA", T_CREATEDIALOG, p, t, _ReturnAddress());
    return r_CreateDialogParamA(i, t, p, f, l);
}

static HWND WINAPI h_CreateDialogIndirectParamA(HINSTANCE i, LPCDLGTEMPLATEA t, HWND p, DLGPROC f, LPARAM l)
{
    trace_dialog("CreateDialogIndirectParamA", T_CREATEDIALOG, p, (LPCSTR)t, _ReturnAddress());
    return r_CreateDialogIndirectParamA(i, t, p, f, l);
}

static BOOL WINAPI h_BitBlt(HDC d, int x, int y, int w, int h, HDC s, int sx, int sy, DWORD rop)
{
    char e[64];
    _snprintf_s(e, sizeof(e), _TRUNCATE, "(%d,%d %dx%d rop=%lX)", x, y, w, h, rop);
    trace_dc("BitBlt", T_BITBLT, d, _ReturnAddress(), e);
    return r_BitBlt(d, x, y, w, h, s, sx, sy, rop);
}

static BOOL WINAPI h_StretchBlt(HDC d, int x, int y, int w, int h, HDC s, int sx, int sy, int sw, int sh, DWORD rop)
{
    char e[64];
    _snprintf_s(e, sizeof(e), _TRUNCATE, "(%d,%d %dx%d)", x, y, w, h);
    trace_dc("StretchBlt", T_STRETCHBLT, d, _ReturnAddress(), e);
    return r_StretchBlt(d, x, y, w, h, s, sx, sy, sw, sh, rop);
}

static int WINAPI h_StretchDIBits(HDC d, int x, int y, int w, int h, int sx, int sy, int sw, int sh, const void *bits,
                                  const BITMAPINFO *bi, UINT u, DWORD rop)
{
    char e[64];
    _snprintf_s(e, sizeof(e), _TRUNCATE, "(%d,%d %dx%d)", x, y, w, h);
    trace_dc("StretchDIBits", T_STRETCHDIBITS, d, _ReturnAddress(), e);
    return r_StretchDIBits(d, x, y, w, h, sx, sy, sw, sh, bits, bi, u, rop);
}

static int WINAPI h_SetDIBitsToDevice(HDC d, int x, int y, DWORD w, DWORD h, int sx, int sy, UINT a, UINT b,
                                      const void *bits, const BITMAPINFO *bi, UINT u)
{
    char e[64];
    _snprintf_s(e, sizeof(e), _TRUNCATE, "(%d,%d %lux%lu)", x, y, w, h);
    trace_dc("SetDIBitsToDevice", T_SETDIBITS, d, _ReturnAddress(), e);
    return r_SetDIBitsToDevice(d, x, y, w, h, sx, sy, a, b, bits, bi, u);
}

// --- IAT patching -----------------------------------------------------------------------------

struct HookEntry { void **real; void *hook; };

static int patch_module(HMODULE mod, const char *dll, const HookEntry *hooks, int nHooks)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return 0;
    int patched = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((const char *)(base + imp->Name), dll) != 0)
            continue;
        for (IMAGE_THUNK_DATA *t = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk); t->u1.Function; t++) {
            for (int i = 0; i < nHooks; i++) {
                if (!*hooks[i].real || (void *)t->u1.Function != *hooks[i].real)
                    continue;
                DWORD old;
                if (VirtualProtect(&t->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
                    t->u1.Function = (ULONG_PTR)hooks[i].hook;
                    VirtualProtect(&t->u1.Function, sizeof(void *), old, &old);
                    patched++;
                }
            }
        }
    }
    return patched;
}

void hooks_install()
{
    static bool done;
    if (done)
        return;
    done = true;
    // always: the crash reporter sees the game's SetUnhandledExceptionFilter / TerminateProcess / ExitProcess
    HookEntry k32[8], mm[2];
    void **kReal[4], *kHook[4];
    const char *kDll[4];
    int nK32 = crash_hooks(kReal, kHook), nMm = 0;
    for (int i = 0; i < nK32; i++) {
        k32[i].real = kReal[i];
        k32[i].hook = kHook[i];
    }
    // maxgameticks: the game's clocks (kernel32 / winmm)
    int nClock = clock_hooks(kReal, kHook, kDll);
    for (int i = 0; i < nClock; i++) {
        HookEntry &e = _stricmp(kDll[i], "winmm.dll") == 0 ? mm[nMm++] : k32[nK32++];
        e.real = kReal[i];
        e.hook = kHook[i];
    }
    HMODULE u32 = GetModuleHandleA("user32.dll"), g32 = GetModuleHandleA("gdi32.dll");
    r_GetCursorPos = (PFN_GetCursorPos)GetProcAddress(u32, "GetCursorPos");
    r_SetCursorPos = (PFN_SetCursorPos)GetProcAddress(u32, "SetCursorPos");
    r_ClipCursor = (PFN_ClipCursor)GetProcAddress(u32, "ClipCursor");
    r_ScreenToClient = (PFN_PtConv)GetProcAddress(u32, "ScreenToClient");
    r_ClientToScreen = (PFN_PtConv)GetProcAddress(u32, "ClientToScreen");
    r_GetClientRect = (PFN_GetRect)GetProcAddress(u32, "GetClientRect");
    r_GetWindowRect = (PFN_GetRect)GetProcAddress(u32, "GetWindowRect");
    r_MapWindowPoints = (PFN_MapWindowPoints)GetProcAddress(u32, "MapWindowPoints");
    r_WindowFromPoint = (PFN_WindowFromPoint)GetProcAddress(u32, "WindowFromPoint");
    r_GetSystemMetrics = (PFN_GetSystemMetrics)GetProcAddress(u32, "GetSystemMetrics");
    r_SetWindowPos = (PFN_SetWindowPos)GetProcAddress(u32, "SetWindowPos");
    r_MoveWindow = (PFN_MoveWindow)GetProcAddress(u32, "MoveWindow");
    HookEntry user[32], gdi[8];
    int nUser = 0, nGdi = 0;
    if (g_cfg.hookMouse) {
        const HookEntry m[] = {
            {(void **)&r_GetCursorPos, (void *)h_GetCursorPos},     {(void **)&r_SetCursorPos, (void *)h_SetCursorPos},
            {(void **)&r_ClipCursor, (void *)h_ClipCursor},         {(void **)&r_ScreenToClient, (void *)h_ScreenToClient},
            {(void **)&r_ClientToScreen, (void *)h_ClientToScreen}, {(void **)&r_GetClientRect, (void *)h_GetClientRect},
            {(void **)&r_GetWindowRect, (void *)h_GetWindowRect},     {(void **)&r_MapWindowPoints, (void *)h_MapWindowPoints},
            {(void **)&r_WindowFromPoint, (void *)h_WindowFromPoint}, {(void **)&r_GetSystemMetrics, (void *)h_GetSystemMetrics},
            {(void **)&r_SetWindowPos, (void *)h_SetWindowPos},       {(void **)&r_MoveWindow, (void *)h_MoveWindow},
        };
        for (const HookEntry &e : m) user[nUser++] = e;
    }
    if (g_cfg.keepCursor || g_cfg.cursorProbe) {   // keepcursor: the game's own ShowCursor calls are counted
        r_ShowCursor = (PFN_ShowCursor)GetProcAddress(u32, "ShowCursor");
        user[nUser++] = {(void **)&r_ShowCursor, (void *)h_ShowCursor};
    }
    if (g_cfg.cursorProbe) {
        r_SetCursor = (PFN_SetCursor)GetProcAddress(u32, "SetCursor");
        r_SetClassLongA = (PFN_SetClassLongA)GetProcAddress(u32, "SetClassLongA");
        const HookEntry c[] = {
            {(void **)&r_SetCursor, (void *)h_SetCursor},
            {(void **)&r_SetClassLongA, (void *)h_SetClassLongA},
        };
        for (const HookEntry &e : c) user[nUser++] = e;
    }
    if (g_cfg.trace) {
        r_GetDC = (PFN_GetDC)GetProcAddress(u32, "GetDC");
        r_GetWindowDC = (PFN_GetDC)GetProcAddress(u32, "GetWindowDC");
        r_BeginPaint = (PFN_BeginPaint)GetProcAddress(u32, "BeginPaint");
        r_DialogBoxParamA = (PFN_DialogBoxParamA)GetProcAddress(u32, "DialogBoxParamA");
        r_DialogBoxIndirectParamA = (PFN_DialogBoxIndirectParamA)GetProcAddress(u32, "DialogBoxIndirectParamA");
        r_CreateDialogParamA = (PFN_CreateDialogParamA)GetProcAddress(u32, "CreateDialogParamA");
        r_CreateDialogIndirectParamA = (PFN_CreateDialogIndirectParamA)GetProcAddress(u32, "CreateDialogIndirectParamA");
        r_GetUpdateRect = (PFN_GetUpdateRect)GetProcAddress(u32, "GetUpdateRect");
        r_ValidateRect = (PFN_ValidateRect)GetProcAddress(u32, "ValidateRect");
        r_InvalidateRect = (PFN_InvalidateRect)GetProcAddress(u32, "InvalidateRect");
        r_RedrawWindow = (PFN_RedrawWindow)GetProcAddress(u32, "RedrawWindow");
        r_UpdateWindow = (PFN_UpdateWindow)GetProcAddress(u32, "UpdateWindow");
        r_LockWindowUpdate = (PFN_LockWindowUpdate)GetProcAddress(u32, "LockWindowUpdate");
        r_TextOutA = (PFN_TextOutA)GetProcAddress(g32, "TextOutA");
        r_BitBlt = (PFN_BitBlt)GetProcAddress(g32, "BitBlt");
        r_StretchBlt = (PFN_StretchBlt)GetProcAddress(g32, "StretchBlt");
        r_StretchDIBits = (PFN_StretchDIBits)GetProcAddress(g32, "StretchDIBits");
        r_SetDIBitsToDevice = (PFN_SetDIBitsToDevice)GetProcAddress(g32, "SetDIBitsToDevice");
        const HookEntry t[] = {
            {(void **)&r_GetDC, (void *)h_GetDC},
            {(void **)&r_GetWindowDC, (void *)h_GetWindowDC},
            {(void **)&r_BeginPaint, (void *)h_BeginPaint},
            {(void **)&r_DialogBoxParamA, (void *)h_DialogBoxParamA},
            {(void **)&r_DialogBoxIndirectParamA, (void *)h_DialogBoxIndirectParamA},
            {(void **)&r_CreateDialogParamA, (void *)h_CreateDialogParamA},
            {(void **)&r_CreateDialogIndirectParamA, (void *)h_CreateDialogIndirectParamA},
            {(void **)&r_GetUpdateRect, (void *)h_GetUpdateRect},
            {(void **)&r_ValidateRect, (void *)h_ValidateRect},
            {(void **)&r_InvalidateRect, (void *)h_InvalidateRect},
            {(void **)&r_RedrawWindow, (void *)h_RedrawWindow},
            {(void **)&r_UpdateWindow, (void *)h_UpdateWindow},
            {(void **)&r_LockWindowUpdate, (void *)h_LockWindowUpdate},
        };
        for (const HookEntry &e : t) user[nUser++] = e;
        const HookEntry g[] = {
            {(void **)&r_BitBlt, (void *)h_BitBlt},
            {(void **)&r_StretchBlt, (void *)h_StretchBlt},
            {(void **)&r_StretchDIBits, (void *)h_StretchDIBits},
            {(void **)&r_SetDIBitsToDevice, (void *)h_SetDIBitsToDevice},
            {(void **)&r_TextOutA, (void *)h_TextOutA},
        };
        for (const HookEntry &e : g) gdi[nGdi++] = e;
    }

    // Patch every module that is not part of Windows: the executable, DLLs in the game directory and
    // the ones injected from elsewhere (QEC's Ares / Phobos / cncnet5 come from ClientMain\ClientResource).
    char winDir[MAX_PATH];
    UINT winLen = GetWindowsDirectoryA(winDir, MAX_PATH);
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&hooks_install, &self);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE)
        return;
    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me)) {
        if (me.hModule == self)
            continue;
        if (winLen && _strnicmp(me.szExePath, winDir, winLen) == 0)
            continue;   // Windows' own DLLs (user32 itself, the GL driver, ...)
        int n = patch_module(me.hModule, "user32.dll", user, nUser);
        int ng = nGdi ? patch_module(me.hModule, "gdi32.dll", gdi, nGdi) : 0;
        int nk = patch_module(me.hModule, "kernel32.dll", k32, nK32);
        int nm = nMm ? patch_module(me.hModule, "winmm.dll", mm, nMm) : 0;
        if (n || ng || nk || nm)
            log_event("hooks: patched %d user32 + %d gdi32 + %d kernel32 + %d winmm imports in %s", n, ng, nk, nm,
                      me.szExePath);
    }
    CloseHandle(snap);
}
