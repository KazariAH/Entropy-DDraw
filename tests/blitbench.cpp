// Q150: offline timing of the copy kernels for QEC's full-screen blit (2560x1440x16 = 7.4 MB, contiguous rows,
// page-aligned surfaces). No window, no game. Scenarios:
//   cold      - caches flushed before each copy (the source was drawn long ago)
//   drawn     - the game has just written the source (its tail is in the cache), as before QEC's blit
//   renderer  - "drawn", and after each blit a second thread on the same CCD copies the destination to a shadow
//   otherCCD  - the same with that thread on the other CCD (L3 of its own)
// Variants beyond the kernels: two threads sharing one copy (wall time = what the game waits), and compare-before-store
// (only 64-byte blocks that differ are written) at several fractions of changed rows.
#include "../src/blit.h"
#include <windows.h>
#include <emmintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

static const int W = 2560, H = 1440, PITCH = W * 2;
static const size_t BYTES = (size_t)PITCH * H;
static unsigned char *g_src, *g_dst, *g_flush, *g_shadow;
static const size_t FLUSH = 64u << 20;

static double now_ms()
{
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

// a top-down 16 bpp DIB section, as the DLL allocates its surfaces
static unsigned char *dib_alloc()
{
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 16;
    void *bits = NULL;
    return CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0) ? (unsigned char *)bits : NULL;
}

static void flush_caches()
{
    for (size_t i = 0; i < FLUSH; i += 64) g_flush[i]++;
}

static void draw_source(int frame)
{
    // the game draws its frame into the source: every pixel written
    WORD v = (WORD)(frame * 2654435761u >> 16);
    WORD *p = (WORD *)g_src;
    for (size_t i = 0; i < BYTES / 2; i++) p[i] = (WORD)(v + i);
}

// --- a second thread for the split copy -------------------------------------------------------
static HANDLE g_go, g_done;
static volatile int g_method;
static volatile LONG g_quit;
static DWORD WINAPI half_worker(LPVOID)
{
    for (;;) {
        WaitForSingleObject(g_go, INFINITE);
        if (g_quit) return 0;
        blit_rows(g_method, g_dst + BYTES / 2, PITCH, g_src + BYTES / 2, PITCH, PITCH, H / 2);
        SetEvent(g_done);
    }
}
static void split_copy(int method)
{
    g_method = method;
    SetEvent(g_go);
    blit_rows(method, g_dst, PITCH, g_src, PITCH, PITCH, H / 2);
    WaitForSingleObject(g_done, INFINITE);
}

// --- the renderer: copies the destination to its shadow right after each blit (as our render thread does when
// the Blt wakes it), so the next blit writes lines another core (another CCD) has just read ------------------
static volatile LONG g_rendererOn;
static HANDLE g_rgo, g_rdone;
static DWORD WINAPI renderer(LPVOID)
{
    for (;;) {
        WaitForSingleObject(g_rgo, INFINITE);
        if (g_quit) return 0;
        memcpy(g_shadow, g_dst, BYTES);
        SetEvent(g_rdone);
    }
}

// --- compare before store ------------------------------------------------------------------------
static void copy_cmp(unsigned char *d, const unsigned char *s, size_t n)
{
    for (size_t i = 0; i + 64 <= n; i += 64) {
        __m128i a = _mm_load_si128((const __m128i *)(s + i)), b = _mm_load_si128((const __m128i *)(s + i + 16));
        __m128i c = _mm_load_si128((const __m128i *)(s + i + 32)), e = _mm_load_si128((const __m128i *)(s + i + 48));
        __m128i x = _mm_and_si128(_mm_and_si128(_mm_cmpeq_epi8(a, _mm_load_si128((const __m128i *)(d + i))),
                                                _mm_cmpeq_epi8(b, _mm_load_si128((const __m128i *)(d + i + 16)))),
                                  _mm_and_si128(_mm_cmpeq_epi8(c, _mm_load_si128((const __m128i *)(d + i + 32))),
                                                _mm_cmpeq_epi8(e, _mm_load_si128((const __m128i *)(d + i + 48)))));
        if (_mm_movemask_epi8(x) != 0xFFFF) {
            _mm_store_si128((__m128i *)(d + i), a);
            _mm_store_si128((__m128i *)(d + i + 16), b);
            _mm_store_si128((__m128i *)(d + i + 32), c);
            _mm_store_si128((__m128i *)(d + i + 48), e);
        }
    }
}

