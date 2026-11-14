// Crash forensics. A vectored handler logs every fatal-type exception at first chance (under a debugger
// such as Syringe nothing else runs); the unhandled exception filter writes the full report - code,
// module + offset, registers, frame-pointer call chain, the last 32 events - to ddraw.log and to
// ddraw_crash_<time>.txt, and a minidump to ddraw_crash_<time>.dmp. dbghelp.dll is only loaded at that
// moment (not in the import table; Windows 7 ships MiniDumpWriteDump in dbghelp 6.1).
//
// Games install their own filter too (Entropy.exe imports SetUnhandledExceptionFilter). Ours stays on top:
// it is set again in DirectDrawCreate and the game modules' SetUnhandledExceptionFilter import is
// redirected, so a filter the game sets later is called after ours instead of replacing it.
#include "qdd.h"
#include <dbghelp.h>   // types only
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <intrin.h>   // _ReturnAddress

static const DWORD CRASHTEST_CODE = 0xE0514444;   // crashtest=1 ("QDD")
static const int KEEP_DUMPS = 5;                   // older ddraw_crash_* files are deleted

typedef LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI *PFN_SUEF)(LPTOP_LEVEL_EXCEPTION_FILTER);
typedef decltype(&MiniDumpWriteDump) PFN_MiniDumpWriteDump;   // dbghelp.h declares it; nothing is linked

static PFN_SUEF r_SetUnhandledExceptionFilter;
static LPTOP_LEVEL_EXCEPTION_FILTER s_next;        // the game's filter, called after ours
static volatile LONG s_inFilter;
static volatile bool s_testFault;   // crashtest: the deliberate fault is under way

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep);
static void debugger_first_chance(EXCEPTION_POINTERS *ep);

typedef BOOL(WINAPI *PFN_TerminateProcess)(HANDLE, UINT);
typedef void(WINAPI *PFN_ExitProcess)(UINT);
static PFN_TerminateProcess r_TerminateProcess;
static PFN_ExitProcess r_ExitProcess;

// the newest fatal-type exception seen at first chance
static struct {
    volatile DWORD tid;   // 0 = none
    DWORD tick;
    EXCEPTION_RECORD rec;
    CONTEXT ctx;
} s_pending;

// "module.dll+0xOFFSET" for an address inside a loaded module (false and "?" otherwise).
static bool describe_address(ULONG_PTR a, char *out, size_t cap)
{
    HMODULE mod = NULL;
    char name[MAX_PATH];
    if (!a || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCSTR)a, &mod) ||
        !GetModuleFileNameA(mod, name, MAX_PATH)) {
        strcpy_s(out, cap, "?");
        return false;
    }
    const char *base = strrchr(name, '\\');
    base = base ? base + 1 : name;
    _snprintf_s(out, cap, _TRUNCATE, "%s+0x%lX", base, (unsigned long)(a - (ULONG_PTR)mod));
    return true;
}

static const struct {
    DWORD code;
    const char *name;
} s_exceptionNames[] = {
    {EXCEPTION_ACCESS_VIOLATION, "ACCESS_VIOLATION"},       {EXCEPTION_ILLEGAL_INSTRUCTION, "ILLEGAL_INSTRUCTION"},
    {EXCEPTION_INT_DIVIDE_BY_ZERO, "INT_DIVIDE_BY_ZERO"},   {EXCEPTION_STACK_OVERFLOW, "STACK_OVERFLOW"},
    {EXCEPTION_PRIV_INSTRUCTION, "PRIV_INSTRUCTION"},       {EXCEPTION_ARRAY_BOUNDS_EXCEEDED, "ARRAY_BOUNDS_EXCEEDED"},
    {EXCEPTION_IN_PAGE_ERROR, "IN_PAGE_ERROR"},             {EXCEPTION_FLT_DIVIDE_BY_ZERO, "FLT_DIVIDE_BY_ZERO"},
    {EXCEPTION_FLT_INVALID_OPERATION, "FLT_INVALID_OPERATION"}, {EXCEPTION_BREAKPOINT, "BREAKPOINT"},
    {0xC0000409, "STACK_BUFFER_OVERRUN"},                   {0xC0000374, "HEAP_CORRUPTION"},
    {0xE06D7363, "C++ exception"},                          {CRASHTEST_CODE, "crashtest (deliberate)"},
};

