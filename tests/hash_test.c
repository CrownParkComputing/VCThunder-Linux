/* hash_test.c -- SHA-256, against the published vectors and against itself.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * WHY THIS EXISTS AT ALL. src/hash.c's own comment named this file for two
 * sessions while it did not exist, and that gap is precisely how a broken hash
 * went unnoticed: hash.c was in no test, so nothing here compiled it, and it
 * had been left calling a hex_digits() that was never defined and assigning
 * struct fields hash.h did not declare. Building this test IS compiling
 * hash.c -- the pattern every other test in this directory follows, and the
 * reason the pattern is worth the duplication.
 *
 * WHAT IS BEING PINNED, in the order it is likely to break:
 *
 *   1. The NIST vectors: absolute correctness of the algorithm.
 *   2. The STREAMING paths: sha256_update()'s partial-block buffer. A bug in
 *      the top-up or in the 56-mod-64 padding shows only for SOME chunk sizes,
 *      so "one call with the whole buffer" passing proves very little. Every
 *      vector is therefore also fed one byte at a time, and a second input is
 *      fed at nine different chunk sizes.
 *   3. The output contract: 64 lowercase hex digits and a NUL in 65 bytes, and
 *      nothing written past the NUL.
 *   4. The digests of the two supported executables, IF the player's own data is
 *      present. That is the comparison game_map() makes before it will execute
 *      anything, so it is the one check here that can fail on a machine where
 *      the algorithm is fine and the recorded profile is not.
 *
 * WHY THE SAME FILE ALSO BUILDS ON WINDOWS. hash.c's two halves are different
 * implementations of one API -- CNG there, FIPS 180-4 here -- and the whole
 * point is that they agree. Run the same source built by mingw and the vectors
 * below are CNG's answer to them, so agreement is proven by both passing, with
 * no cross-OS comparison logic that could itself be wrong.
 */
#include <stdio.h>
#include <string.h>

#include "../src/hash.c"

static int s_failures;
static int s_checks;

static void check(int ok, const char *what)
{
    s_checks++;
    if (!ok) {
        printf("  FAIL: %s\n", what);
        s_failures++;
    }
}

/* NIST's own examples, plus the 56-byte length that the padding note below
 * explains. Verified here against the system's independent implementation
 * before being pinned, because a vector typed from memory is a bug report
 * waiting to be trusted. */
static const struct {
    const char *text;
    const char *want;
    const char *what;
} k_vectors[] = {
    { "",
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
      "empty" },
    { "abc",
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      "abc" },
    { "abcd",
      "88d4266fd4e6338d13b845fcf289579d209c897823b9217da3e161936f031589",
      "abcd" },
    /* FIFTY-SIX BYTES IS THE INTERESTING LENGTH, and the reason it is in the
     * table rather than a fourth arbitrary string: the message is padded with
     * 0x80 and then an 8-byte big-endian bit count, and a block is 64 bytes. At
     * 56 bytes the 0x80 lands in the last block but the length does not fit, so
     * the terminator and the count end up in DIFFERENT blocks and a second
     * compression runs. `padlen = (used < 56) ? 56 - used : 120 - used` is the
     * line that decides this, and it is the line that a whole-buffer test never
     * exercises. */
    { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
      "56 bytes: padding spans two blocks" },
    { "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnh"
      "ijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
      "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1",
      "112 bytes" },
};

/* Feeds `data` through the streaming API `chunk` bytes at a time. */
static int digest_in_chunks(const unsigned char *data, size_t len,
                            size_t chunk, char out[65])
{
    sha256_t s;
    size_t off = 0;

    if (!sha256_begin(&s))
        return 0;
    while (off < len) {
        size_t n = len - off < chunk ? len - off : chunk;
        if (!sha256_update(&s, data + off, n)) {
            sha256_end(&s, out);
            return 0;
        }
        off += n;
    }
    return sha256_end(&s, out);
}

