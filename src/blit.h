// Q150: copy kernels for the blits without colour key (QEC copies a full 2560x1440x16 surface ~63 times a second:
// 7.4 MB each, far more than the CPU caches hold). Window-free and free of the DLL's globals so tests\blittest.cpp
// can check them and tests\blitbench.cpp can time them.
#pragma once
#include <stddef.h>

enum BlitMethod {
    BLIT_CRT,      // memmove per row (the Q149 way)
    BLIT_SSE2,     // 16-byte loads / stores
    BLIT_SSE2_NT,  // 16-byte loads, non-temporal stores (no read-for-ownership of the destination, cache bypassed)
    BLIT_AVX_NT,   // 32-byte loads, non-temporal stores (needs AVX; falls back to SSE2_NT)
    BLIT_MOVSB,    // rep movsb
    BLIT_COUNT
};
const char *blit_method_name(int m);
bool blit_have_avx();   // CPU and OS support AVX (cpuid + xgetbv)

// Copies rows bytes x rowBytes from src (spitch bytes per row) to dst (dpitch). The rectangles must not overlap
// (a blit inside one surface uses memmove). Rows that are contiguous in both (pitch == rowBytes) are copied as
// one block. Non-temporal methods end with a store fence.
void blit_rows(int method, void *dst, int dpitch, const void *src, int spitch, size_t rowBytes, int rows);

// the method for a copy of 'bytes' in total: non-temporal stores only pay for copies larger than the caches; small
// ones stay in the cache for whoever reads them next (QEC: cameos, the sidebar)
int blit_pick(int preferred, size_t bytes, size_t ntThreshold);

// Nearest-neighbour stretch columns: map[i] = ((first + i) * srcLen) / dstLen, the source column of destination
// column (first + i) - the same formula the per-pixel loop used, computed once per blit instead of per pixel.
void stretch_map(int *map, int n, int first, int srcLen, int dstLen);
// runs of consecutive source columns (map[i + 1] == map[i] + 1): QEC's near-1:1 stretches (2560 <-> 2543) become
// a few memcpy per row. Returns the number of runs, or -1 if more than maxRuns (then copy pixel by pixel).
struct StretchRun { int dst, src, len; };
int stretch_runs(const int *map, int n, StretchRun *runs, int maxRuns);
