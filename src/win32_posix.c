/* win32_posix.c -- native Linux bindings behind the game's ETS kernel boundary.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The game is an unmodified 1999 i386 binary whose code calls Phar Lap ETS's
 * Win32-decorated entry points (_CreateThread@24, _VirtualAlloc@16 ...). Those
 * names, argument shapes and struct layouts are the game's ABI and cannot be
 * removed; win32_bind.c rewrites the import slots, pointing them at kernel32 on
 * Windows and at this file on Linux. What sits BEHIND each name here is a native
 * Linux primitive -- mmap, malloc, __thread, clock_gettime, open/read/write,
 * getenv, __atomic -- not a reimplementation of Win32 semantics. This is the
 * same move the netcode tier already made: the Winsock names bind straight to
 * BSD sockets rather than to a fake Winsock.
 *
 * The bindings cover the two supported titles' memory, files, threading,
 * synchronisation, clocks, locale and console calls. Exception raising and
 * unwinding remain incomplete; normal game execution uses the host's signal
 * handler and per-thread fs:[0] chains.
 *
 * CONVENTION: every function is __stdcall, so a pointer installed into an
 * __imp__ slot is entered with the callee-cleans convention the game's code
 * expects. This is the item-10/item-12 coupling the handover records.
 */
#include "posix_compat.h"

#include <dlfcn.h>
#include <malloc.h>
#include <sys/time.h>
#include <time.h>

#define W32API __attribute__((stdcall))

/* ---- the Win32 structs the game reads by value --------------------------- */
/* Layouts are load-bearing: the game copies and reads these in place.
 * FILETIME and SYSTEMTIME come from posix_compat.h. */

typedef struct {
    LONG       Bias;
    WORD       StandardName[32];
    SYSTEMTIME StandardDate;
    LONG       StandardBias;
    WORD       DaylightName[32];
    SYSTEMTIME DaylightDate;
    LONG       DaylightBias;
} TIME_ZONE_INFORMATION;

typedef struct {
    DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion,
          dwBuildNumber, dwPlatformId;
    CHAR  szCSDVersion[128];
} OSVERSIONINFOA;

typedef struct {
    DWORD dwMaxCharSize;
    BYTE  DefaultChar[2];
    BYTE  LeadByte[12];
} CPINFO;

#define VER_PLATFORM_WIN32_NT 2
#define TLS_MINIMUM_AVAILABLE 64
#define CP_ACP   0
#define CP_OEMCP 1
#define CP_UTF8  65001

/* ---- virtual memory (mmap) ---------------------------------------------- */

static void *W32API w32p_VirtualAlloc(void *addr, size_t size, DWORD type,
                                      DWORD protect)
{
    /* The same mmap-backed allocation the host itself uses. The `type` bits are
     * one mmap on POSIX (reserve and commit coincide). */
    return VirtualAlloc(addr, size, type, protect);
}

static int W32API w32p_VirtualFree(void *addr, size_t size, DWORD type)
{
    return VirtualFree(addr, size, type);
}

/* ---- the process heap (malloc) ------------------------------------------ */
/* Win32 heap handles are opaque here: every heap is malloc-backed, which is all
 * the titles need to boot. HeapSize uses malloc_usable_size so HeapReAlloc can
 * copy the right number of bytes. */

static void *W32API w32p_HeapCreate(DWORD options, size_t initial, size_t max)
{
    (void)options; (void)initial; (void)max;
    return (void *)(intptr_t)1;      /* a single shared heap */
}

static int W32API w32p_HeapDestroy(void *heap)
{
    (void)heap;
    return TRUE;
}

static void *W32API w32p_HeapAlloc(void *heap, DWORD flags, size_t n)
{
    void *p = malloc(n ? n : 1);

    (void)heap;
    if (p && (flags & HEAP_ZERO_MEMORY))
        memset(p, 0, n);
    return p;
}

static int W32API w32p_HeapFree(void *heap, DWORD flags, void *p)
{
    (void)heap; (void)flags;
    if (p)
        free(p);
    return TRUE;
}

static size_t W32API w32p_HeapSize(void *heap, DWORD flags, const void *p)
{
    (void)heap; (void)flags;
    if (!p)
        return (size_t)-1;
    return malloc_usable_size((void *)p);
}

static void *W32API w32p_HeapReAlloc(void *heap, DWORD flags, void *p, size_t n)
{
    void *q;
    size_t old;

    (void)heap;
    if (!p)
        return w32p_HeapAlloc(heap, flags, n);
    if (!n) {
        w32p_HeapFree(heap, 0, p);
        return NULL;
    }
    old = malloc_usable_size(p);
    if (flags & 0x10 /* HEAP_REALLOC_IN_PLACE_ONLY */)
        return n <= old ? p : NULL;
    q = realloc(p, n);
    if (q && (flags & HEAP_ZERO_MEMORY) && n > old)
        memset((char *)q + old, 0, n - old);
    return q;
}

static void *W32API w32p_LocalAlloc(DWORD flags, size_t n)
{
    return w32p_HeapAlloc(NULL, (flags & 0x40 /* LMEM_ZEROINIT */)
                          ? HEAP_ZERO_MEMORY : 0, n);
}

static void *W32API w32p_LocalFree(void *p)
{
    w32p_HeapFree(NULL, 0, p);
    return NULL;
}

static void *W32API w32p_LocalReAlloc(void *p, size_t n, DWORD flags)
{
    return w32p_HeapReAlloc(NULL, (flags & 0x40) ? HEAP_ZERO_MEMORY : 0, p, n);
}

/* ---- thread-local storage ------------------------------------------------- */
/* A fixed table of per-thread values. The allocation flags are shared (TlsAlloc
 * runs during single-threaded startup); the values are __thread so each game
 * thread sees its own. No pthread key API is needed, which also keeps the test
 * build free of a -lpthread dependency. */

static int s_tls_used[TLS_MINIMUM_AVAILABLE];
static __thread void *s_tls_value[TLS_MINIMUM_AVAILABLE];

static DWORD W32API w32p_TlsAlloc(void)
{
    int i;

    for (i = 0; i < TLS_MINIMUM_AVAILABLE; i++) {
        if (!s_tls_used[i]) {
            s_tls_used[i] = 1;
            return (DWORD)i;
        }
    }
    return 0xFFFFFFFFu;               /* TLS_OUT_OF_INDEXES */
}

static int W32API w32p_TlsFree(DWORD idx)
{
    if (idx >= TLS_MINIMUM_AVAILABLE || !s_tls_used[idx])
        return FALSE;
    s_tls_used[idx] = 0;
    return TRUE;
}

static void *W32API w32p_TlsGetValue(DWORD idx)
{
    if (idx >= TLS_MINIMUM_AVAILABLE)
        return NULL;
    return s_tls_value[idx];
}

static int W32API w32p_TlsSetValue(DWORD idx, void *value)
{
    if (idx >= TLS_MINIMUM_AVAILABLE)
        return FALSE;
    s_tls_value[idx] = value;
    return TRUE;
}

/* ---- time --------------------------------------------------------------- */

static DWORD W32API w32p_GetTickCount(void)
{
    return GetTickCount();
}

static int W32API w32p_QueryPerformanceFrequency(LARGE_INTEGER *out)
{
    return QueryPerformanceFrequency(out);
}

