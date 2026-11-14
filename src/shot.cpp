// Screenshot file writer: 24-bit PNG with a small built-in deflate encoder (LZ77 over a 32 KB window with
// hash chains, the fixed Huffman code of RFC 1951). No zlib / GDI+ / WIC - the import table stays as it is.
// Game pictures (flat areas, repeated tiles) come out at a fraction of a BMP's size.
#include "qdd.h"
#include <stdlib.h>
#include <string.h>

// --- growing byte buffer with an LSB-first bit writer -----------------------------------------------

struct Out {
    BYTE *p;
    size_t len, cap;
    DWORD bits;
    int nbits;
    bool failed;
};

static void out_byte(Out &o, BYTE b)
{
    if (o.len == o.cap) {
        size_t cap = o.cap ? o.cap * 2 : 1 << 20;
        BYTE *p = (BYTE *)realloc(o.p, cap);
        if (!p) {
            o.failed = true;
            return;
        }
        o.p = p;
        o.cap = cap;
    }
    o.p[o.len++] = b;
}

static void out_be32(Out &o, DWORD v)
{
    out_byte(o, (BYTE)(v >> 24));
    out_byte(o, (BYTE)(v >> 16));
    out_byte(o, (BYTE)(v >> 8));
    out_byte(o, (BYTE)v);
}

static void put_bits(Out &o, DWORD v, int n)
{
    o.bits |= v << o.nbits;
    o.nbits += n;
    while (o.nbits >= 8) {
        out_byte(o, (BYTE)o.bits);
        o.bits >>= 8;
        o.nbits -= 8;
    }
}

static void flush_bits(Out &o)
{
    if (o.nbits > 0)
        out_byte(o, (BYTE)o.bits);
    o.bits = 0;
    o.nbits = 0;
}

// Huffman codes are sent most significant bit first
static void put_code(Out &o, DWORD code, int len)
{
    DWORD r = 0;
    for (int i = 0; i < len; i++)
        r |= ((code >> i) & 1) << (len - 1 - i);
    put_bits(o, r, len);
}

// --- fixed Huffman symbols ------------------------------------------------------------------------------

static void put_litlen(Out &o, int sym)
{
    if (sym < 144) put_code(o, 0x30 + sym, 8);
    else if (sym < 256) put_code(o, 0x190 + sym - 144, 9);
    else if (sym < 280) put_code(o, sym - 256, 7);
    else put_code(o, 0xC0 + sym - 280, 8);
}

static int high_bit(unsigned v)
{
    int b = 0;
    while (v >>= 1) b++;
    return b;
}

// Length 3..258 and distance 1..32768 as symbol + extra bits. Past the first codes every symbol covers
// a power-of-two range, four length symbols / two distance symbols per extra-bit count.
static void put_match(Out &o, int len, int dist)
{
    unsigned v = (unsigned)(len - 3);
    if (len == 258) {
        put_litlen(o, 285);
    } else if (v < 8) {
        put_litlen(o, 257 + v);
    } else {
        int e = high_bit(v) - 2;
        put_litlen(o, 257 + 4 * e + (v >> e));
        put_bits(o, v & ((1u << e) - 1), e);
    }
    v = (unsigned)(dist - 1);
    if (v < 4) {
        put_code(o, v, 5);
    } else {
        int e = high_bit(v) - 1;
        put_code(o, 2 * e + (v >> e), 5);
        put_bits(o, v & ((1u << e) - 1), e);
    }
}

// --- deflate (one final block, fixed codes) -----------------------------------------------------------

static const int WINDOW = 32768, HASH_BITS = 15, MAX_CHAIN = 24, MIN_MATCH = 3, MAX_MATCH = 258;

