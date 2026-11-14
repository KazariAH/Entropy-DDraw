// Q149: checks of the window-free present planning (src\frameplan.cpp): write-watch pages -> row bands of the
// primary, and present coalescing. No window, no DirectDraw.
#include "../src/frameplan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_pass, s_fail;
static void check(bool ok, const char *what)
{
    if (ok) s_pass++;
    else { s_fail++; printf("FAIL: %s\n", what); }
}

static bool well_formed(const RowBands &b, int mergeGap)
{
    for (int i = 0; i < b.n; i++) {
        if (b.bottom[i] <= b.top[i]) return false;
        if (i > 0 && b.top[i] - b.bottom[i - 1] <= mergeGap) return false;   // sorted, disjoint, gaps kept
    }
    return b.n <= BANDS_MAX;
}

static bool covers(const RowBands &b, int row)
{
    for (int i = 0; i < b.n; i++)
        if (row >= b.top[i] && row < b.bottom[i]) return true;
    return false;
}

static void test_bands()
{
    RowBands b;
    bands_clear(&b);
    bands_add(&b, 10, 20, 0);
    bands_add(&b, 30, 40, 0);
    check(b.n == 2 && b.top[0] == 10 && b.bottom[1] == 40, "two separate bands stay separate");
    bands_add(&b, 20, 30, 0);
    check(b.n == 1 && b.top[0] == 10 && b.bottom[0] == 40, "a range that touches both joins them");
    bands_add(&b, 0, 5, 0);
    bands_add(&b, 100, 101, 0);
    check(b.n == 3 && b.top[0] == 0 && b.top[2] == 100, "inserted in order");
    bands_add(&b, 3, 12, 0);
    check(b.n == 2 && b.top[0] == 0 && b.bottom[0] == 40, "an overlap joins the front bands");
    bands_add(&b, 7, 7, 0);
    check(b.n == 2, "an empty range changes nothing");

    bands_clear(&b);
    bands_add(&b, 0, 10, 8);
    bands_add(&b, 18, 20, 8);
    check(b.n == 1 && b.bottom[0] == 20, "bands up to mergeGap rows apart are joined");
    bands_add(&b, 29, 30, 8);
    check(b.n == 2, "bands further apart than mergeGap stay separate");

    // full list: the closest neighbours are joined, nothing is lost
    bands_clear(&b);
    for (int i = 0; i < BANDS_MAX; i++)
        bands_add(&b, i * 100, i * 100 + 10, 0);
    check(b.n == BANDS_MAX, "list filled");
    bands_add(&b, 2000, 2001, 0);
    check(b.n == BANDS_MAX && covers(b, 2000) && covers(b, 0) && covers(b, 1505) && well_formed(b, 0),
          "a full list makes room by joining and still covers everything");
    bands_add(&b, 55, 56, 0);   // goes between bands that may just have been joined
    check(b.n <= BANDS_MAX && covers(b, 55) && well_formed(b, 0), "insert next to a joined band stays sorted");

    // random: every added row is covered, the list stays well formed; while below the limit nothing extra
    srand(149);
    for (int round = 0; round < 2000; round++) {
        int gap = rand() % 4 == 0 ? 0 : rand() % 6;
        static bool want[1500];
        memset(want, 0, sizeof(want));
        bands_clear(&b);
        int adds = 1 + rand() % 30, maxN = 0;
        for (int k = 0; k < adds; k++) {
            int t = rand() % 1440, h = 1 + rand() % (rand() % 3 == 0 ? 200 : 8);
            int bo = t + h > 1440 ? 1440 : t + h;
            bands_add(&b, t, bo, gap);
            if (b.n > maxN) maxN = b.n;
            for (int r = t; r < bo; r++) want[r] = true;
        }
        bool ok = well_formed(b, gap);
        for (int r = 0; r < 1440 && ok; r++)
            if (want[r] && !covers(b, r)) ok = false;
        if (!ok) { check(false, "random adds: covered and well formed"); break; }
        // below the limit the bands are exactly the wanted rows plus gaps of at most 'gap' rows filled in
        if (maxN < BANDS_MAX) {   // never full: no forced joins
            for (int r = 0; r < 1440 && ok; r++) {
                if (covers(b, r) && !want[r]) {
                    // an unwanted covered row must sit in a filled gap: wanted rows on both sides within gap
                    int up = r, dn = r;
                    while (up >= 0 && !want[up]) up--;
                    while (dn < 1440 && !want[dn]) dn++;
                    if (up < 0 || dn >= 1440 || dn - up - 1 > gap) ok = false;
                }
            }
            if (!ok) { check(false, "random adds: no extra rows below the limit"); break; }
        }
    }
    check(true, "random band adds (2000 rounds)");
}