static int W32API w32p_QueryPerformanceCounter(LARGE_INTEGER *out)
{
    return QueryPerformanceCounter(out);
}

/* gettimeofday -> SYSTEMTIME. `local` selects the local-timezone split. */
static void fill_systime(SYSTEMTIME *st, int local)
{
    struct timeval tv;
    struct tm tm;

    gettimeofday(&tv, NULL);
    if (local)
        localtime_r(&tv.tv_sec, &tm);
    else
        gmtime_r(&tv.tv_sec, &tm);

    st->wYear         = (WORD)(tm.tm_year + 1900);
    st->wMonth        = (WORD)(tm.tm_mon + 1);
    st->wDayOfWeek    = (WORD)tm.tm_wday;
    st->wDay          = (WORD)tm.tm_mday;
    st->wHour         = (WORD)tm.tm_hour;
    st->wMinute       = (WORD)tm.tm_min;
    st->wSecond       = (WORD)tm.tm_sec;
    st->wMilliseconds = (WORD)(tv.tv_usec / 1000);
}

static void W32API w32p_GetSystemTime(SYSTEMTIME *st)
{
    fill_systime(st, 0);
}

static void W32API w32p_GetLocalTime(SYSTEMTIME *st)
{
    fill_systime(st, 1);
}

/* The cabinet clock is the game's own belief, not the host's wall clock; a
 * host never sets the real system time from inside a game. Accept and move on. */
static int W32API w32p_SetLocalTime(const SYSTEMTIME *st)
{
    (void)st;
    return TRUE;
}

static DWORD W32API w32p_GetTimeZoneInformation(TIME_ZONE_INFORMATION *tzi)
{
    time_t t = time(NULL);
    struct tm lt, ut;

    if (!tzi)
        return 0xFFFFFFFFu;           /* TIME_ZONE_ID_INVALID */
    memset(tzi, 0, sizeof *tzi);
    localtime_r(&t, &lt);
    gmtime_r(&t, &ut);
    /* Bias = UTC - local, in minutes. Approximate via tm_gmtoff when present. */
    tzi->Bias = (LONG)(lt.tm_gmtoff / 60);
    (void)ut;
    return 2;                          /* TIME_ZONE_ID_UNKNOWN */
}

/* Days from 1601-01-01 to 1970-01-01 (the FILETIME epoch offset). */
#define FILETIME_EPOCH_DIFF 11644473600ULL

static int W32API w32p_SystemTimeToFileTime(const SYSTEMTIME *st, FILETIME *ft)
{
    struct tm tm;
    uint64_t secs;

    if (!st || !ft)
        return FALSE;
    memset(&tm, 0, sizeof tm);
    tm.tm_year  = st->wYear - 1900;
    tm.tm_mon   = st->wMonth - 1;
    tm.tm_mday  = st->wDay;
    tm.tm_hour  = st->wHour;
    tm.tm_min   = st->wMinute;
    tm.tm_sec   = st->wSecond;
    secs = (uint64_t)timegm(&tm) + FILETIME_EPOCH_DIFF;
    secs = secs * 10000000ULL + (uint64_t)st->wMilliseconds * 10000ULL;
    ft->dwLowDateTime  = (DWORD)(secs & 0xFFFFFFFFu);
    ft->dwHighDateTime = (DWORD)(secs >> 32);
    return TRUE;
}

/* ---- interlocked -------------------------------------------------------- */

static LONG W32API w32p_InterlockedIncrement(LONG volatile *p)
{
    return InterlockedIncrement(p);
}

static LONG W32API w32p_InterlockedDecrement(LONG volatile *p)
{
    return InterlockedDecrement(p);
}

/* ---- last-error --------------------------------------------------------- */

static DWORD W32API w32p_GetLastError(void)
{
    return GetLastError();            /* posix_compat.h: reads errno */
}

static void W32API w32p_SetLastError(DWORD e)
{
    SetLastError(e);
}

/* ---- environment -------------------------------------------------------- */

static DWORD W32API w32p_GetEnvironmentVariableA(const char *name, char *buf,
                                                 DWORD n)
{
    const char *v = getenv(name);
    size_t len;

    if (!v) {
        if (buf && n)
            buf[0] = '\0';
        return 0;
    }
    len = strlen(v) + 1;
    if (len > n) {
        if (buf && n)
            buf[0] = '\0';
        return (DWORD)len;            /* size needed, per the contract */
    }
    memcpy(buf, v, len);
    return (DWORD)(len - 1);
}

static int W32API w32p_SetEnvironmentVariableA(const char *name, const char *val)
{
    if (!val)
        return unsetenv(name) == 0;
    return setenv(name, val, 1) == 0;
}

static char *W32API w32p_GetEnvironmentStrings(void)
{
    /* The block is NUL-separated entries, double-NUL terminated. The game walks
     * it; build it from environ. */
    extern char **environ;
    char *block, *p;
    size_t total = 1;                 /* the final extra NUL */
    int i;

    for (i = 0; environ[i]; i++)
        total += strlen(environ[i]) + 1;
    block = (char *)malloc(total);
    if (!block)
        return NULL;
    p = block;
    for (i = 0; environ[i]; i++) {
        size_t l = strlen(environ[i]) + 1;
        memcpy(p, environ[i], l);
        p += l;
    }
    *p = '\0';
    return block;
}

static int W32API w32p_FreeEnvironmentStringsA(char *block)
{
    free(block);
    return TRUE;
}

static DWORD W32API w32p_GetACP(void)      { return 1252; }
static DWORD W32API w32p_GetOEMCP(void)    { return 437;  }
static DWORD W32API w32p_GetUserDefaultLCID(void) { return 0x0409; }

/* ---- process / module / directory --------------------------------------- */

typedef struct {
    DWORD cb;
    char *lpReserved, *lpDesktop, *lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars,
          dwFillAttribute, dwFlags;
    WORD  wShowWindow, cbReserved2;
    BYTE *lpReserved2;
    void *hStdInput, *hStdOutput, *hStdError;
} STARTUPINFOA;

static void *W32API w32p_GetCurrentProcess(void)   { return GetCurrentProcess(); }
static void *W32API w32p_GetCurrentThread(void)    { return (void *)(intptr_t)-2; }
static DWORD W32API w32p_GetCurrentThreadId(void)  { return GetCurrentThreadId(); }

static DWORD W32API w32p_GetVersion(void) { return 0x0501; }

static int W32API w32p_GetVersionExA(OSVERSIONINFOA *info)
{
    if (!info)
        return FALSE;
    memset(info, 0, sizeof *info);
    info->dwOSVersionInfoSize = sizeof *info;
    info->dwMajorVersion = 5;
    info->dwMinorVersion = 1;
    info->dwBuildNumber  = 2600;
    info->dwPlatformId   = VER_PLATFORM_WIN32_NT;
    return TRUE;
}

static void *W32API w32p_GetModuleHandleA(const char *name)
{
    if (!name)
        return (void *)(intptr_t)1;   /* the process image */
    return NULL;                      /* no other modules are ETS-visible */
}

static DWORD W32API w32p_GetModuleFileNameA(void *mod, char *buf, DWORD n)
{
    ssize_t r;

    (void)mod;
    if (!buf || !n)
        return 0;
    r = readlink("/proc/self/exe", buf, n - 1);
    if (r < 0) {
        buf[0] = '\0';
        return 0;
    }
    buf[r] = '\0';
    return (DWORD)r;
}