static bool deflate_fixed(Out &o, const BYTE *in, size_t n)
{
    int *head = (int *)malloc(sizeof(int) << HASH_BITS);
    int *prev = (int *)malloc(sizeof(int) * WINDOW);
    if (!head || !prev) {
        free(head);
        free(prev);
        return false;
    }
    memset(head, 0xFF, sizeof(int) << HASH_BITS);   // -1: empty
    put_bits(o, 1, 1);   // BFINAL
    put_bits(o, 1, 2);   // BTYPE = fixed Huffman
    auto hash = [&](size_t i) { return (DWORD)((in[i] << 10) ^ (in[i + 1] << 5) ^ in[i + 2]) & ((1u << HASH_BITS) - 1); };
    auto insert = [&](size_t i) {
        DWORD h = hash(i);
        prev[i & (WINDOW - 1)] = head[h];
        head[h] = (int)i;
    };
    size_t i = 0;
    while (i < n) {
        int best = 0, bestDist = 0;
        if (i + MIN_MATCH <= n) {
            int cand = head[hash(i)];
            size_t maxLen = n - i < (size_t)MAX_MATCH ? n - i : MAX_MATCH;
            for (int chain = 0; cand >= 0 && i - (size_t)cand <= (size_t)WINDOW && chain < MAX_CHAIN; chain++) {
                const BYTE *a = in + cand, *b = in + i;
                if (a[best] == b[best]) {
                    int l = 0;
                    while ((size_t)l < maxLen && a[l] == b[l]) l++;
                    if (l > best) {
                        best = l;
                        bestDist = (int)(i - cand);
                        if ((size_t)l == maxLen) break;
                    }
                }
                int nx = prev[cand & (WINDOW - 1)];
                if (nx >= cand) break;   // slot reused by a newer position
                cand = nx;
            }
            insert(i);
        }
        if (best >= MIN_MATCH) {
            put_match(o, best, bestDist);
            for (size_t k = i + 1; k < i + best; k++)
                if (k + MIN_MATCH <= n) insert(k);
            i += best;
        } else {
            put_litlen(o, in[i]);
            i++;
        }
    }
    put_litlen(o, 256);   // end of block
    flush_bits(o);
    free(head);
    free(prev);
    return !o.failed;
}

// --- PNG container --------------------------------------------------------------------------------------

static DWORD s_crcTable[256];

static DWORD crc32(DWORD crc, const BYTE *p, size_t n)
{
    if (!s_crcTable[1])
        for (DWORD i = 0; i < 256; i++) {
            DWORD c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            s_crcTable[i] = c;
        }
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
        crc = s_crcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void chunk(Out &o, const char *type, const BYTE *data, size_t n)
{
    out_be32(o, (DWORD)n);
    size_t start = o.len;
    for (int i = 0; i < 4; i++) out_byte(o, (BYTE)type[i]);
    for (size_t i = 0; i < n; i++) out_byte(o, data[i]);
    if (!o.failed)
        out_be32(o, crc32(0, o.p + start, o.len - start));
}

bool png_write(const char *path, const DWORD *rgba, int w, int h)
{
    if (w <= 0 || h <= 0)
        return false;
    // raw scanlines: filter byte 0 + RGB
    size_t rowLen = (size_t)w * 3 + 1, rawLen = rowLen * h;
    BYTE *raw = (BYTE *)malloc(rawLen);
    if (!raw)
        return false;
    for (int y = 0; y < h; y++) {
        BYTE *d = raw + rowLen * y;
        *d++ = 0;
        const DWORD *s = rgba + (size_t)w * y;
        for (int x = 0; x < w; x++) {
            *d++ = (BYTE)s[x];
            *d++ = (BYTE)(s[x] >> 8);
            *d++ = (BYTE)(s[x] >> 16);
        }
    }
    // zlib stream: header, deflate data, Adler-32 of the raw bytes
    Out z = {};
    out_byte(z, 0x78);
    out_byte(z, 0x01);
    bool ok = deflate_fixed(z, raw, rawLen);
    DWORD a = 1, b = 0;
    for (size_t i = 0; i < rawLen; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    out_be32(z, (b << 16) | a);
    free(raw);

    Out f = {};
    static const BYTE sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    for (int i = 0; i < 8; i++) out_byte(f, sig[i]);
    BYTE ihdr[13] = {(BYTE)(w >> 24), (BYTE)(w >> 16), (BYTE)(w >> 8), (BYTE)w, (BYTE)(h >> 24), (BYTE)(h >> 16),
                     (BYTE)(h >> 8), (BYTE)h, 8, 2 /* RGB */, 0, 0, 0};
    chunk(f, "IHDR", ihdr, sizeof(ihdr));
    if (ok && !z.failed)
        chunk(f, "IDAT", z.p, z.len);
    chunk(f, "IEND", NULL, 0);
    ok = ok && !z.failed && !f.failed;
    free(z.p);
    if (ok) {
        HANDLE fh = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD wr = 0;
        ok = fh != INVALID_HANDLE_VALUE && WriteFile(fh, f.p, (DWORD)f.len, &wr, NULL) && wr == f.len;
        if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
    }
    free(f.p);
    return ok;
}
