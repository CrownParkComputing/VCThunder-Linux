/* loader.c -- map the game at its link-time base.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The executable image already reserves the required low-address span; this
 * loader makes it writable, copies CODE, zeros the remainder, and lets
 * __pl_unpackrom materialise DATA and .bss.
 */
#include "vcthunder.h"
#include "hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPAN_BASE  G->image_base
#define SPAN_SIZE  (G->span_end - G->image_base)

/* The image the profile's addresses were derived from is identified by BOTH
 * G->file_size and G->sha256. They are an execution boundary, not metadata:
 * every address below assumes that exact build, and a plausible address in a
 * different build is more dangerous than an obvious failure. Verify before the
 * first byte of the fixed image is made writable or copied. */

static void *s_region;


static int read_all(const char *path, unsigned char **buf, size_t *len)
{
    char norm[MAX_PATH];
    FILE *fp;
    long sz;

    if (!path) {
        LOGE("cannot open '%s'", path ? path : "(null)");
        return 0;
    }
    path = posix_norm(norm, sizeof norm, path);
    fp = fopen(path, "rb");
    if (!fp) {
        LOGE("cannot open '%s'", path);
        return 0;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (sz = ftell(fp)) < 0) {
        LOGE("cannot size '%s'", path);
        fclose(fp);
        return 0;
    }
    rewind(fp);

    *buf = (unsigned char *)malloc((size_t)sz);
    if (!*buf) {
        LOGE("out of memory reading '%s' (%ld bytes)", path, sz);
        fclose(fp);
        return 0;
    }
    if (fread(*buf, 1, (size_t)sz, fp) != (size_t)sz) {
        LOGE("short read on '%s'", path);
        free(*buf);
        *buf = NULL;
        fclose(fp);
        return 0;
    }
    fclose(fp);
    *len = (size_t)sz;
    return 1;
}

int game_map(const char *exe_path)
{
    unsigned char *file = NULL;
    char actual_sha256[65];
    size_t len = 0;

    if (!read_all(exe_path, &file, &len))
        return 0;

    LOGI("loaded '%s' (%zu bytes)", exe_path, len);
    if (len != G->file_size) {
        LOGE("refusing unexpected file size %zu: %s's profile requires "
             "%u bytes (sha256 %.16s...)", len, G->id, G->file_size,
             G->sha256);
        free(file);
        return 0;
    }
    if (!sha256_hex(file, len, actual_sha256)) {
        LOGE("BCrypt SHA-256 of the image failed");
        free(file);
        return 0;
    }
    if (_stricmp(actual_sha256, G->sha256) != 0) {
        LOGE("refusing image sha256 %s: %s's profile requires %s",
             actual_sha256, G->id, G->sha256);
        free(file);
        return 0;
    }
    LOGI("verified image sha256 %s", actual_sha256);

    if (len < G->code_file_off + G->code_size) {
        LOGE("file is too small to contain CODE (need %u bytes)",
             (unsigned)(G->code_file_off + G->code_size));
        free(file);
        return 0;
    }

    /* Own the span. On Windows it is already ours as part of VCThunder.exe's
     * image (src/stub.c); on Linux the stub does not exist, so the span is
     * claimed with one fixed-address mapping. Both paths end with the span RWX:
     * CODE holds the __imp__ table and the record stream as well as
     * instructions, so it is simultaneously code, writable data and a patch
     * target. This also overwrites the stub's own headers and .text on Windows,
     * which is why nothing in the stub may be needed after this point. */
    /* The game is a flat LinkLoc image with no relocations: the address is not
     * a request, it is the only address that works. MAP_FIXED_NOREPLACE claims
     * exactly that span or fails (EEXIST) rather than silently clobbering
     * whatever the process already mapped there -- the same "refuse rather than
     * corrupt" the stub enforces on Windows. */
    {
        void *p = mmap((void *)(uintptr_t)SPAN_BASE, SPAN_SIZE,
                       PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                       -1, 0);
        if (p == MAP_FAILED) {
            LOGE("mmap(0x%08X, %u KB, RWX, MAP_FIXED_NOREPLACE) failed: %s; "
                 "something is already mapped inside the game's span",
                 SPAN_BASE, (unsigned)(SPAN_SIZE / 1024), strerror(errno));
            free(file);
            return 0;
        }
        if ((uintptr_t)p != SPAN_BASE) {
            LOGE("mmap returned %p instead of 0x%08X", p, SPAN_BASE);
            munmap(p, SPAN_SIZE);
            free(file);
            return 0;
        }
        s_region = p;
    }
    LOGI("span 0x%08X..0x%08X (%u KB) mapped RWX at its fixed base",
         SPAN_BASE, G->span_end, (unsigned)(SPAN_SIZE / 1024));

    /* Clear the span from the game's first CODE byte to its end, then copy
     * CODE in. On Windows this preserves the host PE headers below 0x6b0
     * (runtime libraries may inspect them after mapping); on Linux the region
     * below CODE is a fresh anonymous mapping that is already zero. */
    memset((void *)(uintptr_t)G->code_va, 0,
           G->span_end - G->code_va);
    memcpy((void *)(uintptr_t)G->code_va,
           file + G->code_file_off, G->code_size);
    free(file);

    LOGI("CODE  0x%08X..0x%08X copied", G->code_va,
         G->code_va + G->code_size);
    LOGI("DATA  0x%08X  .bss 0x%08X  .stack 0x%08X  (zeroed, awaiting unpack)",
         G->data_va, G->bss_va, G->stack_va);
    return 1;
}

