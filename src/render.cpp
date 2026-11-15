// Presentation thread: snapshots the changed parts of the primary surface into a shadow copy and
// draws it with OpenGL 1.1 (glTexSubImage2D + one textured quad).
//
// The shadow is the single source of truth for what is in the texture: changed pixels are copied
// primary -> shadow, the texture is only ever updated from the shadow. writewatch (Q149, off by default): the primary is
// write-watched memory; Windows lists the pages written since the last copy (reset before copying, so a write
// that lands during the copy is listed again), the copy runs under g_pixLock without g_lock, and a burst of
// writes is presented once it is over. Without it (legacy): dirty rects plus a scan of the whole picture
// against the shadow (pollprimary), under g_lock.
// 16 bpp is uploaded as GL_UNSIGNED_SHORT_5_6_5 and 32 bpp as GL_BGRA straight from the shadow;
// only 8 bpp goes through a palette lookup into an RGBA staging image.
#include "qdd.h"
#include "frameplan.h"
#include <GL/gl.h>
#include <dwmapi.h>   // DWM_TIMING_INFO only, the function is resolved at run time
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#define GL_STREAM_DRAW 0x88E0
#define GL_WRITE_ONLY 0x88B9
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#define GL_MAP_UNSYNCHRONIZED_BIT 0x0020

typedef BOOL(WINAPI *PFN_wglSwapIntervalEXT)(int);
typedef ptrdiff_t GLsizeiptr_;
typedef ptrdiff_t GLintptr_;
typedef void(APIENTRY *PFN_glGenBuffers)(GLsizei, GLuint *);
typedef void(APIENTRY *PFN_glDeleteBuffers)(GLsizei, const GLuint *);
typedef void(APIENTRY *PFN_glBindBuffer)(GLenum, GLuint);
typedef void(APIENTRY *PFN_glBufferData)(GLenum, GLsizeiptr_, const void *, GLenum);
typedef void *(APIENTRY *PFN_glMapBufferRange)(GLenum, GLintptr_, GLsizeiptr_, GLbitfield);
typedef void *(APIENTRY *PFN_glMapBuffer)(GLenum, GLenum);
typedef GLboolean(APIENTRY *PFN_glUnmapBuffer)(GLenum);

static HWND s_hwnd;
static HANDLE s_thread, s_evt;
static volatile LONG s_stop;

// dirty state (s_dcs is a leaf lock; never take g_lock while holding it)
static CRITICAL_SECTION s_dcs;
static bool s_dcsInit;
static RECT s_dirty;
static bool s_hasDirty, s_fullDirty, s_palDirty;
static long s_dirtyN;               // presentsync: drawing calls that changed the primary since the render thread took the dirty state
// Q165 framesync: full-surface copies to the primary (game frames), the time of the last one (qpc_ms), and the
// intervals between them (written by the game thread only, read and cleared at each stats flush)
static volatile LONG s_frameN;
static volatile double s_lastFrameMs;
static long s_frameHist[1000];
static long s_frameHistN;
static volatile LONG s_force;       // window event: re-upload everything and present (rate limited)
static volatile LONG s_forceCount;
static volatile LONG s_rebuild;     // display mode / presentation change: new texture + complete upload
static const char *volatile s_rebuildWhy;
static LONG s_rebuildCount;
static volatile LONG s_shotReq;     // keyscreenshot pressed: save the picture on the next tick
static const char *volatile s_backend = "?";

// render-thread-only state
static BYTE *s_shadow;              // copy of the primary (same size / pitch), what the texture shows
static size_t s_shadowSize;
static const BYTE *s_shadowOf;
static DWORD *s_staging;            // 8 bpp only: RGBA image of the visible part
static int s_imgW, s_imgH, s_imgX, s_imgY;
static int s_texW, s_texH;
static DWORD s_lut8[256];
static GLuint s_tex;

static void ensure_cs()
{
    if (!s_dcsInit) {
        InitializeCriticalSection(&s_dcs);
        s_dcsInit = true;
    }
}

// --- Q149 cost accounting ----------------------------------------------------------------------
// Time the game spends inside our drawing calls (QueryPerformanceCounter ticks) and waiting for a lock the render
// thread holds. Summed by the calling threads, read and cleared by the render thread with the perf line.
static volatile LONG64 s_callTicks[GC_COUNT];
static volatile LONG s_calls[GC_COUNT];
static volatile LONG64 s_waitTicks[LK_COUNT], s_waitMax[LK_COUNT];
static volatile LONG s_waits[LK_COUNT];

// --- Q153 rendercluster ---------------------------------------------------------------------------------------
volatile LONG g_gameCpu = -1;   // processor the game thread made its last full-screen copy on (surface.cpp)

// the processors that share an L3 cache with processor cpu (one CCD of a Ryzen), 0 if unknown; group 0 only
static DWORD_PTR l3_mask_of(LONG cpu)
{
    static DWORD_PTR masks[32];
    static int n = -1;
    if (n < 0) {
        n = 0;
        DWORD len = 0;
        GetLogicalProcessorInformationEx(RelationCache, NULL, &len);
        BYTE *buf = len ? (BYTE *)malloc(len) : NULL;
        if (buf && GetLogicalProcessorInformationEx(RelationCache, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
            for (DWORD at = 0; at < len && n < 32;) {
                PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + at);
                if (e->Relationship == RelationCache && e->Cache.Level == 3 && e->Cache.GroupMask.Group == 0)
                    masks[n++] = (DWORD_PTR)e->Cache.GroupMask.Mask;
                if (!e->Size) break;
                at += e->Size;
            }
        }
        free(buf);
        log_event("rendercluster: %d L3 cache group(s)%s", n, n ? "" : " - not available");
        for (int i = 0; i < n && i < 4; i++)
            log_event("rendercluster: L3 group %d = processors mask 0x%Ix", i, masks[i]);
    }
    if (cpu < 0 || cpu >= (LONG)(sizeof(DWORD_PTR) * 8))
        return 0;
    for (int i = 0; i < n; i++)
        if (masks[i] & ((DWORD_PTR)1 << cpu)) return masks[i];
    return 0;
}

// once a second on the render thread: follow the game thread's L3 group (on) / back to all processors (off)
static void cluster_step(bool on, DWORD_PTR *applied, long *moves)
{
    if (on) {
        DWORD_PTR m = l3_mask_of(g_gameCpu);
        if (m && m != *applied && SetThreadAffinityMask(GetCurrentThread(), m)) {
            if (++*moves <= 5)
                log_event("rendercluster: render thread kept on processors 0x%Ix (the game thread was on %ld)", m, g_gameCpu);
            *applied = m;
        }
    } else if (*applied) {
        DWORD_PTR proc = 0, sys = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) && SetThreadAffinityMask(GetCurrentThread(), proc))
            log_event("rendercluster: render thread back on all processors (0x%Ix)", proc);
        *applied = 0;
    }
}
static volatile DWORD s_renderTid;

void perf_call(int id, LONGLONG ticks)
{
    InterlockedExchangeAdd64(&s_callTicks[id], ticks);
    InterlockedIncrement(&s_calls[id]);
}

void lock_enter(CRITICAL_SECTION *cs, int id)
{
    if (TryEnterCriticalSection(cs))
        return;
    LONGLONG t0 = perf_now();
    EnterCriticalSection(cs);
    if (GetCurrentThreadId() == s_renderTid)
        return;   // only what the game (and the window / log threads) wait for
    LONGLONG dt = perf_now() - t0;
    InterlockedExchangeAdd64(&s_waitTicks[id], dt);
    InterlockedIncrement(&s_waits[id]);
    if (dt > s_waitMax[id])
        s_waitMax[id] = dt;   // a lost race only loses a maximum
}

void compute_viewport(int cw, int ch, int iw, int ih, RECT *vp)
{
    if (iw <= 0 || ih <= 0 || cw <= 0 || ch <= 0) {
        SetRect(vp, 0, 0, cw > 0 ? cw : 1, ch > 0 ? ch : 1);
        return;
    }
    int w = cw, h = ch;
    if (g_cfg.scale != SCALE_STRETCH) {
        if (g_cfg.scale == SCALE_INTEGER && cw >= iw && ch >= ih) {
            int s = cw / iw < ch / ih ? cw / iw : ch / ih;
            w = iw * s;
            h = ih * s;
        } else if ((long long)cw * ih > (long long)ch * iw) {
            w = (int)((long long)ch * iw / ih);
        } else {
            h = (int)((long long)cw * ih / iw);
        }
    }
    vp->left = (cw - w) / 2;
    vp->top = (ch - h) / 2;
    vp->right = vp->left + w;
    vp->bottom = vp->top + h;
}

// --- dirty tracking (called from the game thread) --------------------------------------

void render_dirty(Surface *s, const RECT *r)
{
    DDraw *dd = g_dd;
    if (!dd || dd->primary != s || !s_thread)
        return;
    ensure_cs();
    lock_enter(&s_dcs, LK_DIRTY);
    if (!r) {
        s_fullDirty = true;
        s_dirtyN++;
    } else if (!s_hasDirty) {
        s_dirty = *r;
        s_dirtyN++;
        s_hasDirty = true;
    } else {
        UnionRect(&s_dirty, &s_dirty, r);
        s_dirtyN++;
    }
    LeaveCriticalSection(&s_dcs);
    SetEvent(s_evt);
}

static double qpc_ms();

// Q165: the game copied a surface onto the whole primary - its frame is complete. Marks everything dirty (the copy is
// what gets uploaded) and counts the frame for the render thread; the interval between frames goes to the histogram.
void render_frame(Surface *s)
{
    DDraw *dd = g_dd;
    if (!dd || dd->primary != s || !s_thread)
        return;
    double now = qpc_ms(), prev = s_lastFrameMs;
    if (prev > 0) {
        double iv = now - prev;
        if (iv > 0 && iv < 250.0) {
            s_frameHist[(int)(iv * 4.0)]++;
            s_frameHistN++;
        }
    }
    ensure_cs();
    lock_enter(&s_dcs, LK_DIRTY);
    s_fullDirty = true;
    s_dirtyN++;
    s_lastFrameMs = now;
    InterlockedIncrement(&s_frameN);
    LeaveCriticalSection(&s_dcs);
    SetEvent(s_evt);
}