static const char *exception_name(DWORD code)
{
    for (const auto &e : s_exceptionNames)
        if (e.code == code)
            return e.name;
    return "?";
}

static bool fatal_type(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
           code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW ||
           code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED || code == CRASHTEST_CODE;
}

// " read of" / " write to" / " execute at" + the data address of an access violation
static void access_text(const EXCEPTION_RECORD *er, char *out, size_t cap)
{
    out[0] = 0;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2)
        return;
    ULONG_PTR kind = er->ExceptionInformation[0], at = er->ExceptionInformation[1];
    _snprintf_s(out, cap, _TRUNCATE, ",%s %p", kind == 0 ? " read of" : kind == 1 ? " write to" : kind == 8 ? " execute at" : " access to",
                (void *)at);
}

// --- first chance --------------------------------------------------------------------------------

// Q156: a probe read must not raise an exception inside our exception handler (a nested access violation while the
// game is inside a window callback was the second fault in the 0xC000041D crash reports): the page is checked first,
// the __try only remains as a backstop for a page that changes in between
static bool readable(ULONG_PTR at, size_t len)
{
    if (at < 0x10000 || at + len < at)
        return false;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((const void *)at, &mi, sizeof(mi)) || mi.State != MEM_COMMIT ||
        (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
        !(mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                        PAGE_EXECUTE_WRITECOPY)))
        return false;
    return at + len <= (ULONG_PTR)mi.BaseAddress + mi.RegionSize || readable((ULONG_PTR)mi.BaseAddress + mi.RegionSize, 0);
}

