/* loader_fixed_test.c -- prove the Linux fixed-base loader maps the game's span.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The smoke test for the Linux half of the loader (handover item 9): the game
 * is a flat LinkLoc image with no relocations and must live at its link-time
 * base, 0x100000. On Windows src/stub.c reserves the span by basing the
 * executable there; on Linux the span is claimed with
 * mmap(MAP_FIXED_NOREPLACE). This test performs that claim against the real
 * Hydro Thunder image and proves the three things a synthetic capture cannot:
 *
 *   - the whole span (0x100000..0x71d830, the largest title's) is free in a
 *     normal 32-bit process, and MAP_FIXED_NOREPLACE returns exactly 0x100000;
 *   - the game's CODE (file offset 0x6b0) lands byte-exact at 0x1006b0; and
 *   - the fixed mapping is executable (a store-then-call round trip), which is
 *     the property the whole "copy then call" strategy depends on.
 *
 * It reads data/hydro/HYDRO.EXE and exits 77 ("SKIPPED, no game data") where
 * that file is absent, so the suite reports it as neither a pass nor a failure
 * rather than as a green that did not test anything. Image-identity checks (the
 * SHA-256 gate) belong to the loader, not to a test of the mapping primitive,
 * so only the size is pinned here.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* Hydro Thunder v01.01b, from generated/hydro_profile.c. Pinned verbatim rather
 * than read from the profile so a drift between the two is a visible failure
 * rather than a silent agreement. */
#define IMAGE_BASE    0x00100000u
#define SPAN_END      0x0071D830u   /* largest span (hydro)                 */
#define CODE_FILE_OFF 0x000006B0u
#define CODE_VA       0x001006B0u
#define CODE_SIZE     0x0019C525u
#define HYDRO_EXE     "data/hydro/HYDRO.EXE"
#define HYDRO_SIZE    2239936u       /* == 0x222DC0, hydro's file_size      */

static int read_all(const char *path, unsigned char **buf, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    long sz;

    if (!fp) {
        *buf = NULL;
        *len = 0;
        return 0;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (sz = ftell(fp)) < 0) {
        fclose(fp);
        return 0;
    }
    rewind(fp);
    *buf = (unsigned char *)malloc((size_t)sz);
    if (!*buf) {
        fclose(fp);
        return 0;
    }
    if (fread(*buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(*buf);
        *buf = NULL;
        fclose(fp);
        return 0;
    }
    fclose(fp);
    *len = (size_t)sz;
    return 1;
}

int main(void)
{
    const size_t span = SPAN_END - IMAGE_BASE;
    unsigned char *file = NULL;
    size_t len = 0;
    void *p;
    int fail = 0;

    if (!read_all(HYDRO_EXE, &file, &len)) {
        printf("SKIPPED, no game data at %s (item 9 loader smoke test did not "
               "run)\n", HYDRO_EXE);
        return 77;
    }
    if (len != HYDRO_SIZE) {
        printf("FAIL: %s is %zu bytes, expected %u\n", HYDRO_EXE, len,
               HYDRO_SIZE);
        free(file);
        return 1;
    }

    /* MAP_FIXED_NOREPLACE claims exactly the span or refuses (EEXIST) rather
     * than silently clobbering whatever the process already mapped there. */
    p = mmap((void *)(uintptr_t)IMAGE_BASE, span,
             PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) {
        printf("FAIL: mmap(0x%X, %u KB, RWX, MAP_FIXED_NOREPLACE): %s\n",
               IMAGE_BASE, (unsigned)(span / 1024), strerror(errno));
        free(file);
        return 1;
    }
    if ((uintptr_t)p != IMAGE_BASE) {
        printf("FAIL: mmap returned %p, not 0x%X\n", p, IMAGE_BASE);
        munmap(p, span);
        free(file);
        return 1;
    }

    /* Exactly what loader.c's Linux branch does: clear from the first CODE byte
     * and copy CODE in at its link-time address. */
    memset((void *)(uintptr_t)CODE_VA, 0, SPAN_END - CODE_VA);
    memcpy((void *)(uintptr_t)CODE_VA, file + CODE_FILE_OFF, CODE_SIZE);
    if (memcmp((void *)(uintptr_t)CODE_VA, file + CODE_FILE_OFF, CODE_SIZE)) {
        printf("FAIL: CODE did not copy byte-exact into 0x%X\n", CODE_VA);
        fail = 1;
    }

    /* Prove the fixed mapping is executable: store a `ret` below CODE (the
     * zeroed region, not game bytes) and call it. x86's instruction cache is
     * coherent, so no explicit flush is needed. */
    {
        unsigned char *code = (unsigned char *)(uintptr_t)IMAGE_BASE;
        void (*fn)(void) = (void (*)(void))(uintptr_t)IMAGE_BASE;

        code[0] = 0xC3;                 /* ret */
        fn();
    }

    munmap(p, span);
    free(file);

    if (fail)
        return 1;
    printf("span 0x%X..0x%X mapped RWX at its fixed base; CODE byte-exact; "
           "mapping executable\n", IMAGE_BASE, SPAN_END);
    return 0;
}