static void *W32API w32p_GetProcAddress(void *mod, const char *name)
{
    return dlsym(mod ? mod : RTLD_DEFAULT, name);
}

static void *W32API w32p_LoadLibraryA(const char *name)
{
    return dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}

static int W32API w32p_FreeLibrary(void *mod)
{
    return dlclose(mod) == 0;
}

static char *W32API w32p_GetCommandLineA(void)
{
    static char cmd[64] = "vcthunder";
    return cmd;
}

static void W32API w32p_GetStartupInfoA(STARTUPINFOA *si)
{
    if (si)
        memset(si, 0, sizeof *si);
    if (si)
        si->cb = sizeof *si;
}

static DWORD W32API w32p_GetCurrentDirectoryA(DWORD n, char *buf)
{
    if (!buf || !n)
        return 0;
    if (!getcwd(buf, n))
        return 0;
    return (DWORD)strlen(buf);
}

static int W32API w32p_SetCurrentDirectoryA(const char *path)
{
    return chdir(path) == 0;
}

static void *W32API w32p_GetStdHandle(DWORD which)
{
    if (which == (DWORD)-10)          /* STD_INPUT_HANDLE  */
        return HANDLE_OF(STDIN_FILENO);
    if (which == (DWORD)-11)          /* STD_OUTPUT_HANDLE */
        return HANDLE_OF(STDOUT_FILENO);
    if (which == (DWORD)-12)          /* STD_ERROR_HANDLE  */
        return HANDLE_OF(STDERR_FILENO);
    return (void *)(intptr_t)-1;
}

static int W32API w32p_SetStdHandle(DWORD which, void *h)
{
    (void)which; (void)h;
    return TRUE;
}

static DWORD W32API w32p_SetHandleCount(DWORD n)
{
    (void)n;
    return 255;
}

/* ---- file I/O (wrap the POSIX fd layer in posix_compat.h) --------------- */

static void *W32API w32p_CreateFileA(const char *name, DWORD access, DWORD share,
                                     void *sa, DWORD disp, DWORD attrs, void *tmpl)
{
    return CreateFileA(name, access, share, sa, disp, attrs, tmpl);
}

static int W32API w32p_ReadFile(void *h, void *buf, DWORD n, DWORD *got, void *ov)
{
    return ReadFile(h, buf, n, got, ov);
}

static int W32API w32p_WriteFile(void *h, const void *buf, DWORD n, DWORD *put,
                                 void *ov)
{
    return WriteFile(h, buf, n, put, ov);
}

static int close_object(void *h);

int W32API w32p_CloseHandle(void *h)
{
    int result = close_object(h);
    return result < 0 ? CloseHandle(h) : result;
}

static DWORD W32API w32p_GetFileSize(void *h, DWORD *high)
{
    LARGE_INTEGER li;

    if (high)
        *high = 0;
    if (!GetFileSizeEx(h, &li))
        return 0xFFFFFFFFu;
    if (high)
        *high = li.HighPart;
    return li.LowPart;
}

static DWORD W32API w32p_SetFilePointer(void *h, LONG dist, LONG *moved,
                                        DWORD method)
{
    return SetFilePointer(h, dist, moved, method);
}

static int W32API w32p_DeleteFileA(const char *path)
{
    return DeleteFileA(path);
}

static int W32API w32p_FlushFileBuffers(void *h)
{
    return FlushFileBuffers(h);
}

static int W32API w32p_SetEndOfFile(void *h)
{
    off_t at = lseek(FD_OF(h), 0, SEEK_CUR);

    return at >= 0 && ftruncate(FD_OF(h), at) == 0;
}

static DWORD W32API w32p_GetFileType(void *h)
{
    if (!h || h == INVALID_HANDLE_VALUE)
        return 0;                      /* FILE_TYPE_UNKNOWN */
    return 1;                          /* FILE_TYPE_DISK */
}

static DWORD W32API w32p_GetDriveTypeA(const char *root)
{
    (void)root;
    return 3;                          /* DRIVE_FIXED: C: and D: were IDE */
}

/* ---- console ------------------------------------------------------------ */

static void W32API w32p_OutputDebugStringA(const char *s)
{
    if (s)
        (void)!write(STDERR_FILENO, s, strlen(s));
}

static int W32API w32p_WriteConsoleA(void *h, const void *buf, DWORD n,
                                     DWORD *written, void *reserved)
{
    ssize_t r;

    (void)h; (void)reserved;
    r = write(STDOUT_FILENO, buf, n);
    if (written)
        *written = r < 0 ? 0 : (DWORD)r;
    return r < 0 ? FALSE : TRUE;
}

static int W32API w32p_ReadConsoleA(void *h, void *buf, DWORD n, DWORD *got,
                                    void *reserved)
{
    ssize_t r;

    (void)h; (void)reserved;
    r = read(STDIN_FILENO, buf, n);
    if (got)
        *got = r < 0 ? 0 : (DWORD)r;
    return r < 0 ? FALSE : TRUE;
}

static int W32API w32p_GetConsoleMode(void *h, DWORD *mode)
{
    (void)h;
    if (mode)
        *mode = 0;
    return TRUE;
}

static int W32API w32p_MessageBoxA(void *owner, const char *text,
                                   const char *caption, DWORD type)
{
    (void)owner; (void)type;
    if (caption)
        (void)!write(STDERR_FILENO, caption, strlen(caption));
    (void)!write(STDERR_FILENO, ": ", 2);
    if (text)
        (void)!write(STDERR_FILENO, text, strlen(text));
    (void)!write(STDERR_FILENO, "\n", 1);
    return 1;                          /* IDOK */
}

static void W32API w32p_FatalAppExitA(DWORD code, const char *msg)
{
    (void)!write(STDERR_FILENO, msg ? msg : "fatal", msg ? strlen(msg) : 5);
    (void)!write(STDERR_FILENO, "\n", 1);
    _exit((int)code);
}

static int W32API w32p_SetConsoleCtrlHandler(void *handler, int add)
{
    (void)handler; (void)add;
    return TRUE;
}

/* ---- threads and synchronisation objects -------------------------------- */

typedef unsigned long (W32API *w32_thread_proc)(void *);

enum { W32_OBJ_EVENT, W32_OBJ_MUTEX, W32_OBJ_CRITSECT, W32_OBJ_TIMER, W32_OBJ_THREAD };

typedef struct w32_obj {
    int kind;
    unsigned refs;
    struct w32_obj *next;
    pthread_t thread;
    pthread_mutex_t lock;      /* recursive: doubles as the mutex/critsect */
    pthread_cond_t cond;
    int manual;
    int signaled;
    struct timespec due;       /* waitable timer fire time (absolute)      */
} w32_obj_t;

static pthread_mutex_t s_objects_lock = PTHREAD_MUTEX_INITIALIZER;
static w32_obj_t *s_objects;

static w32_obj_t *obj_new(int kind)
{
    w32_obj_t *o;
    pthread_mutexattr_t ma;

    o = (w32_obj_t *)calloc(1, sizeof *o);
    if (!o)
        return NULL;
    o->kind = kind;
    o->refs = 1;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&o->lock, &ma);
    pthread_mutexattr_destroy(&ma);
    pthread_cond_init(&o->cond, NULL);
    pthread_mutex_lock(&s_objects_lock);
    o->next = s_objects;
    s_objects = o;
    pthread_mutex_unlock(&s_objects_lock);
    return o;
}

