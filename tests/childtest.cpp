// Q146: checks of the window-free fixchilds parts (src\childwin.cpp) - no window is created, painting goes into
// memory DCs. The rule checked for the paint-through: what a child window gets painted must be exactly the part of
// the whole picture (as the renderer shows it in the client area) that lies under the child - for 8 / 16 / 32 bpp,
// 1:1, letterboxed and scaled pictures, and a windowed view that reaches past the primary.
#include "../src/childwin.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int s_pass, s_fail;
static void check(bool ok, const char *what)
{
    if (ok) s_pass++;
    else { s_fail++; printf("FAIL: %s\n", what); }
}

// a 32 bpp top-down DIB section selected into a memory DC, filled with a marker colour
struct Canvas {
    HDC dc;
    HBITMAP bmp, old;
    DWORD *px;
    int w, h;
};
static Canvas canvas(int w, int h)
{
    Canvas c;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    c.dc = CreateCompatibleDC(NULL);
    void *bits = NULL;
    c.bmp = CreateDIBSection(c.dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    c.old = (HBITMAP)SelectObject(c.dc, c.bmp);
    c.px = (DWORD *)bits;
    c.w = w;
    c.h = h;
    for (int i = 0; i < w * h; i++) c.px[i] = 0x00123456;   // "not painted"
    return c;
}
static void done(Canvas &c)
{
    SelectObject(c.dc, c.old);
    DeleteObject(c.bmp);
    DeleteDC(c.dc);
}
static DWORD at(const Canvas &c, int x, int y) { return c.px[y * c.w + x] & 0xFFFFFF; }

// a primary-sized source with a pattern where neighbouring pixels differ
struct Src {
    BYTE *buf;
    PicSrc s;
    DWORD lut[256];
};
static Src source(int bpp, int primW, int primH, int pitchExtra)
{
    Src r;
    int bytes = bpp / 8;
    r.s.pitch = primW * bytes + pitchExtra * bytes;
    r.buf = (BYTE *)calloc((size_t)r.s.pitch * primH, 1);
    for (int i = 0; i < 256; i++) r.lut[i] = (DWORD)((i * 7) & 255) | ((DWORD)((i * 13) & 255) << 8) | ((DWORD)((i * 29) & 255) << 16);
    for (int y = 0; y < primH; y++)
        for (int x = 0; x < primW; x++) {
            unsigned v = (unsigned)(x * 37 + y * 101 + 11);
            BYTE *p = r.buf + (size_t)y * r.s.pitch + (size_t)x * bytes;
            if (bpp == 8) *p = (BYTE)(v & 255);
            else if (bpp == 16) *(WORD *)p = (WORD)(((v & 31) << 11) | (((v >> 5) & 63) << 5) | ((v >> 3) & 31));
            else *(DWORD *)p = (v * 2654435761u) & 0xFFFFFF;
        }
    r.s.bits = r.buf;
    r.s.bpp = bpp;
    r.s.primW = primW;
    r.s.primH = primH;
    r.s.imgX = 0;
    r.s.imgY = 0;
    r.s.imgW = primW;
    r.s.imgH = primH;
    r.s.lut8 = r.lut;
    r.s.linear = false;
    return r;
}

// paints the whole client (cw x ch, picture at vp) and a child at (cx,cy) of size w x h; the child must equal
// the client painting's region under it
static void paint_through_case(const char *name, const PicSrc &s, int cw, int ch, RECT vp, int cx, int cy, int w, int h)
{
    Canvas full = canvas(cw, ch);
    RECT all = {0, 0, cw, ch};
    picture_paint(full.dc, all, vp, s);
    Canvas child = canvas(w, h);
    RECT area, v;
    child_coords(cw, ch, vp, cx, cy, &area, &v);
    picture_paint(child.dc, area, v, s);
    int bad = 0, unpainted = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if (at(child, x, y) == 0x123456) unpainted++;
            if (cx + x < cw && cy + y < ch && at(child, x, y) != at(full, cx + x, cy + y)) bad++;
        }
    char msg[200];
    sprintf_s(msg, "%s: child (%d,%d) %dx%d equals the client picture under it (%d differing, %d unpainted)", name, cx, cy, w,
              h, bad, unpainted);
    check(bad == 0 && unpainted == 0, msg);
    done(child);
    done(full);
}