static bool read_dword(ULONG_PTR at, DWORD *v)
{
    if (!readable(at, sizeof(DWORD)))
        return false;
    __try {
        *v = *(const DWORD *)at;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static LONG CALLBACK first_chance(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!fatal_type(code))
        return EXCEPTION_CONTINUE_SEARCH;
    // kept for a crash handler of the game that ends the process itself (see h_TerminateProcess)
    s_pending.tid = 0;
    s_pending.rec = *ep->ExceptionRecord;
    s_pending.rec.ExceptionRecord = NULL;
    s_pending.ctx = *ep->ContextRecord;
    s_pending.tick = GetTickCount();
    s_pending.tid = GetCurrentThreadId();
    static volatile LONG seen;
    if (InterlockedIncrement(&seen) > 20)
        return EXCEPTION_CONTINUE_SEARCH;
    void *addr = ep->ExceptionRecord->ExceptionAddress;
    char where[MAX_PATH + 16], access[64];
    describe_address((ULONG_PTR)addr, where, sizeof(where));
    access_text(ep->ExceptionRecord, access, sizeof(access));
    log_event("EXCEPTION 0x%08lX %s (first chance) at %p = %s, thread %lu%s", code, exception_name(code), addr, where,
              GetCurrentThreadId(), access);
#if defined(_M_IX86)
    const CONTEXT *c = ep->ContextRecord;
    log_event("  regs EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX EIP=%08lX EFL=%08lX",
              c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip, c->EFlags);
#endif
    // A filter set past our IAT hooks (a module loaded later, GetProcAddress) would take the crash
    // without a report: put ours back on top, the other one is chained.
    if (r_SetUnhandledExceptionFilter) {
        LPTOP_LEVEL_EXCEPTION_FILTER cur = r_SetUnhandledExceptionFilter(crash_filter);
        if (cur != crash_filter) {
            s_next = cur;
            char mod[MAX_PATH + 16];
            describe_address((ULONG_PTR)cur, mod, sizeof(mod));
            log_event("  the unhandled exception filter had been replaced by %p = %s - ours on top again, that one chained",
                      cur, mod);
        }
    }
    // Under a debugger (QEC runs Entropy.exe under EntropyLoader, like Syringe) Windows skips the unhandled
    // exception filter: the second chance goes to the debugger, which ends the process. So the report is
    // written now, while the exception is still in flight.
    if (IsDebuggerPresent() && !s_inFilter && code != EXCEPTION_BREAKPOINT)
        debugger_first_chance(ep);
    log_flush();
    return EXCEPTION_CONTINUE_SEARCH;
}

// --- the report ----------------------------------------------------------------------------------

static char s_report[24 * 1024];
static int s_reportLen;

static void rep(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(line);
    log_crash("%s", line);
    if (s_reportLen + n + 2 < (int)sizeof(s_report)) {
        memcpy(s_report + s_reportLen, line, n);
        s_reportLen += n;
        s_report[s_reportLen++] = '\r';
        s_report[s_reportLen++] = '\n';
    }
}

static void report_stack(const CONTEXT *c)
{
#if defined(_M_IX86)
    char mod[MAX_PATH + 16];
    // frame-pointer chain: [EBP] = caller's EBP, [EBP+4] = return address (code built without frame
    // pointers breaks the chain early; the stack scan below still finds return addresses)
    ULONG_PTR ebp = c->Ebp;
    for (int i = 0; i < 16 && ebp; i++) {
        DWORD next, ret;
        if (!read_dword(ebp, &next) || !read_dword(ebp + 4, &ret) || !ret)
            break;
        describe_address(ret, mod, sizeof(mod));
        rep("  frame %2d: EBP=%08lX return %08lX = %s", i, (DWORD)ebp, ret, mod);
        if (next <= ebp || next - ebp > 1024 * 1024)
            break;
        ebp = next;
    }
    // top of the stack: words that point into a loaded module are usually return addresses
    int shown = 0;
    for (int i = 0; i < 256 && shown < 12; i++) {
        DWORD v;
        if (!read_dword(c->Esp + i * 4, &v))
            break;
        if (describe_address(v, mod, sizeof(mod))) {
            rep("  stack [ESP+0x%03X] %08lX = %s", i * 4, v, mod);
            shown++;
        }
    }
#else
    (void)c;
#endif
}

// keeps the newest KEEP_DUMPS - 1 dumps (the one about to be written is the last); their .txt goes with them
static void prune_dumps(const char *dir)
{
    char pattern[MAX_PATH], names[64][64];
    int n = 0;
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%sddraw_crash_*.dmp", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (n < 64 && strlen(fd.cFileName) < 60)
            strcpy_s(names[n++], fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    // the time stamp makes the names sort chronologically
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(names[j], names[i]) < 0) {
                char t[64];
                strcpy_s(t, names[i]);
                strcpy_s(names[i], names[j]);
                strcpy_s(names[j], t);
            }
    for (int i = 0; i < n - (KEEP_DUMPS - 1); i++) {
        char path[MAX_PATH];
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s%s", dir, names[i]);
        DeleteFileA(path);
        char *dot = strrchr(path, '.');
        if (dot) {
            strcpy_s(dot, path + sizeof(path) - dot, ".txt");
            DeleteFileA(path);
        }
    }
}

struct DumpJob {
    EXCEPTION_POINTERS *ep;
    DWORD threadId;
    const char *path;
    bool ok;
    DWORD error, typeUsed;
    LONGLONG size;
};

// runs on its own thread: the crashed one may be out of stack, and MiniDumpWriteDump wants to suspend it
static DWORD WINAPI dump_thread(LPVOID p)
{
    DumpJob *job = (DumpJob *)p;
    char sys[MAX_PATH];
    UINT n = GetSystemDirectoryA(sys, MAX_PATH);
    HMODULE dbghelp = NULL;
    if (n && n < MAX_PATH - 16) {
        strcat_s(sys, "\\dbghelp.dll");   // Windows' own copy, not one a game folder may carry
        dbghelp = LoadLibraryA(sys);
    }
    if (!dbghelp)
        dbghelp = LoadLibraryA("dbghelp.dll");
    PFN_MiniDumpWriteDump write = dbghelp ? (PFN_MiniDumpWriteDump)GetProcAddress(dbghelp, "MiniDumpWriteDump") : NULL;
    if (!write) {
        job->error = GetLastError();
        return 0;
    }
    HANDLE f = CreateFileA(job->path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        job->error = GetLastError();
        return 0;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId = job->threadId;
    mei.ExceptionPointers = job->ep;   // NULL for a hang report: no exception, all threads as they are
    mei.ClientPointers = FALSE;
    DWORD common = MiniDumpWithHandleData | MiniDumpWithUnloadedModules | MiniDumpWithFullMemoryInfo |
                   MiniDumpWithThreadInfo | MiniDumpWithProcessThreadData;
    DWORD types[3] = {
        g_cfg.crashDump == CRASHDUMP_FULL ? (DWORD)MiniDumpWithFullMemory | common
                                          : (DWORD)MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory | common,
        MiniDumpWithDataSegs | MiniDumpWithHandleData,   // older dbghelp without the newer flags
        MiniDumpNormal,
    };
    for (int i = 0; i < 3 && !job->ok; i++) {
        SetFilePointer(f, 0, NULL, FILE_BEGIN);
        SetEndOfFile(f);
        job->ok = write(GetCurrentProcess(), GetCurrentProcessId(), f, (MINIDUMP_TYPE)types[i], job->ep ? &mei : NULL, NULL, NULL) != FALSE;
        if (job->ok)
            job->typeUsed = types[i];
        else
            job->error = GetLastError();
    }
    LARGE_INTEGER sz;
    if (GetFileSizeEx(f, &sz))
        job->size = sz.QuadPart;
    CloseHandle(f);
    if (!job->ok)
        DeleteFileA(job->path);
    return 0;
}

// --- hang report (Q147) --------------------------------------------------------------------------
// Called from the log thread when the game hangs (window_hang_check) or on request (named event
// Local\QuantumDDraw.HangReport.<pid>). Every other thread is suspended just long enough to copy its registers
// and the top of its stack, then resumed - nothing that may take a lock (module lookups, the heap) runs while a
// thread is held. Also lists which thread owns each of our locks, and writes a minidump of all threads.
static char s_hang[64 * 1024];
static int s_hangLen;

static void hrep(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(line);
    if (s_hangLen + n + 2 < (int)sizeof(s_hang)) {
        memcpy(s_hang + s_hangLen, line, n);
        s_hangLen += n;
        s_hang[s_hangLen++] = '\r';
        s_hang[s_hangLen++] = '\n';
    }
}

static void lock_line(const char *name, CRITICAL_SECTION *cs)
{
    if (!cs || !cs->DebugInfo)
        return;
    DWORD owner = (DWORD)(ULONG_PTR)cs->OwningThread;
    hrep("  lock %-14s %s%lu (recursion %ld)", name, owner ? "held by thread " : "free", owner, owner ? cs->RecursionCount : 0);
}

struct ThreadShot {
    DWORD tid;
    bool ok;
    CONTEXT c;
    DWORD stack[8192];   // 32 KB from ESP
    SIZE_T words;
};
static ThreadShot s_shots[48];

void hang_report(const char *why)
{
    static volatile LONG busy;
    if (InterlockedExchange(&busy, 1))
        return;
    s_hangLen = 0;
    char stamp[32], base[MAX_PATH], dmp[MAX_PATH], txt[MAX_PATH];
    local_stamp(stamp, sizeof(stamp), false);
    _snprintf_s(base, sizeof(base), _TRUNCATE, "%sddraw_hang_%s", log_dir(), stamp);
    _snprintf_s(dmp, sizeof(dmp), _TRUNCATE, "%s.dmp", base);
    _snprintf_s(txt, sizeof(txt), _TRUNCATE, "%s.txt", base);
    hrep("HANG REPORT %s: %s", stamp, why);
    hrep("  threads: game %lu, render %lu, log %lu (this report)", g_gameThread, render_thread_id(), GetCurrentThreadId());
    lock_line("g_lock", &g_lock);
    lock_line("pixels", &g_pixLock);
    lock_line("render dirty", render_dirty_lock());
    lock_line("log", log_lock());

    // 1. copy registers + stack of every other thread (each suspended only for the copy)
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te;
        te.dwSize = sizeof(te);
        DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
        for (BOOL ok = Thread32First(snap, &te); ok && n < 48; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
                continue;
            ThreadShot &s = s_shots[n++];
            s.tid = te.th32ThreadID;
            s.ok = false;
            s.words = 0;
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, s.tid);
            if (!th)
                continue;
            if (SuspendThread(th) != (DWORD)-1) {
                memset(&s.c, 0, sizeof(s.c));
                s.c.ContextFlags = CONTEXT_FULL;
                s.ok = GetThreadContext(th, &s.c) != FALSE;
#if defined(_M_IX86)
                if (s.ok) {
                    SIZE_T got = 0;
                    ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(ULONG_PTR)s.c.Esp, s.stack, sizeof(s.stack), &got);
                    if (!got) {   // the stack may end before 32 KB: try smaller pieces
                        for (SIZE_T sz = sizeof(s.stack) / 2; sz >= 256 && !got; sz /= 2)
                            ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(ULONG_PTR)s.c.Esp, s.stack, sz, &got);
                    }
                    s.words = got / 4;
                }
#endif
                ResumeThread(th);
            }
            CloseHandle(th);
        }
        CloseHandle(snap);
    }

    // 2. describe them (all threads running again)
    for (int i = 0; i < n; i++) {
        ThreadShot &s = s_shots[i];
        const char *who = s.tid == g_gameThread ? " [GAME THREAD]" : s.tid == render_thread_id() ? " [render thread]" : "";
        if (!s.ok) {
            hrep("thread %lu%s: context not available", s.tid, who);
            continue;
        }
#if defined(_M_IX86)
        char where[MAX_PATH + 16];
        describe_address(s.c.Eip, where, sizeof(where));
        hrep("thread %lu%s: EIP=%08lX = %s  ESP=%08lX EBP=%08lX", s.tid, who, s.c.Eip, where, s.c.Esp, s.c.Ebp);
        // frame-pointer chain inside the copied stack
        ULONG_PTR ebp = s.c.Ebp, lo = s.c.Esp, hi = s.c.Esp + s.words * 4;
        for (int f = 0; f < 24 && ebp >= lo && ebp + 8 <= hi; f++) {
            DWORD next = s.stack[(ebp - lo) / 4], ret = s.stack[(ebp - lo) / 4 + 1];
            describe_address(ret, where, sizeof(where));
            hrep("    frame %2d: return %08lX = %s", f, ret, where);
            if (next <= ebp)
                break;
            ebp = next;
        }
        // words on the stack that point into a module (return addresses, most of them)
        int shown = 0;
        for (SIZE_T w = 0; w < s.words && shown < 24; w++) {
            if (describe_address(s.stack[w], where, sizeof(where)) && !strstr(where, ".exe+0x0")) {
                hrep("    stack [ESP+0x%04lX] %08lX = %s", (unsigned long)(w * 4), s.stack[w], where);
                shown++;
            }
        }
#endif
    }

    // 3. minidump of all threads (dbghelp suspends them itself)
    DumpJob job = {NULL, g_gameThread, dmp, false, 0, 0, 0};
    HANDLE t = CreateThread(NULL, 0, dump_thread, &job, 0, NULL);
    if (t) {
        WaitForSingleObject(t, 60000);
        CloseHandle(t);
    }
    if (job.ok)
        hrep("minidump: %s (%lld bytes, type 0x%lX)", dmp, job.size, job.typeUsed);
    else
        hrep("minidump FAILED: %s (error %lu)", dmp, job.error);
    HANDLE f = CreateFileA(txt, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wr;
        WriteFile(f, s_hang, s_hangLen, &wr, NULL);
        CloseHandle(f);
    }
    InterlockedExchange(&busy, 0);
}

