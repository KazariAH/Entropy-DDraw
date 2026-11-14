// QuantumDDraw - minimal DirectDraw replacement (windowed OpenGL 1.1 presenter)
// Shared declarations for all translation units.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "childwin.h"
#include "journal.h"
#include <ddraw.h>

// ---------------------------------------------------------------------------
// Method table: every interface method / export gets a counter.
// X(name, implemented)  implemented = 1 real implementation, 0 stub (counted only)
// ---------------------------------------------------------------------------
#define QDD_METHODS(X) \
    X(DirectDrawCreate, 1) X(DirectDrawCreateEx, 1) X(DirectDrawCreateClipper, 1) \
    X(DirectDrawEnumerateA, 1) X(DirectDrawEnumerateW, 1) X(DirectDrawEnumerateExA, 1) X(DirectDrawEnumerateExW, 1) \
    X(DD_QueryInterface, 1) X(DD_AddRef, 1) X(DD_Release, 1) X(DD_Compact, 0) X(DD_CreateClipper, 1) \
    X(DD_CreatePalette, 1) X(DD_CreateSurface, 1) X(DD_DuplicateSurface, 0) X(DD_EnumDisplayModes, 1) \
    X(DD_EnumSurfaces, 0) X(DD_FlipToGDISurface, 0) X(DD_GetCaps, 1) X(DD_GetDisplayMode, 1) \
    X(DD_GetFourCCCodes, 1) X(DD_GetGDISurface, 1) X(DD_GetMonitorFrequency, 1) X(DD_GetScanLine, 0) \
    X(DD_GetVerticalBlankStatus, 0) X(DD_Initialize, 0) X(DD_RestoreDisplayMode, 1) X(DD_SetCooperativeLevel, 1) \
    X(DD_SetDisplayMode, 1) X(DD_WaitForVerticalBlank, 1) X(DD_GetAvailableVidMem, 1) X(DD_GetSurfaceFromDC, 0) \
    X(DD_RestoreAllSurfaces, 0) X(DD_TestCooperativeLevel, 0) X(DD_GetDeviceIdentifier, 1) X(DD_StartModeTest, 0) \
    X(DD_EvaluateMode, 0) \
    X(S_QueryInterface, 1) X(S_AddRef, 1) X(S_Release, 1) X(S_AddAttachedSurface, 1) X(S_AddOverlayDirtyRect, 0) \
    X(S_Blt, 1) X(S_BltBatch, 0) X(S_BltFast, 1) X(S_DeleteAttachedSurface, 1) X(S_EnumAttachedSurfaces, 1) \
    X(S_EnumOverlayZOrders, 0) X(S_Flip, 1) X(S_GetAttachedSurface, 1) X(S_GetBltStatus, 0) X(S_GetCaps, 1) \
    X(S_GetClipper, 1) X(S_GetColorKey, 1) X(S_GetDC, 1) X(S_GetFlipStatus, 0) X(S_GetOverlayPosition, 0) \
    X(S_GetPalette, 1) X(S_GetPixelFormat, 1) X(S_GetSurfaceDesc, 1) X(S_Initialize, 0) X(S_IsLost, 0) \
    X(S_Lock, 1) X(S_ReleaseDC, 1) X(S_Restore, 0) X(S_SetClipper, 1) X(S_SetColorKey, 1) \
    X(S_SetOverlayPosition, 0) X(S_SetPalette, 1) X(S_Unlock, 1) X(S_UpdateOverlay, 0) X(S_UpdateOverlayDisplay, 0) \
    X(S_UpdateOverlayZOrder, 0) X(S_GetDDInterface, 1) X(S_PageLock, 0) X(S_PageUnlock, 0) X(S_SetSurfaceDesc, 0) \
    X(S_SetPrivateData, 0) X(S_GetPrivateData, 0) X(S_FreePrivateData, 0) X(S_GetUniquenessValue, 0) \
    X(S_ChangeUniquenessValue, 0) X(S_SetPriority, 0) X(S_GetPriority, 0) X(S_SetLOD, 0) X(S_GetLOD, 0) \
    X(P_QueryInterface, 1) X(P_AddRef, 1) X(P_Release, 1) X(P_GetCaps, 1) X(P_GetEntries, 1) X(P_Initialize, 0) \
    X(P_SetEntries, 1) \
    X(C_QueryInterface, 1) X(C_AddRef, 1) X(C_Release, 1) X(C_GetClipList, 1) X(C_GetHWnd, 1) X(C_Initialize, 0) \
    X(C_IsClipListChanged, 0) X(C_SetClipList, 0) X(C_SetHWnd, 1)