static void test_paint()
{
    for (int bpp = 8; bpp <= 32; bpp *= 2) {
        if (bpp == 8 || bpp == 16 || bpp == 32) {
            Src a = source(bpp, 64, 36, 3);
            char n[64];
            sprintf_s(n, "%d bpp 1:1", bpp);
            paint_through_case(n, a.s, 64, 36, RECT{0, 0, 64, 36}, 13, 7, 20, 11);
            paint_through_case(n, a.s, 64, 36, RECT{0, 0, 64, 36}, 0, 0, 64, 36);
            sprintf_s(n, "%d bpp letterbox (bars top/bottom)", bpp);
            paint_through_case(n, a.s, 64, 46, RECT{0, 5, 64, 41}, 0, 0, 64, 36);   // QEC: dialog at game coords
            sprintf_s(n, "%d bpp pillarbox", bpp);
            paint_through_case(n, a.s, 80, 36, RECT{8, 0, 72, 36}, 2, 3, 30, 20);
            sprintf_s(n, "%d bpp scaled x2", bpp);
            paint_through_case(n, a.s, 128, 72, RECT{0, 0, 128, 72}, 9, 5, 41, 23);
            sprintf_s(n, "%d bpp scaled x1.5 + bars", bpp);
            paint_through_case(n, a.s, 100, 60, RECT{2, 3, 98, 57}, 30, 10, 50, 45);
            free(a.buf);
        }
    }
    // the 1:1 child must show the source pixels themselves (not just "the same as the full painting"):
    // 32 bpp source = exact colours
    {
        Src a = source(32, 16, 8, 0);
        Canvas c = canvas(5, 4);
        RECT area, v;
        child_coords(16, 8, RECT{0, 0, 16, 8}, 6, 2, &area, &v);
        picture_paint(c.dc, area, v, a.s);
        int bad = 0;
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 5; x++)
                if (at(c, x, y) != (((const DWORD *)(a.buf + (size_t)(y + 2) * a.s.pitch))[x + 6] & 0xFFFFFF)) bad++;
        check(bad == 0, "32 bpp child at (6,2): exactly the source pixels (x+6, y+2)");
        done(c);
        free(a.buf);
    }
    // letterbox: the rows of the child above the picture are black
    {
        Src a = source(16, 64, 36, 0);
        Canvas c = canvas(64, 36);
        RECT area, v;
        child_coords(64, 46, RECT{0, 5, 64, 41}, 0, 0, &area, &v);
        picture_paint(c.dc, area, v, a.s);
        bool black = true;
        for (int y = 0; y < 5; y++)
            for (int x = 0; x < 64; x++)
                if (at(c, x, y) != 0) black = false;
        check(black, "letterbox: child rows 0-4 (above the picture) are black");
        done(c);
        free(a.buf);
    }
    // a windowed view reaching past the primary: the part outside is black, the rest is the picture
    {
        Src a = source(16, 32, 20, 0);
        a.s.imgX = -4;
        a.s.imgY = 0;
        a.s.imgW = 32;
        a.s.imgH = 20;
        paint_through_case("view starting 4 px left of the primary", a.s, 32, 20, RECT{0, 0, 32, 20}, 0, 0, 10, 10);
        Canvas c = canvas(4, 20);
        RECT area, v;
        child_coords(32, 20, RECT{0, 0, 32, 20}, 0, 0, &area, &v);
        picture_paint(c.dc, area, v, a.s);
        bool black = true;
        for (int y = 0; y < 20; y++)
            for (int x = 0; x < 4; x++)
                if (at(c, x, y) != 0) black = false;
        check(black, "view past the primary: the 4 columns outside it are black");
        done(c);
        free(a.buf);
    }
    // no picture yet: everything black
    {
        PicSrc s;
        memset(&s, 0, sizeof(s));
        Canvas c = canvas(8, 8);
        RECT area = {0, 0, 8, 8}, vp = {0, 0, 8, 8};
        picture_paint(c.dc, area, vp, s);
        bool black = true;
        for (int i = 0; i < 64; i++)
            if ((c.px[i] & 0xFFFFFF) != 0) black = false;
        check(black, "no image: the area is painted black");
        done(c);
    }
}

static void test_eligible()
{
    check(child_paint_eligible("#32770", 2560, 1440, 0), "a QEC dialog (#32770, full size) gets the picture");
    check(child_paint_eligible("Static", 452, 432, 0), "a static control gets the picture");
    check(!child_paint_eligible("Edit", 300, 20, 0), "an Edit field is left alone");
    check(!child_paint_eligible("edit", 300, 20, 0), "class name compared without case");
    check(!child_paint_eligible("#32770", 1, 200, 0), "1 px wide window is ignored");
    check(!child_paint_eligible("#32770", 0, 0, 0), "zero-size window is ignored");
    check(!child_paint_eligible("#32770", 500, 300, WS_EX_TRANSPARENT), "WS_EX_TRANSPARENT window is ignored");
    check(child_paint_eligible(NULL, 10, 10, 0), "unknown class name still qualifies");
}

