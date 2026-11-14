// Q149: the window-free parts of planning a present - which rows of the primary a write-watch page list touches,
// and when a burst of writes into the primary is complete enough to show it. Kept free of the DLL's globals so
// tests\frametest.cpp can check it without any window.
#pragma once
#include <stddef.h>
#include <stdint.h>

// sorted, disjoint row ranges [top, bottom) of the primary
enum { BANDS_MAX = 16 };
struct RowBands {
    int n;
    int top[BANDS_MAX], bottom[BANDS_MAX];
};

void bands_clear(RowBands *b);
// Adds rows [top, bottom). Bands that overlap, touch or are less than mergeGap rows apart are joined; when the
// list is full, the two neighbours with the smallest gap are joined (the list then covers a few unchanged rows).
void bands_add(RowBands *b, int top, int bottom, int mergeGap);
int bands_rows(const RowBands *b);

// Rows [0, rows) of a surface whose row 0 starts at 'bits' (pitch bytes per row) that the page at 'page'
// (pageSize bytes) overlaps. false if the page lies outside those rows (guard rows, the rounded-up tail).
bool page_rows(uintptr_t page, size_t pageSize, uintptr_t bits, int pitch, int rows, int *top, int *bottom);

// all pages of a GetWriteWatch list -> bands
void bands_from_pages(RowBands *b, void *const *pages, size_t n, size_t pageSize, uintptr_t bits, int pitch, int rows,
                      int mergeGap);

// Present coalescing: the game draws a frame into the primary as a burst of writes (QEC: blits and direct
// writes, ~30 bursts a second). The renderer waits until the number of written pages has not grown for quietMs
// (the burst is over) or capMs have passed since the burst began, then copies and presents once.
struct Settle {
    bool pending;
    double first, last;   // burst began / pages last grew (ms)
    long count;           // written pages seen last time
};
enum { SETTLE_IDLE, SETTLE_WAIT, SETTLE_GO };
void settle_reset(Settle *s);
// count = pages written since the last copy (does not shrink until the caller copies). SETTLE_GO also ends the
// burst; SETTLE_WAIT sets *waitMs to the time until the next decision.
int settle_step(Settle *s, double now, long count, double quietMs, double capMs, double *waitMs);