enum MethodId {
#define QDD_ENUM(n, impl) M_##n,
    QDD_METHODS(QDD_ENUM)
#undef QDD_ENUM
    M_COUNT
};

// ---------------------------------------------------------------------------
// Logging (log.cpp). Counters are lock-free; text is flushed every 5 seconds.
// ---------------------------------------------------------------------------
extern volatile LONG g_counts[M_COUNT];
#define COUNT(n) InterlockedIncrement(&g_counts[M_##n])

enum HistId { H_BLT, H_BLTFAST, H_LOCK, H_LOCK_PRIMARY, H_COLORFILL, H_COUNT };

void log_init(HMODULE self);
void log_start_thread();
void log_shutdown();
void log_flush();
void log_event(const char *fmt, ...);
void log_hist(int hist, int w, int h);
void log_bltflags(DWORD flags);
void log_frame(int uploadedPixels);
void log_crash(const char *fmt, ...);            // crash report line (may use the journal's reserve)
int log_recent(char *buf, int cap);              // the last 32 events, oldest first (kept even with log=false)
void local_stamp(char *buf, size_t cap, bool millis);  // yyyymmdd_hhmmss[_mmm], local time
const char *log_dir();                           // folder of the DLL / ddraw.log, with the trailing backslash

// crash.cpp
void crash_init();                               // first-chance logger + unhandled exception filter (DllMain)
void crash_reassert();                           // DirectDrawCreate: our filter on top again, the game's chained
int crash_hooks(void ***reals, void **hooks);    // IAT hooks for the game modules: SetUnhandledExceptionFilter, TerminateProcess, ExitProcess (3)
void crash_test_raise();                         // crashtest=1: the deliberate exception (render thread)

// monitor.cpp - monitors as the DLL sees them: the real ones (EnumDisplayMonitors) plus the simulated
// one of `monitortest` (handle QDD_FAKE_MONITOR, device "\\.\QDDTEST1", never handed to Windows)
#define QDD_FAKE_MONITOR ((HMONITOR)(LONG_PTR)0x7E570001)
struct MonInfo {
    HMONITOR h;
    RECT rc, work;
    char dev[32];
    bool primary, fake;
    UINT dpi;                                    // effective DPI, 0 if unknown
};
int monitors_list(MonInfo *out, int cap);        // enumeration order, the simulated one last
void monitor_of_rect(const RECT &r, MonInfo *out);   // most overlap, else nearest (always fills *out)
void monitor_of_window(HWND h, MonInfo *out);
bool monitor_by_device(const char *dev, MonInfo *out);
void monitor_primary(MonInfo *out);
void monitors_log(const char *why);
UINT dpi_of_window(HWND h);                      // GetDpiForWindow, else the system DPI
void dpi_init();                                 // DllMain: declare the dpi= awareness, log it
void dpi_describe(char *buf, size_t cap);        // awareness in effect + system DPI
BOOL adjust_window_rect(RECT *r, DWORD style, DWORD ex, UINT dpi);   // AdjustWindowRectExForDpi when available

// clock.cpp - maxgameticks
int clock_hooks(void ***reals, void **hooks, const char **dlls);   // IAT entries of the game clocks (up to 4)
extern volatile DWORD g_gameThread;              // thread of the game window (set by window_attach)
void clock_report(bool final);                   // ~1/s from the render thread, and at exit

// shot.cpp
bool png_write(const char *path, const DWORD *rgba, int w, int h);   // pixels R | G << 8 | B << 16, top-down

// ---------------------------------------------------------------------------
// Configuration (config.cpp) - ddraw.ini next to the DLL, section [ddraw]
// ---------------------------------------------------------------------------
enum ScaleMode { SCALE_STRETCH, SCALE_ASPECT, SCALE_INTEGER };
enum RendererMode { RENDERER_AUTO, RENDERER_OPENGL, RENDERER_GDI };
enum CrashDumpMode { CRASHDUMP_OFF, CRASHDUMP_NORMAL, CRASHDUMP_FULL };
enum DpiMode { DPI_OFF, DPI_SYSTEM, DPI_PERMONITOR };

