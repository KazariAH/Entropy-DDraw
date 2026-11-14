// Child windows of the game (QEC's dialogs: briefing, Esc menu, save / load, loading screen) - the parts that
// need no window: painting the picture into a DC, which children qualify, and when to repaint them.
// Kept free of the DLL's globals so tests\childtest.cpp can compile and check it without any window.
#pragma once
#include <windows.h>

// the picture as the renderer shows it: rows of the shadow copy of the primary
struct PicSrc {
    const BYTE *bits;     // first row of the primary-sized buffer
    int pitch, bpp;       // bytes per row, 8 / 16 / 32
    int primW, primH;     // primary size
    int imgX, imgY;       // the shown part of the primary (windowed desktop view may start outside it)
    int imgW, imgH;
    const DWORD *lut8;    // 8 bpp: 256 entries R | G << 8 | B << 16
    bool linear;          // smooth scaling
};

// Paints the picture into dc. 'area' is the renderer's whole client area and 'vp' the picture rectangle inside
// it, both in dc's coordinates (a child window's DC: shifted by minus the child's position). Everything of 'area'
// outside the picture is black; GDI clips to the DC.
void picture_paint(HDC dc, const RECT &area, const RECT &vp, const PicSrc &s);

// a direct child of the game window that gets the picture painted into it
bool child_paint_eligible(const char *cls, int w, int h, LONG exStyle);

// shifts the client area / picture rectangle into the coordinates of a child at (cx, cy) of the client
void child_coords(int cw, int ch, const RECT &vp, int cx, int cy, RECT *area, RECT *vpOut);

// When to act on the set of visible child windows. Fed every render tick; returns CS_* bits.
enum { CS_REPAINT = 1, CS_PRESENT = 4 };
struct ChildSched {
    DWORD changedAt;
    int repaints;   // still due: 3 = at once, 2 = +250 ms, 1 = +1000 ms
};
int child_sched_step(ChildSched *s, DWORD now, bool setChanged, int visible);

// Q155: horizontal shift for a window wider than its monitor's work area whose client area fits (QEC: client 2560 =
// the screen width, frame 3 px on each side). The frame goes off-screen instead of the picture: returns the dx that
// puts the client fully inside [workLeft, workRight); 0 when the window fits, the client does not fit, or the client
// is inside already.
long fit_client_dx(long winLeft, long winRight, long clientLeft, long clientRight, long workLeft, long workRight);
