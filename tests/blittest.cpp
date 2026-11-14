// Q150: checks of the copy kernels (src\blit.cpp): every method copies exactly the rectangle - all alignments,
// widths, pitches, contiguous blocks - and writes nothing outside it.
#include "../src/blit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_pass, s_fail;
static void check(bool ok, const char *what)
{
    if (ok) s_pass++;
    else { s_fail++; printf("FAIL: %s\n", what); }
}

static bool one_case(int method, int dOff, int sOff, int rowBytes, int rows, int dpitch, int spitch)
{
    size_t dsz = (size_t)dpitch * rows + 256, ssz = (size_t)spitch * rows + 256;
    unsigned char *d = (unsigned char *)malloc(dsz), *ref = (unsigned char *)malloc(dsz);
    unsigned char *s = (unsigned char *)malloc(ssz);
    for (size_t i = 0; i < ssz; i++) s[i] = (unsigned char)(rand() ^ (i * 7));
    for (size_t i = 0; i < dsz; i++) d[i] = ref[i] = (unsigned char)(0xA5 ^ i);
    for (int y = 0; y < rows; y++)
        memcpy(ref + dOff + (size_t)y * dpitch, s + sOff + (size_t)y * spitch, rowBytes);
    blit_rows(method, d + dOff, dpitch, s + sOff, spitch, rowBytes, rows);
    bool ok = memcmp(d, ref, dsz) == 0;
    free(d);
    free(ref);
    free(s);
    return ok;
}

int main()
{
    srand(150);
    for (int m = 0; m < BLIT_COUNT; m++) {
        bool ok = true;
        for (int k = 0; k < 3000 && ok; k++) {
            int rowBytes = 1 + rand() % (k % 10 == 0 ? 6000 : 300);
            int rows = 1 + rand() % 12;
            int dOff = rand() % 64, sOff = rand() % 64;
            int dpitch = rowBytes + (rand() % 3 == 0 ? 0 : rand() % 100);
            int spitch = rowBytes + (rand() % 3 == 0 ? 0 : rand() % 100);
            ok = one_case(m, dOff, sOff, rowBytes, rows, dpitch, spitch);
        }
        char what[96];
        sprintf_s(what, "%s: 3000 random rectangles copied exactly, nothing around them touched", blit_method_name(m));
        check(ok, what);
        // QEC's case: a full 2560x1440x16 surface, contiguous rows, page aligned
        sprintf_s(what, "%s: full 2560x1440x16 surface", blit_method_name(m));
        check(one_case(m, 0, 0, 5120, 1440, 5120, 5120), what);
    }
    // stretch columns: the table equals the per-pixel formula; runs cover the row exactly
    {
        static int map[6000];
        static StretchRun runs[6000];
        bool ok = true, runsOk = true;
        for (int k = 0; k < 4000 && ok && runsOk; k++) {
            int dstLen = 1 + rand() % 3000, srcLen = 1 + rand() % 3000;
            if (k % 4 == 0) { dstLen = 2543 + rand() % 40; srcLen = 2560; }
            int first = rand() % dstLen, n = 1 + rand() % (dstLen - first);
            stretch_map(map, n, first, srcLen, dstLen);
            for (int i = 0; i < n && ok; i++)
                if (map[i] != (int)(((long long)(first + i) * srcLen) / dstLen)) ok = false;
            int nr = stretch_runs(map, n, runs, 6000);
            int at = 0;
            for (int r = 0; r < nr && runsOk; r++) {
                if (runs[r].dst != at || runs[r].len <= 0) runsOk = false;
                for (int i = 0; i < runs[r].len && runsOk; i++)
                    if (map[at + i] != runs[r].src + i) runsOk = false;
                at += runs[r].len;
            }
            if (nr < 0 || at != n) runsOk = false;
        }
        check(ok, "stretch_map equals ((first + i) * srcLen) / dstLen for 4000 random mappings");
        check(runsOk, "stretch_runs cover the row exactly with consecutive source columns");
        stretch_map(map, 2543, 0, 2560, 2543);
        int nr = stretch_runs(map, 2543, runs, 6000);
        check(nr > 0 && nr <= 20, "2560 -> 2543: at most 20 runs per row");
        check(stretch_runs(map, 2543, runs, 5) == -1, "too many runs: -1");
    }
    check(blit_pick(BLIT_SSE2_NT, 1000, 262144) == BLIT_CRT, "small copies keep cached stores");
    check(blit_pick(BLIT_SSE2_NT, 7372800, 262144) == BLIT_SSE2_NT, "large copies stream");
    check(blit_pick(BLIT_SSE2, 10, 262144) == BLIT_SSE2, "non-streaming methods are kept");
    printf("blittest: %d passed, %d failed (AVX %s)\n", s_pass, s_fail, blit_have_avx() ? "available" : "not available");
    return s_fail ? 1 : 0;
}
