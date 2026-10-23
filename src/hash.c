/* hash.c -- SHA-256.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Windows hashes through its own CNG provider. Linux has none, so the portable
 * implementation below is used there instead -- and it is not optional polish:
 * game_map() REFUSES to run an image whose digest does not match the title's
 * profile, so without this there is no loader at all on Linux.
 *
 * The two implementations are behind one API and are expected to agree exactly;
 * tests/hash_test.c pins both against the FIPS 180-4 vectors and against each
 * other, because a digest that differs between the OSes would silently make
 * every saved title refuse to load on one of them.
 */
#include "hash.h"

/* One digit of the hex the API returns. Both halves below end by formatting a
 * digest this way, and the whole point of the two implementations is that a
 * caller cannot tell them apart -- not from the digest either, so the formatting
 * is written once. It is still one definition in each branch rather than one
 * above `#ifdef _WIN32`: a file-scope `static` that only the OTHER branch calls
 * would be an unused-function warning at -Werror on every real build. */

#include <string.h>

static char hex_of(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    return hex[v & 15u];
}

/* FIPS 180-4. Kept self-contained rather than reaching for a library: this is
 * the digest a game image is VERIFIED against before it is allowed to execute,
 * so it is worth being able to read end to end and audit. */
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
    0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
    0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
    0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const unsigned char *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, hh, t1, t2;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    for (; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    a = h[0]; b = h[1]; c = h[2]; d = h[3];
    e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        t1 = hh + S1 + ch + K[i] + w[i];
        t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

int sha256_begin(sha256_t *s)
{
    s->h[0] = 0x6a09e667; s->h[1] = 0xbb67ae85;
    s->h[2] = 0x3c6ef372; s->h[3] = 0xa54ff53a;
    s->h[4] = 0x510e527f; s->h[5] = 0x9b05688c;
    s->h[6] = 0x1f83d9ab; s->h[7] = 0x5be0cd19;
    s->len = 0;
    s->used = 0;
    return 1;
}

int sha256_update(sha256_t *s, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;

    s->len += len;
    /* Top up a partial block first, then run whole blocks straight from the
     * caller's buffer. Hashing a multi-megabyte image in 64-byte copies would
     * be correct and needlessly slow. */
    if (s->used) {
        size_t need = 64 - s->used;
        size_t take = len < need ? len : need;
        memcpy(s->buf + s->used, p, take);
        s->used += take; p += take; len -= take;
        if (s->used == 64) { sha256_block(s->h, s->buf); s->used = 0; }
    }
    while (len >= 64) { sha256_block(s->h, p); p += 64; len -= 64; }
    if (len) { memcpy(s->buf, p, len); s->used = len; }
    return 1;
}

int sha256_end(sha256_t *s, char out[65])
{
    unsigned char pad[72];
    uint64_t bits = (uint64_t)s->len * 8u;
    size_t padlen;
    int i;

    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    /* Pad to 56 mod 64, then 8 bytes of big-endian length. */
    padlen = (s->used < 56) ? (56 - s->used) : (120 - s->used);
    sha256_update(s, pad, padlen);
    for (i = 0; i < 8; i++)
        pad[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha256_update(s, pad, 8);

    /* Each state word is 32 bits = EIGHT hex digits, and the digest is the
     * eight words concatenated big-endian: 64 characters, then the NUL.
     * Written as a loop over the nibbles instead of the eight open-coded lines
     * that were here, because those lines were wrong twice at once: they wrote
     * four digits per word at a stride of eight, so half the output buffer was
     * never touched, and their shift sequence 24,16,8,0 skipped every odd
     * nibble. `hex_of()` keeps only the low four bits, so a missing `& 15` is
     * not the failure mode -- a missing shift is. */
    for (i = 0; i < 8; i++) {
        int shift;
        for (shift = 28; shift >= 0; shift -= 4)
            out[8 * i + (28 - shift) / 4] = hex_of(s->h[i] >> (unsigned)shift);
    }
    out[64] = 0;
    return 1;
}


int sha256_hex(const void *data, size_t len, char out[65])
{
    sha256_t s;
    if (!sha256_begin(&s))
        return 0;
    if (!sha256_update(&s, data, len)) {
        sha256_end(&s, out);
        out[0] = 0;
        return 0;
    }
    return sha256_end(&s, out);
}