struct Config {
    bool windowed;      // emulate EXCLUSIVE fullscreen inside a window
    int  windowScale;   // initial client scale for emulated window (0 = auto)
    int  scale;         // ScaleMode
    bool linear;        // GL_LINEAR filtering when not integer scaled
    bool vsync;
    int  maxfps;        // 0 = only limited by dirty updates
    bool hookMouse;     // remap cursor APIs while emulating fullscreen in a window
    int  dpi;           // DpiMode: DPI awareness the process declares (permonitor = per-monitor v2)
    bool resizable;
    bool noActivateApp; // hide focus loss from the game (keeps running in the background)
    bool ownIni;        // ddraw.ini carries [ddraw] client=Entropy-DDraw (ours, not a cnc-ddraw / other renderer ini)
    bool foreignNoActivate; // a foreign ini said noactivateapp=false; ignored, our default true kept
    bool log;
    int  defaultBpp;    // bpp reported before SetDisplayMode
    bool pollPrimary;   // compare the visible primary with a shadow copy every tick (writes outside Lock/Blt)
    bool dump;        // debug: write ddraw_primary.bmp / ddraw_lastlock.bmp on every log flush
    bool normalEmu;   // DDSCL_NORMAL games: emulate a display mode of the client size (primary = client, coordinates client relative)
    bool pbo;         // upload through pixel buffer objects when the driver has them
    bool trace;       // debug: log GDI / dialog calls made by the game modules (GetDC, BeginPaint, BitBlt, ...)
    bool border;          // windowed state keeps its caption / frame (false = borderless window)
    bool fullscreen;      // start in borderless fullscreen (windowed=true) instead of a window
    int  refreshRate;     // real mode switch (windowed=false): refresh rate in Hz, 0 = driver default
    int  keyToggleFullscreen; // Alt + this virtual key toggles window <-> fullscreen, 0 = off
    int  keyUnlockCursor;     // Ctrl + this virtual key releases / re-applies the cursor clip, 0 = off
    bool saveSettings;    // remember window rectangle and fullscreen state in quantumddraw.state
    float adjMouse;       // relative mouse speed while the cursor is locked to the game (0.1 - 10)
    bool centerCursorFix; // put the cursor in the middle of the game screen after a mode / presentation change
    bool mouseProbe;      // debug: log what the GetCursorPos hook reports to the game
    bool vsyncTest;       // debug: present continuously (frame pacing / vsync measurement)
    bool letterboxOffset; // mouse input of the game's child windows follows the picture (black bars / scaling)
    bool fixAltKeyStuck;  // focus changes also reset the thread key state of Alt / Ctrl / Shift
    bool fixNotResponding;// game thread busy: our calls dispatch its window messages (not only peek)
    bool gameHandlesClose;// the close button is left to the game (false: we end the process)
    bool flipClear;       // clear the back buffer after Flip, all surfaces after SetDisplayMode / Restore
    int  fixChilds;       // 1: the game's child windows (QEC dialogs) are invalidated when they appear / change, so the
                          // game paints them (it draws those screens into the primary only on WM_PAINT); 0: off
    bool childPaint;      // fixchilds: the picture under each visible child window is painted into it after a present
    int  childRepaint;    // fixchilds: 0 = never invalidate the dialogs, 1 = dialogs only (as the game does), 2 = dialogs + controls
    int  lockCursor;      // 0 off, 1 = cursor confined to the picture while a button is held (default), 2 = after a click (cnc-like)
    bool keepClipChildren;// with fixchilds + childpaint: leave WS_CLIPCHILDREN on the game window (QEC briefing text)
    int  renderer;        // RendererMode (auto: OpenGL, GDI when no usable GL context)
    bool forceGlFail;     // debug: the OpenGL context counts as failed (tests the auto fallback)
    int  crashDump;       // CrashDumpMode of the dump the unhandled exception filter writes
    int  crashTest;       // debug: 2 s after the first present the render thread raises an exception (1) / writes to NULL (2)
    bool centerWindow;    // the windowed state starts centred on the primary monitor
    bool removeMenu;      // take the menu bar off the game window
    int  keyScreenshot;   // this virtual key saves the picture as Screenshots\<time>.png, 0 = off
    int  keyNextMonitor;  // Ctrl + this virtual key moves the window / fullscreen to the next monitor, 0 = off
    int  monitorTestW, monitorTestH; // debug: a simulated monitor of this size right of the desktop (0 = none)
    char resolutions[256];   // EnumDisplayModes: only these sizes ("2560x1440,1920x1080"), empty = all
    int  maxResolutions;     // EnumDisplayModes: at most this many sizes (the largest), 0 = all
    int  maxResW, maxResH;   // EnumDisplayModes: no size above this (max_resolutions=WxH), 0 = no limit
    bool guardLines;      // a sentinel row before and after every surface, checked at Lock / Unlock / Release
    bool guardTest;       // debug: deliberately overwrite guard bytes (tests the detection)
    int  maxGameTicks;    // the game's clocks advance at most this many ms between two reads, 0 = unlimited
    int  stallTest;       // debug: the game thread is held this many ms inside a drawing call (3 times)
    bool writeWatch;      // Q149: the primary's pixels are write-watched memory: the renderer copies only the pages the game
                          // wrote (no scan, copy outside g_lock, one present per drawn frame); false = the Q148 way
    int  presentQuiet;    // writewatch: present once no new page was written for this many ms (0 = at once)
    int  presentCap;      // writewatch: ... but at most this many ms after the first write of a frame
    int  perfAB;          // debug: alternate "before" and the configured way every this many seconds (same session A/B)
    bool presentSync;     // Q150 (writewatch=false): present once the game's Blt / Unlock signals to the primary stop for
                          // presentquiet ms (at most presentcap) - one present per drawn frame without write watching
                          // (default off since Q163: the coalescing made QEC's irregular drawing bursts stutter)
    bool fastStretch;     // Q150: stretch blits with a column table / memcpy runs instead of a division per pixel
    bool ntCopy;          // Q150 (blitcopy=sse2nt): copies of 1 MB+ between surfaces with non-temporal stores
    bool frameSync;       // Q165: present once per game frame (full-surface copy to the primary), partial writes in between are not uploaded
    int  frameFallback;   // Q165: ms without such a copy before the frame-less way (dirty rects + pollprimary) takes over again
    int  renderPriority;  // Q164 experiment: SetThreadPriority of the render thread (0 = normal, default; 1 = above normal)
    bool emptyPresents;   // Q164 experiment: legacy - also present on a wake-up that brought nothing new (default true)
    bool paceStats;       // Q164 debug: present interval / upload time percentiles every 10 s in every mode
    bool blitStats;       // Q152 debug: full-size copies by surface pair, and what the game touches after a copy to the primary
    bool fusedUpload;     // Q153 B: changed rows read once from the primary, into the shadow and the pixel buffer
    bool renderCluster;   // Q153 C: the render thread follows the game thread's core cluster (shared L3 cache)
    int  verifyTex;       // Q153 debug: every this many seconds the texture is read back and compared with the shadow, 0 = off
};
extern Config g_cfg;
void config_load(HMODULE self);

