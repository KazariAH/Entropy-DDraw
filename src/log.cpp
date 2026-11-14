// Probe log: per-method call counters, rect-size histograms and a small event
// journal. Everything is collected in memory; a background thread rewrites
// ddraw.log every 5 seconds (stats table first, events after).
#include "qdd.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

volatile LONG g_counts[M_COUNT];

static const char *const s_names[M_COUNT] = {
#define QDD_NAME(n, impl) #n,
    QDD_METHODS(QDD_NAME)
#undef QDD_NAME
};
static const int s_impl[M_COUNT] = {
#define QDD_IMPL(n, impl) impl,
    QDD_METHODS(QDD_IMPL)
#undef QDD_IMPL
};

static CRITICAL_SECTION s_cs;
CRITICAL_SECTION *log_lock() { return &s_cs; }
static bool s_ready;
static char s_path[MAX_PATH];
static HANDLE s_thread, s_stopEvt;
static DWORD s_startTick;

// event journal
static const int EVENT_CAP = 96 * 1024;
static char *s_events;
static int s_eventLen;
static LONG s_eventsDropped;
static const int CRASH_RESERVE = 16 * 1024;   // end of the journal kept free for a crash report

// the newest events, independent of the journal (full after a long session) and of log=false
static const int RING_LINES = 32, RING_WIDTH = 240;
static char s_ring[RING_LINES][RING_WIDTH];
static LONG s_ringNext;

// histograms: open addressing, key = (w << 16) | h
static const int HIST_SLOTS = 256;
struct HistSlot { DWORD key; LONG count; };
static HistSlot s_hist[H_COUNT][HIST_SLOTS];
static LONG s_histOverflow[H_COUNT];
static const char *const s_histNames[H_COUNT] = { "Blt", "BltFast", "Lock", "Lock(primary)", "Blt(COLORFILL)" };

// blt flag bits seen
static const int FLAG_BITS = 32;
static LONG s_bltFlagBits[FLAG_BITS];

// frame stats
static volatile LONG s_frames;
static volatile LONG64 s_uploadPixels;

// snapshot of the previous flush, for per-second rates
static LONG s_prevCounts[M_COUNT];
static LONG s_prevFrames;
static DWORD s_prevTick;

static void event_add(bool crash, const char *fmt, va_list ap)
{
    if (!s_ready)
        return;
    char line[512];
    DWORD t = GetTickCount() - s_startTick;
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%7lu.%03lu] ", t / 1000, t % 1000);
    if (n < 0) n = 0;
    int m = _vsnprintf_s(line + n, sizeof(line) - n - 2, _TRUNCATE, fmt, ap);
    if (m < 0) m = (int)strlen(line + n);
    n += m;
    if (!crash) {
        LONG slot = (InterlockedIncrement(&s_ringNext) - 1) % RING_LINES;
        strncpy_s(s_ring[slot], RING_WIDTH, line, _TRUNCATE);
    }
    if (!g_cfg.log)
        return;
    line[n++] = '\r';
    line[n++] = '\n';
    EnterCriticalSection(&s_cs);
    if (s_eventLen + n < EVENT_CAP - (crash ? 0 : CRASH_RESERVE)) {
        memcpy(s_events + s_eventLen, line, n);
        s_eventLen += n;
    } else {
        s_eventsDropped++;
    }
    LeaveCriticalSection(&s_cs);
}

void log_event(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    event_add(false, fmt, ap);
    va_end(ap);
}

// crash report lines: may use the reserve at the end of a full journal, not kept in the ring
void log_crash(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    event_add(true, fmt, ap);
    va_end(ap);
}

int log_recent(char *buf, int cap)
{
    LONG next = s_ringNext;
    int len = 0;
    buf[0] = 0;
    for (LONG i = next > RING_LINES ? next - RING_LINES : 0; i < next && len < cap - 1; i++) {
        int m = _snprintf_s(buf + len, cap - len, _TRUNCATE, "%s\r\n", s_ring[i % RING_LINES]);
        if (m < 0) break;
        len += m;
    }
    return len;
}

// local time as yyyymmdd_hhmmss[_mmm] - file names that sort chronologically
void local_stamp(char *buf, size_t cap, bool millis)
{
    SYSTEMTIME now;
    GetLocalTime(&now);
    int n = _snprintf_s(buf, cap, _TRUNCATE, "%04u%02u%02u_%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour,
                        now.wMinute, now.wSecond);
    if (millis && n > 0)
        _snprintf_s(buf + n, cap - n, _TRUNCATE, "_%03u", now.wMilliseconds);
}

const char *log_dir()
{
    static char dir[MAX_PATH];
    if (!dir[0] && s_path[0]) {
        strcpy_s(dir, s_path);
        char *slash = strrchr(dir, '\\');
        if (slash) slash[1] = 0;
        else dir[0] = 0;
    }
    return dir;
}