void game_unmap(void)
{
    /* On Linux the span is an anonymous mapping this loader made; it is ours to
     * release. */
    if (s_region)
        munmap(s_region, SPAN_SIZE);
    s_region = NULL;
}

/* ---- the ETS kernel public info block -----------------------------------
 *
 * __pl_unpackrom tests every record's destination against two reserved
 * (base, length) pairs at offsets 0x44/0x48 and 0x4C/0x50 of a BY-VALUE COPY of
 * the ETS kernel public info block, and refuses the whole unpack (status 2) on
 * an intersection:
 *
 *     refuse if  (A.base <  dst+len && A.base+A.len >  dst)
 *             || (B.base <  dst+len && B.base+B.len >  dst)
 *
 * The block reaches it through _EtsGetSystemInfo, whose `int 0xFE` gate our
 * stubs answer with 0, and _EtsGetSystemInfo copies NOTHING when the pointer is
 * 0, leaving the unpacker's 0x110-byte stack buffer as it found it. The game
 * then tests all 6,578 records against host stack residue.
 *
 * A zeroed block is the truthful answer, not a placation: base 0 with length 0
 * intersects nothing, and this host reserves nothing inside the span. Anything
 * it ever does reserve there must be DECLARED in these two pairs rather than
 * hidden by zeroing them.
 *
 * Nothing here comes from an ETS header: the offsets and the comparison are
 * from the games' own disassembly of __pl_unpackrom. The remaining 0xF0 bytes
 * are unread on this path and stay zero. */
#define ETS_PUBLIC_INFO_SIZE 0x100

static uint32_t s_ets_public_info[ETS_PUBLIC_INFO_SIZE / 4];

static uint32_t __cdecl ov_ets_kernel_public_info(void)
{
    return (uint32_t)(uintptr_t)s_ets_public_info;
}

/* Overwrites the ETSGATE stub installed by stubs_install_m1(); must therefore
 * run after it and before the unpack. game_unpack() calls it so the ordering
 * cannot be got wrong from outside. */
static int ets_public_info_install(void)
{
    if (!game_require("loader", "__p_EtsGetKernelPublicInfoPointer",
                      G->ets_kernel_public_info))
        return 0;

    memset(s_ets_public_info, 0, sizeof s_ets_public_info);
    patch_jmp(G->ets_kernel_public_info, (void *)ov_ets_kernel_public_info);
    LOGI("ETS kernel public info @ 0x%08X -> %zu-byte host block at %p "
         "(no reserved regions inside the span)",
         G->ets_kernel_public_info, sizeof s_ets_public_info,
         (void *)s_ets_public_info);
    return 1;
}

int game_unpack(void)
{
    int (*unpackrom)(void) = (int (*)(void))(uintptr_t)G->unpackrom;
    int rc;

    /* Without this the unpacker's reserved-region test reads uninitialised
     * stack, and its verdict is decided by the host's code layout. */
    if (!ets_public_info_install())
        return 0;

    LOGI("calling __pl_unpackrom @ 0x%08X", G->unpackrom);
    rc = unpackrom();
    if (rc != 0) {
        /* __p_start maps nonzero onto one of two fatal strings; 2 is a distinct
         * case there, so surface it rather than flattening to "failed". 2 is
         * the reserved-region refusal above; 1 is an unknown record type. */
        LOGE("__pl_unpackrom returned %d (expected 0): DATA/.bss are not "
             "initialised; do not continue", rc);
        return 0;
    }
    LOGI("__pl_unpackrom OK: DATA + .bss expanded");
    return 1;
}