// The report + dump. how = what ends the process; tid = the thread the exception happened on.
static void crash_report(EXCEPTION_POINTERS *ep, DWORD tid, const char *how)
{
    s_reportLen = 0;
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    DWORD code = er->ExceptionCode;
    char stamp[32], base[MAX_PATH], dmp[MAX_PATH], txt[MAX_PATH];
    local_stamp(stamp, sizeof(stamp), false);
    _snprintf_s(base, sizeof(base), _TRUNCATE, "%sddraw_crash_%s", log_dir(), stamp);
    _snprintf_s(dmp, sizeof(dmp), _TRUNCATE, "%s.dmp", base);
    _snprintf_s(txt, sizeof(txt), _TRUNCATE, "%s.txt", base);

    char where[MAX_PATH + 16], access[64];
    describe_address((ULONG_PTR)er->ExceptionAddress, where, sizeof(where));
    access_text(er, access, sizeof(access));
    rep("CRASH %s: exception 0x%08lX %s at %p = %s, thread %lu%s%s", stamp, code, exception_name(code),
        er->ExceptionAddress, where, tid, access, (er->ExceptionFlags & EXCEPTION_NONCONTINUABLE) ? ", non-continuable" : "");
    rep("  %s", how);
#if defined(_M_IX86)
    const CONTEXT *c = ep->ContextRecord;
    rep("  EIP=%08lX ESP=%08lX EBP=%08lX EFL=%08lX", c->Eip, c->Esp, c->Ebp, c->EFlags);
    rep("  EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX", c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi);
#endif
    report_stack(ep->ContextRecord);
    static char recent[32 * 256];
    log_recent(recent, sizeof(recent));
    rep("  last 32 events before the crash:");
    for (char *line = recent, *end; *line; line = end) {
        end = strstr(line, "\r\n");
        if (end) *end = 0;
        rep("    %s", line);
        if (!end) break;
        end += 2;
    }

    if (g_cfg.crashDump != CRASHDUMP_OFF) {
        prune_dumps(log_dir());
        DumpJob job = {ep, tid, dmp, false, 0, 0, 0};
        HANDLE t = CreateThread(NULL, 0, dump_thread, &job, 0, NULL);
        DWORD t0 = GetTickCount();
        if (t) {
            WaitForSingleObject(t, 120000);
            CloseHandle(t);
        } else {
            dump_thread(&job);   // no thread: try on the crashed one
        }
        if (job.ok)
            rep("  minidump: %s (%lld bytes, %s, type 0x%lX, %lu ms)", dmp, job.size,
                g_cfg.crashDump == CRASHDUMP_FULL ? "crashdump=full" : "crashdump=normal", job.typeUsed, GetTickCount() - t0);
        else
            rep("  minidump FAILED: %s (error %lu)", dmp, job.error);
    } else {
        rep("  minidump: off (crashdump=false)");
    }
    rep("  report: %s", txt);

    HANDLE f = CreateFileA(txt, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wr;
        WriteFile(f, s_report, s_reportLen, &wr, NULL);
        CloseHandle(f);
    }
    if (g_dd && g_dd->realModeChanged)
        ChangeDisplaySettingsA(NULL, 0);   // the desktop must not stay in the game's resolution
    log_flush();
}