void render_full_dirty()
{
    ensure_cs();
    lock_enter(&s_dcs, LK_DIRTY);
    s_fullDirty = true;
    s_dirtyN++;
    LeaveCriticalSection(&s_dcs);
    if (s_evt) SetEvent(s_evt);
}

void render_palette_changed(Palette *p)
{
    DDraw *dd = g_dd;
    if (!dd || !dd->primary || (p && dd->primary->pal != p))
        return;
    ensure_cs();
    lock_enter(&s_dcs, LK_DIRTY);
    s_palDirty = true;
    LeaveCriticalSection(&s_dcs);
    if (s_evt) SetEvent(s_evt);
}

void render_kick()
{
    if (s_evt) SetEvent(s_evt);
}

void render_force()
{
    jr(J_FORCE);
    InterlockedExchange(&s_force, 1);
    if (s_evt) SetEvent(s_evt);
}

void render_rebuild(const char *why)
{
    s_rebuildWhy = why;   // string literal
    InterlockedExchange(&s_rebuild, 1);
    if (s_evt) SetEvent(s_evt);
}

bool render_paused()
{
    return s_hwnd && IsIconic(s_hwnd);
}

// --- 8 bpp palette lookup ------------------------------------------------------------------

static void build_lut8(Palette *p)
{
    for (int i = 0; i < 256; i++) {
        if (p)
            s_lut8[i] = p->ents[i].peRed | (p->ents[i].peGreen << 8) | (p->ents[i].peBlue << 16) | 0xFF000000u;
        else
            s_lut8[i] = (DWORD)i * 0x010101u | 0xFF000000u;
    }
}

// shadow rect r (primary coords, inside the visible part) -> RGBA staging image
static void convert8(int pitch, const RECT &r)
{
    int w = r.right - r.left;
    for (int y = r.top; y < r.bottom; y++) {
        DWORD *dst = s_staging + (size_t)(y - s_imgY) * s_imgW + (r.left - s_imgX);
        const BYTE *src = s_shadow + (size_t)y * pitch + r.left;
        for (int x = 0; x < w; x++)
            dst[x] = s_lut8[src[x]];
    }
}

// --- primary -> shadow ---------------------------------------------------------------------
// Caller holds g_lock (legacy) or g_pixLock (writewatch).

static bool shadow_ensure(const Surface *p)
{
    size_t size = (size_t)p->pitch * p->h;
    if (s_shadow && s_shadowSize == size && s_shadowOf == p->bits)
        return false;
    free(s_shadow);
    s_shadow = (BYTE *)malloc(size);
    s_shadowSize = s_shadow ? size : 0;
    s_shadowOf = p->bits;
    return true;   // new shadow: caller copies everything
}

static void shadow_copy(const Surface *p, const RECT &r)
{
    int bpp = p->bpp / 8;
    size_t off = (size_t)r.left * bpp, len = (size_t)(r.right - r.left) * bpp;
    for (int y = r.top; y < r.bottom; y++) {
        size_t at = (size_t)y * p->pitch + off;
        memcpy(s_shadow + at, p->bits + at, len);
    }
}

// Q153 fusedupload: rect r of the primary to the shadow and, in the same pass (the row is still in the cache), into
// the mapped pixel buffer laid out like the shown image (imgW pixels per row, image origin imgX / imgY); fromShadow
// copies the rect from the shadow instead (rows the scan has just put there)
static void fused_copy(const Surface *p, const RECT &r, BYTE *pbo, int imgX, int imgY, int imgW, bool fromShadow)
{
    int bpp = p->bpp / 8;
    size_t off = (size_t)r.left * bpp, len = (size_t)(r.right - r.left) * bpp;
    for (int y = r.top; y < r.bottom; y++) {
        size_t at = (size_t)y * p->pitch + off;
        BYTE *dst = pbo + ((size_t)(y - imgY) * imgW + (size_t)(r.left - imgX)) * bpp;
        if (fromShadow) {
            memcpy(dst, s_shadow + at, len);
        } else {
            memcpy(s_shadow + at, p->bits + at, len);
            memcpy(dst, p->bits + at, len);
        }
    }
}

// pollprimary: some games keep the pointer from an earlier Lock and write the primary after Unlock
// (QEC's Entropy.exe), so no dirty rect is reported. Rows of vis outside [skipTop, skipBottom) are
// compared with the shadow; changed rows are copied and returned as one band.
static bool shadow_scan(const Surface *p, const RECT &vis, int skipTop, int skipBottom, RECT *changed)
{
    int bpp = p->bpp / 8;
    size_t off = (size_t)vis.left * bpp, len = (size_t)(vis.right - vis.left) * bpp;
    int top = -1, bottom = -1;
    for (int y = vis.top; y < vis.bottom; y++) {
        if (y >= skipTop && y < skipBottom) {
            y = skipBottom - 1;
            continue;
        }
        size_t at = (size_t)y * p->pitch + off;
        if (memcmp(p->bits + at, s_shadow + at, len) != 0) {
            memcpy(s_shadow + at, p->bits + at, len);
            if (top < 0) top = y;
            bottom = y + 1;
        }
    }
    if (top < 0)
        return false;
    SetRect(changed, vis.left, top, vis.right, bottom);
    return true;
}

static int next_pot(int v)
{
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

// --- timing statistics (written to the log every 10 s) ------------------------------------

enum StatId { ST_LOCK, ST_COPY, ST_SCAN, ST_HOLD, ST_WW, ST_UPLOAD, ST_DRAW, ST_SWAP, ST_TOTAL, ST_COUNT };
static const char *const s_statNames[ST_COUNT] = {"wait g_lock", "copy dirty", "scan (memcmp)", "hold g_lock",
                                                  "GetWriteWatch", "upload",
                                                  "draw", "SwapBuffers", "present total"};
static const int IV_BINS = 1000;   // 0.25 ms bins below 250 ms (the interval_add limit)
struct Stats {
    double sum[ST_COUNT], max[ST_COUNT];
    long ticks, presents, updates, fullUploads, forced, polled;
    long settled, settleCapped, bands;   // writewatch: presents after a burst, of them cut by presentcap, bands copied
    long verifyChecks, verifyBad;          // verifytex: texture read back and compared with the shadow / with differences
    double settleSum, settleMax;          // ms from the first write of a burst to its copy
    long long upBytes;
    // present-to-present intervals (< 250 ms) and their distance to the display's refresh grid
    long ivN, ivOnGrid;
    double ivSum, ivSq, ivGridErr;
    long ivHist[IV_BINS];   // the same intervals in 0.25 ms bins, for the presentsync / pacestats percentiles
    long upHist[IV_BINS];   // pacestats: upload time per present in 0.05 ms bins (the last bin holds 50 ms and more)
    long frameTicks, deferred;   // framesync: presents for a game frame; wake-ups whose partial writes waited for the frame
    // where in the refresh cycle each present ended (0 = right at the vertical blank, 1 = just before the next)
    long phN, phEarly;
    double phSum;
};

static void phase_add(Stats &s, double phase)
{
    s.phN++;
    s.phSum += phase;
    if (phase < 0.25) s.phEarly++;   // first quarter of the cycle: 25% if presents were not tied to the blank
}

// refresh period of the monitor the window is on (ms), 0 if unknown
static double refresh_period_ms(HWND hwnd, DWORD *hz)
{
    MONITORINFOEXA mi;
    mi.cbSize = sizeof(mi);
    DEVMODEA dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    *hz = 0;
    if (!GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi) ||
        !EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) || dm.dmDisplayFrequency <= 1)
        return 0;
    *hz = dm.dmDisplayFrequency;
    return 1000.0 / dm.dmDisplayFrequency;
}

static void interval_add(Stats &s, double iv, double period)
{
    if (iv <= 0 || iv >= 250.0)
        return;   // idle gaps say nothing about pacing
    s.ivN++;
    s.ivSum += iv;
    s.ivSq += iv * iv;
    s.ivHist[(int)(iv * 4.0)]++;
    if (period > 0) {
        double k = (double)(long)(iv / period + 0.5);
        double e = iv - k * period;
        if (e < 0) e = -e;
        s.ivGridErr += e;
        if (e <= 0.5) s.ivOnGrid++;
    }
}