// ---------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------
struct Palette;
struct Clipper;
struct Surface;

struct DDraw {
    const void *const *vtbl1;   // IDirectDraw
    const void *const *vtbl2;   // IDirectDraw2
    const void *const *vtbl4;   // IDirectDraw4 / IDirectDraw7
    LONG ref;
    HWND hwnd;
    DWORD coop;
    int modeW, modeH, modeBpp;
    bool modeSet;
    bool realModeChanged;       // ChangeDisplaySettings in effect
    bool normalEmu;             // DDSCL_NORMAL window treated as a display mode of its client size (see SetCooperativeLevel)
    Surface *primary;           // front buffer (what is shown)
};

struct Palette {
    const void *const *vtbl;
    LONG ref;
    DWORD caps;
    PALETTEENTRY ents[256];
};

struct Clipper {
    const void *const *vtbl;
    LONG ref;
    HWND hwnd;
};

struct Surface {
    const void *const *vtbl;
    LONG ref;
    DDraw *dd;
    DWORD caps;
    int w, h, bpp, pitch;
    BYTE *bits;
    HBITMAP dib;
    HDC memdc;
    HGDIOBJ oldbmp;
    Palette *pal;
    Clipper *clip;
    Surface *attached;          // next surface in the flip chain (back buffer)
    Surface *chainOwner;        // primary that owns this back buffer (not ref-counted)
    DWORD ckFlags;              // DDCKEY_SRCBLT / DDCKEY_DESTBLT present
    DDCOLORKEY ckSrc, ckDst;
    RECT lockRect;
    bool locked;
    int id;                     // creation order (blitstats)
    BYTE *wwBase;               // writewatch: VirtualAlloc(MEM_WRITE_WATCH) block holding the pixels (no DIB section)
    size_t wwSize;
    BYTE *gdiBits;              // writewatch: pixels of the DIB that GetDC hands out (copied in / out)
};

// global lock protecting object graph & dirty state shared with the render thread
extern CRITICAL_SECTION g_lock;
// Q149: held by the render thread while it copies the primary's pixels without g_lock; whoever frees or swaps
// surface pixels (Release, Flip) takes it inside g_lock. Order: g_lock -> g_pixLock; the render dirty lock is a leaf.
extern CRITICAL_SECTION g_pixLock;
extern DDraw *g_dd;
extern Surface *volatile g_lastLocked;   // debug dump: most recently locked off-screen surface