static void obj_release(w32_obj_t *o)
{
    w32_obj_t **link;
    pthread_mutex_lock(&s_objects_lock);
    if (--o->refs) {
        pthread_mutex_unlock(&s_objects_lock);
        return;
    }
    for (link = &s_objects; *link != o; link = &(*link)->next)
        ;
    *link = o->next;
    pthread_mutex_unlock(&s_objects_lock);
    pthread_mutex_destroy(&o->lock);
    pthread_cond_destroy(&o->cond);
    free(o);
}

static int close_object(void *h)
{
    w32_obj_t *o;
    pthread_mutex_lock(&s_objects_lock);
    for (o = s_objects; o && o != h; o = o->next)
        ;
    pthread_mutex_unlock(&s_objects_lock);
    if (!o)
        return -1;
    if (o->kind == W32_OBJ_THREAD)
        pthread_detach(o->thread);
    obj_release(o);
    return TRUE;
}

struct w32_thread_arg {
    w32_thread_proc proc;
    void *param;
    w32_obj_t *object;
};

/* Each thread entering the old CRT needs its own fs:[0] exception chain. */
extern void install_teb(void) __attribute__((weak));

static void thread_finished(void *p)
{
    w32_obj_t *o = p;
    pthread_mutex_lock(&o->lock);
    o->signaled = 1;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
    obj_release(o);
}

static void *w32_thread_trampoline(void *p)
{
    struct w32_thread_arg a = *(struct w32_thread_arg *)p;
    void *result;
    free(p);
    if (install_teb)
        install_teb();
    pthread_cleanup_push(thread_finished, a.object);
    result = (void *)(uintptr_t)a.proc(a.param);
    pthread_cleanup_pop(1);
    return result;
}

void *W32API w32p_CreateThread(void *sa, size_t stack,
                              w32_thread_proc proc, void *param,
                              DWORD flags, DWORD *tid)
{
    struct w32_thread_arg *a;
    w32_obj_t *o;
    (void)sa; (void)stack; (void)flags;
    if (tid)
        *tid = 0;
    if (!proc)
        return NULL;
    o = obj_new(W32_OBJ_THREAD);
    if (!o)
        return NULL;
    o->manual = 1;
    a = malloc(sizeof *a);
    if (!a) {
        obj_release(o);
        return NULL;
    }
    a->proc = proc;
    a->param = param;
    a->object = o;
    o->refs = 2;
    if (pthread_create(&o->thread, NULL, w32_thread_trampoline, a) != 0) {
        free(a);
        obj_release(o);
        obj_release(o);
        return NULL;
    }
    if (tid)
        *tid = (DWORD)(uintptr_t)o->thread;
    return o;
}

static void W32API w32p_ExitThread(DWORD code)
{
    pthread_exit((void *)(uintptr_t)code);
}

DWORD W32API w32p_GetThreadId(void *handle)
{
    w32_obj_t *o = handle;
    return o && o->kind == W32_OBJ_THREAD ? (DWORD)(uintptr_t)o->thread : 0;
}

static int W32API w32p_SetThreadPriority(void *thread, int prio)
{
    (void)thread; (void)prio;
    return TRUE;                       /* best effort on Linux */
}

static int W32API w32p_GetThreadPriority(void *thread)
{
    (void)thread;
    return 0;                          /* THREAD_PRIORITY_NORMAL */
}

static void W32API w32p_TerminateProcess(void *proc, DWORD code)
{
    (void)proc;
    exit((int)code);
}

static void W32API w32p_ExitProcess(DWORD code)
{
    exit((int)code);
}

/* ---- events ------------------------------------------------------------ */

void *W32API w32p_CreateEventA(void *sa, int manual, int initial,
                                      const char *name)
{
    w32_obj_t *o = obj_new(W32_OBJ_EVENT);

    (void)sa; (void)name;
    if (!o)
        return NULL;
    o->manual = manual;
    o->signaled = initial;
    return o;
}