static double qpc_ms()
{
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void stat_add(Stats &s, int id, double ms)
{
    s.sum[id] += ms;
    if (ms > s.max[id]) s.max[id] = ms;
}

static ULONGLONG ft64(const FILETIME &f)
{
    return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime;   // 100 ns units
}

// Q149: CPU of the process / render thread / game thread (GetProcessTimes / GetThreadTimes, the numbers Task
// Manager and Get-Process show; cycle counts as the precise second opinion) and the game's time inside our calls
// and lock waits, since the last call
static void cost_flush(double seconds, const char *mode)
{
    static ULONGLONG last[6];
    static ULONG64 lastCyc[3];
    static bool have;
    static HANDLE game;
    static DWORD gameTid;
    DWORD tid = g_gameThread;
    if (tid != gameTid) {
        if (game) CloseHandle(game);
        game = tid ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid) : NULL;
        gameTid = tid;
        have = false;
    }
    ULONGLONG now[6] = {0, 0, 0, 0, 0, 0};   // process user / kernel, render user / kernel, game user / kernel
    ULONG64 cyc[3] = {0, 0, 0};
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) { now[0] = ft64(u); now[1] = ft64(k); }
    if (GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) { now[2] = ft64(u); now[3] = ft64(k); }
    if (game && GetThreadTimes(game, &c, &e, &k, &u)) { now[4] = ft64(u); now[5] = ft64(k); }
    QueryProcessCycleTime(GetCurrentProcess(), &cyc[0]);
    QueryThreadCycleTime(GetCurrentThread(), &cyc[1]);
    if (game) QueryThreadCycleTime(game, &cyc[2]);

    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    double tms = 1000.0 / (double)f.QuadPart / seconds;   // QPC ticks -> ms per second
    double callMs[GC_COUNT], callN[GC_COUNT], callSum = 0;
    for (int i = 0; i < GC_COUNT; i++) {
        callMs[i] = (double)InterlockedExchange64(&s_callTicks[i], 0) * tms;
        callN[i] = InterlockedExchange(&s_calls[i], 0) / seconds;
        if (i < GC_PARTS) callSum += callMs[i];   // the parts are inside Blt / BltFast
    }
    double waitMs[LK_COUNT], waitMax[LK_COUNT];
    long waitN[LK_COUNT];
    for (int i = 0; i < LK_COUNT; i++) {
        waitMs[i] = (double)InterlockedExchange64(&s_waitTicks[i], 0) * tms;
        waitMax[i] = (double)InterlockedExchange64(&s_waitMax[i], 0) * 1000.0 / (double)f.QuadPart;
        waitN[i] = InterlockedExchange(&s_waits[i], 0);
    }
    if (have) {
        double d[6];
        for (int i = 0; i < 6; i++) d[i] = (double)(now[i] - last[i]) / 10000.0 / seconds;   // ms per second
        double mc[3];
        for (int i = 0; i < 3; i++) mc[i] = (double)(cyc[i] - lastCyc[i]) / 1e6 / seconds;
        log_event("cost %.1fs [%s]: CPU process %.1f ms/s (user %.1f kernel %.1f, %.1f%% of one core, %.0f Mcycles/s) | "
                  "render thread %.1f ms/s (%.0f Mcycles/s) | game thread %.1f ms/s (kernel %.1f, %.0f Mcycles/s) | game "
                  "inside our calls %.2f ms/s: Lock %.3f (%.0f/s) Unlock %.3f (%.0f/s) Blt %.2f (%.0f/s) BltFast %.2f "
                  "(%.0f/s) Flip %.3f (%.0f/s) GetDC %.3f (%.0f/s) [Blt parts: fill %.2f (%.0f/s), copy 1M+ px %.2f (%.0f/s, "
                  "%.0f us each), copy %.2f (%.0f/s), stretch %.2f (%.0f/s, %.0f us each)]",
                  seconds, mode, d[0] + d[1], d[0], d[1], (d[0] + d[1]) / 10.0, mc[0], d[2] + d[3], mc[1], d[4] + d[5],
                  d[5], mc[2], callSum, callMs[GC_LOCK], callN[GC_LOCK], callMs[GC_UNLOCK], callN[GC_UNLOCK],
                  callMs[GC_BLT], callN[GC_BLT], callMs[GC_BLTFAST], callN[GC_BLTFAST], callMs[GC_FLIP], callN[GC_FLIP],
                  callMs[GC_GETDC], callN[GC_GETDC], callMs[GC_FILL], callN[GC_FILL], callMs[GC_COPYBIG], callN[GC_COPYBIG],
                  callN[GC_COPYBIG] > 0 ? callMs[GC_COPYBIG] / callN[GC_COPYBIG] * 1000.0 : 0.0, callMs[GC_COPY], callN[GC_COPY],
                  callMs[GC_STRETCH], callN[GC_STRETCH],
                  callN[GC_STRETCH] > 0 ? callMs[GC_STRETCH] / callN[GC_STRETCH] * 1000.0 : 0.0);
        // own line: the first one already reaches the log's line length
        log_event("cost %.1fs [%s]: game waited for our locks: g_lock %.3f ms/s (%ld times, max %.2f ms), dirty %.3f (%ld, max "
                  "%.2f), pixels %.3f (%ld, max %.2f)",
                  seconds, mode, waitMs[LK_G], waitN[LK_G], waitMax[LK_G], waitMs[LK_DIRTY],
                  waitN[LK_DIRTY], waitMax[LK_DIRTY], waitMs[LK_PIX], waitN[LK_PIX], waitMax[LK_PIX]);
    }
    memcpy(last, now, sizeof(last));
    memcpy(lastCyc, cyc, sizeof(lastCyc));
    have = true;
}

// p50 / p90 / p99 (upper bin edges) and the upper edge of the last filled bin of a histogram with n entries
static void hist_percentiles(const long *hist, long n, double binMs, double at[3], double *top)
{
    static const double pct[] = {50, 90, 99};
    at[0] = at[1] = at[2] = *top = 0;
    long seen = 0;
    int k = 0;
    for (int b = 0; b < IV_BINS; b++) {
        if (!hist[b])
            continue;
        seen += hist[b];
        *top = (b + 1) * binMs;
        while (k < 3 && seen * 100.0 >= pct[k] * n)
            at[k++] = *top;
    }
}

// the Q148 way without presentsync, where presentquiet / presentcap play no part
static bool legacy_mode(const char *mode)
{
    return strcmp(mode, "legacy") == 0;
}

static void stats_flush(Stats &s, double seconds, const char *how, double period, const char *mode)
{
    if (s.ivN > 1) {
        double avg = s.ivSum / s.ivN, var = s.ivSq / s.ivN - avg * avg;
        // uniformly random present times would sit period/4 off the refresh grid on average
        log_event("pacing %.1fs: %ld present intervals avg %.2f ms sd %.2f ms | refresh grid %.2f ms: mean distance %.2f ms "
                  "(random would be %.2f), %.0f%% within 0.5 ms",
                  seconds, s.ivN, avg, var > 0 ? sqrt(var) : 0.0, period, s.ivGridErr / s.ivN, period / 4,
                  100.0 * s.ivOnGrid / s.ivN);
    }
    // presentsync coalesces the game's drawing bursts into one present; uneven bursts give uneven intervals (Q163) -
    // the spread of the intervals is what a later retune of presentquiet / presentcap has to look at. pacestats (Q164)
    // logs the same in every mode, with the upload time per present and the bytes uploaded per present.
    bool presentSyncMode = strcmp(mode, "presentsync") == 0;
    if (s.ivN > 1 && (presentSyncMode || g_cfg.paceStats)) {
        double at[3], top;
        hist_percentiles(s.ivHist, s.ivN, 0.25, at, &top);
        log_event("present intervals %.1fs [%s]: %ld, p50 %.2f ms p90 %.2f p99 %.2f max %.2f (bin upper edges, "
                  "0.25 ms bins; presentquiet=%d presentcap=%d%s)",
                  seconds, mode, s.ivN, at[0], at[1], at[2], top, g_cfg.presentQuiet, g_cfg.presentCap,
                  presentSyncMode || !legacy_mode(mode) ? "" : ", not used in this mode");
    }
    if (g_cfg.paceStats && s.presents > 0) {
        long n = 0;
        for (int b = 0; b < IV_BINS; b++) n += s.upHist[b];
        double at[3] = {0, 0, 0}, top = 0;
        if (n > 0)
            hist_percentiles(s.upHist, n, 0.05, at, &top);
        log_event("upload per present %.1fs [%s]: %ld presents (%ld with new pixels), p50 %.2f ms p90 %.2f p99 %.2f max %.2f, "
                  "%.0f KB uploaded per present (%.0f KB per present with new pixels)",
                  seconds, mode, s.presents, s.updates, at[0], at[1], at[2], top, s.upBytes / 1024.0 / s.presents,
                  s.updates ? s.upBytes / 1024.0 / s.updates : 0.0);
    }
    {   // Q165: the game's frames (full-surface copies to the primary) as the game made them
        long n = s_frameHistN;
        if (n > 1 || s.frameTicks || s.deferred) {
            double at[3] = {0, 0, 0}, top = 0;
            if (n > 1)
                hist_percentiles(s_frameHist, n, 0.25, at, &top);
            log_event("game frames %.1fs [%s]: %ld full copies to the primary (%.1f/s), interval p50 %.2f ms p90 %.2f p99 %.2f "
                      "max %.2f | framesync %s: %ld presents for a frame, %ld wake-ups with partial writes held back",
                      seconds, mode, n, n / seconds, at[0], at[1], at[2], top,
                      g_cfg.frameSync ? "on" : "off", s.frameTicks, s.deferred);
        }
        memset(s_frameHist, 0, sizeof(s_frameHist));
        s_frameHistN = 0;
    }
    if (s.phN > 1)
        log_event("vblank phase %.1fs: %ld presents, mean phase %.2f of a refresh (0.50 if unrelated), %.0f%% in the first "
                  "quarter after the blank (25%% if unrelated)",
                  seconds, s.phN, s.phSum / s.phN, 100.0 * s.phEarly / s.phN);
    char line[900];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "perf %.1fs [%s]: %.1f fps presented, %.1f/s with new pixels, %ld full, %ld forced, %ld via poll, "
                        "%.1f MB/s uploaded (%s), %.0f ticks/s, scan cost %.0f ms/s | avg/max ms:",
                        seconds, mode, s.presents / seconds, s.updates / seconds, s.fullUploads, s.forced, s.polled,
                        s.upBytes / seconds / 1048576.0, how, s.ticks / seconds, s.sum[ST_SCAN] / seconds);
    for (int i = 0; i < ST_COUNT && n > 0 && n < (int)sizeof(line); i++) {
        long div = i <= ST_WW ? s.ticks : s.presents;   // lock / copy / scan / GetWriteWatch per tick, the rest per present
        int m = _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " %s %.2f/%.1f", s_statNames[i],
                            div ? s.sum[i] / div : 0.0, s.max[i]);
        if (m < 0) break;
        n += m;
    }
    log_event("%s", line);
    if (s.settled)
        log_event("present plan %.1fs [%s]: %ld presents after a burst of writes - %.2f ms from its first write to the copy "
                  "(max %.1f), %ld cut at presentcap, %.1f row bands per copy",
                  seconds, mode, s.settled, s.settleSum / s.settled, s.settleMax, s.settleCapped, (double)s.bands / s.settled);
    if (s.verifyChecks)
        log_event("verifytex %.1fs [%s]: %ld texture read-back(s) compared with the shadow, %ld with differences", seconds,
                  mode, s.verifyChecks, s.verifyBad);
    cost_flush(seconds, mode);
    blitstats_flush(seconds);
    memset(&s, 0, sizeof(s));
}