// ddraw_main.cpp
DDraw *dd_from_iface(void *iface);

// surface.cpp
HRESULT surface_create(DDraw *dd, const DDSURFACEDESC2 *desc, Surface **out);
void surface_clear(Surface *s);                  // flipclear: black pixels
void guard_check(Surface *s, const char *when);  // guard_lines: sentinel rows intact? (logged and repaired)
void surface_fill_desc(Surface *s, DDSURFACEDESC2 *d, DWORD size);
void fill_pixel_format(int bpp, DDPIXELFORMAT *pf);
extern const void *const g_surfaceVtbl[];

// palclip.cpp
Palette *palette_create(DWORD flags, const PALETTEENTRY *ents);
Clipper *clipper_create();
void palette_addref(Palette *p);
void palette_release(Palette *p);
void clipper_addref(Clipper *c);
void clipper_release(Clipper *c);

// render.cpp
void render_start(HWND hwnd);
void render_stop();
void render_dirty(Surface *s, const RECT *r);   // r == NULL => whole surface
void render_frame(Surface *s);                  // Q165: a copy onto the whole primary - the game finished a frame
void render_full_dirty();
void render_palette_changed(Palette *p);
void render_kick();                              // request a redraw
void render_force();                             // window event: complete re-upload + present (rate limited)
void compute_viewport(int cw, int ch, int iw, int ih, RECT *vp); // image placement inside the client area
bool render_paused();
void render_screenshot();                        // keyscreenshot: save the picture on the next tick
const char *render_backend();                    // "OpenGL" / "GDI" once the render thread has chosen
void render_rebuild(const char *why);           // new texture + complete upload (display mode / presentation change)

// window.cpp
void window_attach(HWND hwnd);
void window_detach();
void window_apply_mode(DDraw *dd);
void window_normal_emu(DDraw *dd);               // start emulation for a DDSCL_NORMAL window (no restyle)
void window_restore_mode(DDraw *dd);
void hooks_install();
bool emu_active();                               // fullscreen emulated in a window
void emu_game_to_screen(POINT *p);
void emu_screen_to_game(POINT *p);
void window_save_state();                        // write quantumddraw.state (savesettings)
void window_pump();                              // game thread inside a ddraw call: keep the window responsive
void window_watch();                             // render thread, ~1/s: long-frame / hung-window diagnostics
void window_children_tick();                      // render thread, every tick: fixchilds - repaint new / changed child windows
struct ChildTarget { HWND hwnd; int x, y, w, hh; };   // a child window, position in our client area, size
int  window_child_targets(ChildTarget *t, int max);  // render thread: visible direct children that get the picture

// Q147 hang forensics
void hang_report(const char *why);                // crash.cpp: all threads' stacks + lock owners + minidump
DWORD render_thread_id();                         // render.cpp
CRITICAL_SECTION *render_dirty_lock();            // render.cpp
CRITICAL_SECTION *log_lock();                     // log.cpp
bool window_hang_check(char *why, int cap);       // window.cpp, log thread: the game thread is hung (once per episode)

// Q149 cost accounting (render.cpp): time the game spends inside our drawing calls and waiting for our locks,
// written to ddraw.log with the perf lines ("cost ...")
enum GameCallId { GC_LOCK, GC_UNLOCK, GC_BLT, GC_BLTFAST, GC_FLIP, GC_GETDC,
                  GC_PARTS,   // from here: parts of Blt / BltFast (not added to the total)
                  GC_FILL = GC_PARTS, GC_COPYBIG, GC_COPY, GC_STRETCH, GC_COUNT };
enum LockId { LK_G, LK_DIRTY, LK_PIX, LK_COUNT };
void perf_call(int id, LONGLONG ticks);
void lock_enter(CRITICAL_SECTION *cs, int id);   // EnterCriticalSection; a wait (lock busy) is timed and counted
inline LONGLONG perf_now()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}
struct PerfCall {
    int id;
    LONGLONG t0;
    explicit PerfCall(int i) : id(i), t0(perf_now()) {}
    ~PerfCall() { perf_call(id, perf_now() - t0); }
};

// Q150 blit options, read by every Blt (the render thread switches them for perfab)
enum { BM_FASTSTRETCH = 1, BM_NTCOPY = 2 };
extern volatile LONG g_blitMode;
void blitstats_flush(double seconds);            // surface.cpp, blitstats=true: one log line (render thread)
extern volatile LONG g_gameCpu;                 // render.cpp: processor of the game thread's last full-screen copy