/* Maintain a bounded lock-free census of threads observed in game code. CAS
 * claims distinct slots; excess threads are counted and reported. */
#define GAME_THREADS_MAX 8
static volatile LONG s_gt_id[GAME_THREADS_MAX];
static volatile LONG s_gt_hits[GAME_THREADS_MAX];
static const char *s_gt_where[GAME_THREADS_MAX];
static volatile LONG s_gt_count;
static volatile LONG s_gt_logic;

static void gt_note(int i, LONG me, const char *where, int logic)
{
    LONG n;

    s_gt_where[i] = where;              /* written once, before the count */
    n = InterlockedIncrement(&s_gt_count);
    if (n == 1)
        LOGI("game: thread %lu runs game code (first seen at %s)",
             (unsigned long)me, where);
    else
        LOGW("game: thread %lu ALSO runs game code (first seen at %s); %ld "
             "threads now. The game's INLINE cli/sti are emulated, so a "
             "critical section built from those does exclude these threads "
             "from each other, but stubs.c still stubs _EtsSetInterruptFlag "
             "to a bare return, and one built from THAT does not. See "
             "heal_privileged() and diego_frame_boundary().",
             (unsigned long)me, where, n);
    if (logic)
        InterlockedExchange(&s_gt_logic, me);
}

static void gt_seen(const char *where, int logic)
{
    LONG me = (LONG)GetCurrentThreadId();
    int i;

    for (i = 0; i < GAME_THREADS_MAX; i++) {
        LONG got = InterlockedCompareExchange(&s_gt_id[i], me, 0);

        if (got == me) {                /* already recorded: the hot path */
            InterlockedIncrement(&s_gt_hits[i]);
            if (logic && InterlockedCompareExchange(&s_gt_logic, 0, 0) != me)
                InterlockedExchange(&s_gt_logic, me);
            return;
        }
        if (got == 0) {                 /* we just claimed slot i */
            InterlockedIncrement(&s_gt_hits[i]);
            gt_note(i, me, where, logic);
            return;
        }
    }
    LOGW("game: thread %lu runs game code and the census is full (%d)",
         (unsigned long)me, GAME_THREADS_MAX);
}

void game_thread_seen(const char *where)
{
    gt_seen(where, 0);
}

void game_thread_logic_seen(const char *where)
{
    gt_seen(where, 1);
}

void game_thread_report(void)
{
    int i;

    for (i = 0; i < GAME_THREADS_MAX; i++) {
        LONG id = InterlockedCompareExchange(&s_gt_id[i], 0, 0);

        if (!id)
            break;
        LOGI("game: thread %lu%s; %ld crossings, first at %s",
             (unsigned long)id,
             id == InterlockedCompareExchange(&s_gt_logic, 0, 0) ?
                 " (the game's own logic)" : "",
             InterlockedCompareExchange(&s_gt_hits[i], 0, 0),
             s_gt_where[i] ? s_gt_where[i] : "?");
    }
}

unsigned game_thread_count(void)
{
    LONG n = InterlockedCompareExchange(&s_gt_count, 0, 0);

    return n < 0 ? 0u : (unsigned)n;
}

DWORD game_thread_id(unsigned i)
{
    if (i >= GAME_THREADS_MAX)
        return 0;
    return (DWORD)InterlockedCompareExchange(&s_gt_id[i], 0, 0);
}

DWORD game_thread_logic(void)
{
    return (DWORD)InterlockedCompareExchange(&s_gt_logic, 0, 0);
}

void game_run(void)
{
    void (*crt_startup)(void) = (void (*)(void))(uintptr_t)G->crt_startup;

    LOGI("entering _mainCRTStartup @ 0x%08X (-> __cinit -> _main @ 0x%08X)",
         G->crt_startup, G->main);
    game_thread_seen("game_run");
    crt_startup();

    /* Written with raw Win32, deliberately bypassing the logger.
     *
     * The log stops dead at this call and the process then exits 0, which has
     * exactly two explanations: the CRT startup returned and our logging is
     * broken, or it never returned. Every instrument that could tell them apart
     * is itself the logger, so this one is not. */
    {
        DWORD w;
        HANDLE h = CreateFileA("crt-returned.txt", GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            WriteFile(h, "_mainCRTStartup returned\r\n", 26, &w, NULL);
            CloseHandle(h);
        }
    }
    LOGW("_mainCRTStartup returned: the game normally exits via ExitProcess");
}
