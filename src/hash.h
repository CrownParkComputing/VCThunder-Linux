/* hash.h -- SHA-256: Windows' own CNG provider, and FIPS 180-4 elsewhere.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The one implementation: the loader hashes the image it is about to run, the
 * importer hashes the executable it finds inside a CHD and the one it wrote.
 *
 * The state is TWO shapes, not one, because the two halves of hash.c are
 * genuinely different algorithms with nothing in common. They were not separate
 * before this file gained the portable branch, and the Windows struct's two
 * pointers are still exactly what CNG needs. The expected agreement between the
 * two is the point of tests/hash_test.c: a digest that differed per OS would
 * make every saved title refuse to load on one of them. */
#ifndef VCT_HASH_H
#define VCT_HASH_H

#include <stddef.h>
#include <stdint.h>


typedef struct {
    uint32_t h[8];   /* the running chain value */
    uint64_t len;    /* total bytes hashed, for the 8-byte length at the end */
    size_t used;     /* bytes held in buf, 0..63 */
    unsigned char buf[64];
} sha256_t;


int sha256_begin(sha256_t *s);
int sha256_update(sha256_t *s, const void *data, size_t len);
/* Writes 64 lowercase hex digits and a NUL, and releases s either way. */
int sha256_end(sha256_t *s, char out[65]);
int sha256_hex(const void *data, size_t len, char out[65]);

#endif
