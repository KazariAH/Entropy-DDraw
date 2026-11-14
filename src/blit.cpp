// Q150: copy kernels (see blit.h)
#include "blit.h"
#include <string.h>
#include <stdint.h>
#include <intrin.h>
#include <emmintrin.h>
#include <immintrin.h>

const char *blit_method_name(int m)
{
    static const char *const n[BLIT_COUNT] = {"crt-memmove", "sse2", "sse2-nt", "avx-nt", "rep-movsb"};
    return m >= 0 && m < BLIT_COUNT ? n[m] : "?";
}

bool blit_have_avx()
{
    static int have = -1;
    if (have < 0) {
        int r[4];
        __cpuid(r, 1);
        bool osxsave = (r[2] & (1 << 27)) != 0, avx = (r[2] & (1 << 28)) != 0;
        int h = 0;
        if (osxsave && avx)
            h = (_xgetbv(0) & 6) == 6 ? 1 : 0;   // the OS saves XMM and YMM state
        have = h;
    }
    return have == 1;
}

// bytes until p is aligned to a (power of two), at most n
static size_t head_bytes(const void *p, size_t a, size_t n)
{
    size_t h = (size_t)(-(intptr_t)p) & (a - 1);
    return h < n ? h : n;
}

static void copy_sse2(unsigned char *d, const unsigned char *s, size_t n)
{
    size_t h = head_bytes(d, 16, n);
    memcpy(d, s, h);
    d += h; s += h; n -= h;
    for (; n >= 64; n -= 64, d += 64, s += 64) {
        __m128i a = _mm_loadu_si128((const __m128i *)s), b = _mm_loadu_si128((const __m128i *)(s + 16));
        __m128i c = _mm_loadu_si128((const __m128i *)(s + 32)), e = _mm_loadu_si128((const __m128i *)(s + 48));
        _mm_store_si128((__m128i *)d, a);
        _mm_store_si128((__m128i *)(d + 16), b);
        _mm_store_si128((__m128i *)(d + 32), c);
        _mm_store_si128((__m128i *)(d + 48), e);
    }
    memcpy(d, s, n);
}

static void copy_sse2_nt(unsigned char *d, const unsigned char *s, size_t n)
{
    size_t h = head_bytes(d, 16, n);
    memcpy(d, s, h);
    d += h; s += h; n -= h;
    for (; n >= 64; n -= 64, d += 64, s += 64) {
        _mm_prefetch((const char *)s + 512, _MM_HINT_NTA);
        __m128i a = _mm_loadu_si128((const __m128i *)s), b = _mm_loadu_si128((const __m128i *)(s + 16));
        __m128i c = _mm_loadu_si128((const __m128i *)(s + 32)), e = _mm_loadu_si128((const __m128i *)(s + 48));
        _mm_stream_si128((__m128i *)d, a);
        _mm_stream_si128((__m128i *)(d + 16), b);
        _mm_stream_si128((__m128i *)(d + 32), c);
        _mm_stream_si128((__m128i *)(d + 48), e);
    }
    memcpy(d, s, n);
}

static void copy_avx_nt(unsigned char *d, const unsigned char *s, size_t n)
{
    size_t h = head_bytes(d, 32, n);
    memcpy(d, s, h);
    d += h; s += h; n -= h;
    for (; n >= 128; n -= 128, d += 128, s += 128) {
        _mm_prefetch((const char *)s + 1024, _MM_HINT_NTA);
        __m256i a = _mm256_loadu_si256((const __m256i *)s), b = _mm256_loadu_si256((const __m256i *)(s + 32));
        __m256i c = _mm256_loadu_si256((const __m256i *)(s + 64)), e = _mm256_loadu_si256((const __m256i *)(s + 96));
        _mm256_stream_si256((__m256i *)d, a);
        _mm256_stream_si256((__m256i *)(d + 32), b);
        _mm256_stream_si256((__m256i *)(d + 64), c);
        _mm256_stream_si256((__m256i *)(d + 96), e);
    }
    _mm256_zeroupper();
    memcpy(d, s, n);
}

static void copy_one(int method, unsigned char *d, const unsigned char *s, size_t n)
{
    switch (method) {
    case BLIT_SSE2: copy_sse2(d, s, n); break;
    case BLIT_SSE2_NT: copy_sse2_nt(d, s, n); break;
    case BLIT_AVX_NT: copy_avx_nt(d, s, n); break;
    case BLIT_MOVSB: __movsb(d, s, n); break;
    default: memmove(d, s, n); break;
    }
}

void blit_rows(int method, void *dst, int dpitch, const void *src, int spitch, size_t rowBytes, int rows)
{
    if (rows <= 0 || !rowBytes)
        return;
    if (method == BLIT_AVX_NT && !blit_have_avx())
        method = BLIT_SSE2_NT;
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if ((size_t)dpitch == rowBytes && (size_t)spitch == rowBytes) {
        copy_one(method, d, s, rowBytes * (size_t)rows);   // contiguous: one block
    } else {
        for (int y = 0; y < rows; y++)
            copy_one(method, d + (ptrdiff_t)y * dpitch, s + (ptrdiff_t)y * spitch, rowBytes);
    }
    if (method == BLIT_SSE2_NT || method == BLIT_AVX_NT)
        _mm_sfence();   // the streamed stores are visible to other threads (the renderer) before Blt returns
}

int blit_pick(int preferred, size_t bytes, size_t ntThreshold)
{
    if ((preferred == BLIT_SSE2_NT || preferred == BLIT_AVX_NT) && bytes < ntThreshold)
        return BLIT_CRT;
    return preferred;
}

void stretch_map(int *map, int n, int first, int srcLen, int dstLen)
{
    // incremental form of ((first + i) * srcLen) / dstLen: quotient and remainder carried along
    long long num = (long long)first * srcLen;
    int q = (int)(num / dstLen), r = (int)(num % dstLen);
    int stepQ = srcLen / dstLen, stepR = srcLen % dstLen;
    for (int i = 0; i < n; i++) {
        map[i] = q;
        q += stepQ;
        r += stepR;
        if (r >= dstLen) { r -= dstLen; q++; }
    }
}

int stretch_runs(const int *map, int n, StretchRun *runs, int maxRuns)
{
    int k = 0;
    for (int i = 0; i < n;) {
        int j = i + 1;
        while (j < n && map[j] == map[j - 1] + 1) j++;
        if (k == maxRuns) return -1;
        runs[k].dst = i;
        runs[k].src = map[i];
        runs[k].len = j - i;
        k++;
        i = j;
    }
    return k;
}
