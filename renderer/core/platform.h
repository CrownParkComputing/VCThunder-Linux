/* platform.h -- native Linux renderer utilities.
 * SPDX-License-Identifier: MIT
 * Clocks, environment, paths and readable-memory queries use POSIX/libc.
 * Glide entry points retain the original i386 calling convention.
 */
#ifndef VCGLIDE_PLATFORM_H
#define VCGLIDE_PLATFORM_H


#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The Glide stubs are stdcall because the PE export table says so and the
 * host's thunks in src/glide_bind.c are built against that. GCC implements the
 * attribute on i386, so keeping it here means an exported stub has the same
 * stack discipline on ELF as it does on PE. On any other target it degrades to
 * the ordinary C convention, which is all cdecl-compatible callers need. */
#if defined(__i386__) && !defined(__APPLE__)
#define __stdcall __attribute__((stdcall))
#else
#define __stdcall
#endif

typedef int                BOOL;
typedef unsigned long      DWORD;
typedef void              *HMODULE;
typedef const char        *LPCSTR;

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

/* The frame-dump path separator.
 *
 * Two callers build a path for a frame PNG by joining a directory and a
 * filename: vgl_dump_frame() in vcglide.c and dump_frame() in
 * tools/glide-replay.c. Both write a backslash, which is right on Windows and
 * is also what the capture's own path convention uses. On Linux a backslash is
 * an ordinary character in a filename, so the join produced a file called
 * "somedir\frame00000.png" sitting in the working directory next to an empty
 * somedir/ -- no error, because that is a perfectly legal filename here. That
 * is what a silent wrong answer looks like, and the create-directory call
 * beside it already had to be papered over for the same reason (see
 * CreateDirectoryA below), so the join is now made with the host's separator
 * instead and the two agree.
 *
 * A slash is not merely tolerated by the Windows file APIs, it is equivalent --
 * but the Windows build must not change at all, so this is per-platform rather
 * than "always use /". */
#define VGL_DIR_SEP "/"

#define MEM_COMMIT  0x1000
#define PAGE_NOACCESS 0x01
#define PAGE_READWRITE 0x04
#define PAGE_GUARD   0x100

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

typedef struct {
    void        *BaseAddress;
    void        *AllocationBase;
    DWORD        AllocationProtect;
    size_t       RegionSize;
    DWORD        State;
    DWORD        Protect;
    DWORD        Type;
} MEMORY_BASIC_INFORMATION;

/* GetTickCount: milliseconds, monotonic. Only ever written into log lines, so
 * the origin does not matter -- but it must not go backwards. */
static inline DWORD GetTickCount(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)((unsigned long long)ts.tv_sec * 1000u +
                   (unsigned long long)ts.tv_nsec / 1000000u);
}

/* GetEnvironmentVariableA's contract exactly, because cfg_str/cfg_num branch on
 * it: 0 when unset, the length including the NUL when the buffer is too small,
 * and otherwise the length copied, NUL excluded. */
static inline DWORD GetEnvironmentVariableA(const char *name, char *buf, DWORD n)
{
    const char *v = getenv(name);
    size_t len;

    if (!v)
        return 0;
    len = strlen(v) + 1;
    if (n == 0 || len > n)
        return (DWORD)len;
    memcpy(buf, v, len);
    return (DWORD)(len - 1);
}

#define _stricmp strcasecmp

/* CreateDirectoryA. Called only for the optional golden-frame dump, and the
 * caller builds that path with backslashes, so translate before handing it to
 * mkdir. */
static inline BOOL CreateDirectoryA(LPCSTR path, void *sec)
{
    char buf[MAX_PATH];
    size_t i;

    (void)sec;
    if (!path || !*path)
        return 0;
    for (i = 0; i + 1 < sizeof buf && path[i]; i++)
        buf[i] = (path[i] == '\\') ? '/' : path[i];
    buf[i] = '\0';
    return mkdir(buf, 0777) == 0;
}

/* VirtualQuery over /proc/self/maps. capture.c asks one question -- is this
 * caller payload backed by readable memory -- and answers it from the region
 * that contains the address. There is no PAGE_GUARD on Linux; PROT_NONE is
 * reported as PAGE_NOACCESS, which is the distinction capture.c actually
 * makes. A region split mid-range is reported as its first mapping, matching
 * the Windows behaviour of describing the region that starts the query. */
static inline int VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi,
                               size_t len)
{
    FILE *f;
    char line[512];
    uintptr_t want = (uintptr_t)addr;
    uintptr_t start = 0, end = 0;
    int readable = 0;
    int found = 0;

    (void)len;
    if (!mbi || !addr)
        return 0;
    f = fopen("/proc/self/maps", "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        uintptr_t lo, hi;
        char perms[8];

        if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %7s", &lo, &hi, perms) != 3)
            continue;
        if (want < lo || want >= hi)
            continue;
        start = lo;
        end = hi;
        readable = (perms[0] == 'r');
        found = 1;
        break;
    }
    fclose(f);
    if (!found)
        return 0;

    memset(mbi, 0, sizeof *mbi);
    mbi->BaseAddress = (void *)start;
    mbi->AllocationBase = (void *)start;
    mbi->RegionSize = end - start;
    mbi->State = MEM_COMMIT;
    mbi->Protect = readable ? PAGE_READWRITE : PAGE_NOACCESS;
    return 1;
}

#endif /* VCGLIDE_PLATFORM_H */