int main(void)
{
    size_t i, chunk;
    char got[65];
    char label[128];

    /* ---- 1 and 2: every vector, whole and one byte at a time ------------ */
    for (i = 0; i < sizeof k_vectors / sizeof k_vectors[0]; i++) {
        const char *text = k_vectors[i].text;
        size_t len = strlen(text);

        if (!sha256_hex(text, len, got)) {
            printf("  FAIL: sha256_hex refused %s\n", k_vectors[i].what);
            s_failures++;
        } else {
            check(strcmp(got, k_vectors[i].want) == 0, k_vectors[i].what);
            if (strcmp(got, k_vectors[i].want) != 0)
                printf("    got %s\n    want %s\n", got, k_vectors[i].want);
        }

        /* The same bytes, dribbled in. An implementation that only worked in
         * one call would pass the check above and fail here -- and would also
         * be useless, since game_map() hashes a multi-megabyte image a read at
         * a time. */
        memset(got, 0, sizeof got);
        if (!digest_in_chunks((const unsigned char *)text, len, 1, got)) {
            printf("  FAIL: streaming %s refused\n", k_vectors[i].what);
            s_failures++;
        } else if (strcmp(got, k_vectors[i].want) != 0) {
            snprintf(label, sizeof label, "%s, one byte at a time",
                     k_vectors[i].what);
            check(0, label);
            printf("    streaming %s gave %s\n", k_vectors[i].what, got);
        } else {
            snprintf(label, sizeof label, "%s, one byte at a time",
                     k_vectors[i].what);
            check(1, label);
        }
    }

    /* ---- 2b: one input, every chunk size from 1 to 70 -------------------
     *
     * 70 is deliberate: past one block, so the sizes straddle the "fill the
     * buffer, run a block, keep going" path as well as the "top up a partial
     * block" path. All of them must agree with the published 112-byte digest,
     * not merely with each other. */
    {
        const char *text = k_vectors[4].text;
        size_t len = strlen(text);
        const char *want = k_vectors[4].want;

        for (chunk = 1; chunk <= 70; chunk++) {
            if (!digest_in_chunks((const unsigned char *)text, len, chunk, got) ||
                strcmp(got, want) != 0) {
                printf("  FAIL: chunk size %u gave %s\n",
                       (unsigned)chunk, got);
                s_failures++;
            }
            s_checks++;
        }
    }

    /* ---- 2c: a million 'a's, the third NIST example ---------------------
     *
     * Generated rather than literal so the input is 15625 full blocks and the
     * only buffering exercised is the straight-through path. One byte at a
     * time would be a million calls and is already covered above. */
    {
        unsigned char block[64];
        sha256_t s;
        int round;
        const char *want =
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";

        memset(block, 'a', sizeof block);
        check(sha256_begin(&s), "begin for the million-byte case");
        for (round = 0; round < 15625; round++)
            if (!sha256_update(&s, block, sizeof block)) {
                printf("  FAIL: update refused at round %d\n", round);
                s_failures++;
                break;
            }
        if (sha256_end(&s, got))
            check(strcmp(got, want) == 0, "1,000,000 'a'");
        else
            check(0, "end for the million-byte case");
        s_checks++;
    }

    /* ---- 3: the output contract -----------------------------------------
     *
     * 64 hex digits, lowercase, a NUL at [64], and nothing written at [65].
     * The buffer is one byte longer than the API asks for and guarded with a
     * canary, because the callers pass `char digest[65]` and an off-by-one
     * here lands on whatever is next to it in the caller's frame. */
    {
        unsigned char guard = 0xa5;
        char buf[66];

        memset(buf, 0, sizeof buf);
        buf[65] = (char)guard;
        if (!sha256_hex("abc", 3, buf))
            check(0, "sha256_hex failed on a case that must work");
        check(buf[64] == '\0', "the 65th byte is the NUL");
        check(buf[65] == (char)guard, "nothing written past the NUL");
        for (i = 0; i < 64; i++) {
            int hexdigit = (buf[i] >= '0' && buf[i] <= '9') ||
                           (buf[i] >= 'a' && buf[i] <= 'f');
            if (!hexdigit) {
                printf("  FAIL: byte %u of the digest is '%c'\n",
                       (unsigned)i, buf[i]);
                s_failures++;
                break;
            }
        }
        s_checks++;
    }

    /* ---- 4: the real executables, when the player's data is here --------
     *
     * These are the digests the shipped profiles record, and the ones the
     * README promises the importer checks BEFORE writing anything. Skipping is
     * reported rather than silent: a check that did not run must never read as
     * one that passed. */
    {
        static const struct { const char *path; const char *want; } k_files[] = {
            { "data/hydro/HYDRO.EXE",
              "a421f5fd4ee0d90f57815975d4a47bb784f2b46d48085e42b422605a029f8e3f" },
            { "data/offroad/offroad3.exe",
              "fa739d0f1a80c6e429ca73fdee04c3d6d6325bb95848cb701114f74bf2a5e9bf" },
        };
        int present = 0;

        for (i = 0; i < sizeof k_files / sizeof k_files[0]; i++) {
            FILE *f = fopen(k_files[i].path, "rb");
            unsigned char buf[65536];
            size_t n;
            sha256_t s;

            if (!f)
                continue;
            present++;
            sha256_begin(&s);
            /* Hashed the way the loader will: a read at a time, through the
             * streaming API, never the whole image in memory. */
            while ((n = fread(buf, 1, sizeof buf, f)) > 0)
                sha256_update(&s, buf, n);
            fclose(f);
            if (sha256_end(&s, got))
                check(strcmp(got, k_files[i].want) == 0, k_files[i].path);
            else
                check(0, k_files[i].path);
            if (strcmp(got, k_files[i].want) != 0)
                printf("    got  %s\n    want %s\n", got, k_files[i].want);
        }
        if (!present)
            printf("  note: no game data here, so the two executable digests "
                   "were NOT checked\n");
        s_checks += (unsigned)present;
    }

    if (s_failures) {
        printf("hash_test: %d of %d checks FAILED\n", s_failures, s_checks);
        return 1;
    }
    printf("hash_test: %d checks, all passed\n", s_checks);
    return 0;
}
