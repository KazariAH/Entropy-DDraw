// Q149: write-watch pages -> row bands, and present coalescing (see frameplan.h)
#include "frameplan.h"

void bands_clear(RowBands *b)
{
    b->n = 0;
}

int bands_rows(const RowBands *b)
{
    int r = 0;
    for (int i = 0; i < b->n; i++)
        r += b->bottom[i] - b->top[i];
    return r;
}

// joins band i with band i + 1
static void join_next(RowBands *b, int i)
{
    if (b->bottom[i + 1] > b->bottom[i])
        b->bottom[i] = b->bottom[i + 1];
    for (int k = i + 1; k + 1 < b->n; k++) {
        b->top[k] = b->top[k + 1];
        b->bottom[k] = b->bottom[k + 1];
    }
    b->n--;
}

void bands_add(RowBands *b, int top, int bottom, int mergeGap)
{
    if (bottom <= top)
        return;
    if (mergeGap < 0)
        mergeGap = 0;
    // insert sorted by top
    int at = 0;
    while (at < b->n && b->top[at] < top)
        at++;
    if (b->n == BANDS_MAX) {
        // full: make room by joining the closest neighbours (or swallow the new range into its neighbour)
        int best = -1, bestGap = 0;
        for (int i = 0; i + 1 < b->n; i++) {
            int gap = b->top[i + 1] - b->bottom[i];
            if (best < 0 || gap < bestGap) { best = i; bestGap = gap; }
        }
        join_next(b, best);   // removes index best + 1
        if (at > best + 1)
            at--;
    }
    for (int k = b->n; k > at; k--) {
        b->top[k] = b->top[k - 1];
        b->bottom[k] = b->bottom[k - 1];
    }
    b->top[at] = top;
    b->bottom[at] = bottom;
    b->n++;
    // join with the neighbours that overlap or are closer than mergeGap
    int i = at > 0 ? at - 1 : 0;
    while (i + 1 < b->n) {
        if (b->top[i + 1] - b->bottom[i] <= mergeGap)
            join_next(b, i);
        else if (i + 1 > at)
            break;
        else
            i++;
    }
}

bool page_rows(uintptr_t page, size_t pageSize, uintptr_t bits, int pitch, int rows, int *top, int *bottom)
{
    if (pitch <= 0 || rows <= 0)
        return false;
    uintptr_t end = page + pageSize;
    uintptr_t last = bits + (uintptr_t)pitch * (uintptr_t)rows;
    if (end <= bits || page >= last)
        return false;
    uintptr_t from = page > bits ? page - bits : 0;
    uintptr_t to = (end < last ? end : last) - bits;   // exclusive
    *top = (int)(from / (uintptr_t)pitch);
    *bottom = (int)((to + (uintptr_t)pitch - 1) / (uintptr_t)pitch);
    return true;
}

void bands_from_pages(RowBands *b, void *const *pages, size_t n, size_t pageSize, uintptr_t bits, int pitch, int rows,
                      int mergeGap)
{
    // GetWriteWatch lists pages in ascending order: join runs of rows before adding them
    int runTop = -1, runBottom = -1;
    for (size_t i = 0; i < n; i++) {
        int t, bo;
        if (!page_rows((uintptr_t)pages[i], pageSize, bits, pitch, rows, &t, &bo))
            continue;
        if (runTop >= 0 && t <= runBottom) {
            if (bo > runBottom) runBottom = bo;
            continue;
        }
        if (runTop >= 0)
            bands_add(b, runTop, runBottom, mergeGap);
        runTop = t;
        runBottom = bo;
    }
    if (runTop >= 0)
        bands_add(b, runTop, runBottom, mergeGap);
}

void settle_reset(Settle *s)
{
    s->pending = false;
    s->first = s->last = 0;
    s->count = 0;
}

int settle_step(Settle *s, double now, long count, double quietMs, double capMs, double *waitMs)
{
    *waitMs = 0;
    if (count <= 0) {
        if (!s->pending)
            return SETTLE_IDLE;
        settle_reset(s);   // the caller copied in between (count went back to 0): nothing left to wait for
        return SETTLE_IDLE;
    }
    if (!s->pending) {
        s->pending = true;
        s->first = s->last = now;
        s->count = count;
    } else if (count != s->count) {
        s->count = count;
        s->last = now;
    }
    double quietLeft = quietMs - (now - s->last), capLeft = capMs - (now - s->first);
    if (quietMs <= 0 || quietLeft <= 0 || capLeft <= 0) {
        settle_reset(s);
        return SETTLE_GO;
    }
    *waitMs = quietLeft < capLeft ? quietLeft : capLeft;
    return SETTLE_WAIT;
}