void log_hist(int hist, int w, int h)
{
    if (!s_ready)
        return;
    DWORD key = ((DWORD)(w & 0xFFFF) << 16) | (DWORD)(h & 0xFFFF);
    DWORD idx = (key * 2654435761u) >> 24;   // 8 bits -> 256 slots
    EnterCriticalSection(&s_cs);
    for (int i = 0; i < HIST_SLOTS; i++) {
        HistSlot &s = s_hist[hist][(idx + i) & (HIST_SLOTS - 1)];
        if (s.count == 0) { s.key = key; s.count = 1; LeaveCriticalSection(&s_cs); return; }
        if (s.key == key) { s.count++; LeaveCriticalSection(&s_cs); return; }
    }
    s_histOverflow[hist]++;
    LeaveCriticalSection(&s_cs);
}

void log_bltflags(DWORD flags)
{
    for (int i = 0; i < FLAG_BITS; i++)
        if (flags & (1u << i))
            InterlockedIncrement(&s_bltFlagBits[i]);
}

void log_frame(int uploadedPixels)
{
    InterlockedIncrement(&s_frames);
    InterlockedExchangeAdd64(&s_uploadPixels, uploadedPixels);
}

// --- flushing ---------------------------------------------------------------

struct OutBuf {
    char *p;
    int len, cap;
};

static void out(OutBuf &b, const char *fmt, ...)
{
    if (b.len >= b.cap - 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(b.p + b.len, b.cap - b.len, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) b.len = b.cap - 1;
    else b.len += n;
}

static int cmp_slots(const void *a, const void *b)
{
    LONG ca = ((const HistSlot *)a)->count, cb = ((const HistSlot *)b)->count;
    return ca < cb ? 1 : (ca > cb ? -1 : 0);
}

void log_flush()
{
    if (!s_ready || !g_cfg.log)
        return;
    OutBuf b;
    b.cap = 512 * 1024;
    b.p = (char *)malloc(b.cap);
    b.len = 0;
    if (!b.p)
        return;

    DWORD now = GetTickCount();
    double dt = (now - s_prevTick) / 1000.0;
    if (dt <= 0.0) dt = 1.0;

    LONG counts[M_COUNT];
    for (int i = 0; i < M_COUNT; i++)
        counts[i] = g_counts[i];
    LONG frames = s_frames;

    out(b, "QuantumDDraw probe log  (uptime %.1fs, rewritten every 5s)\r\n", (now - s_startTick) / 1000.0);
    out(b, "frames presented: %ld  (%.1f fps last interval)  uploaded px: %lld\r\n",
        frames, (frames - s_prevFrames) / dt, (long long)s_uploadPixels);
    out(b, "%-30s %4s %10s %9s\r\n", "method", "impl", "calls", "per-sec");
    for (int i = 0; i < M_COUNT; i++) {
        if (counts[i] == 0)
            continue;
        out(b, "%-30s %4s %10ld %9.1f\r\n", s_names[i], s_impl[i] ? "yes" : "STUB",
            counts[i], (counts[i] - s_prevCounts[i]) / dt);
    }

    EnterCriticalSection(&s_cs);
    for (int h = 0; h < H_COUNT; h++) {
        HistSlot tmp[HIST_SLOTS];
        memcpy(tmp, s_hist[h], sizeof(tmp));
        qsort(tmp, HIST_SLOTS, sizeof(HistSlot), cmp_slots);
        if (tmp[0].count == 0)
            continue;
        out(b, "\r\n%s rect sizes (top 16 of WxH):", s_histNames[h]);
        for (int i = 0; i < 16 && tmp[i].count; i++)
            out(b, "%s %lux%lu=%ld", i ? "," : "", tmp[i].key >> 16, tmp[i].key & 0xFFFF, tmp[i].count);
        if (s_histOverflow[h])
            out(b, " (+%ld in overflow)", s_histOverflow[h]);
    }
    out(b, "\r\nBlt/BltFast flag bits seen:");
    for (int i = 0; i < FLAG_BITS; i++)
        if (s_bltFlagBits[i])
            out(b, " 0x%08lX=%ld", 1ul << i, s_bltFlagBits[i]);
    out(b, "\r\n\r\n--- events ---\r\n");
    if (b.len + s_eventLen < b.cap - 64) {
        memcpy(b.p + b.len, s_events, s_eventLen);
        b.len += s_eventLen;
    }
    if (s_eventsDropped)
        out(b, "(%ld further events dropped)\r\n", s_eventsDropped);
    LeaveCriticalSection(&s_cs);

    memcpy(s_prevCounts, counts, sizeof(counts));
    s_prevFrames = frames;
    s_prevTick = now;

    HANDLE f = CreateFileA(s_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wr;
        WriteFile(f, b.p, b.len, &wr, NULL);
        CloseHandle(f);
    }
    free(b.p);
}

// Debug (dump=true): the surface as a top-down BMP next to ddraw.log. 16 bit is written as
// BI_BITFIELDS RGB565, 32 bit as BI_RGB; 8 bit is skipped. Caller holds g_lock.
static void dump_surface(const Surface *s, const char *name)
{
    if (!s || !s->bits || (s->bpp != 16 && s->bpp != 32))
        return;
    char path[MAX_PATH];
    strcpy_s(path, s_path);
    char *slash = strrchr(path, '\\');
    if (slash) slash[1] = 0;
    else path[0] = 0;
    strcat_s(path, name);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    DWORD masks[3] = {0xF800, 0x07E0, 0x001F};
    DWORD extra = s->bpp == 16 ? sizeof(masks) : 0;
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof(ih);
    ih.biWidth = s->w;
    ih.biHeight = -s->h;
    ih.biPlanes = 1;
    ih.biBitCount = (WORD)s->bpp;
    ih.biCompression = s->bpp == 16 ? BI_BITFIELDS : BI_RGB;
    ih.biSizeImage = (DWORD)s->pitch * s->h;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih) + extra;
    fh.bfSize = fh.bfOffBits + ih.biSizeImage;
    DWORD wr;
    WriteFile(f, &fh, sizeof(fh), &wr, NULL);
    WriteFile(f, &ih, sizeof(ih), &wr, NULL);
    if (extra)
        WriteFile(f, masks, extra, &wr, NULL);
    WriteFile(f, s->bits, ih.biSizeImage, &wr, NULL);   // DIB rows are 4-byte aligned, as BMP wants
    CloseHandle(f);
}