// --- thread ------------------------------------------------------------------------------

// OpenGL context for the window (pixel format, wglCreateContext, wglMakeCurrent). false + reason on failure.
static bool gl_create(HDC dc, HGLRC *out, char *why, size_t cap)
{
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.iLayerType = PFD_MAIN_PLANE;
    if (GetPixelFormat(dc) == 0) {
        int pf = ChoosePixelFormat(dc, &pfd);
        if (!pf || !SetPixelFormat(dc, pf, &pfd)) {
            _snprintf_s(why, cap, _TRUNCATE, "%s failed (error %lu)", pf ? "SetPixelFormat" : "ChoosePixelFormat",
                        GetLastError());
            return false;
        }
    }
    if (g_cfg.forceGlFail) {
        _snprintf_s(why, cap, _TRUNCATE, "wglCreateContext failed (simulated by forceglfail=true)");
        return false;
    }
    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) {
        _snprintf_s(why, cap, _TRUNCATE, "%s failed (error %lu)", rc ? "wglMakeCurrent" : "wglCreateContext", GetLastError());
        if (rc) wglDeleteContext(rc);
        return false;
    }
    *out = rc;
    return true;
}

// GDI backend: the shadow goes to the window DC with StretchDIBits (16 bpp as 5-6-5 bit fields, 8 bpp
// with the palette as colour table); the black bars around the viewport are filled separately (no flicker).
static PicSrc pic_src(int bpp, int pitch, int primW, int primH)
{
    PicSrc s;
    s.bits = s_shadow;
    s.pitch = pitch;
    s.bpp = bpp;
    s.primW = primW;
    s.primH = primH;
    s.imgX = s_imgX;
    s.imgY = s_imgY;
    s.imgW = s_imgW;
    s.imgH = s_imgH;
    s.lut8 = s_lut8;
    s.linear = g_cfg.linear;
    return s;
}

static void gdi_present(HDC dc, int cw, int ch, const RECT &vp, int bpp, int pitch, int primW, int primH)
{
    RECT all = {0, 0, cw, ch};
    picture_paint(dc, all, vp, pic_src(bpp, pitch, primW, primH));
}

// fixchilds paint-through: the game's dialogs are child windows over the client area and Windows shows them
// above our output; each gets the part of the picture that lies under it, exactly as the screen shows it there
// (bars and scaling included). Only after a present with new pixels, a forced one, or every 500 ms.
static void children_paint(int cw, int ch, const RECT &vp, int bpp, int pitch, int primW, int primH)
{
    ChildTarget t[16];
    int n = window_child_targets(t, 16);
    if (!n)
        return;
    PicSrc s = pic_src(bpp, pitch, primW, primH);
    for (int i = 0; i < n; i++) {
        // Q156: the dialog may have been closed since the list was made (the game thread destroys them at will)
        if (!IsWindow(t[i].hwnd) || !IsWindowVisible(t[i].hwnd))
            continue;
        jr(J_CHILDPAINT, (ULONG_PTR)t[i].hwnd, t[i].w, t[i].hh);
        HDC dc = GetDC(t[i].hwnd);
        if (!dc)
            continue;
        RECT area, v;
        child_coords(cw, ch, vp, t[i].x, t[i].y, &area, &v);
        picture_paint(dc, area, v, s);
        ReleaseDC(t[i].hwnd, dc);
    }
    static LONG logged;
    if (InterlockedIncrement(&logged) <= 5)
        log_event("fixchilds: picture painted into %d child window(s), first %p at (%d,%d) %dx%d", n, t[0].hwnd, t[0].x,
                  t[0].y, t[0].w, t[0].hh);
}
// keyscreenshot: the picture at game resolution (no bars, no scaling) as RGBA, encoded by a worker thread
struct ShotJob {
    DWORD *rgba;
    int w, h;
    char path[MAX_PATH];
};

static DWORD WINAPI shot_thread(LPVOID p)
{
    ShotJob *job = (ShotJob *)p;
    double t0 = qpc_ms();
    bool ok = png_write(job->path, job->rgba, job->w, job->h);
    log_event("screenshot: %s %dx%d -> %s (%.0f ms)", ok ? "saved" : "FAILED to save", job->w, job->h, job->path,
              qpc_ms() - t0);
    free(job->rgba);
    free(job);
    return 0;
}

static void screenshot_take(int bpp, int pitch, int primW, int primH)
{
    ShotJob *job = (ShotJob *)calloc(1, sizeof(ShotJob));
    if (!job)
        return;
    job->w = s_imgW;
    job->h = s_imgH;
    job->rgba = (DWORD *)calloc((size_t)s_imgW * s_imgH, sizeof(DWORD));
    if (!job->rgba) {
        free(job);
        return;
    }
    for (int y = 0; y < s_imgH; y++) {
        int sy = s_imgY + y;
        if (sy < 0 || sy >= primH)
            continue;
        const BYTE *row = s_shadow + (size_t)sy * pitch;
        DWORD *dst = job->rgba + (size_t)y * s_imgW;
        for (int x = 0; x < s_imgW; x++) {
            int sx = s_imgX + x;
            if (sx < 0 || sx >= primW)
                continue;
            DWORD v;
            if (bpp == 8) {
                v = s_lut8[row[sx]];
            } else if (bpp == 16) {
                WORD c = ((const WORD *)row)[sx];
                DWORD r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
                v = ((r << 3) | (r >> 2)) | (((g << 2) | (g >> 4)) << 8) | (((b << 3) | (b >> 2)) << 16);
            } else {
                DWORD c = ((const DWORD *)row)[sx];
                v = ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16);
            }
            dst[x] = v;
        }
    }
    char dir[MAX_PATH];
    _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%sScreenshots", log_dir());
    CreateDirectoryA(dir, NULL);
    char stamp[32];
    local_stamp(stamp, sizeof(stamp), true);
    _snprintf_s(job->path, sizeof(job->path), _TRUNCATE, "%s\\%s.png", dir, stamp);
    HANDLE t = CreateThread(NULL, 0, shot_thread, job, 0, NULL);
    if (t) CloseHandle(t);
    else shot_thread(job);
}

DWORD render_thread_id() { return s_renderTid; }
CRITICAL_SECTION *render_dirty_lock() { return &s_dcs; }

