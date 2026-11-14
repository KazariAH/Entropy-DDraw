// Q157: activity journal - what our DLL does and is asked to do, with time and thread, in a lock-free ring.
// Written to ddraw_journal_<time>_<why>.txt on a crash (first chance, under the debugger) and on request
// (named event Local\QuantumDDraw.Journal.<pid>, e.g. by the test script 3 s after a Load click), so the seconds
// before a crash can be compared with the same seconds of a load that did not crash. Frequent calls (Lock, Unlock,
// Blt to off-screen surfaces, cursor / coordinate hooks) are counted and written as one entry per 100 ms.
#pragma once
#include <windows.h>

enum JournalKind {
    J_API,          // a game call into our surface / DirectDraw API: a = JApi, b = surface id, c = detail
    J_CREATE,       // CreateSurface: a = id, b = width << 16 | height, c = caps
    J_FREE,         // a surface freed: a = id
    J_SUBMSG,       // our subclass procedure saw a message: a = hwnd, b = message, c = wParam
    J_HOOKMSG,      // our message hook saw a message taken from the queue: a = hwnd, b = message, c = wParam
    J_HOOKCALL,     // the game called one of our window / cursor hooks (rare ones): a = JHook, b, c = arguments
    J_PRESENT,      // render thread presented: a = new pixels (0 / 1), b = forced, c = rects
    J_CHILDSET,     // render thread: the set of visible child windows changed: a = count, b = first hwnd
    J_CHILDPAINT,   // render thread painted the picture into a child window: a = hwnd, b = w, c = h
    J_FORCE,        // a forced redraw was requested: a = JForce reason
    J_LOGTHREAD,    // log thread: a = 0 flush, 1 hang check reported, 2 journal dump
    J_COUNTS,       // counters of the last 100 ms: a = Lock, b = Blt (all), c = cursor / coordinate hook calls
    J_NOTE,         // free text pointer (string literal): a = (ULONG_PTR)text
    J_KINDS
};
enum JApi { JA_RELEASE, JA_FLIP, JA_BLT_PRIMARY, JA_GETDC, JA_RELEASEDC, JA_SETPALETTE, JA_RESTORE, JA_SETCOOP,
            JA_SETMODE, JA_RESTOREMODE, JA_CLIPPER, JA_COUNT };
enum JHook { JH_CLIPCURSOR, JH_SETCURSORPOS, JH_SETWINDOWPOS, JH_MOVEWINDOW, JH_WINDOWFROMPOINT, JH_SHOWWINDOW,
             JH_COUNT };
enum JCount { JC_LOCK, JC_BLT, JC_CURSOR, JC_COUNT };

void jr(int kind, ULONG_PTR a = 0, ULONG_PTR b = 0, ULONG_PTR c = 0);
void jr_count(int counter);
void jr_tick();                                   // render thread: once per 100 ms writes the counters
void journal_dump(const char *why, double seconds); // last 'seconds' of the journal -> file next to ddraw.log