static void test_coords()
{
    RECT area, v, vp = {0, 80, 2560, 1520};
    child_coords(2560, 1600, vp, 0, 0, &area, &v);
    RECT whole = {0, 0, 2560, 1600};
    check(EqualRect(&area, &whole) && EqualRect(&v, &vp), "child at (0,0): coordinates unchanged");
    child_coords(2560, 1600, vp, 971, 493, &area, &v);
    check(area.left == -971 && area.top == -493 && area.right == 2560 - 971 && area.bottom == 1600 - 493,
          "child at (971,493): client area shifted by (-971,-493)");
    check(v.left == -971 && v.top == 80 - 493 && v.right == 2560 - 971 && v.bottom == 1520 - 493,
          "child at (971,493): picture rectangle shifted by (-971,-493)");
}

static void test_sched()
{
    ChildSched s;
    memset(&s, 0, sizeof(s));
    int r = child_sched_step(&s, 10000, true, 5);
    check(r == (CS_PRESENT | CS_REPAINT), "dialog appears: present + repaint at once");
    check(child_sched_step(&s, 10100, false, 5) == 0, "+100 ms: nothing");
    check(child_sched_step(&s, 10250, false, 5) == CS_REPAINT, "+250 ms: second repaint");
    check(child_sched_step(&s, 10999, false, 5) == 0, "+999 ms: nothing");
    check(child_sched_step(&s, 11000, false, 5) == CS_REPAINT, "+1000 ms: third repaint");
    int later = 0;
    for (DWORD t = 11050; t < 20000; t += 50) later |= child_sched_step(&s, t, false, 5);
    check(later == 0, "after +1000 ms: nothing more while the set stays the same");
    check(child_sched_step(&s, 20000, true, 0) == CS_PRESENT, "dialog closed: present only");
    later = 0;
    for (DWORD t = 20050; t < 25000; t += 50) later |= child_sched_step(&s, t, false, 0);
    check(later == 0, "no dialog: no repaint");

    memset(&s, 0, sizeof(s));
    int n = 0;
    for (DWORD t = 0xFFFFFF00u; t != 0xFFFFFF00u + 3000; t += 50)   // tick counter wraps during the schedule
        if (child_sched_step(&s, t, t == 0xFFFFFF00u, 3) & CS_REPAINT) n++;
    check(n == 3, "3 repaints also across the tick counter wrap");

    memset(&s, 0, sizeof(s));
    n = 0;
    child_sched_step(&s, 1000, true, 4);
    child_sched_step(&s, 1300, true, 6);   // a second dialog (the 'saved' box) appears inside the schedule
    for (DWORD t = 1300; t < 6000; t += 50) if (child_sched_step(&s, t, false, 6) & CS_REPAINT) n++;
    check(n == 2, "a change during the schedule starts it again (at once was returned by the change, +250 and +1000 follow)");
    check(!(CS_REPAINT & CS_PRESENT), "action bits are distinct");
}
// Q155: a window wider than the screen puts its frame off-screen, never the picture
static void test_fit()
{
    // QEC on a 2560 screen: window 2566 wide created at x = 0, client from 3 to 2563
    check(fit_client_dx(0, 2566, 3, 2563, 0, 2560) == -3, "window at 0: moved 3 px left, client 0..2560");
    check(fit_client_dx(-3, 2563, 0, 2560, 0, 2560) == 0, "client already on the screen: unchanged");
    check(fit_client_dx(-10, 2556, -7, 2553, 0, 2560) == 7, "client starting left of the screen: moved right");
    check(fit_client_dx(100, 1380, 103, 1377, 0, 2560) == 0, "a window that fits is left where it is (user's choice)");
    check(fit_client_dx(0, 2600, 3, 2597, 0, 2560) == 0, "a client wider than the screen cannot fit: unchanged");
    check(fit_client_dx(2560, 5126, 2563, 5123, 2560, 5120) == -3, "second monitor right of the first: same rule");
    check(fit_client_dx(-2566, 0, -2563, -3, -2560, 0) == 3, "monitor left of the primary (negative coordinates)");
}

int main()
{
    test_eligible();
    test_coords();
    test_sched();
    test_paint();
    test_fit();
    printf("childtest: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
