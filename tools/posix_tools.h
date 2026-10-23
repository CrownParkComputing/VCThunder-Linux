/* posix_tools.h -- POSIX loading, file mapping and clocks for replay tools.
 * SPDX-License-Identifier: MIT */
#ifndef VCTHUNDER_POSIX_TOOLS_H
#define VCTHUNDER_POSIX_TOOLS_H

#include "../renderer/core/platform.h"


#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

typedef void              *HANDLE;

/* 64-bit on purpose, and `long long` rather than the pointer-sized type that
 * was here first. This builds -m32, where intptr_t is 32 bits, and QuadPart is
 * the nanosecond tick below: at 32 bits it wraps every 2.147 s, so any replay
 * longer than that reported a difference of the wrap rather than of the two
 * readings -- including a negative one. src/posix_compat.h has always had this
 * right (`typedef long long LONGLONG`); the two headers now agree, and the
 * halves of the union below stay 32 bits each, which is what Windows gives.
 */
typedef long long          LONGLONG;

typedef union {
    LONGLONG          QuadPart;
    unsigned long     LowPart;
    unsigned long     HighPart;
} LARGE_INTEGER;

/* Assert the width rather than trusting the typedef, because the failure is a
 * wrong number and not a compile error: at 32 bits the nanosecond tick below
 * wraps and every elapsed time the harness prints becomes the wrap. This line
 * is the whole reason the paragraph above is in this header. */
typedef char vgl_large_integer_must_be_64_bits[sizeof(LARGE_INTEGER) == 8
                                               ? 1 : -1];

#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define GENERIC_READ       0x80000000u
#define FILE_SHARE_READ    0x00000001u
#define OPEN_EXISTING      3
#define FILE_ATTRIBUTE_NORMAL 0x00000080u
#define PAGE_READONLY      0x02
#define FILE_MAP_READ      0x0004

/* load() below leaves the file handle INVALID on POSIX, so the Windows cleanup
 * at the end of main() compiles and does nothing. Present for that reason and
 * no other: nothing here ever holds a real handle. */
#define CloseHandle(h) ((void)(h))

/* pump() pumps a window message loop the harness only needs when presenting,
 * and the conformance run sets VCGLIDE_PRESENT=0. There is no window here, so
 * PeekMessageA always reports nothing and the loop body never runs -- which is
 * the correct behaviour rather than a stub that hides a call. */
typedef struct {
    int  message;
    void *wParam;
    void *lParam;
} MSG;

#define PM_REMOVE          0
#define PeekMessageA(...)  0
#define TranslateMessage(m)   ((void)(m))
#define DispatchMessageA(m)   ((void)(m))

/* The POSIX mapping is left in place for the life of the process (see
 * replay_map_file), so the Windows cleanup at the end of main() has nothing to
 * do here either. */
#define UnmapViewOfFile(p) ((void)(p))

/* GetLastError has no meaning here; dlerror does. */
static unsigned long GetLastError(void)
{
    return 0;
}

/* dlopen, but the caller expects a null on failure and this program reports
 * the reason itself. */
static HMODULE LoadLibraryA(const char *name)
{
    return (HMODULE)dlopen(name, RTLD_NOW | RTLD_LOCAL);
}

/* No FreeLibrary shim: the harness loads the provider once and exits, so a
 * dlclose path would be dead code in a header every tool includes. */

static void *GetProcAddress(HMODULE m, const char *name)
{
    return dlsym(m, name);
}

static BOOL SetEnvironmentVariableA(const char *name, const char *value)
{
    if (!value)
        return unsetenv(name) == 0;
    return setenv(name, value, 1) == 0;
}

/* QueryPerformanceCounter/Frequency as a monotonic pair. Only differences are
 * ever taken, so the epoch does not matter -- but the frequency must be real,
 * because the harness divides by it to turn ticks into seconds. */
static BOOL QueryPerformanceCounter(LARGE_INTEGER *out)
{
    struct timespec ts;

    if (!out)
        return 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    out->QuadPart = (LONGLONG)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return 1;
}

static BOOL QueryPerformanceFrequency(LARGE_INTEGER *out)
{
    if (!out)
        return 0;
    out->QuadPart = 1000000000LL;
    return 1;
}

/* The capture, mapped read-only. mmap rather than a read: the replay walks the
 * stream by pointer and the file is already page-aligned work. */
static int replay_map_file(const char *path, const unsigned char **out,
                           size_t *out_len)
{
    struct stat sb;
    const unsigned char *p;
    int fd;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    if (fstat(fd, &sb) != 0 || sb.st_size <= 0) {
        close(fd);
        return 0;
    }
    p = (const unsigned char *)mmap(NULL, (size_t)sb.st_size, PROT_READ,
                                    MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED)
        return 0;
    *out = p;
    *out_len = (size_t)sb.st_size;
    return 1;
}

static void replay_unmap_file(const unsigned char *p, size_t n)
{
    if (p)
        munmap((void *)p, n);
}


/* The provider's exports are decorated on PE (`grFoo@28`) and plain on ELF.
 * Strip the suffix on ELF; on Windows the caller asks for the decorated form.
 *
 * The ELF form carries an explicit precision rather than a bare "%s". Both
 * produce the same bytes, but the bare form leaves the output size unbounded as
 * far as -Wformat-truncation is concerned, and an over-long name from a
 * malformed capture would then be a build warning at every call site. The
 * precision states the bound that snprintf was already enforcing. Truncation is
 * deliberate, not a fallback: an over-long name truncates to something no
 * provider exports, so the lookup simply fails to bind and is counted. */
#define REPLAY_EXPORT_NAME(buf, name, argbytes) \
    snprintf((buf), sizeof(buf), "%.*s", (int)sizeof(buf) - 1, (name))

#endif /* VCTHUNDER_POSIX_TOOLS_H */