static void dump_surfaces()
{
    EnterCriticalSection(&g_lock);
    if (g_dd)
        dump_surface(g_dd->primary, "ddraw_primary.bmp");
    dump_surface(g_lastLocked, "ddraw_lastlock.bmp");
    LeaveCriticalSection(&g_lock);
}

// The log thread also watches for a hung game (Q147): every second window_hang_check(), and a named event
// (Local\QuantumDDraw.HangReport.<pid>, set by a test script or a support tool) asks for a hang report on demand.
// The checks come before log_flush: a hung thread may hold the log lock.
static DWORD WINAPI flush_thread(LPVOID)
{
    char name[64];
    _snprintf_s(name, sizeof(name), _TRUNCATE, "Local\\QuantumDDraw.HangReport.%lu", GetCurrentProcessId());
    HANDLE req = CreateEventA(NULL, FALSE, FALSE, name);
    // Q157: Local\QuantumDDraw.Journal.<pid> writes the last 5 s of the activity journal (test script, support)
    _snprintf_s(name, sizeof(name), _TRUNCATE, "Local\\QuantumDDraw.Journal.%lu", GetCurrentProcessId());
    HANDLE jreq = CreateEventA(NULL, FALSE, FALSE, name);
    HANDLE waits[3] = {s_stopEvt, req, jreq};
    int tick = 0;
    for (;;) {
        DWORD r = WaitForMultipleObjects(req && jreq ? 3 : req ? 2 : 1, waits, FALSE, 1000);
        if (r == WAIT_OBJECT_0)
            break;
        if (r == WAIT_OBJECT_0 + 2) {
            jr(J_LOGTHREAD, 2);
            journal_dump("requested", 5.0);
            continue;
        }
        char why[200];
        if (r == WAIT_OBJECT_0 + 1) {
            jr(J_LOGTHREAD, 1);
            hang_report("requested (named event)");
        } else if (window_hang_check(why, sizeof(why))) {
            jr(J_LOGTHREAD, 1);
            hang_report(why);
        }
        if (++tick % 5 == 0 || r == WAIT_OBJECT_0 + 1) {
            if (!TryEnterCriticalSection(&s_cs))
                continue;   // someone holds the log lock (maybe the hung thread): do not join it
            LeaveCriticalSection(&s_cs);
            jr(J_LOGTHREAD, 0);
            log_flush();
            if (g_cfg.dump)
                dump_surfaces();
        }
    }
    if (req)
        CloseHandle(req);
    if (jreq)
        CloseHandle(jreq);
    return 0;
}

void log_init(HMODULE self)
{
    InitializeCriticalSection(&s_cs);
    s_startTick = s_prevTick = GetTickCount();
    GetModuleFileNameA(self, s_path, MAX_PATH);
    char *slash = strrchr(s_path, '\\');
    if (slash) slash[1] = 0;
    else s_path[0] = 0;
    strcat_s(s_path, "ddraw.log");
    s_events = (char *)malloc(EVENT_CAP);
    if (!s_events)
        return;
    s_ready = true;
}

// Started lazily (DirectDrawCreate) - creating threads inside DllMain is unsafe.
void log_start_thread()
{
    if (s_thread || !s_ready || !g_cfg.log)
        return;
    s_stopEvt = CreateEventA(NULL, TRUE, FALSE, NULL);
    s_thread = CreateThread(NULL, 0, flush_thread, NULL, 0, NULL);
}

void log_shutdown()
{
    // Called from DLL_PROCESS_DETACH: other threads are already gone on process
    // exit, so just write the final snapshot synchronously.
    log_flush();
}
