// maxgameticks: the game's clocks (QueryPerformanceCounter, timeGetTime, GetTickCount, GetTickCount64 as
// imported by the game modules) never advance more than maxgameticks ms between two reads. After a stall
// (the machine froze, the game thread was blocked) a game that measures the elapsed time would otherwise
// run all the missed time at once - it "fast-forwards". The time held back is kept as one offset shared
// by all four clocks, so they stay consistent with each other and never run backwards.
#include "qdd.h"

typedef BOOL(WINAPI *PFN_QPC)(LARGE_INTEGER *);
typedef DWORD(WINAPI *PFN_Tick)(void);
typedef ULONGLONG(WINAPI *PFN_Tick64)(void);

static PFN_QPC r_QueryPerformanceCounter;
static PFN_Tick r_timeGetTime, r_GetTickCount;
static PFN_Tick64 r_GetTickCount64;

volatile DWORD g_gameThread;

static CRITICAL_SECTION s_cs;
static LONGLONG s_freq, s_limit;     // QPC ticks per second, per maxgameticks
static LONGLONG s_last;              // real QPC value of the previous read (0 = none yet)
static LONGLONG s_held;              // QPC ticks held back so far
static LONG s_clamps, s_reported;
static double s_maxJump;             // ms
static volatile LONG s_reads[4];     // per clock, for the log
static DWORD s_lastTime, s_lastTick;
static ULONGLONG s_lastTick64;
static LONGLONG s_lastQpc;

// one read of any clock: returns the held-back time in QPC ticks. Only the reads of the game thread (the
// one owning the game window, which runs the frames) are measured: other threads keep reading while the
// game thread is stuck (QEC's timer callbacks call timeGetTime 60 times a second), and a helper thread
// that sleeps for seconds is no stall of the game.
static LONGLONG advance(LONGLONG now)
{
    EnterCriticalSection(&s_cs);
    if (GetCurrentThreadId() != g_gameThread) {
        LONGLONG held = s_held;
        LeaveCriticalSection(&s_cs);
        return held;
    }
    if (s_last && now - s_last > s_limit) {
        LONGLONG jump = now - s_last;
        s_held += jump - s_limit;
        double ms = jump * 1000.0 / s_freq;
        if (ms > s_maxJump) s_maxJump = ms;
        LONG n = ++s_clamps;
        if (n <= 30 || n % 100 == 0)
            log_event("maxgameticks: clamp #%ld - the game's clocks were not read for %.0f ms, they advance %d ms "
                      "(%.0f ms held back in total)", n, ms, g_cfg.maxGameTicks, s_held * 1000.0 / s_freq);
    }
    if (now > s_last)
        s_last = now;
    LONGLONG held = s_held;
    LeaveCriticalSection(&s_cs);
    return held;
}

static LONGLONG qpc_now()
{
    LARGE_INTEGER t;
    r_QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static BOOL WINAPI h_QueryPerformanceCounter(LARGE_INTEGER *t)
{
    if (!t)
        return r_QueryPerformanceCounter(t);
    InterlockedIncrement(&s_reads[0]);
    LONGLONG now = qpc_now(), v = now - advance(now);
    EnterCriticalSection(&s_cs);
    if (v < s_lastQpc) v = s_lastQpc;
    s_lastQpc = v;
    LeaveCriticalSection(&s_cs);
    t->QuadPart = v;
    return TRUE;
}

// millisecond clocks: real value minus the held time; never below the value returned before (their
// granularity is coarser than QPC, the subtraction alone could step back a few ms)
static DWORD ms_clock(PFN_Tick real, DWORD *last, int id)
{
    InterlockedIncrement(&s_reads[id]);
    DWORD v = real() - (DWORD)(advance(qpc_now()) * 1000 / s_freq);
    EnterCriticalSection(&s_cs);
    if ((LONG)(v - *last) < 0 && *last) v = *last;
    *last = v;
    LeaveCriticalSection(&s_cs);
    return v;
}

static DWORD WINAPI h_timeGetTime() { return ms_clock(r_timeGetTime, &s_lastTime, 1); }
static DWORD WINAPI h_GetTickCount() { return ms_clock(r_GetTickCount, &s_lastTick, 2); }

static ULONGLONG WINAPI h_GetTickCount64()
{
    InterlockedIncrement(&s_reads[3]);
    ULONGLONG v = r_GetTickCount64() - (ULONGLONG)(advance(qpc_now()) * 1000 / s_freq);
    EnterCriticalSection(&s_cs);
    if (v < s_lastTick64) v = s_lastTick64;
    s_lastTick64 = v;
    LeaveCriticalSection(&s_cs);
    return v;
}

int clock_hooks(void ***reals, void **hooks, const char **dlls)
{
    if (g_cfg.maxGameTicks <= 0)
        return 0;
    InitializeCriticalSectionAndSpinCount(&s_cs, 4000);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    s_freq = f.QuadPart;
    s_limit = s_freq * g_cfg.maxGameTicks / 1000;
    HMODULE k32 = GetModuleHandleA("kernel32.dll"), mm = GetModuleHandleA("winmm.dll");
    r_QueryPerformanceCounter = (PFN_QPC)GetProcAddress(k32, "QueryPerformanceCounter");
    r_GetTickCount = (PFN_Tick)GetProcAddress(k32, "GetTickCount");
    r_GetTickCount64 = (PFN_Tick64)GetProcAddress(k32, "GetTickCount64");
    r_timeGetTime = mm ? (PFN_Tick)GetProcAddress(mm, "timeGetTime") : NULL;
    int n = 0;
    reals[n] = (void **)&r_QueryPerformanceCounter; hooks[n] = (void *)h_QueryPerformanceCounter; dlls[n++] = "kernel32.dll";
    reals[n] = (void **)&r_GetTickCount; hooks[n] = (void *)h_GetTickCount; dlls[n++] = "kernel32.dll";
    if (r_GetTickCount64) { reals[n] = (void **)&r_GetTickCount64; hooks[n] = (void *)h_GetTickCount64; dlls[n++] = "kernel32.dll"; }
    if (r_timeGetTime) { reals[n] = (void **)&r_timeGetTime; hooks[n] = (void *)h_timeGetTime; dlls[n++] = "winmm.dll"; }
    log_event("maxgameticks=%d: the game's QueryPerformanceCounter / GetTickCount%s%s advance at most %d ms between "
              "two reads", g_cfg.maxGameTicks, r_GetTickCount64 ? " / GetTickCount64" : "",
              r_timeGetTime ? " / timeGetTime" : " (winmm not loaded: timeGetTime not covered)", g_cfg.maxGameTicks);
    return n;
}

void clock_report(bool final)
{
    if (g_cfg.maxGameTicks <= 0 || !s_freq)
        return;
    // every 10 s when there were new clamps, every 60 s otherwise
    static DWORD lastAt;
    DWORD now = GetTickCount();
    LONG c = s_clamps;
    if (!final && now - lastAt < (c != s_reported ? 10000u : 60000u))
        return;
    lastAt = now;
    s_reported = c;
    log_event("maxgameticks: %ld clamp(s) so far, longest gap %.0f ms, %.0f ms held back; reads QPC %ld, "
              "timeGetTime %ld, GetTickCount %ld, GetTickCount64 %ld%s", c, s_maxJump, s_held * 1000.0 / s_freq,
              s_reads[0], s_reads[1], s_reads[2], s_reads[3], final ? " (final)" : "");
}