static void test_pages()
{
    const size_t P = 4096;
    const int pitch = 5120, rows = 1440;   // 2560 x 1440 x 16 bpp
    uintptr_t base = 0x10000000, bits = base + pitch;   // guard row in front (guard_lines)
    int t, b;
    check(!page_rows(base, P, bits, pitch, rows, &t, &b), "page holding only the guard row: no rows");
    check(page_rows(base + P, P, bits, pitch, rows, &t, &b) && t == 0 && b == 1, "page 1 = rows 0..0 (bytes 0..3071)");
    check(page_rows(base + 2 * P, P, bits, pitch, rows, &t, &b) && t == 0 && b == 2, "page 2 spans rows 0 and 1");
    uintptr_t lastRowEnd = bits + (uintptr_t)pitch * rows;
    uintptr_t lastPage = (lastRowEnd - 1) & ~(uintptr_t)(P - 1);
    check(page_rows(lastPage, P, bits, pitch, rows, &t, &b) && b == rows && t == rows - 1, "last page ends at the last row");
    check(!page_rows(lastPage + P, P, bits, pitch, rows, &t, &b), "page of the lower guard row / tail: no rows");

    // every byte of the surface: its row is inside the rows of its page
    bool ok = true;
    for (uintptr_t a = bits; a < lastRowEnd && ok; a += 997) {
        uintptr_t pg = a & ~(uintptr_t)(P - 1);
        int row = (int)((a - bits) / pitch);
        if (!page_rows(pg, P, bits, pitch, rows, &t, &b) || row < t || row >= b) ok = false;
    }
    check(ok, "the page of any byte covers that byte's row");

    // page list -> bands: consecutive pages become one band
    void *pages[8];
    for (int i = 0; i < 5; i++) pages[i] = (void *)(base + (10 + i) * P);
    pages[5] = (void *)(base + 900 * P);
    RowBands bb;
    bands_clear(&bb);
    bands_from_pages(&bb, pages, 6, P, bits, pitch, rows, 0);
    int r0, r1, r2, r3;
    page_rows(base + 10 * P, P, bits, pitch, rows, &r0, &r1);
    page_rows(base + 14 * P, P, bits, pitch, rows, &r2, &r3);
    check(bb.n == 2 && bb.top[0] == r0 && bb.bottom[0] == r3, "a run of pages is one band");
    // the whole surface written: one band of all rows
    static void *all[2000];
    size_t n = 0;
    for (uintptr_t pg = base; pg < lastRowEnd + pitch; pg += P) all[n++] = (void *)pg;
    bands_clear(&bb);
    bands_from_pages(&bb, all, n, P, bits, pitch, rows, 0);
    check(bb.n == 1 && bb.top[0] == 0 && bb.bottom[0] == rows, "all pages = rows 0..1439");
}

static void test_settle()
{
    Settle s;
    settle_reset(&s);
    double w;
    check(settle_step(&s, 0, 0, 2, 8, &w) == SETTLE_IDLE, "no writes: idle");
    check(settle_step(&s, 10, 5, 2, 8, &w) == SETTLE_WAIT && w == 2, "first writes: wait the quiet time");
    check(settle_step(&s, 11, 9, 2, 8, &w) == SETTLE_WAIT && w == 2, "more pages: quiet time starts again");
    check(settle_step(&s, 12.5, 9, 2, 8, &w) == SETTLE_WAIT && w == 0.5, "no new pages yet: the rest of the quiet time");
    check(settle_step(&s, 13, 9, 2, 8, &w) == SETTLE_GO, "quiet for 2 ms: present");
    check(!s.pending, "present ends the burst");
    // continuous writes: the cap
    settle_reset(&s);
    int r = SETTLE_WAIT;
    double t = 100;
    long c = 1;
    while (r == SETTLE_WAIT && t < 200) { r = settle_step(&s, t, c++, 2, 8, &w); t += 1; }
    check(r == SETTLE_GO && t - 1 >= 108 && t - 1 < 109, "writes that never stop: present after the cap (8 ms)");
    check(settle_step(&s, 300, 3, 0, 8, &w) == SETTLE_GO, "quiet 0 = present at once");
    settle_reset(&s);
    settle_step(&s, 0, 4, 2, 8, &w);
    check(settle_step(&s, 1, 0, 2, 8, &w) == SETTLE_IDLE && !s.pending, "copied meanwhile (count 0): burst forgotten");
    // wait never exceeds the cap
    settle_reset(&s);
    settle_step(&s, 0, 1, 5, 6, &w);
    check(settle_step(&s, 4, 2, 5, 6, &w) == SETTLE_WAIT && w == 2, "the wait is cut to the cap");
}

int main()
{
    test_bands();
    test_pages();
    test_settle();
    printf("frametest: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