// Exception handlers registered on this thread's SEH chain outside Windows' own modules (x86). None: only
// the thread start frame is left, whose filter is the unhandled exception filter - the exception is
// certainly not handled. first gets the first such handler ("module+0x...").
static int seh_game_handlers(char *first, size_t cap)
{
    strcpy_s(first, cap, "-");
    int n = 0;
#if defined(_M_IX86)
    ULONG_PTR rec = (ULONG_PTR)((NT_TIB *)NtCurrentTeb())->ExceptionList;
    for (int i = 0; i < 64 && rec && rec != (ULONG_PTR)-1; i++) {
        DWORD next, handler;
        if (!read_dword(rec, &next) || !read_dword(rec + 4, &handler))
            break;
        char mod[MAX_PATH + 16];
        describe_address(handler, mod, sizeof(mod));
        if (_strnicmp(mod, "ntdll.dll+", 10) != 0 && _strnicmp(mod, "kernel32.dll+", 13) != 0 &&
            _strnicmp(mod, "kernelbase.dll+", 15) != 0) {
            if (!n++)
                strcpy_s(first, cap, mod);
        }
        rec = next;
    }
#endif
    return n;
}

static void debugger_first_chance(EXCEPTION_POINTERS *ep)
{
    char first[MAX_PATH + 16], how[MAX_PATH + 200];
    int handlers = seh_game_handlers(first, sizeof(first));
    if (handlers == 0) {
        _snprintf_s(how, sizeof(how), _TRUNCATE,
                    "a debugger is attached (the unhandled exception filter will not run) and no handler of the game is "
                    "on this thread's SEH chain: certainly unhandled - reported at first chance");
    } else {
        // the game may still handle it; only the first one of these is reported
        static volatile LONG once;
        if (InterlockedExchange(&once, 1))
            return;
        _snprintf_s(how, sizeof(how), _TRUNCATE,
                    "a debugger is attached (the unhandled exception filter will not run); reported at first chance - "
                    "%d game handler(s) on the SEH chain (first %s) may still handle it, then this report is moot",
                    handlers, first);
    }
    if (handlers == 0 && InterlockedExchange(&s_inFilter, 1))
        return;
    crash_report(ep, GetCurrentThreadId(), how);
    journal_dump("crash", 6.0);   // Q157: what our DLL did and was asked to do in the seconds before
    if (handlers == 0 && s_testFault)
        r_TerminateProcess(GetCurrentProcess(), ep->ExceptionRecord->ExceptionCode);   // the self test ends here
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    if (InterlockedExchange(&s_inFilter, 1))   // a second thread crashing meanwhile: only the first is reported
        return s_next ? s_next(ep) : EXCEPTION_CONTINUE_SEARCH;
    crash_report(ep, GetCurrentThreadId(), "unhandled - reported by the unhandled exception filter");
    if (s_testFault) {
        // the self test ends here, without Windows' error report or the game's crash handler
        r_TerminateProcess(GetCurrentProcess(), ep->ExceptionRecord->ExceptionCode);
        return EXCEPTION_EXECUTE_HANDLER;
    }
    if (s_next) {
        log_crash("  handing over to the game's exception filter %p", s_next);
        log_flush();
        return s_next(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// --- the game's own crash handling -------------------------------------------------------------------
// QEC's Quantum.dll has a vectored handler that ends the process itself (TerminateProcess with the
// exception code) - the unhandled exception filter never runs. A game module ending the process on the
// thread of a fatal exception seen within the last 5 s is taken as that: report + dump first.

static void before_game_exit(const char *api, UINT exitCode, void *caller)
{
    DWORD tid = s_pending.tid;
    if (!tid || tid != GetCurrentThreadId() || GetTickCount() - s_pending.tick > 5000)
        return;
    if (InterlockedExchange(&s_inFilter, 1))
        return;
    char mod[MAX_PATH + 16], how[MAX_PATH + 160];
    describe_address((ULONG_PTR)caller, mod, sizeof(mod));
    _snprintf_s(how, sizeof(how), _TRUNCATE,
                "the game's own exception handling ends the process: %s(0x%X) called from %s, %lu ms after the exception",
                api, exitCode, mod, GetTickCount() - s_pending.tick);
    EXCEPTION_POINTERS ep = {&s_pending.rec, &s_pending.ctx};
    crash_report(&ep, tid, how);
}

static BOOL WINAPI h_TerminateProcess(HANDLE process, UINT code)
{
    if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId())
        before_game_exit("TerminateProcess", code, _ReturnAddress());
    return r_TerminateProcess(process, code);
}

static void WINAPI h_ExitProcess(UINT code)
{
    before_game_exit("ExitProcess", code, _ReturnAddress());
    r_ExitProcess(code);
}

// --- installation --------------------------------------------------------------------------------

static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI h_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f)
{
    LPTOP_LEVEL_EXCEPTION_FILTER old = s_next;
    if (f != crash_filter) {
        s_next = f;
        char mod[MAX_PATH + 16];
        describe_address((ULONG_PTR)f, mod, sizeof(mod));
        log_event("crash: the game set an unhandled exception filter %p = %s - chained behind ours", f, mod);
    }
    return old;
}

void crash_init()
{
    AddVectoredExceptionHandler(0, first_chance);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    r_TerminateProcess = (PFN_TerminateProcess)GetProcAddress(k32, "TerminateProcess");
    r_ExitProcess = (PFN_ExitProcess)GetProcAddress(k32, "ExitProcess");
    r_SetUnhandledExceptionFilter = (PFN_SUEF)GetProcAddress(k32, "SetUnhandledExceptionFilter");
    if (r_SetUnhandledExceptionFilter)
        s_next = r_SetUnhandledExceptionFilter(crash_filter);
}

void crash_reassert()
{
    if (!r_SetUnhandledExceptionFilter)
        return;
    LPTOP_LEVEL_EXCEPTION_FILTER prev = r_SetUnhandledExceptionFilter(crash_filter);
    if (prev != crash_filter) {
        s_next = prev;
        char mod[MAX_PATH + 16];
        describe_address((ULONG_PTR)prev, mod, sizeof(mod));
        log_event("crash: unhandled exception filter re-installed on top, the game's %p = %s is chained", prev, mod);
    }
    log_event("crash: report + %s minidump on an unhandled exception or when the game's own handler ends the process "
              "(ddraw_crash_<time>.dmp/.txt, newest %d kept)",
              g_cfg.crashDump == CRASHDUMP_FULL ? "full" : g_cfg.crashDump == CRASHDUMP_NORMAL ? "normal" : "no",
              KEEP_DUMPS);
}

int crash_hooks(void ***reals, void **hooks)
{
    reals[0] = (void **)&r_SetUnhandledExceptionFilter;
    hooks[0] = (void *)h_SetUnhandledExceptionFilter;
    reals[1] = (void **)&r_TerminateProcess;
    hooks[1] = (void *)h_TerminateProcess;
    reals[2] = (void **)&r_ExitProcess;
    hooks[2] = (void *)h_ExitProcess;
    return 3;
}
void crash_test_raise()
{
    s_testFault = true;
    if (g_cfg.crashTest == 2) {
        log_event("crashtest=av: writing to address 0x10 in the render thread");
        *(volatile int *)(ULONG_PTR)0x10 = 0x51444;
    } else {
        log_event("crashtest: raising exception 0x%08lX in the render thread", CRASHTEST_CODE);
        RaiseException(CRASHTEST_CODE, EXCEPTION_NONCONTINUABLE, 0, NULL);
    }
}