static DWORD WINAPI render_thread(LPVOID)
{
    s_renderTid = GetCurrentThreadId();
    if (g_cfg.renderPriority != THREAD_PRIORITY_NORMAL) {
        BOOL pr = SetThreadPriority(GetCurrentThread(), g_cfg.renderPriority);
        log_event("render: thread priority %d %s", g_cfg.renderPriority, pr ? "set" : "could not be set");
    }
    HWND hwnd = s_hwnd;
    HDC dc = GetDC(hwnd);
    if (!dc) {
        log_event("render: GetDC failed (%lu)", GetLastError());
        return 1;
    }
    bool useGL = g_cfg.renderer != RENDERER_GDI;
    HGLRC rc = NULL;
    const char *glver = NULL, *ext = NULL;
    if (useGL) {
        char why[160];
        if (!gl_create(dc, &rc, why, sizeof(why))) {
            if (g_cfg.renderer != RENDERER_AUTO) {
                log_event("render: renderer=opengl: %s - nothing can be shown", why);
                ReleaseDC(hwnd, dc);
                return 1;
            }
            log_event("render: renderer=auto: OpenGL not usable (%s) - falling back to GDI", why);
            useGL = false;
        } else {
            glver = (const char *)glGetString(GL_VERSION);
            const char *glren = (const char *)glGetString(GL_RENDERER);
            log_event("render: GL_VENDOR=%s | GL_RENDERER=%s | GL_VERSION=%s", (const char *)glGetString(GL_VENDOR), glren,
                      glver);
            // no display driver (remote desktop, basic display adapter): Windows' software GL 1.1 is slower than GDI
            if (g_cfg.renderer == RENDERER_AUTO && glren && strcmp(glren, "GDI Generic") == 0) {
                log_event("render: renderer=auto: OpenGL is the Microsoft software implementation (GDI Generic, no "
                          "driver) - falling back to GDI");
                wglMakeCurrent(NULL, NULL);
                wglDeleteContext(rc);
                rc = NULL;
                glver = NULL;
                useGL = false;
            }
        }
    }
    s_backend = useGL ? "OpenGL" : "GDI";
    log_event("render: backend %s (renderer=%s)%s", s_backend,
              g_cfg.renderer == RENDERER_AUTO ? "auto" : g_cfg.renderer == RENDERER_GDI ? "gdi" : "opengl",
              useGL ? "" : " - StretchDIBits to the window DC, no OpenGL context");

    int glMajor = glver ? atoi(glver) : 1, glMinor = 0;
    if (glver && strchr(glver, '.')) glMinor = atoi(strchr(glver, '.') + 1);
    bool gl12 = glMajor > 1 || glMinor >= 2;   // packed 5_6_5 and GL_BGRA
    if (useGL)
        ext = (const char *)glGetString(GL_EXTENSIONS);

    // pacing check: phase of every present relative to the display's last vertical blank, from DWM's
    // timing information (Vista+, resolved dynamically; measurement only)
    typedef HRESULT(WINAPI * PFN_DwmTiming)(HWND, DWM_TIMING_INFO *);
    HMODULE dwm = LoadLibraryA("dwmapi.dll");
    PFN_DwmTiming dwmTiming = dwm ? (PFN_DwmTiming)GetProcAddress(dwm, "DwmGetCompositionTimingInfo") : NULL;
    PFN_wglSwapIntervalEXT swapInterval = useGL ? (PFN_wglSwapIntervalEXT)wglGetProcAddress("wglSwapIntervalEXT") : NULL;
    if (swapInterval) {
        swapInterval(g_cfg.vsync ? 1 : 0);
        log_event("render: wglSwapIntervalEXT(%d)", g_cfg.vsync ? 1 : 0);
    } else if (useGL) {
        log_event("render: wglSwapIntervalEXT not available, vsync setting skipped");
    }

    // pixel buffer object for asynchronous uploads (GL 1.5 / ARB_pixel_buffer_object), optional
    PFN_glGenBuffers glGenBuffers_ = NULL;
    PFN_glDeleteBuffers glDeleteBuffers_ = NULL;
    PFN_glBindBuffer glBindBuffer_ = NULL;
    PFN_glBufferData glBufferData_ = NULL;
    PFN_glMapBufferRange glMapBufferRange_ = NULL;
    PFN_glMapBuffer glMapBuffer_ = NULL;
    PFN_glUnmapBuffer glUnmapBuffer_ = NULL;
    bool havePbo = false;
    if (useGL && g_cfg.pbo && (glMajor >= 2 || (ext && strstr(ext, "GL_ARB_pixel_buffer_object")))) {
        glGenBuffers_ = (PFN_glGenBuffers)wglGetProcAddress("glGenBuffers");
        glDeleteBuffers_ = (PFN_glDeleteBuffers)wglGetProcAddress("glDeleteBuffers");
        glBindBuffer_ = (PFN_glBindBuffer)wglGetProcAddress("glBindBuffer");
        glBufferData_ = (PFN_glBufferData)wglGetProcAddress("glBufferData");
        glMapBufferRange_ = (PFN_glMapBufferRange)wglGetProcAddress("glMapBufferRange");
        glMapBuffer_ = (PFN_glMapBuffer)wglGetProcAddress("glMapBuffer");
        glUnmapBuffer_ = (PFN_glUnmapBuffer)wglGetProcAddress("glUnmapBuffer");
        havePbo = glGenBuffers_ && glDeleteBuffers_ && glBindBuffer_ && glBufferData_ && glMapBuffer_ && glUnmapBuffer_;
    }
    const int PBO_COUNT = 2;
    GLuint pbo[PBO_COUNT] = {0, 0};
    size_t pboSize = 0;
    int pboNext = 0;
    if (havePbo)
        glGenBuffers_(PBO_COUNT, pbo);

    GLint maxTex = 0;
    build_lut8(NULL);
    if (useGL) {
        log_event("render: GL %d.%d, upload path: %s%s", glMajor, glMinor, gl12 ? "direct 5_6_5 / BGRA" : "RGBA conversion",
                  havePbo ? " via pixel buffer objects" : (g_cfg.pbo ? " (no PBO support, synchronous)" : " (pbo=false)"));
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
        glGenTextures(1, &s_tex);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_LIGHTING);
        glDisable(GL_BLEND);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
    }

    Palette *lastPal = NULL;
    double lastPresent = 0, lastForced = 0, lastChange = 0;
    LONG frameSeen = 0;   // framesync: the last game frame presented
    bool texValid = false;
    int texBpp = 0;
    int lastFilter = -1;
    Stats st;
    memset(&st, 0, sizeof(st));
    double statStart = qpc_ms();
    DWORD hz = 0;
    double period = refresh_period_ms(hwnd, &hz);
    double firstPresent = 0;
    double lastChildPaint = 0;
    log_event("render: display refresh %lu Hz, vsync %s", hz,
              !useGL ? "n/a (GDI)" : g_cfg.vsync ? "on (swap interval 1 + glFinish)" : "off");

    // Q149 writewatch state
    Settle settle;
    settle_reset(&settle);
    double nextWait = -1;          // ms until a burst of writes is looked at again (-1: the normal tick)
    void **wwPages = NULL;         // GetWriteWatch's page list
    size_t wwCap = 0;
    bool wwBroken = false, wwActive = false;
    double abStart = qpc_ms();     // perfab: phases of perfab seconds, legacy first
    int abPhase = 0;
    const char *statMode = NULL;
    bool fusedFailed = false;      // fusedupload: mapping failed once, copies go via the shadow
    DWORD_PTR clusterMask = 0;     // rendercluster: the affinity mask applied (0 = none)
    long clusterMoves = 0;
    double lastVerify = 0;

    while (!s_stop) {
        // the Q148 way ("legacy"): dirty rects + comparing the primary with the shadow (pollprimary) under g_lock.
        // presentsync (Q150; default off since Q163): legacy, but a frame is presented once the game's drawing calls to the primary
        // (Blt / Unlock) stop for presentquiet ms - one present per drawn frame. writewatch: the pages the game
        // wrote, copied without g_lock once the burst of writes is over. perfab (Q153) rotates "before" (the Q151
        // way), B = fusedupload and B+C = fusedupload + rendercluster, the rest as configured.
        double tw = qpc_ms();
        int phase = g_cfg.perfAB > 0 ? (int)((tw - abStart) / (g_cfg.perfAB * 1000.0)) : 0;
        int abKind = g_cfg.perfAB > 0 ? phase % 3 : -1;   // perfab: 0 before, 1 B, 2 B+C; -1 = the configuration
        bool legacy = !g_cfg.writeWatch;
        bool sync = legacy && g_cfg.presentSync;
        // Q153 B (fusedupload): changed rows read once, into the shadow and the pixel buffer; C (rendercluster): the
        // render thread runs on the core cluster (shared L3) the game thread was last seen on
        bool fusedOn = !fusedFailed && useGL && havePbo && gl12 && legacy &&
                       (abKind < 0 ? g_cfg.fusedUpload : abKind >= 1);
        bool clusterOn = abKind < 0 ? g_cfg.renderCluster : abKind == 2;
        const char *mode = abKind == 0 ? "before" : abKind == 1 ? "B" : abKind == 2 ? "B+C"
                         : !legacy ? "writewatch" : sync ? "presentsync" : "legacy";
        if (!statMode)
            statMode = mode;
        if (legacy && !sync)
            settle_reset(&settle);
        // legacy pollprimary: ~100 Hz while the picture is changing, ~30 Hz after half a second without changes
        // (Lock / Blt still wake the thread at once). writewatch: 125 / 30 Hz looks at the page count, and a
        // burst of writes is looked at again when its quiet time is over. Without either: 10 Hz idle redraws.
        bool recent = tw - lastChange < 500.0;
        DWORD wait;
        if (g_cfg.vsyncTest)
            wait = 0;
        else if (nextWait >= 0)
            wait = nextWait < 1.0 ? 1 : (DWORD)ceil(nextWait);
        else if (!legacy && wwActive)
            wait = recent ? 8 : 33;
        else
            wait = !g_cfg.pollPrimary ? 100 : recent ? 10 : 33;
        nextWait = -1;
        bool signaled = WaitForSingleObject(s_evt, wait) == WAIT_OBJECT_0;
        if (s_stop)
            break;
        if (!IsWindow(hwnd))
            break;
        double t0 = qpc_ms();
        window_children_tick();
        jr_tick();
        static double lastWatch;
        if (t0 - lastWatch >= 1000.0) {
            lastWatch = t0;
            window_watch();
            cluster_step(clusterOn, &clusterMask, &clusterMoves);
            clock_report(false);
        }
        // perf lines every 10 s; with perfab at every phase change, so each line covers one way only
        if (g_cfg.perfAB > 0 ? phase != abPhase : t0 - statStart >= 10000.0) {
            stats_flush(st, (t0 - statStart) / 1000.0, !useGL ? "GDI" : havePbo ? "PBO" : "sync", period, statMode);
            statStart = t0;
            if (g_cfg.perfAB > 0 && phase != abPhase)
                log_event("perfab: phase %d - %s for %d s (rendercluster moves so far %ld)", phase, mode, g_cfg.perfAB,
                          clusterMoves);
            abPhase = phase;
            statMode = mode;
        }
        if (g_cfg.crashTest && firstPresent > 0 && t0 - firstPresent >= 2000.0)
            crash_test_raise();
        if (IsIconic(hwnd) || !IsWindowVisible(hwnd))
            continue;   // minimized / hidden: do not present

        // the client rectangle and its screen position are read live every tick (window may be dragged)
        RECT cr;
        GetClientRect(hwnd, &cr);
        int cw = cr.right, ch = cr.bottom;
        if (cw <= 0 || ch <= 0)
            continue;

        // window events (paint / move / size / activation) force a complete refresh, at most 10 per second
        bool forced = false;
        if (s_force && t0 - lastForced >= 100.0) {
            InterlockedExchange(&s_force, 0);
            forced = true;
            lastForced = t0;
        }
        // display mode / presentation change: the texture is created again and filled from scratch,
        // not rate limited (the frame after a switch must not be black)
        bool rebuild = InterlockedExchange(&s_rebuild, 0) != 0;
        if (rebuild) {
            forced = true;
            InterlockedExchange(&s_force, 0);
            lastForced = t0;
            period = refresh_period_ms(hwnd, &hz);
            log_event("render: rebuild #%ld (%s): client %dx%d, display %lu Hz - texture re-created, full upload",
                      ++s_rebuildCount, s_rebuildWhy ? s_rebuildWhy : "?", cw, ch, hz);
        }

        RECT rects[BANDS_MAX + 1];   // what changed (primary coords), uploaded one by one
        BYTE *fusedMap = NULL;       // fusedupload: the mapped pixel buffer the changed rows went into this tick
        GLuint fusedBuf = 0;
        int nRects = 0;
        bool haveImage = false, realloc = false, full = false, polled = false, settling = false;
        int bpp = 0, pitch = 0, primW = 0, primH = 0;
        bool exclusive = false;

        // presentsync: while the game's drawing calls to the primary keep coming, it is still drawing this frame -
        // look again when they have stopped for presentquiet ms (no copy, no scan meanwhile)
        if (sync && !forced && texValid) {
            EnterCriticalSection(&s_dcs);
            long n = s_dirtyN;
            LeaveCriticalSection(&s_dcs);
            double first = settle.pending ? settle.first : t0, w = 0;
            int sr = settle_step(&settle, t0, n, g_cfg.presentQuiet, g_cfg.presentCap, &w);
            if (sr == SETTLE_WAIT) {
                nextWait = w;
                continue;
            }
            if (sr == SETTLE_GO) {
                double dur = t0 - first;
                st.settled++;
                st.settleSum += dur;
                if (dur > st.settleMax) st.settleMax = dur;
                if (g_cfg.presentCap > 0 && dur >= g_cfg.presentCap - 0.05) st.settleCapped++;
            }
        }

        // framesync (Q165): while the game copies whole frames onto the primary, present once per such copy. Writes
        // between two frames only wake the thread; they are not copied, not scanned and not uploaded - the next frame
        // copy contains them anyway. No frame for framefallback ms: the frame-less way below (dirty rects, pollprimary)
        // takes over again (menus, dialogs, the briefing, writes after the last frame).
        bool frameTick = false;
        if (legacy && !sync && g_cfg.frameSync && !forced && !rebuild && texValid && !g_cfg.vsyncTest) {
            LONG fn = InterlockedCompareExchange(&s_frameN, 0, 0);
            double lastFrame = s_lastFrameMs;
            if (fn != frameSeen) {
                frameSeen = fn;
                frameTick = true;
                st.frameTicks++;
            } else if (lastFrame > 0 && t0 - lastFrame < g_cfg.frameFallback) {
                EnterCriticalSection(&s_dcs);
                bool pending = s_fullDirty || s_hasDirty;
                LeaveCriticalSection(&s_dcs);
                if (pending && signaled)
                    st.deferred++;
                nextWait = lastFrame + g_cfg.frameFallback - t0;
                continue;
            }
        }

        EnterCriticalSection(&g_lock);
        bool lockHeld = true;
        double t1 = qpc_ms();
        stat_add(st, ST_LOCK, t1 - t0);
        st.ticks++;
        double tScan = 0;
        DDraw *dd = g_dd;
        Surface *p = dd ? dd->primary : NULL;
        if (p && p->bits) {
            haveImage = true;
            bpp = p->bpp;
            pitch = p->pitch;
            primW = p->w;
            primH = p->h;
            exclusive = (dd->coop & DDSCL_EXCLUSIVE) != 0 || dd->normalEmu;   // primary == the game screen
            RECT src;
            if (exclusive) {
                SetRect(&src, 0, 0, p->w, p->h);
            } else {
                // windowed: show the part of the (desktop sized) primary under our client area
                src = cr;
                MapWindowPoints(hwnd, NULL, (POINT *)&src, 2);
            }
            int iw = src.right - src.left, ih = src.bottom - src.top;
            if (iw != s_imgW || ih != s_imgH || bpp != texBpp || rebuild) {
                s_imgW = iw;
                s_imgH = ih;
                texBpp = bpp;
                realloc = true;
                full = true;
            }
            if (src.left != s_imgX || src.top != s_imgY) {
                s_imgX = src.left;
                s_imgY = src.top;
                full = true;
            }
            if (bpp == 8 && (realloc || !s_staging)) {
                free(s_staging);
                s_staging = (DWORD *)calloc((size_t)iw * ih, sizeof(DWORD));
            }
            if (bpp == 8 && p->pal != lastPal) {
                lastPal = p->pal;
                s_palDirty = true;
            }
            if (shadow_ensure(p))
                full = true;
            if (forced || frameTick)
                full = true;

            EnterCriticalSection(&s_dcs);
            if (s_palDirty) {
                if (bpp == 8) {
                    build_lut8(p->pal);
                    full = true;   // CPU palette lookup: palette change => full reconvert
                }
                s_palDirty = false;
            }
            RECT d = {0, 0, 0, 0};
            if (full || s_fullDirty) {
                full = true;
                d = src;
            } else if (s_hasDirty) {
                d = s_dirty;
            }
            s_fullDirty = s_hasDirty = false;
            s_dirtyN = 0;
            LeaveCriticalSection(&s_dcs);

            RECT bounds = {0, 0, p->w, p->h};
            RECT vis;
            if (s_shadow && IntersectRect(&vis, &src, &bounds)) {
                bool ww = !legacy && p->wwBase && !wwBroken;
                wwActive = ww;
                if (!legacy) {
                    // the pixels stay valid under g_pixLock (Release / Flip wait for it): g_lock, which the
                    // game's calls take, is free while we copy
                    EnterCriticalSection(&g_pixLock);
                    stat_add(st, ST_HOLD, qpc_ms() - t1);
                    LeaveCriticalSection(&g_lock);
                    lockHeld = false;
                }
                if (ww) {
                    // writewatch: Windows lists the 4 KB pages written since the last reset. The dirty rect is
                    // not needed (our own Blt writes are listed too). While the page count still grows the game
                    // is drawing its frame: wait until it is quiet for presentquiet ms (at most presentcap).
                    size_t need = p->wwSize / 4096 + 2;
                    if (need > wwCap) {
                        free(wwPages);
                        wwPages = (void **)malloc(need * sizeof(void *));
                        wwCap = wwPages ? need : 0;
                    }
                    ULONG_PTR count = wwCap;
                    ULONG gran = 0;
                    bool go = full;
                    double tw0 = qpc_ms();
                    if (!full) {
                        UINT r = wwPages ? GetWriteWatch(0, p->wwBase, p->wwSize, wwPages, &count, &gran) : 1;
                        if (r != 0) {
                            wwBroken = true;
                            log_event("writewatch: GetWriteWatch failed (%lu) - changes found by comparing from now on",
                                      GetLastError());
                            go = full = true;
                        } else {
                            double first = settle.pending ? settle.first : t0, w = 0;
                            int sr = settle_step(&settle, t0, (long)count, g_cfg.presentQuiet, g_cfg.presentCap, &w);
                            if (sr == SETTLE_WAIT) {
                                nextWait = w;
                                settling = true;
                            } else if (sr == SETTLE_GO) {
                                go = true;
                                double dur = t0 - first;
                                st.settled++;
                                st.settleSum += dur;
                                if (dur > st.settleMax) st.settleMax = dur;
                                if (g_cfg.presentCap > 0 && dur >= g_cfg.presentCap - 0.05) st.settleCapped++;
                            }
                        }
                    }
                    if (go) {
                        // reset first, then copy: a write that lands during the copy is listed again next time
                        count = wwCap;
                        UINT r = wwPages && !wwBroken ? GetWriteWatch(WRITE_WATCH_FLAG_RESET, p->wwBase, p->wwSize,
                                                                      wwPages, &count, &gran)
                                                      : 1;
                        double tw1 = qpc_ms();
                        stat_add(st, ST_WW, tw1 - tw0);
                        if (full || r != 0) {
                            shadow_copy(p, vis);
                            rects[nRects++] = vis;
                        } else {
                            RowBands b;
                            bands_clear(&b);
                            bands_from_pages(&b, wwPages, count, gran ? gran : 4096, (uintptr_t)p->bits, p->pitch, p->h,
                                             8);
                            for (int i = 0; i < b.n; i++) {
                                RECT rr = {vis.left, b.top[i] > vis.top ? b.top[i] : vis.top, vis.right,
                                           b.bottom[i] < vis.bottom ? b.bottom[i] : vis.bottom};
                                if (rr.bottom > rr.top) {
                                    shadow_copy(p, rr);
                                    rects[nRects++] = rr;
                                }
                            }
                            st.bands += nRects;
                        }
                        settle_reset(&settle);
                        stat_add(st, ST_COPY, qpc_ms() - tw1);
                    } else {
                        stat_add(st, ST_WW, qpc_ms() - tw0);
                    }
                } else {
                    RECT up = {0, 0, 0, 0};
                    RECT dv;
                    if (IntersectRect(&dv, &d, &vis)) {
                        // fusedupload: the changed rows go to the shadow and into the next pixel buffer in one pass,
                        // instead of a second copy shadow -> buffer at upload time
                        if (fusedOn && bpp != 8 && !fusedMap) {
                            size_t need = (size_t)s_imgW * s_imgH * (bpp / 8);
                            fusedBuf = pbo[pboNext];
                            pboNext = (pboNext + 1) % PBO_COUNT;
                            if (need > pboSize) {
                                pboSize = need;
                                for (int i = 0; i < PBO_COUNT; i++) {
                                    glBindBuffer_(GL_PIXEL_UNPACK_BUFFER, pbo[i]);
                                    glBufferData_(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr_)pboSize, NULL, GL_STREAM_DRAW);
                                }
                            }
                            glBindBuffer_(GL_PIXEL_UNPACK_BUFFER, fusedBuf);
                            fusedMap = glMapBufferRange_
                                           ? (BYTE *)glMapBufferRange_(GL_PIXEL_UNPACK_BUFFER, 0, (GLsizeiptr_)need,
                                                                       GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT)
                                           : (BYTE *)glMapBuffer_(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
                            glBindBuffer_(GL_PIXEL_UNPACK_BUFFER, 0);   // no buffer bound for other GL calls meanwhile
                            if (!fusedMap) {
                                log_event("render: fusedupload: mapping the pixel buffer failed - copies go via the shadow");
                                fusedFailed = true;
                            }
                        }
                        if (fusedMap) {
                            fused_copy(p, dv, fusedMap, s_imgX, s_imgY, s_imgW, false);
                            rects[nRects++] = dv;
                        } else {
                            shadow_copy(p, dv);
                        }
                        up = dv;
                    }
                    double t2 = qpc_ms();
                    stat_add(st, ST_COPY, t2 - t1);
                    if (g_cfg.pollPrimary && !full) {
                        // rows a full-width dirty rect just copied need no comparison
                        int skipTop = 0, skipBottom = 0;
                        if (up.right > up.left && up.left <= vis.left && up.right >= vis.right) {
                            skipTop = up.top;
                            skipBottom = up.bottom;
                        }
                        RECT sc;
                        if (shadow_scan(p, vis, skipTop, skipBottom, &sc)) {
                            static bool reported;
                            if (up.right <= up.left && !reported && texValid) {
                                reported = true;
                                log_event("render: primary changed without Lock/Blt (rows %ld-%ld) - picked up by pollprimary",
                                          sc.top, sc.bottom);
                            }
                            if (fusedMap) {
                                // its own rect: rows between it and the dirty rect are not in the buffer
                                fused_copy(p, sc, fusedMap, s_imgX, s_imgY, s_imgW, true);
                                rects[nRects++] = sc;
                            } else if (up.right > up.left) UnionRect(&up, &up, &sc);
                            else up = sc;
                            polled = true;
                        }
                    }
                    tScan = qpc_ms() - t2;
                    if (!fusedMap && up.right > up.left && up.bottom > up.top)
                        rects[nRects++] = up;
                }
                if (!legacy)
                    LeaveCriticalSection(&g_pixLock);
            }
        }
        if (lockHeld) {
            stat_add(st, ST_HOLD, qpc_ms() - t1);
            LeaveCriticalSection(&g_lock);
        }
        stat_add(st, ST_SCAN, tScan);
        if (haveImage && s_shadow && InterlockedExchange(&s_shotReq, 0))
            screenshot_take(bpp, pitch, primW, primH);

        RECT up = {0, 0, 0, 0};   // bounds of everything that changed
        for (int i = 0; i < nRects; i++) {
            if (i == 0) up = rects[0];
            else UnionRect(&up, &up, &rects[i]);
        }
        bool changed = nRects > 0;
        // trace=true (Q146 diagnosis): bright pixels of the shadow left of QEC's briefing text box, logged on change
        if (g_cfg.trace && changed && s_shadow && bpp == 16 && primW >= 960 && primH >= 780) {
            static int lastBright = -1, logged;
            int bright = 0;
            for (int y = 490; y < 780; y++) {
                const WORD *row = (const WORD *)(s_shadow + (size_t)y * pitch);
                for (int x = 740; x < 960; x++) {
                    WORD c = row[x];
                    if (((c >> 11) & 31) * 8 + ((c >> 5) & 63) * 4 + (c & 31) * 8 > 360) bright++;
                }
            }
            if (bright != lastBright && logged++ < 60)
                log_event("trace textprobe: %d bright px left of the briefing text box (was %d), update rect (%ld,%ld)-(%ld,%ld)%s",
                          bright, lastBright, up.left, up.top, up.right, up.bottom, polled ? " via poll" : "");
            lastBright = bright;
        }
        if (changed) lastChange = t0;
        // the game is still drawing this frame: present once it is done (writewatch)
        if (settling && !forced && !realloc && texValid && !g_cfg.vsyncTest)
            continue;
        // ticks with nothing new: keep a 10 Hz redraw rate instead of presenting every tick (legacy without
        // presentsync and emptypresents=true, the default: every wake-up by a drawing call presents, even when it
        // changed nothing - the game writes the primary several times per frame, ~30 such presents a second while
        // panning the map in QEC)
        if (!changed && !forced && !realloc && texValid && (!legacy || sync || !signaled || !g_cfg.emptyPresents) &&
            t0 - lastPresent < 100.0 &&
            !g_cfg.vsyncTest)
            continue;

        double tu0 = qpc_ms();
        if (!useGL && haveImage && (realloc || !texValid)) {
            texValid = true;
            log_event("render: image %dx%d at (%d,%d) %dbpp (GDI: no texture)", s_imgW, s_imgH, s_imgX, s_imgY, bpp);
        }
        if (useGL && haveImage && s_shadow) {
            glBindTexture(GL_TEXTURE_2D, s_tex);
            GLenum fmt = GL_RGBA, type = GL_UNSIGNED_BYTE;
            int texelBytes = 4;
            if (bpp == 16 && gl12) { fmt = GL_RGB; type = GL_UNSIGNED_SHORT_5_6_5; texelBytes = 2; }
            else if (bpp == 32 && gl12) { fmt = GL_BGRA; type = GL_UNSIGNED_BYTE; }
            bool direct = bpp == 8 ? false : gl12;
            if (realloc || !texValid) {
                s_texW = next_pot(s_imgW);
                s_texH = next_pot(s_imgH);
                if (maxTex && (s_texW > maxTex || s_texH > maxTex))
                    log_event("render: texture %dx%d exceeds GL_MAX_TEXTURE_SIZE %d", s_texW, s_texH, maxTex);
                void *zero = calloc((size_t)s_texW * s_texH, texelBytes);   // parts outside the primary stay black
                glTexImage2D(GL_TEXTURE_2D, 0, direct && bpp == 16 ? GL_RGB : GL_RGBA, s_texW, s_texH, 0, fmt, type, zero);
                free(zero);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl12 ? GL_CLAMP_TO_EDGE : GL_CLAMP);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl12 ? GL_CLAMP_TO_EDGE : GL_CLAMP);
                texValid = true;
                lastFilter = -1;
                log_event("render: image %dx%d at (%d,%d) %dbpp -> texture %dx%d", s_imgW, s_imgH, s_imgX, s_imgY, bpp,
                          s_texW, s_texH);
            }
            if (changed && !direct) {
                // 8 bpp (or GL 1.1): palette / RGBA conversion into the staging image
                if (!s_staging) s_staging = (DWORD *)calloc((size_t)s_imgW * s_imgH, sizeof(DWORD));
                for (int ri = 0; ri < nRects && s_staging; ri++) {
                    const RECT &u = rects[ri];
                    int upW = u.right - u.left, upH = u.bottom - u.top;
                    int tx = u.left - s_imgX, ty = u.top - s_imgY;
                    if (bpp == 8) convert8(pitch, u);
                    else {
                        for (int y = u.top; y < u.bottom; y++) {
                            DWORD *dst = s_staging + (size_t)(y - s_imgY) * s_imgW + tx;
                            const BYTE *row = s_shadow + (size_t)y * pitch;
                            for (int x = 0; x < upW; x++) {
                                DWORD v;
                                if (bpp == 16) {
                                    WORD c = ((const WORD *)row)[u.left + x];
                                    DWORD r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
                                    v = ((r << 3) | (r >> 2)) | (((g << 2) | (g >> 4)) << 8) | (((b << 3) | (b >> 2)) << 16);
                                } else {
                                    DWORD c = ((const DWORD *)row)[u.left + x];
                                    v = ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16);
                                }
                                dst[x] = v | 0xFF000000u;
                            }
                        }
                    }
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, s_imgW);
                    glPixelStorei(GL_UNPACK_SKIP_PIXELS, tx);
                    glPixelStorei(GL_UNPACK_SKIP_ROWS, ty);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, tx, ty, upW, upH, GL_RGBA, GL_UNSIGNED_BYTE, s_staging);
                    st.upBytes += (long long)upW * upH * 4;
                }
            } else if (changed) {
                int bytes = bpp / 8;
                bool done = false;
                if (fusedMap) {
                    // fusedupload: the rows are already in the buffer, laid out like the image
                    glBindBuffer_(GL_PIXEL_UNPACK_BUFFER, fusedBuf);
                    glUnmapBuffer_(GL_PIXEL_UNPACK_BUFFER);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, bytes == 2 ? 2 : 4);
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, s_imgW);
                    for (int ri = 0; ri < nRects; ri++) {
                        const RECT &u = rects[ri];
                        glPixelStorei(GL_UNPACK_SKIP_PIXELS, u.left - s_imgX);
                        glPixelStorei(GL_UNPACK_SKIP_ROWS, u.top - s_imgY);
                        glTexSubImage2D(GL_TEXTURE_2D, 0, u.left - s_imgX, u.top - s_imgY, u.right - u.left, u.bottom - u.top,
                                        fmt, type, (const void *)0);
                    }
                    glBindBuffer_(GL_PIXEL_UNPACK_BUFFER, 0);
                    fusedMap = NULL;
                    done = true;
                }
                if (havePbo && !done) {
                    // the rows of all changed rects go into one streaming buffer, one after the other; the driver
                    // DMAs them to the texture asynchronously instead of blocking on client memory
                    size_t need = 0;
                    for (int ri = 0; ri < nRects; ri++)
                        need += (size_t)(rects[ri].right - rects[ri].left) * bytes * (rects[ri].bottom - rects[ri].top);
                    GLuint b = pbo[pboNext];
                    pboNext = (pboNext + 1) % PBO_COUNT;
                    glBindBuffer_(GL_PIXEL_UNPACK_BUFFER, b);
                    if (need > pboSize) {
                        pboSize = (size_t)s_imgW * s_imgH * bytes;
                        if (pboSize < need) pboSize = need;
                        for (int i = 0; i < PBO_COUNT; i++) {
                            glBindBuffer_(GL_PIXEL_UNPACK_BUFFER, pbo[i]);
                            glBufferData_(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr_)pboSize, NULL, GL_STREAM_DRAW);
                        }
                        glBindBuffer_(GL_PIXEL_UNPACK_BUFFER, b);
                    }
                    BYTE *m = glMapBufferRange_
                                  ? (BYTE *)glMapBufferRange_(GL_PIXEL_UNPACK_BUFFER, 0, (GLsizeiptr_)need,
                                                              GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT)
                                  : (BYTE *)glMapBuffer_(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
                    if (m) {
                        size_t at = 0;
                        for (int ri = 0; ri < nRects; ri++) {
                            const RECT &u = rects[ri];
                            size_t rowBytes = (size_t)(u.right - u.left) * bytes;
                            const BYTE *first = s_shadow + (size_t)u.top * pitch + (size_t)u.left * bytes;
                            for (int y = 0; y < u.bottom - u.top; y++)
                                memcpy(m + at + rowBytes * y, first + (size_t)y * pitch, rowBytes);
                            at += rowBytes * (u.bottom - u.top);
                        }
                        glUnmapBuffer_(GL_PIXEL_UNPACK_BUFFER);
                        glPixelStorei(GL_UNPACK_ALIGNMENT, bytes == 2 ? 2 : 4);
                        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                        at = 0;
                        for (int ri = 0; ri < nRects; ri++) {
                            const RECT &u = rects[ri];
                            int upW = u.right - u.left, upH = u.bottom - u.top;
                            glTexSubImage2D(GL_TEXTURE_2D, 0, u.left - s_imgX, u.top - s_imgY, upW, upH, fmt, type,
                                            (const void *)at);
                            at += (size_t)upW * bytes * upH;
                        }
                        done = true;
                    }
                    glBindBuffer_(GL_PIXEL_UNPACK_BUFFER, 0);
                    if (!done) {
                        log_event("render: glMapBuffer failed, pixel buffer objects disabled");
                        havePbo = false;
                    }
                }
                if (!done) {
                    glPixelStorei(GL_UNPACK_ALIGNMENT, bytes == 2 ? 2 : 4);
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / bytes);
                    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                    for (int ri = 0; ri < nRects; ri++) {
                        const RECT &u = rects[ri];
                        glTexSubImage2D(GL_TEXTURE_2D, 0, u.left - s_imgX, u.top - s_imgY, u.right - u.left,
                                        u.bottom - u.top, fmt, type,
                                        s_shadow + (size_t)u.top * pitch + (size_t)u.left * bytes);
                    }
                }
                for (int ri = 0; ri < nRects; ri++)
                    st.upBytes += (long long)(rects[ri].right - rects[ri].left) * (rects[ri].bottom - rects[ri].top) * bytes;
            }
            if (changed) {
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
            }
        }
        double tu1 = qpc_ms();
        stat_add(st, ST_UPLOAD, tu1 - tu0);
        if (g_cfg.paceStats) {
            int ub = (int)((tu1 - tu0) * 20.0);
            st.upHist[ub < 0 ? 0 : ub >= IV_BINS ? IV_BINS - 1 : ub]++;
        }
        // verifytex (Q153 debug): every N s the texture is read back and compared with the shadow - after an upload
        // they must be identical (16 / 32 bpp, direct upload paths)
        if (g_cfg.verifyTex > 0 && useGL && haveImage && texValid && s_shadow && gl12 && (bpp == 16 || bpp == 32) &&
            tu1 - lastVerify >= g_cfg.verifyTex * 1000.0) {
            lastVerify = tu1;
            int bytes = bpp / 8;
            BYTE *tex = (BYTE *)malloc((size_t)s_texW * s_texH * bytes);
            if (tex) {
                glPixelStorei(GL_PACK_ALIGNMENT, bytes == 2 ? 2 : 4);
                glGetTexImage(GL_TEXTURE_2D, 0, bpp == 16 ? GL_RGB : GL_BGRA, bpp == 16 ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_BYTE, tex);
                long bad = 0;
                int bx = -1, by = -1;
                for (int y = 0; y < s_imgH; y++) {
                    int py = s_imgY + y;
                    if (py < 0 || py >= primH) continue;
                    for (int x = 0; x < s_imgW; x++) {
                        int px = s_imgX + x;
                        if (px < 0 || px >= primW) continue;
                        const BYTE *a = tex + ((size_t)y * s_texW + x) * bytes, *b = s_shadow + (size_t)py * pitch + (size_t)px * bytes;
                        bool same = bytes == 2 ? *(const WORD *)a == *(const WORD *)b
                                               : (*(const DWORD *)a & 0xFFFFFF) == (*(const DWORD *)b & 0xFFFFFF);
                        if (!same) {
                            if (!bad) { bx = px; by = py; }
                            bad++;
                        }
                    }
                }
                free(tex);
                st.verifyChecks++;
                if (bad) {
                    st.verifyBad++;
                    log_event("verifytex: texture differs from the shadow in %ld pixel(s), first at (%d,%d) [%s]", bad, bx, by, mode);
                }
            }
        }
        if (fusedMap) {   // mapped but not uploaded (should not happen): never leave a buffer mapped
            glBindBuffer_(GL_PIXEL_UNPACK_BUFFER, fusedBuf);
            glUnmapBuffer_(GL_PIXEL_UNPACK_BUFFER);
            glBindBuffer_(GL_PIXEL_UNPACK_BUFFER, 0);
        }

        RECT vp;
        if (dd && !exclusive)
            SetRect(&vp, 0, 0, cw, ch);   // windowed: 1:1
        else
            compute_viewport(cw, ch, s_imgW, s_imgH, &vp);
        int vw = vp.right - vp.left, vh = vp.bottom - vp.top;

        if (!useGL) {
            if (haveImage && texValid && s_shadow) {
                gdi_present(dc, cw, ch, vp, bpp, pitch, primW, primH);
            } else {
                RECT all = {0, 0, cw, ch};
                FillRect(dc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
            }
        } else {
            glViewport(0, 0, cw, ch);
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
            if (haveImage && texValid && s_texW && s_texH) {
                int filter = (g_cfg.linear && (vw % s_imgW || vh % s_imgH)) ? GL_LINEAR : GL_NEAREST;
                if (filter != lastFilter) {
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
                    lastFilter = filter;
                }
                glViewport(vp.left, ch - vp.bottom, vw, vh);
                float u = (float)s_imgW / s_texW, v = (float)s_imgH / s_texH;
                glEnable(GL_TEXTURE_2D);
                glBegin(GL_QUADS);
                glTexCoord2f(0.f, 0.f); glVertex2f(-1.f, 1.f);
                glTexCoord2f(u, 0.f);   glVertex2f(1.f, 1.f);
                glTexCoord2f(u, v);     glVertex2f(1.f, -1.f);
                glTexCoord2f(0.f, v);   glVertex2f(-1.f, -1.f);
                glEnd();
                glDisable(GL_TEXTURE_2D);
            }
        }
        double tu2 = qpc_ms();
        stat_add(st, ST_DRAW, tu2 - tu1);

        if (g_cfg.maxfps > 0) {
            double minDt = 1000.0 / g_cfg.maxfps;
            double dt = tu2 - lastPresent;
            if (dt < minDt) Sleep((DWORD)(minDt - dt));
        }
        if (useGL) {
            SwapBuffers(dc);
            if (g_cfg.vsync)
                glFinish();   // wait until the swap has really happened, so the present ends at the blank
        }
        double t3 = qpc_ms();
        if (dwmTiming) {
            DWM_TIMING_INFO ti;
            memset(&ti, 0, sizeof(ti));
            ti.cbSize = sizeof(ti);
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (SUCCEEDED(dwmTiming(NULL, &ti)) && ti.qpcRefreshPeriod && (ULONGLONG)now.QuadPart >= ti.qpcVBlank)
                phase_add(st, (double)(((ULONGLONG)now.QuadPart - ti.qpcVBlank) % ti.qpcRefreshPeriod) /
                                  (double)ti.qpcRefreshPeriod);
        }
        stat_add(st, ST_SWAP, t3 - tu2);
        if (g_cfg.fixChilds && g_cfg.childPaint && haveImage && texValid && s_shadow &&
            (changed || forced || t3 - lastChildPaint >= 500.0)) {
            children_paint(cw, ch, vp, bpp, pitch, primW, primH);
            lastChildPaint = t3;
        }
        stat_add(st, ST_TOTAL, t3 - t0);
        if (lastPresent > 0)
            interval_add(st, t3 - lastPresent, period);
        lastPresent = t3;
        jr(J_PRESENT, changed, forced, nRects);
        if (firstPresent == 0)
            firstPresent = t3;
        {   // Q170: which path made the first present, and the first one after every rebuild (mode / presentation switch)
            static LONG loggedFor = -1;
            if (loggedFor != s_rebuildCount) {
                loggedFor = s_rebuildCount;
                const char *path = rebuild ? "rebuild (texture re-created, full upload)" : forced ? "forced refresh (window event)"
                                 : frameTick ? "game frame (full-surface copy to the primary)" : polled ? "pollprimary (written outside Lock/Blt)"
                                 : changed ? "dirty rect (Blt / Unlock to the primary)" : "idle redraw";
                log_event("render: first present%s via %s, %s, primary %dx%d", s_rebuildCount ? " after rebuild" : "", path,
                          changed ? "new pixels" : "no new pixels", primW, primH);
            }
        }
        st.presents++;
        if (changed) st.updates++;
        if (changed && full) st.fullUploads++;
        if (forced) { st.forced++; InterlockedIncrement(&s_forceCount); }
        if (polled) st.polled++;
        log_frame((up.right - up.left) * (up.bottom - up.top));
    }

    if (useGL) {
        if (havePbo || pbo[0]) {
            if (glDeleteBuffers_) glDeleteBuffers_(PBO_COUNT, pbo);
        }
        glDeleteTextures(1, &s_tex);
        s_tex = 0;
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(rc);
    }
    ReleaseDC(hwnd, dc);
    free(s_staging);
    free(wwPages);
    free(s_shadow);
    s_shadow = NULL;
    s_shadowSize = 0;
    s_shadowOf = NULL;
    s_staging = NULL;
    s_imgW = s_imgH = s_imgX = s_imgY = 0;
    log_event("render: thread exit");
    return 0;
}

void render_start(HWND hwnd)
{
    ensure_cs();
    if (s_thread && s_hwnd == hwnd)
        return;
    if (s_thread)
        render_stop();
    s_hwnd = hwnd;
    s_stop = 0;
    if (!s_evt)
        s_evt = CreateEventA(NULL, FALSE, TRUE, NULL);
    s_fullDirty = true;
    s_thread = CreateThread(NULL, 0, render_thread, NULL, 0, NULL);
    log_event("render: thread started for hwnd %p", hwnd);
}

void render_stop()
{
    if (!s_thread)
        return;
    InterlockedExchange(&s_stop, 1);
    SetEvent(s_evt);
    if (WaitForSingleObject(s_thread, 3000) == WAIT_TIMEOUT)
        log_event("render: thread did not stop within 3s");
    CloseHandle(s_thread);
    s_thread = NULL;
}

void render_screenshot()
{
    InterlockedExchange(&s_shotReq, 1);
    if (s_evt) SetEvent(s_evt);
}

const char *render_backend()
{
    return s_backend;
}