int W32API w32p_SetEvent(void *h)
{
    w32_obj_t *o = (w32_obj_t *)h;

    if (!o)
        return FALSE;
    pthread_mutex_lock(&o->lock);
    o->signaled = 1;
    if (o->manual)
        pthread_cond_broadcast(&o->cond);
    else
        pthread_cond_signal(&o->cond);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

int W32API w32p_ResetEvent(void *h)
{
    w32_obj_t *o = (w32_obj_t *)h;

    if (!o)
        return FALSE;
    pthread_mutex_lock(&o->lock);
    o->signaled = 0;
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ---- mutexes ----------------------------------------------------------- */

static void *W32API w32p_CreateMutexA(void *sa, int initial_owner,
                                      const char *name)
{
    w32_obj_t *o = obj_new(W32_OBJ_MUTEX);

    (void)sa; (void)name;
    if (!o)
        return NULL;
    if (initial_owner)
        pthread_mutex_lock(&o->lock);
    return o;
}

static int W32API w32p_ReleaseMutex(void *h)
{
    w32_obj_t *o = (w32_obj_t *)h;

    if (!o)
        return FALSE;
    return pthread_mutex_unlock(&o->lock) == 0;
}

/* ---- critical sections -------------------------------------------------- */
/* The game allocates a 40-byte CRITICAL_SECTION by value; its first four bytes
 * hold our heap object pointer, and the game never reads the bytes itself. */

static void W32API w32p_InitializeCriticalSection(void *cs)
{
    *(w32_obj_t **)cs = obj_new(W32_OBJ_CRITSECT);
}

static void W32API w32p_EnterCriticalSection(void *cs)
{
    w32_obj_t *o = *(w32_obj_t **)cs;

    if (o)
        pthread_mutex_lock(&o->lock);
}

static void W32API w32p_LeaveCriticalSection(void *cs)
{
    w32_obj_t *o = *(w32_obj_t **)cs;

    if (o)
        pthread_mutex_unlock(&o->lock);
}

static void W32API w32p_DeleteCriticalSection(void *cs)
{
    w32_obj_t *o = *(w32_obj_t **)cs;

    if (!o)
        return;
    obj_release(o);
    *(w32_obj_t **)cs = NULL;
}

/* ---- waitable timers ---------------------------------------------------- */

void *W32API w32p_CreateWaitableTimerA(void *sa, int manual,
                                              const char *name)
{
    w32_obj_t *o = obj_new(W32_OBJ_TIMER);

    (void)sa; (void)name;
    if (!o)
        return NULL;
    o->manual = manual;
    return o;
}

int W32API w32p_SetWaitableTimer(void *h, const LARGE_INTEGER *due,
                                        LONG period, void *routine,
                                        void *arg, int resume)
{
    w32_obj_t *o = (w32_obj_t *)h;

    (void)period; (void)routine; (void)arg; (void)resume;
    if (!o)
        return FALSE;
    pthread_mutex_lock(&o->lock);
    clock_gettime(CLOCK_REALTIME, &o->due);
    if (due && due->QuadPart) {
        if (due->QuadPart < 0) {
            /* relative, in 100 ns units */
            long long ns = -due->QuadPart * 100LL;
            o->due.tv_sec += (time_t)(ns / 1000000000LL);
            o->due.tv_nsec += (long)(ns % 1000000000LL);
        } else {
            /* absolute FILETIME: 100 ns since 1601-01-01 */
            long long t = due->QuadPart - 116444736000000000LL;
            o->due.tv_sec = (time_t)(t / 10000000LL);
            o->due.tv_nsec = (long)((t % 10000000LL) * 100LL);
        }
        if (o->due.tv_nsec >= 1000000000L) {
            o->due.tv_sec++;
            o->due.tv_nsec -= 1000000000L;
        }
        if (o->due.tv_nsec < 0) {
            o->due.tv_sec--;
            o->due.tv_nsec += 1000000000L;
        }
    }
    o->signaled = 0;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

int W32API w32p_CancelWaitableTimer(void *h)
{
    w32_obj_t *o = (w32_obj_t *)h;

    if (!o)
        return FALSE;
    pthread_mutex_lock(&o->lock);
    o->due.tv_sec = o->due.tv_nsec = 0;
    o->signaled = 0;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ---- WaitForSingleObject ------------------------------------------------ */

DWORD W32API w32p_WaitForSingleObject(void *h, DWORD ms)
{
    w32_obj_t *o = h;
    struct timespec deadline, now, wake;
    int rc = 0;
    if (!o || h == INVALID_HANDLE_VALUE)
        return 0xffffffffu;
    if (o->kind == W32_OBJ_MUTEX) {
        rc = ms == 0 ? pthread_mutex_trylock(&o->lock) : pthread_mutex_lock(&o->lock);
        return rc == 0 ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    }
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += ms / 1000;
    deadline.tv_nsec += (long)(ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&o->lock);
    while (!o->signaled) {
        clock_gettime(CLOCK_REALTIME, &now);
        if (o->kind == W32_OBJ_TIMER &&
            (o->due.tv_sec || o->due.tv_nsec) &&
            (now.tv_sec > o->due.tv_sec ||
             (now.tv_sec == o->due.tv_sec && now.tv_nsec >= o->due.tv_nsec))) {
            o->signaled = 1;
            o->due.tv_sec = o->due.tv_nsec = 0;
            break;
        }
        if (ms != INFINITE &&
            (now.tv_sec > deadline.tv_sec ||
             (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))) {
            pthread_mutex_unlock(&o->lock);
            return WAIT_TIMEOUT;
        }
        wake = deadline;
        if (o->kind == W32_OBJ_TIMER && (o->due.tv_sec || o->due.tv_nsec) &&
            (ms == INFINITE || o->due.tv_sec < wake.tv_sec ||
             (o->due.tv_sec == wake.tv_sec && o->due.tv_nsec < wake.tv_nsec)))
            wake = o->due;
        if (ms == INFINITE && !(o->due.tv_sec || o->due.tv_nsec))
            rc = pthread_cond_wait(&o->cond, &o->lock);
        else
            rc = pthread_cond_timedwait(&o->cond, &o->lock, &wake);
        if (rc && rc != ETIMEDOUT) {
            pthread_mutex_unlock(&o->lock);
            return 0xffffffffu;
        }
    }
    if (!o->manual)
        o->signaled = 0;
    pthread_mutex_unlock(&o->lock);
    return WAIT_OBJECT_0;
}

DWORD W32API w32p_WaitForMultipleObjects(DWORD count, const HANDLE *handles,
                                        BOOL all, DWORD ms)
{
    DWORD start = GetTickCount(), i;
    if (!count || !handles || all)
        return 0xffffffffu;
    for (;;) {
        for (i = 0; i < count; i++) {
            DWORD r = w32p_WaitForSingleObject(handles[i], 0);
            if (r == WAIT_OBJECT_0)
                return WAIT_OBJECT_0 + i;
            if (r != WAIT_TIMEOUT)
                return r;
        }
        if (ms != INFINITE && GetTickCount() - start >= ms)
            return WAIT_TIMEOUT;
        Sleep(1);
    }
}

/* ---- codepage / locale (minimal, US-English) ---------------------------- */



/* WCHAR comes from posix_compat.h. */

static int W32API w32p_IsValidCodePage(DWORD cp)
{
    return cp == 1252 || cp == 437 || cp == 65001 || cp == 1200;
}

static int W32API w32p_IsValidLocale(DWORD lcid, DWORD flags)
{
    (void)lcid; (void)flags;
    return TRUE;
}

static int W32API w32p_GetCPInfo(DWORD cp, CPINFO *info)
{
    if (cp != 1252 || !info)
        return FALSE;
    info->dwMaxCharSize = 1;
    info->DefaultChar[0] = 0x1A;      /* SUB */
    info->DefaultChar[1] = 0;
    memset(info->LeadByte, 0, sizeof info->LeadByte);
    return TRUE;
}

static int W32API w32p_CompareStringA(DWORD lcid, DWORD flags, const char *a,
                                      int la, const char *b, int lb)
{
    int r;

    (void)lcid;
    if (flags & 0x1 /* NORM_IGNORECASE */)
        r = strncasecmp(a ? a : "", b ? b : "",
                        la < lb ? (size_t)la : (size_t)lb);
    else
        r = strncmp(a ? a : "", b ? b : "",
                    la < lb ? (size_t)la : (size_t)lb);
    if (r != 0)
        return r < 0 ? 1 : 3;         /* CSTR_LESS_THAN / CSTR_GREATER_THAN */
    return 2;                          /* CSTR_EQUAL */
}

static int W32API w32p_LCMapStringA(DWORD lcid, DWORD flags, const char *src,
                                    int srclen, char *dst, int dstlen)
{
    int i, n;

    (void)lcid;
    if (!src)
        return 0;
    if (srclen < 0)
        srclen = (int)strlen(src);
    n = srclen < dstlen ? srclen : dstlen;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        if (flags & 0x200 /* LCMAP_UPPERCASE */)
            c = (unsigned char)toupper(c);
        else if (flags & 0x100 /* LCMAP_LOWERCASE */)
            c = (unsigned char)tolower(c);
        dst[i] = (char)c;
    }
    return n;
}

/* CP1252 -> UTF-16LE. Bytes map 0x00..0xFF to U+0000..U+00FF; the 0x80..0x9F
 * range is the Latin-1 reading rather than CP1252's specials, which is exact for
 * the ASCII the titles' own strings use and documented here as a simplification. */
static int W32API w32p_MultiByteToWideChar(DWORD cp, DWORD flags,
                                           const char *mb, int mblen,
                                           WCHAR *wc, int wclen)
{
    int i, n;

    (void)cp; (void)flags;
    if (!mb || mblen == 0)
        return 0;
    if (mblen < 0)
        mblen = (int)strlen(mb);
    n = mblen < wclen ? mblen : wclen;
    if (!wc)
        return mblen;                  /* caller asked for the required size */
    for (i = 0; i < n; i++)
        wc[i] = (unsigned char)mb[i];
    return n;
}

static int W32API w32p_WideCharToMultiByte(DWORD cp, DWORD flags,
                                           const WCHAR *wc, int wclen,
                                           char *mb, int mblen,
                                           const char *defchar, int *used)
{
    int i, n;
    char sub = defchar ? defchar[0] : '?';

    (void)cp; (void)flags;
    if (used)
        *used = 0;
    if (!wc || wclen == 0)
        return 0;
    if (wclen < 0) {
        for (wclen = 0; wc[wclen]; wclen++)
            ;
    }
    n = wclen < mblen ? wclen : mblen;
    if (!mb)
        return wclen;
    for (i = 0; i < n; i++) {
        mb[i] = (wc[i] <= 0xFF) ? (char)wc[i] : sub;
        if (wc[i] > 0xFF && used)
            *used = 1;
    }
    return n;
}

/* ---- sleep, thread resume, console input, keys -------------------------- */

static void W32API w32p_Sleep(DWORD ms)
{
    Sleep(ms);
}

static DWORD W32API w32p_ResumeThread(void *thread)
{
    (void)thread;
    return 0;                          /* was not suspended */
}

static int W32API w32p_GetNumberOfConsoleInputEvents(void *h, DWORD *n)
{
    (void)h;
    if (n)
        *n = 0;
    return TRUE;
}

static int W32API w32p_PeekConsoleInputA(void *h, void *buf, DWORD n, DWORD *rd)
{
    (void)h; (void)buf; (void)n;
    if (rd)
        *rd = 0;
    return TRUE;
}

static int W32API w32p_ReadConsoleInputA(void *h, void *buf, DWORD n, DWORD *rd)
{
    (void)h; (void)buf; (void)n;
    if (rd)
        *rd = 0;
    return TRUE;
}

static int W32API w32p_SetConsoleMode(void *h, DWORD mode)
{
    (void)h; (void)mode;
    return TRUE;
}

static short W32API w32p_VkKeyScanA(char ch)
{
    if (ch >= 'a' && ch <= 'z')
        return (short)('A' + (ch - 'a'));
    if (ch >= 'A' && ch <= 'Z')
        return (short)ch;
    if (ch >= '0' && ch <= '9')
        return (short)ch;
    return (short)ch;
}

/* ---- file time conversions --------------------------------------------- */

static int W32API w32p_FileTimeToSystemTime(const FILETIME *ft, SYSTEMTIME *st)
{
    long long t;
    time_t sec;
    struct tm tm;

    if (!ft || !st)
        return FALSE;
    t = ((long long)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    sec = (time_t)(t / 10000000LL - 11644473600LL);
    gmtime_r(&sec, &tm);
    st->wYear = (WORD)(tm.tm_year + 1900);
    st->wMonth = (WORD)(tm.tm_mon + 1);
    st->wDayOfWeek = (WORD)tm.tm_wday;
    st->wDay = (WORD)tm.tm_mday;
    st->wHour = (WORD)tm.tm_hour;
    st->wMinute = (WORD)tm.tm_min;
    st->wSecond = (WORD)tm.tm_sec;
    st->wMilliseconds = (WORD)((t / 10000LL) % 1000);
    return TRUE;
}

static int W32API w32p_FileTimeToDosDateTime(const FILETIME *ft, WORD *fdate,
                                             WORD *ftime)
{
    SYSTEMTIME st;

    if (!w32p_FileTimeToSystemTime(ft, &st))
        return FALSE;
    if (fdate)
        *fdate = (WORD)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
    if (ftime)
        *ftime = (WORD)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
    return TRUE;
}

static int W32API w32p_DosDateTimeToFileTime(WORD fdate, WORD ftime, FILETIME *ft)
{
    struct tm tm;
    time_t sec;
    long long t;

    if (!ft)
        return FALSE;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = (1980 + (fdate >> 9)) - 1900;
    tm.tm_mon = ((fdate >> 5) & 0xF) - 1;
    tm.tm_mday = fdate & 0x1F;
    tm.tm_hour = ftime >> 11;
    tm.tm_min = (ftime >> 5) & 0x3F;
    tm.tm_sec = (ftime & 0x1F) * 2;
    sec = timegm(&tm);
    t = (sec + 11644473600LL) * 10000000LL;
    ft->dwLowDateTime = (DWORD)(t & 0xFFFFFFFFu);
    ft->dwHighDateTime = (DWORD)((unsigned long long)t >> 32);
    return TRUE;
}

static int W32API w32p_LocalFileTimeToFileTime(const FILETIME *local, FILETIME *ft)
{
    if (!local || !ft)
        return FALSE;
    *ft = *local;
    return TRUE;
}

static int W32API w32p_FileTimeToLocalFileTime(const FILETIME *ft, FILETIME *local)
{
    if (!ft || !local)
        return FALSE;
    *local = *ft;
    return TRUE;
}

static int W32API w32p_SetFileTime(void *h, const FILETIME *c, const FILETIME *a,
                                   const FILETIME *w)
{
    (void)h; (void)c; (void)a; (void)w;
    return TRUE;
}

static int W32API w32p_SetSystemTime(const SYSTEMTIME *st)
{
    (void)st;
    return TRUE;
}

/* ---- file search (wrap posix_compat.h's DIR/fnmatch implementations) ---- */

static void *W32API w32p_FindFirstFileA(const char *path, WIN32_FIND_DATAA *d)
{
    return FindFirstFileA(path, d);
}

static int W32API w32p_FindNextFileA(void *handle, WIN32_FIND_DATAA *d)
{
    return FindNextFileA(handle, d);
}

static int W32API w32p_FindClose(void *handle)
{
    return FindClose(handle);
}

/* ---- wide-char environment and locale ----------------------------------- */

static WCHAR *W32API w32p_GetEnvironmentStringsW(void)
{
    extern char **environ;
    size_t total = 1;
    WCHAR *block, *p;
    int i;

    for (i = 0; environ[i]; i++)
        total += strlen(environ[i]) + 1;      /* in WCHARs */
    block = (WCHAR *)malloc(total * sizeof(WCHAR));
    if (!block)
        return NULL;
    p = block;
    for (i = 0; environ[i]; i++) {
        const unsigned char *s = (const unsigned char *)environ[i];
        while (*s)
            *p++ = (WCHAR)*s++;
        *p++ = 0;
    }
    *p = 0;
    return block;
}

static int W32API w32p_FreeEnvironmentStringsW(WCHAR *block)
{
    free(block);
    return TRUE;
}

static int W32API w32p_CompareStringW(DWORD lcid, DWORD flags, const WCHAR *a,
                                      int la, const WCHAR *b, int lb)
{
    int n = la < lb ? la : lb;
    int i, r = 0;

    (void)lcid;
    for (i = 0; i < n; i++) {
        WCHAR ca = a[i], cb = b[i];
        if (flags & 0x1 /* NORM_IGNORECASE */) {
            if (ca < 0x80 && ca >= 'a' && ca <= 'z')
                ca -= 0x20;
            if (cb < 0x80 && cb >= 'a' && cb <= 'z')
                cb -= 0x20;
        }
        if (ca != cb) {
            r = ca < cb ? -1 : 1;
            break;
        }
    }
    if (r == 0 && la != lb)
        r = la < lb ? -1 : 1;
    return r < 0 ? 1 : (r > 0 ? 3 : 2);   /* CSTR_LESS/GREATER/EQUAL */
}

static int W32API w32p_LCMapStringW(DWORD lcid, DWORD flags, const WCHAR *src,
                                    int srclen, WCHAR *dst, int dstlen)
{
    int i, n;

    (void)lcid;
    if (!src)
        return 0;
    if (srclen < 0) {
        for (srclen = 0; src[srclen]; srclen++)
            ;
    }
    n = srclen < dstlen ? srclen : dstlen;
    for (i = 0; i < n; i++) {
        WCHAR c = src[i];
        if ((flags & 0x200) && c < 0x80 && c >= 'a' && c <= 'z')
            c -= 0x20;
        else if ((flags & 0x100) && c < 0x80 && c >= 'A' && c <= 'Z')
            c += 0x20;
        dst[i] = c;
    }
    return n;
}

static int W32API w32p_GetStringTypeA(DWORD lcid, DWORD type, const char *s,
                                      int count, WORD *out)
{
    int i;
    (void)lcid; (void)type;
    if (!s || !out)
        return FALSE;
    for (i = 0; i < count; i++) {
        unsigned char c = (unsigned char)s[i];
        WORD t = 0;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
            t |= 0x100 /* C1_ALPHA */;
        if (c >= '0' && c <= '9')
            t |= 0x4 /* C1_DIGIT */;
        out[i] = t;
    }
    return TRUE;
}

static int W32API w32p_GetStringTypeW(DWORD type, const WCHAR *s,
                                      int count, WORD *out)
{
    int i;
    (void)type;
    if (!s || !out)
        return FALSE;
    for (i = 0; i < count; i++) {
        WCHAR c = s[i];
        WORD t = 0;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
            t |= 0x100;
        if (c >= '0' && c <= '9')
            t |= 0x4;
        out[i] = t;
    }
    return TRUE;
}

static const char *locale_string(DWORD lctype)
{
    switch (lctype) {
    case 0xE:    return ".";          /* LOCALE_SDECIMAL */
    case 0xF:    return ",";          /* LOCALE_STHOUSAND */
    case 0xC:    return ",";          /* LOCALE_SLIST */
    case 0x1D:   return "/";          /* LOCALE_SDATE */
    case 0x1E:   return ":";          /* LOCALE_STIME */
    case 0x20:   return "dddd, MMMM dd, yyyy";
    case 0x1:    return "0409";       /* LOCALE_ILANGUAGE */
    case 0x1004: return "1252";       /* LOCALE_IDEFAULTANSICODEPAGE */
    default:     return "";
    }
}

static int W32API w32p_GetLocaleInfoA(DWORD lcid, DWORD lctype, char *out, int n)
{
    const char *v = locale_string(lctype);

    (void)lcid;
    if (!out || n <= 0)
        return (int)strlen(v);
    snprintf(out, (size_t)n, "%s", v);
    return (int)strlen(v);
}

static int W32API w32p_GetLocaleInfoW(DWORD lcid, DWORD lctype, WCHAR *out, int n)
{
    const char *v = locale_string(lctype);
    int i;

    (void)lcid;
    if (!out || n <= 0)
        return (int)strlen(v);
    for (i = 0; v[i] && i < n - 1; i++)
        out[i] = (WCHAR)(unsigned char)v[i];
    out[i] = 0;
    return (int)strlen(v);
}

static int W32API w32p_EnumSystemLocalesA(void *callback, DWORD flags)
{
    typedef int (W32API *locale_enum_proc)(const char *);
    locale_enum_proc proc = (locale_enum_proc)callback;

    (void)flags;
    if (!proc)
        return FALSE;
    proc("en-US");
    return TRUE;
}

/* ---- exceptions (minimal; the real SEH is item 11) ---------------------- */

static void W32API w32p_RaiseException(DWORD code, DWORD flags, DWORD nargs,
                                       const void *args)
{
    (void)code; (void)flags; (void)nargs; (void)args;
    abort();                           /* replaced by the item-11 layer */
}

static void W32API w32p_RtlUnwind(void *target, void *base, void *eip, void *ctx)
{
    (void)target; (void)base; (void)eip; (void)ctx;
}

static LONG W32API w32p_UnhandledExceptionFilter(void *ep)
{
    (void)ep;
    return 1;                          /* EXCEPTION_EXECUTE_HANDLER */
}

/* ---- path resolution ---------------------------------------------------- */

static DWORD W32API w32p_GetFullPathNameA(const char *path, DWORD n, char *out,
                                          char **filepart)
{
    char resolved[PATH_MAX];

    if (filepart)
        *filepart = NULL;
    if (!path || !out || !n)
        return 0;
    if (realpath(path, resolved)) {
        snprintf(out, n, "%s", resolved);
        return (DWORD)strlen(out);
    }
    if (path[0] != '/') {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof cwd)) {
            snprintf(out, n, "%s/%s", cwd, path);
            return (DWORD)strlen(out);
        }
    }
    snprintf(out, n, "%s", path);
    return (DWORD)strlen(out);
}

/* ---- the binding -------------------------------------------------------- */

static const struct { const char *name; void *fn; } BIND[] = {
    { "VirtualAlloc",           (void *)w32p_VirtualAlloc },
    { "VirtualFree",            (void *)w32p_VirtualFree },
    { "HeapCreate",             (void *)w32p_HeapCreate },
    { "HeapDestroy",            (void *)w32p_HeapDestroy },
    { "HeapAlloc",              (void *)w32p_HeapAlloc },
    { "HeapFree",               (void *)w32p_HeapFree },
    { "HeapReAlloc",            (void *)w32p_HeapReAlloc },
    { "HeapSize",               (void *)w32p_HeapSize },
    { "LocalAlloc",             (void *)w32p_LocalAlloc },
    { "LocalFree",              (void *)w32p_LocalFree },
    { "LocalReAlloc",           (void *)w32p_LocalReAlloc },
    { "TlsAlloc",               (void *)w32p_TlsAlloc },
    { "TlsFree",                (void *)w32p_TlsFree },
    { "TlsGetValue",            (void *)w32p_TlsGetValue },
    { "TlsSetValue",            (void *)w32p_TlsSetValue },
    { "GetTickCount",           (void *)w32p_GetTickCount },
    { "QueryPerformanceFrequency", (void *)w32p_QueryPerformanceFrequency },
    { "QueryPerformanceCounter",   (void *)w32p_QueryPerformanceCounter },
    { "GetSystemTime",          (void *)w32p_GetSystemTime },
    { "GetLocalTime",           (void *)w32p_GetLocalTime },
    { "SetLocalTime",           (void *)w32p_SetLocalTime },
    { "GetTimeZoneInformation", (void *)w32p_GetTimeZoneInformation },
    { "SystemTimeToFileTime",   (void *)w32p_SystemTimeToFileTime },
    { "InterlockedIncrement",   (void *)w32p_InterlockedIncrement },
    { "InterlockedDecrement",   (void *)w32p_InterlockedDecrement },
    { "GetLastError",           (void *)w32p_GetLastError },
    { "SetLastError",           (void *)w32p_SetLastError },
    { "GetEnvironmentVariableA", (void *)w32p_GetEnvironmentVariableA },
    { "SetEnvironmentVariableA", (void *)w32p_SetEnvironmentVariableA },
    { "GetEnvironmentStrings",  (void *)w32p_GetEnvironmentStrings },
    { "FreeEnvironmentStringsA",(void *)w32p_FreeEnvironmentStringsA },
    { "GetACP",                 (void *)w32p_GetACP },
    { "GetOEMCP",               (void *)w32p_GetOEMCP },
    { "GetUserDefaultLCID",     (void *)w32p_GetUserDefaultLCID },
    { "GetCurrentProcess",      (void *)w32p_GetCurrentProcess },
    { "GetCurrentThread",       (void *)w32p_GetCurrentThread },
    { "GetCurrentThreadId",     (void *)w32p_GetCurrentThreadId },
    { "GetVersion",             (void *)w32p_GetVersion },
    { "GetVersionExA",          (void *)w32p_GetVersionExA },
    { "GetModuleHandleA",       (void *)w32p_GetModuleHandleA },
    { "GetModuleFileNameA",     (void *)w32p_GetModuleFileNameA },
    { "GetProcAddress",         (void *)w32p_GetProcAddress },
    { "LoadLibraryA",           (void *)w32p_LoadLibraryA },
    { "FreeLibrary",            (void *)w32p_FreeLibrary },
    { "GetCommandLineA",        (void *)w32p_GetCommandLineA },
    { "GetStartupInfoA",        (void *)w32p_GetStartupInfoA },
    { "GetCurrentDirectoryA",   (void *)w32p_GetCurrentDirectoryA },
    { "SetCurrentDirectoryA",   (void *)w32p_SetCurrentDirectoryA },
    { "GetStdHandle",           (void *)w32p_GetStdHandle },
    { "SetStdHandle",           (void *)w32p_SetStdHandle },
    { "SetHandleCount",         (void *)w32p_SetHandleCount },
    { "CreateFileA",            (void *)w32p_CreateFileA },
    { "ReadFile",               (void *)w32p_ReadFile },
    { "WriteFile",              (void *)w32p_WriteFile },
    { "CloseHandle",            (void *)w32p_CloseHandle },
    { "GetFileSize",            (void *)w32p_GetFileSize },
    { "SetFilePointer",         (void *)w32p_SetFilePointer },
    { "DeleteFileA",            (void *)w32p_DeleteFileA },
    { "FlushFileBuffers",       (void *)w32p_FlushFileBuffers },
    { "SetEndOfFile",           (void *)w32p_SetEndOfFile },
    { "GetFileType",            (void *)w32p_GetFileType },
    { "GetDriveTypeA",          (void *)w32p_GetDriveTypeA },
    { "OutputDebugStringA",     (void *)w32p_OutputDebugStringA },
    { "WriteConsoleA",          (void *)w32p_WriteConsoleA },
    { "ReadConsoleA",           (void *)w32p_ReadConsoleA },
    { "GetConsoleMode",         (void *)w32p_GetConsoleMode },
    { "MessageBoxA",            (void *)w32p_MessageBoxA },
    { "FatalAppExitA",          (void *)w32p_FatalAppExitA },
    { "SetConsoleCtrlHandler",  (void *)w32p_SetConsoleCtrlHandler },
    { "IsValidCodePage",        (void *)w32p_IsValidCodePage },
    { "IsValidLocale",          (void *)w32p_IsValidLocale },
    { "GetCPInfo",              (void *)w32p_GetCPInfo },
    { "CompareStringA",         (void *)w32p_CompareStringA },
    { "LCMapStringA",           (void *)w32p_LCMapStringA },
    { "MultiByteToWideChar",    (void *)w32p_MultiByteToWideChar },
    { "WideCharToMultiByte",    (void *)w32p_WideCharToMultiByte },
    { "CreateThread",           (void *)w32p_CreateThread },
    { "ExitThread",             (void *)w32p_ExitThread },
    { "SetThreadPriority",      (void *)w32p_SetThreadPriority },
    { "GetThreadPriority",      (void *)w32p_GetThreadPriority },
    { "TerminateProcess",       (void *)w32p_TerminateProcess },
    { "ExitProcess",            (void *)w32p_ExitProcess },
    { "WaitForSingleObject",    (void *)w32p_WaitForSingleObject },
    { "CreateEventA",           (void *)w32p_CreateEventA },
    { "SetEvent",               (void *)w32p_SetEvent },
    { "ResetEvent",             (void *)w32p_ResetEvent },
    { "CreateMutexA",           (void *)w32p_CreateMutexA },
    { "ReleaseMutex",           (void *)w32p_ReleaseMutex },
    { "InitializeCriticalSection", (void *)w32p_InitializeCriticalSection },
    { "EnterCriticalSection",   (void *)w32p_EnterCriticalSection },
    { "LeaveCriticalSection",   (void *)w32p_LeaveCriticalSection },
    { "DeleteCriticalSection",  (void *)w32p_DeleteCriticalSection },
    { "CreateWaitableTimerA",   (void *)w32p_CreateWaitableTimerA },
    { "SetWaitableTimer",       (void *)w32p_SetWaitableTimer },
    { "CancelWaitableTimer",    (void *)w32p_CancelWaitableTimer },
    { "Sleep",                  (void *)w32p_Sleep },
    { "ResumeThread",           (void *)w32p_ResumeThread },
    { "GetNumberOfConsoleInputEvents", (void *)w32p_GetNumberOfConsoleInputEvents },
    { "PeekConsoleInputA",      (void *)w32p_PeekConsoleInputA },
    { "ReadConsoleInputA",      (void *)w32p_ReadConsoleInputA },
    { "SetConsoleMode",         (void *)w32p_SetConsoleMode },
    { "VkKeyScanA",             (void *)w32p_VkKeyScanA },
    { "FileTimeToSystemTime",   (void *)w32p_FileTimeToSystemTime },
    { "FileTimeToDosDateTime",  (void *)w32p_FileTimeToDosDateTime },
    { "DosDateTimeToFileTime",  (void *)w32p_DosDateTimeToFileTime },
    { "LocalFileTimeToFileTime", (void *)w32p_LocalFileTimeToFileTime },
    { "FileTimeToLocalFileTime", (void *)w32p_FileTimeToLocalFileTime },
    { "SetFileTime",            (void *)w32p_SetFileTime },
    { "SetSystemTime",          (void *)w32p_SetSystemTime },
    { "FindFirstFileA",         (void *)w32p_FindFirstFileA },
    { "FindNextFileA",          (void *)w32p_FindNextFileA },
    { "FindClose",              (void *)w32p_FindClose },
    { "GetEnvironmentStringsW", (void *)w32p_GetEnvironmentStringsW },
    { "FreeEnvironmentStringsW", (void *)w32p_FreeEnvironmentStringsW },
    { "CompareStringW",         (void *)w32p_CompareStringW },
    { "LCMapStringW",           (void *)w32p_LCMapStringW },
    { "GetStringTypeA",         (void *)w32p_GetStringTypeA },
    { "GetStringTypeW",         (void *)w32p_GetStringTypeW },
    { "GetLocaleInfoA",         (void *)w32p_GetLocaleInfoA },
    { "GetLocaleInfoW",         (void *)w32p_GetLocaleInfoW },
    { "EnumSystemLocalesA",     (void *)w32p_EnumSystemLocalesA },
    { "RaiseException",         (void *)w32p_RaiseException },
    { "RtlUnwind",              (void *)w32p_RtlUnwind },
    { "UnhandledExceptionFilter", (void *)w32p_UnhandledExceptionFilter },
    { "GetFullPathNameA",        (void *)w32p_GetFullPathNameA },
};
#define NBIND (int)(sizeof BIND / sizeof BIND[0])

void *win32_posix_resolve(const char *base)
{
    int i;

    for (i = 0; i < NBIND; i++)
        if (strcmp(base, BIND[i].name) == 0)
            return BIND[i].fn;
    return NULL;
}