struct Result { double med, avg, p90; };
static Result summarize(double *t, int n)
{
    std::sort(t, t + n);
    double s = 0;
    for (int i = 0; i < n; i++) s += t[i];
    Result r = {t[n / 2], s / n, t[n * 9 / 10]};
    return r;
}
static void print(const char *scen, const char *name, Result r)
{
    printf("%-9s %-22s median %6.3f ms  avg %6.3f  p90 %6.3f  = %5.1f GB/s copied\n", scen, name, r.med, r.avg, r.p90,
           BYTES / 1e6 / r.med);
}

int main(int argc, char **argv)
{
    int reps = argc > 1 ? atoi(argv[1]) : 60;
    if (reps < 10) reps = 10;
    SetThreadAffinityMask(GetCurrentThread(), 1);   // keep the "game thread" on one core, the helpers elsewhere
    g_src = dib_alloc();
    g_dst = dib_alloc();
    g_shadow = (unsigned char *)VirtualAlloc(NULL, BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_flush = (unsigned char *)VirtualAlloc(NULL, FLUSH, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_src || !g_dst || !g_shadow || !g_flush) { printf("out of memory\n"); return 2; }
    memset(g_dst, 1, BYTES);
    memset(g_shadow, 1, BYTES);
    memset(g_flush, 1, FLUSH);
    g_go = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_done = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_rgo = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_rdone = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE hw = CreateThread(NULL, 0, half_worker, NULL, 0, NULL);
    HANDLE hr = CreateThread(NULL, 0, renderer, NULL, 0, NULL);
    SetThreadAffinityMask(hw, 2);
    SetThreadAffinityMask(hr, 4);
    printf("blitbench: %dx%dx16 = %.2f MB per copy, %d copies per case, AVX %s\n", W, H, BYTES / 1048576.0, reps,
           blit_have_avx() ? "yes" : "no");

    double *t = (double *)malloc(sizeof(double) * reps);
    const char *scen[4] = {"cold", "drawn", "renderer", "otherCCD"};
    for (int sc = 0; sc < 4; sc++) {
        g_rendererOn = sc >= 2;
        SetThreadAffinityMask(hr, sc == 3 ? (DWORD_PTR)1 << 16 : 4);   // the renderer on the game's CCD / the other CCD
        for (int m = 0; m <= BLIT_COUNT; m++) {   // BLIT_COUNT = two threads, sse2-nt
            for (int i = 0; i < reps; i++) {
                if (sc == 0) flush_caches();
                else draw_source(i);
                double t0 = now_ms();
                if (m == BLIT_COUNT) split_copy(BLIT_SSE2_NT);
                else blit_rows(m, g_dst, PITCH, g_src, PITCH, PITCH, H);
                t[i] = now_ms() - t0;
                if (g_rendererOn) { SetEvent(g_rgo); WaitForSingleObject(g_rdone, INFINITE); }
            }
            print(scen[sc], m == BLIT_COUNT ? "2 threads sse2-nt" : blit_method_name(m), summarize(t, reps));
        }
    }
    g_rendererOn = 0;
    // compare before store: frac of the rows changed since the last copy (destination holds the previous frame)
    const int fr[4] = {100, 50, 10, 0};
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < reps; i++) {
            memcpy(g_dst, g_src, BYTES);
            int changed = H * fr[k] / 100;
            for (int y = 0; y < changed; y++) {
                WORD *row = (WORD *)(g_src + (size_t)y * PITCH);
                for (int x = 0; x < W; x += 7) row[x]++;
            }
            flush_caches();
            double t0 = now_ms();
            copy_cmp(g_dst, g_src, BYTES);
            t[i] = now_ms() - t0;
        }
        char name[40];
        sprintf_s(name, "compare+store %d%%", fr[k]);
        print("cold", name, summarize(t, reps));
    }
    g_quit = 1;
    SetEvent(g_go);
    SetEvent(g_rgo);
    WaitForSingleObject(hw, 2000);
    WaitForSingleObject(hr, 2000);
    return 0;
}
