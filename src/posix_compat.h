/* posix_compat.h -- Linux implementations of the host utility surface.
 * SPDX-License-Identifier: MIT
 * The original ETS code uses Win32-shaped types and names. These helpers use
 * POSIX/libc; no Windows libraries or SDK are loaded. Game-facing ABI adapters
 * live in win32_posix.c, with SDL backends for windows, audio and input.
 */
#ifndef VCTHUNDER_POSIX_COMPAT_H
#define VCTHUNDER_POSIX_COMPAT_H


#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fnmatch.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <malloc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ---- types ------------------------------------------------------------- */

typedef int             BOOL;
typedef unsigned long   DWORD;
typedef unsigned char   BYTE;
typedef unsigned short  WORD;
typedef int             LONG;
typedef short           SHORT;
typedef unsigned short  USHORT;
typedef void           *HANDLE;
typedef void           *HMODULE;
typedef HMODULE         HINSTANCE;
typedef void           *LPVOID;
typedef const void     *LPCVOID;
typedef const char     *LPCSTR;
typedef char           *LPSTR;
typedef unsigned long long ULONGLONG;
typedef long long       LONGLONG;
typedef size_t          SIZE_T;

/* CHAR, LONG and LPVOID pointers, at their narrow spellings. This port never
 * builds the W variants -- every call into it is the A form -- so TCHAR and the
 * rest are not defined, deliberately: a wide path that silently reused them
 * would be a real bug with no compile error. */
typedef char            CHAR;
typedef const char     *LPCTSTR;
typedef char           *LPTSTR;
typedef LONG           *LPLONG;
typedef const LONG     *LPCLONG;
typedef DWORD          *LPDWORD;
typedef const DWORD    *LPCDWORD;
typedef void           *LPUNKNOWN;

/* The window-class handle RegisterClassA returns and UnregisterClassA takes.
 * Nothing on this port ever reads one: the only use is to notice that the
 * class was registered, and on Linux the class is never registered at all. */
typedef WORD            ATOM;
typedef BYTE           *LPBYTE;

typedef union { LONGLONG QuadPart; struct { DWORD LowPart; LONG HighPart; }; } LARGE_INTEGER;

typedef struct { WORD wMajor, wMinor, wBuild, wReserved; } WORD_VER;

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

/* Windows' separator is '\\'; the host's is '/'. Normalise a Win32 path into a
 * caller buffer before any POSIX call, so `data\hydro\HYDRO.EXE` opens as
 * `data/hydro/HYDRO.EXE`. NULL returns NULL, mirroring the wrappers' own
 * "bad path" answer. */
static inline const char *posix_norm(char *dst, size_t dst_sz, const char *src)
{
    size_t i;

    if (!src || !dst || dst_sz == 0)
        return NULL;
    for (i = 0; src[i] && i + 1 < dst_sz; i++)
        dst[i] = (src[i] == '\\') ? '/' : src[i];
    dst[i] = '\0';
    return dst;
}

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

/* ---- constants --------------------------------------------------------- */
/* Declared before the shims below, which use them. */

#define GENERIC_READ       0x80000000u
#define GENERIC_WRITE      0x40000000u
#define FILE_SHARE_READ    0x00000001u
#define FILE_SHARE_WRITE   0x00000002u
#define OPEN_EXISTING      3
#define CREATE_ALWAYS      2
#define OPEN_ALWAYS        4
#define CREATE_NEW         1
#define TRUNCATE_EXISTING  5
#define FILE_APPEND        0x0004
#define FILE_ATTRIBUTE_NORMAL    0x00000080u
#define FILE_ATTRIBUTE_READONLY  0x00000001u
#define FILE_BEGIN              0x00000000u
#define FILE_END                0x00000001u
#define FILE_CURRENT            0x00000001u
#define INVALID_SET_FILE_POINTER ((LONGLONG)-1)

#define PAGE_NOACCESS     0x01
#define PAGE_READONLY     0x02
#define PAGE_READWRITE    0x04
#define PAGE_EXECUTE      0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define MEM_COMMIT        0x1000
#define MEM_RESERVE       0x2000
#define MEM_RELEASE       0x8000

#define WAIT_TIMEOUT      0x102
#define WAIT_OBJECT_0     0x00000000
#define INFINITE          0xFFFFFFFF

#define _stricmp    strcasecmp
#define _strnicmp   strncasecmp
#define _stricmpf   strcasecmp
#define strcpy_s(dst, n, src)  snprintf((dst), (n), "%s", (src))
#define sprintf_s  snprintf

/* Window handles. The host adopts the game's window rather than creating one
 * (src/window.c), so HWND is an opaque token here and nothing in the portable
 * path dereferences it. Making it void* rather than a distinct struct keeps the
 * ~40 files that merely pass a handle around compiling unchanged. */
typedef void *HWND;
typedef void *HDC;
typedef void *HICON;
typedef void *HCURSOR;
typedef void *HBRUSH;
typedef void *HMENU;
typedef void *HGDIOBJ;

typedef unsigned int  UINT;
typedef intptr_t      WPARAM;
typedef intptr_t      LPARAM;
typedef intptr_t      LRESULT;

#define WINAPI __attribute__((stdcall))
#define CALLBACK __attribute__((stdcall))
#define APIENTRY __attribute__((stdcall))

/* __cdecl is the C calling convention and needs no spelling here. Defined
 * object-like rather than function-like so the `(__cdecl *fn)(...)` form in
 * vcthunder.h collapses to `( *fn)(...)`. */
#define __cdecl

/* __stdcall is NOT the same story, and this matters. The host calls the Glide
 * provider through stdcall function pointers (src/widescreen.c), and the
 * provider's 69 exports are genuinely stdcall on i386 ELF -- verified by the
 * `ret $0xc` epilogue the conformance build emits. So on i386 the attribute is
 * kept, and the two halves stay in agreement. Off i386 it degrades to the
 * ordinary convention, which is all a cdecl-compatible target needs. */
#if defined(__i386__) && !defined(__APPLE__)
#define __stdcall __attribute__((stdcall))
#else
#define __stdcall
#endif

/* Win32's own spelling of the same idea, for the D3D9/DirectInput headers. */
#define STDMETHODCALLTYPE
#define STDMETHODCALLTYPE_IMPL

/* DirectInput joystick GUIDs, which src/dinput.c and src/di_canon.h address
 * canonically by guidType.Data1..4 rather than by the driver's reported dwOfs
 * (a deliberate fix for the G29 and the ProDD). The layout is fixed by the
 * DirectInput ABI, so reproducing it exactly is what keeps that fix working
 * rather than a placeholder. */
typedef struct _GUID {
    DWORD Data1;
    WORD  Data2;
    WORD  Data3;
    BYTE  Data4[8];
} GUID;

/* IID is a GUID under a different name -- same layout, and COM compares them
 * interchangeably. Not a typedef of GUID, because on Windows they are distinct
 * types and code that relies on them being interchangeable would then fail to
 * compile on the Windows build where it matters. */
typedef struct _IID {
    DWORD Data1;
    WORD  Data2;
    WORD  Data3;
    BYTE  Data4[8];
} IID;

/* The COM status base, which the DirectInput surface in src/dinput_posix.h is
 * written against. HRESULT is a SIGNED 32-bit value and the sign is the whole
 * convention: zero is success, negative is failure. Win32 spells that as
 * LONG, so the alias is exact rather than a widening typedef -- SUCCEEDED/FAILED
 * compare against 0 and a widening would change which results pass. */
typedef LONG HRESULT;

#define S_OK          ((HRESULT)0x00000000L)
#define S_FALSE       ((HRESULT)0x00000001L)
#define E_FAIL        ((HRESULT)0x80004005L)
#define E_INVALIDARG  ((HRESULT)0x80070057L)
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)

#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define FAILED(hr)    ((HRESULT)(hr) < 0)

/* Byte equality, which is what the real macro is: these GUIDs are compared as
 * opaque identifiers, never ordered or summed, so a field-wise compare would
 * only be slower and could disagree on padding. */
#define IsEqualGUID(a, b) (memcmp((a), (b), sizeof(GUID)) == 0)

/* The Win32 profile (INI) API, which src/ini.c is written against. */
#define GetPrivateProfileStringA  posix_profile_get_string
#define GetPrivateProfileSectionA posix_profile_get_section
#define GetPrivateProfileSectionNamesA posix_profile_section_names
#define GetPrivateProfileIntA     posix_profile_get_int
#define WritePrivateProfileStringA posix_profile_put_string

DWORD posix_profile_get_string(const char *section, const char *key,
                               const char *dflt, char *out, DWORD n,
                               const char *file);

/* --- FindFirstFileA / FindNextFileA / FindClose ---------------------------
 *
 * Pattern matching over a directory. The only use in this tree is
 * rumble_model_test.c sweeping up the ini files a test run left behind, so the
 * contract that matters is that the three calls together walk every match and
 * then report failure -- a loop written as do/while over FindNextFileA stops on
 * the first false, which must therefore mean "no more", not "something went
 * wrong".
 *
 * A Win32 search pattern is matched against the FILE NAME only, not the path,
 * and that is what fnmatch(3) is given here so a pattern with no '/' works the
 * way the caller wrote it.
 */
typedef struct {
    DWORD dwFileAttributes;
    CHAR  cFileName[MAX_PATH];
    CHAR  cAlternateFileName[14];
} WIN32_FIND_DATAA;

typedef struct {
    DIR          *dir;
    char          pattern[MAX_PATH];
} posix_find_state;

/* FindFirstFileA seeds its own first result by calling this, so the two are
 * mutually recursive in effect and need a forward declaration rather than an
 * ordering. */
static inline BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *out);

static inline HANDLE FindFirstFileA(LPCSTR pattern,
                                     WIN32_FIND_DATAA *out)
{
    posix_find_state *st;
    const char *slash;
    char dirpart[MAX_PATH];
    char norm[MAX_PATH];

    if (!pattern || !*pattern || !out)
        return INVALID_HANDLE_VALUE;
    pattern = posix_norm(norm, sizeof norm, pattern);
    if (!pattern)
        return INVALID_HANDLE_VALUE;
    st = calloc(1, sizeof *st);
    if (!st)
        return INVALID_HANDLE_VALUE;

    /* Split "<dir>/<name-pattern>". With no slash the search is the cwd, which
     * is what a bare "rumble-test*.ini" means on Windows too. */
    slash = strrchr(pattern, '/');
    if (slash) {
        size_t n = (size_t)(slash - pattern);
        if (n == 0)
            n = 1;                       /* "/x" -- keep the root */
        if (n >= sizeof dirpart) {
            free(st);
            return INVALID_HANDLE_VALUE;
        }
        memcpy(dirpart, pattern, n);
        dirpart[n] = '\0';
        snprintf(st->pattern, sizeof st->pattern, "%s", slash + 1);
        st->dir = opendir(dirpart);
    } else {
        snprintf(st->pattern, sizeof st->pattern, "%s", pattern);
        st->dir = opendir(".");
    }
    if (!st->dir) {
        free(st);
        return INVALID_HANDLE_VALUE;
    }

    if (!FindNextFileA((HANDLE)st, out)) {
        closedir(st->dir);
        free(st);
        return INVALID_HANDLE_VALUE;
    }
    return (HANDLE)st;
}

static inline BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *out)
{
    posix_find_state *st = (posix_find_state *)h;
    struct dirent *ent;

    if (!st || !st->dir || !out || h == INVALID_HANDLE_VALUE)
        return FALSE;
    while ((ent = readdir(st->dir)) != NULL) {
        if (fnmatch(st->pattern, ent->d_name, 0) != 0)
            continue;
        memset(out, 0, sizeof *out);
        /* FILE_ATTRIBUTE_DIRECTORY, so a caller that sweeps a directory does not
         * try to DeleteFile() the subdirectory it just matched. */
        if (ent->d_type == DT_DIR)
            out->dwFileAttributes = 0x10;
        else
            out->dwFileAttributes = 0x80;   /* FILE_ATTRIBUTE_NORMAL */
        snprintf(out->cFileName, sizeof out->cFileName, "%s", ent->d_name);
        return TRUE;
    }
    return FALSE;
}

static inline BOOL FindClose(HANDLE h)
{
    posix_find_state *st = (posix_find_state *)h;

    if (!st || h == INVALID_HANDLE_VALUE)
        return FALSE;
    if (st->dir)
        closedir(st->dir);
    free(st);
    return TRUE;
}

/* GetFullPathNameA: realpath() already resolves against the cwd and collapses
 * '.', '..' and symlinks, which is what the Win32 call is used for here. Like
 * Windows, it returns the length the buffer needs (excluding the NUL) when the
 * caller's buffer is too small, and the copied length when it fits. */
static inline DWORD GetFullPathNameA(LPCSTR name, DWORD n, LPSTR out,
                                      LPSTR *file_part)
{
    char resolved[PATH_MAX];
    const char *res;
    DWORD need;

    if (file_part)
        *file_part = NULL;
    res = realpath(name, resolved);
    if (!res)
        res = name;
    need = (DWORD)strlen(res);
    if (!out || n == 0)
        return need;
    snprintf(out, n, "%s", res);
    return need < n ? need : n - 1;
}

static inline BOOL CreateDirectoryA(LPCSTR path, void *sec)
{
    char norm[MAX_PATH];
    (void)sec;
    if (!path || !*path || !posix_norm(norm, sizeof norm, path))
        return FALSE;
    return mkdir(norm, 0777) == 0;
}

/* GetTempPathA: TMPDIR if set, else /tmp, always with the trailing separator
 * Windows guarantees. Returns the length written, NUL excluded. */
static inline DWORD GetTempPathA(DWORD n, LPSTR out)
{
    const char *t = getenv("TMPDIR");
    int wrote;

    if (!out || !n)
        return 0;
    if (!t || !*t)
        t = "/tmp/";
    wrote = snprintf(out, n, "%s", t);
    if (wrote < 0)
        return 0;
    if ((DWORD)wrote >= n)
        return n - 1;
    return (DWORD)wrote;
}

/* getpid() truncated to 32 bits, which is what a DWORD carries. The pid is
 * only ever compared for equality with itself. */
static inline DWORD GetCurrentProcessId(void)
{
    return (DWORD)getpid();
}

static inline BOOL DeleteFileA(LPCSTR path)
{
    return path && *path && unlink(path) == 0;
}

static inline BOOL RemoveDirectoryA(LPCSTR path)
{
    return path && *path && rmdir(path) == 0;
}

#define _strdup strdup

#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010u

/* GetFileAttributesA over stat(2). The three attributes anything here asks
 * for -- exists, is a directory, is read-only -- all map onto what stat
 * already reports. */
static inline DWORD GetFileAttributesA(LPCSTR path)
{
    struct stat sb;
    char norm[MAX_PATH];

    if (!path || !*path || !posix_norm(norm, sizeof norm, path))
        return INVALID_FILE_ATTRIBUTES;
    if (stat(norm, &sb) != 0)
        return INVALID_FILE_ATTRIBUTES;
    if (S_ISDIR(sb.st_mode))
        return FILE_ATTRIBUTE_DIRECTORY;
    return (sb.st_mode & S_IWUSR) ? FILE_ATTRIBUTE_NORMAL
                                  : FILE_ATTRIBUTE_READONLY;
}

static inline BOOL FileExistsA(LPCSTR path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* FILETIME and SYSTEMTIME, the two Win32 time structs the host reads by value.
 * win32_posix.c uses these rather than redeclaring them. */
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;

typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;

#define GetFileExInfoStandard 0

static inline BOOL GetFileAttributesExA(LPCSTR path, int info_level, void *out)
{
    struct stat st;
    char norm[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA *d = (WIN32_FILE_ATTRIBUTE_DATA *)out;

    if (info_level != GetFileExInfoStandard || !d)
        return FALSE;
    memset(d, 0, sizeof *d);
    if (!posix_norm(norm, sizeof norm, path) || stat(norm, &st) != 0)
        return FALSE;
    d->dwFileAttributes = GetFileAttributesA(path);
    d->nFileSizeLow = (DWORD)((unsigned long long)st.st_size & 0xFFFFFFFFu);
    d->nFileSizeHigh = (DWORD)((unsigned long long)st.st_size >> 32);
    return TRUE;
}

/* A CRITICAL_SECTION for the host's own code (the game's, in win32_posix.c, is
 * a pointer-in-a-slot of a different layout; the two never meet). */
typedef struct { pthread_mutex_t mutex; } CRITICAL_SECTION;
static inline void InitializeCriticalSection(CRITICAL_SECTION *cs) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cs->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}
static inline void EnterCriticalSection(CRITICAL_SECTION *cs)      { pthread_mutex_lock(&cs->mutex); }
static inline void LeaveCriticalSection(CRITICAL_SECTION *cs)      { pthread_mutex_unlock(&cs->mutex); }
static inline void DeleteCriticalSection(CRITICAL_SECTION *cs)     { pthread_mutex_destroy(&cs->mutex); }

/* COM does not exist on Linux; CoInitializeEx is a no-op that reports S_OK so
 * the host's win32_preload() path keeps its shape without a COM runtime.
 * HRESULT and S_OK are already defined above. */
#define COINIT_MULTITHREADED    0x0
static inline HRESULT CoInitializeEx(void *reserved, DWORD flags)
{
    (void)reserved; (void)flags;
    return S_OK;
}
static inline void CoUninitialize(void) {}

/* ---- the wider Win32 vocabulary win32_bind.c reaches for ----------------- */

typedef void *PVOID;
typedef void  VOID;
typedef void *LPSECURITY_ATTRIBUTES;
typedef CRITICAL_SECTION *LPCRITICAL_SECTION;
typedef DWORD *PDWORD;

#define DRIVE_FIXED                3
#define FILE_APPEND_DATA           0x0004u
#define PAGE_NOCACHE               0x200u
#define PAGE_WRITECOMBINE          0x400u
#define HEAP_REALLOC_IN_PLACE_ONLY 0x10u
#define MEM_FREE                   0x10000u
#define MEM_TOP_DOWN               0x100000u
#define MEM_WRITE_WATCH            0x200000u
#define MEM_MAPPED                 0x40000u
#define MEM_PRIVATE                0x20000u
#define MEM_IMAGE                  0x1000000u

typedef struct {
    void  *BaseAddress;
    void  *AllocationBase;
    DWORD  AllocationProtect;
    size_t RegionSize;
    DWORD  State;
    DWORD  Protect;
    DWORD  Type;
} MEMORY_BASIC_INFORMATION;

/* Heap handles are opaque; every heap is malloc-backed (HeapAlloc/HeapFree are
 * already so). HeapSize uses malloc_usable_size so HeapReAlloc can copy. */
static inline HANDLE HeapCreate(DWORD opts, SIZE_T initial, SIZE_T max)
{
    (void)opts; (void)initial; (void)max;
    return (void *)(intptr_t)1;
}
static inline BOOL HeapDestroy(HANDLE h) { (void)h; return TRUE; }
static inline SIZE_T HeapSize(HANDLE h, DWORD flags, const void *p)
{
    (void)h; (void)flags;
    return p ? (SIZE_T)malloc_usable_size((void *)p) : 0;
}
static inline LPVOID HeapReAlloc(HANDLE h, DWORD flags, void *p, SIZE_T n)
{
    (void)h; (void)flags;
    return p ? realloc(p, n) : malloc(n);
}

/* Slim reader/writer lock. The host only ever takes it exclusively (in the
 * write-watch path, which is inert on Linux), so a plain mutex is honest. */
typedef struct { pthread_mutex_t mutex; } SRWLOCK;
#define SRWLOCK_INIT { PTHREAD_MUTEX_INITIALIZER }
static inline void AcquireSRWLockExclusive(SRWLOCK *l) { pthread_mutex_lock(&l->mutex); }
static inline void ReleaseSRWLockExclusive(SRWLOCK *l) { pthread_mutex_unlock(&l->mutex); }
static inline void AcquireSRWLockShared(SRWLOCK *l)     { pthread_mutex_lock(&l->mutex); }
static inline void ReleaseSRWLockShared(SRWLOCK *l)     { pthread_mutex_unlock(&l->mutex); }

static inline void MemoryBarrier(void) { __sync_synchronize(); }

static inline BOOL TerminateProcess(HANDLE h, DWORD code) { (void)h; exit((int)code); return TRUE; }
static inline void ExitThread(DWORD code) { pthread_exit((void *)(uintptr_t)code); }

/* LoadLibraryA / GetProcAddress / FreeLibrary over dlopen/dlsym/dlclose. */
static inline HMODULE LoadLibraryA(LPCSTR name)
{
    return (HMODULE)dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}
static inline void *GetProcAddress(HMODULE h, LPCSTR name)
{
    /* ELF retains stdcall's ABI but does not decorate its symbol names. */
    char symbol[128];
    if (name[0] == '_')
        name++;
    size_t len = strcspn(name, "@");
    if (len >= sizeof symbol)
        return NULL;
    memcpy(symbol, name, len);
    symbol[len] = 0;
    return dlsym(h ? h : RTLD_DEFAULT, symbol);
}
static inline BOOL FreeLibrary(HMODULE h)
{
    return dlclose(h) == 0;
}

/* The host's own wide argv (main.c's Windows command-line path). The native
 * Linux entry point passes real argv, so these report "no command line". */
typedef unsigned short WCHAR;
typedef WCHAR *LPWSTR;
static inline const WCHAR *GetCommandLineW(void) { return NULL; }
static inline LPWSTR *CommandLineToArgvW(const WCHAR *cmdline, int *argc)
{
    (void)cmdline;
    if (argc)
        *argc = 0;
    return NULL;
}
static inline void *LocalAlloc(DWORD flags, size_t n)
{
    (void)flags;
    return malloc(n);
}
static inline void *LocalFree(void *p)
{
    free(p);
    return NULL;
}

/* The exception codes main.c's on_exception() branches on, and the two SEH
 * disposition values. The handler itself is a POSIX signal handler on Linux. */
#define EXCEPTION_ACCESS_VIOLATION    0xC0000005u
#define EXCEPTION_ILLEGAL_INSTRUCTION 0xC000001Du
#define EXCEPTION_PRIV_INSTRUCTION    0xC0000096u
#define EXCEPTION_INT_DIVIDE_BY_ZERO  0xC0000094u
#define EXCEPTION_FLT_DIVIDE_BY_ZERO  0xC000008Eu
#define EXCEPTION_STACK_OVERFLOW      0xC00000FDu
#define EXCEPTION_BREAKPOINT          0x80000003u
#define EXCEPTION_CONTINUE_EXECUTION  (-1)
#define EXCEPTION_CONTINUE_SEARCH     0
#define DBG_PRINTEXCEPTION_C          0x40010006u

#define YieldProcessor()  __builtin_ia32_pause()
#define SwitchToThread() sched_yield()

/* A few more kernel32 calls the host's main.c reaches directly. */
#define CP_ACP 0
static inline __attribute__((noreturn)) void ExitProcess(DWORD code) { exit((int)code); }
static inline BOOL SetCurrentDirectoryA(LPCSTR path) { return chdir(path) == 0; }
static inline DWORD GetModuleFileNameA(HMODULE h, char *buf, DWORD n)
{
    ssize_t r;
    (void)h;
    if (!buf || !n)
        return 0;
    r = readlink("/proc/self/exe", buf, n - 1);
    if (r < 0) {
        buf[0] = 0;
        return 0;
    }
    buf[r] = 0;
    return (DWORD)r;
}
static inline int WideCharToMultiByte(DWORD cp, DWORD flags, const WCHAR *wc,
                                      int wclen, char *mb, int mblen,
                                      const char *def, int *used)
{
    int i, n;
    (void)cp; (void)flags;
    if (used)
        *used = 0;
    if (!wc)
        return 0;
    if (wclen < 0)
        for (wclen = 0; wc[wclen]; wclen++)
            ;
    n = wclen < mblen ? wclen : mblen;
    if (!mb)
        return wclen;
    for (i = 0; i < n; i++)
        mb[i] = (wc[i] <= 0xFF) ? (char)wc[i] : (def ? def[0] : '?');
    return n;
}

/* No module lookup on Linux: the game image is mapped at its fixed base, not
 * loaded as a DLL, so GetModuleHandleExA answers "no such module". */
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS        0x00000004u
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT  0x00000002u
static inline BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *out)
{
    (void)flags; (void)name;
    if (out)
        *out = NULL;
    return FALSE;
}

/* A few more kernel32 calls glide_bind.c's diagnostics reach directly. */
typedef unsigned long long ULONG64;
typedef uintptr_t          DWORD_PTR;
#define VK_ESCAPE 0x1B
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0d
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_INSERT 0x2d
#define VK_DELETE 0x2e
#define VK_F1 0x70
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F24 0x87
#define VK_LSHIFT 0xa0
#define VK_RSHIFT 0xa1
#define VK_LCONTROL 0xa2
#define VK_RCONTROL 0xa3
#define VK_LMENU 0xa4
#define VK_RMENU 0xa5
#define VK_OEM_PLUS 0xbb
#define VK_OEM_COMMA 0xbc
#define VK_OEM_MINUS 0xbd
#define VK_OEM_PERIOD 0xbe
#define ERROR_DEVICE_NOT_CONNECTED 1167

static inline HMODULE GetModuleHandleA(LPCSTR name) { (void)name; return NULL; }
static inline void *GetCurrentThread(void) { return (void *)(intptr_t)-2; }
static inline int GetThreadPriority(void *thread) { (void)thread; return 0; }
static inline int GetPriorityClass(void *proc) { (void)proc; return 0; }
static inline BOOL SetEnvironmentVariableA(LPCSTR name, LPCSTR val)
{
    return val ? setenv(name, val, 1) == 0 : unsetenv(name) == 0;
}
static inline BOOL GetProcessAffinityMask(void *proc, DWORD_PTR *sys,
                                          DWORD_PTR *aff)
{
    (void)proc;
    if (sys)
        *sys = 1;
    if (aff)
        *aff = 1;
    return TRUE;
}
static inline BOOL IsProcessInJob(void *proc, void *job, BOOL *in)
{
    (void)proc; (void)job;
    if (in)
        *in = FALSE;
    return TRUE;
}
static inline BOOL QueryThreadCycleTime(void *thread, ULONG64 *cycles)
{
    (void)thread;
    if (cycles)
        *cycles = 0;
    return TRUE;
}
static inline BOOL GetProcessTimes(void *proc, FILETIME *c, FILETIME *e,
                                   FILETIME *k, FILETIME *u)
{
    (void)proc;
    if (c) c->dwLowDateTime = c->dwHighDateTime = 0;
    if (e) e->dwLowDateTime = e->dwHighDateTime = 0;
    if (k) k->dwLowDateTime = k->dwHighDateTime = 0;
    if (u) u->dwLowDateTime = u->dwHighDateTime = 0;
    return TRUE;
}

static inline DWORD GetLastError(void)
{
    return (DWORD)errno;
}

/* SetLastError writes errno, which is what GetLastError above reads, so the
 * pair round-trips the way callers expect. Note the ceiling: a POSIX syscall
 * clobbers errno on its own, so a saved code does not survive an intervening
 * call the way a thread-local Win32 value would. config.c saves and reads it
 * across adjacent calls only, which is safe. */
static inline void SetLastError(DWORD v)
{
    errno = (int)v;
}

/* The Win32 error codes this port actually branches on. Config seeding
 * distinguishes "the user's file is already there" (leave it alone) from every
 * other failure (report it), and that distinction has to survive. */
#define ERROR_SUCCESS            0
#define ERROR_FILE_EXISTS       80
#define ERROR_FILE_NOT_FOUND     2
#define ERROR_PATH_NOT_FOUND     3
#define ERROR_ACCESS_DENIED      5
#define ERROR_INVALID_PARAMETER  87

/* ---- VirtualAlloc / VirtualProtect / VirtualFree ------------------------- */

LPVOID VirtualAlloc(LPVOID addr, size_t size, DWORD type, DWORD protect);
BOOL VirtualFree(LPVOID addr, size_t size, DWORD type);
BOOL VirtualProtect(LPVOID addr, size_t size, DWORD protect, DWORD *old);

static inline LPVOID VirtualQuery(const void *addr, void *mbi, size_t len)
{
    /* Only capture.c's readable() asks, and only for readable/not-readable. */
    (void)mbi;
    (void)len;
    (void)addr;
    return NULL;
}

/* ---- Interlocked atomics ------------------------------------------------- */

/* The host's thread census (loader.c) and the game's own critical-section
 * emulation read-modify-write on LONGs and pointers through the Interlocked*
 * family. On i386 these are 4-byte seq-cst operations, which GCC's __atomic
 * builtins lower to the same lock-prefixed instructions Win32's Interlocked*
 * use. __atomic_compare_exchange_n is the compare-and-swap that returns the
 * old value through the expected slot, which is exactly the Interlocked*
 * contract (return the previous value). */

static inline LONG InterlockedIncrement(LONG volatile *p)
{
    return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
}

static inline LONG InterlockedDecrement(LONG volatile *p)
{
    return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST);
}

static inline LONG InterlockedExchange(LONG volatile *p, LONG v)
{
    return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}

static inline LONG InterlockedCompareExchange(LONG volatile *p, LONG x, LONG c)
{
    __atomic_compare_exchange_n(p, &c, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return c;
}

static inline LONG InterlockedExchangeAdd(LONG volatile *p, LONG v)
{
    return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
}

static inline void *InterlockedExchangePointer(void *volatile *p, void *v)
{
    return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}

static inline void *InterlockedCompareExchangePointer(void *volatile *p,
                                                      void *x, void *c)
{
    __atomic_compare_exchange_n(p, &c, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return c;
}

/* ---- the process heap -------------------------------------------------- *
/
 * Win32's GetProcessHeap()/HeapAlloc()/HeapFree() over malloc. net_link.c uses
 * them only for a scratch buffer sized at runtime (the adapter list), with
 * flags 0, so this is the whole of what is asked. HEAP_ZERO_MEMORY is honoured
 * because a caller could reasonably expect it; the heap handle is a fixed
 * non-null sentinel because nothing compares it to anything real. */
#define HEAP_ZERO_MEMORY 0x00000008u

static inline HANDLE GetProcessHeap(void)
{
    return (HANDLE)(intptr_t)1;
}

static inline LPVOID HeapAlloc(HANDLE h, DWORD flags, SIZE_T size)
{
    void *p = malloc(size);

    (void)h;
    if (p && (flags & HEAP_ZERO_MEMORY))
        memset(p, 0, size);
    return p;
}

static inline BOOL HeapFree(HANDLE h, DWORD flags, LPVOID p)
{
    (void)h;
    (void)flags;
    free(p);
    return TRUE;
}

/* ---- file handles ------------------------------------------------------ */

/* A HANDLE here is a POSIX fd biased by one, so that 0 -- a legitimate fd --
 * is never mistaken for "no handle" and INVALID_HANDLE_VALUE stays distinct
 * from every real one. Only CreateFileA produces them. */
#define FD_OF(h)   ((int)((intptr_t)(h) - 1))
#define HANDLE_OF(fd) ((HANDLE)(intptr_t)((fd) + 1))

static inline DWORD access_to_flags(DWORD access)
{
    int f = 0;
    if (access & GENERIC_READ)
        f |= O_RDONLY;
    else if (access & GENERIC_WRITE)
        f |= O_WRONLY;
    if ((access & (GENERIC_READ | GENERIC_WRITE)) ==
        (GENERIC_READ | GENERIC_WRITE))
        f = O_RDWR;
    if (access & FILE_APPEND)
        f |= O_APPEND;
    return (DWORD)f;
}

static inline HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share,
                                 void *sa, DWORD disp, DWORD attrs,
                                 void *tmpl)
{
    int flags, fd;
    char norm[MAX_PATH];

    (void)share; (void)sa; (void)attrs; (void)tmpl;
    if (!name || !*name || !posix_norm(norm, sizeof norm, name))
        return INVALID_HANDLE_VALUE;
    flags = (int)access_to_flags(access);
    if (disp == OPEN_EXISTING)
        flags |= 0;
    else if (disp == CREATE_NEW)
        flags |= O_CREAT | O_EXCL;
    else if (disp == CREATE_ALWAYS)
        flags |= O_CREAT | O_TRUNC;
    else if (disp == OPEN_ALWAYS)
        flags |= O_CREAT;
    fd = open(norm, flags, 0666);
    if (fd < 0 && errno == EEXIST)
        SetLastError(ERROR_FILE_EXISTS);
    return fd < 0 ? INVALID_HANDLE_VALUE : HANDLE_OF(fd);
}

static inline BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, void *ov)
{
    ssize_t r;
    (void)ov;
    if (!h || h == INVALID_HANDLE_VALUE)
        return FALSE;
    r = read(FD_OF(h), buf, n);
    if (r < 0)
        return FALSE;
    if (got)
        *got = (DWORD)r;
    return TRUE;
}

static inline BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *put,
                             void *ov)
{
    ssize_t r;
    (void)ov;
    if (!h || h == INVALID_HANDLE_VALUE)
        return FALSE;
    r = write(FD_OF(h), buf, n);
    if (r < 0)
        return FALSE;
    if (put)
        *put = (DWORD)r;
    return TRUE;
}

static inline DWORD SetFilePointer(HANDLE h, LONG dist, LONG *moved, DWORD method)
{
    int whence = SEEK_SET;
    off_t at;

    if (!h || h == INVALID_HANDLE_VALUE)
        return (DWORD)INVALID_SET_FILE_POINTER;
    if (method == FILE_END)
        whence = SEEK_END;
    else if (method == FILE_CURRENT)
        whence = SEEK_CUR;
    at = lseek(FD_OF(h), dist, whence);
    if (at < 0)
        return (DWORD)INVALID_SET_FILE_POINTER;
    if (moved)
        *moved = (LONG)at;
    return (DWORD)at;
}

static inline BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size)
{
    struct stat sb;

    if (!h || h == INVALID_HANDLE_VALUE || !size)
        return FALSE;
    if (fstat(FD_OF(h), &sb) != 0)
        return FALSE;
    size->QuadPart = (LONGLONG)sb.st_size;
    return TRUE;
}

static inline BOOL GetFileSize(HANDLE h, DWORD *low, DWORD *high)
{
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li))
        return FALSE;
    if (low)
        *low = li.LowPart;
    if (high)
        *high = li.HighPart;
    return TRUE;
}

static inline BOOL CloseHandle(HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE)
        return FALSE;
    return close(FD_OF(h)) == 0;
}

static inline BOOL FlushFileBuffers(HANDLE h)
{
    (void)h;
    return TRUE;
}

static inline BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_exists)
{
    int in, out;
    char buf[65536];
    char frm[MAX_PATH], dst[MAX_PATH];
    ssize_t n;

    if (fail_exists && FileExistsA(to))
        return FALSE;
    if (!posix_norm(frm, sizeof frm, from) || !posix_norm(dst, sizeof dst, to))
        return FALSE;
    in = open(frm, O_RDONLY);
    if (in < 0)
        return FALSE;
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) {
        close(in);
        return FALSE;
    }
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n) {
            close(in); close(out);
            return FALSE;
        }
    close(in);
    return close(out) == 0;
}

/* The pseudo-handle Win32 hands back; only ever compared, never dereferenced,
 * and it is what FlushInstructionCache takes in src/patch.c. */
#define GetCurrentProcess() ((HANDLE)(intptr_t)-1)

/* Linux threads are pthreads and the id is the pthread_t, which is what the
 * host actually wants to key a per-thread table by. Returned as a DWORD because
 * that is the type callers have; POSIX guarantees a pthread_t is at least as
 * wide as a pointer, and this is the one place the truncation is deliberate. */
static inline DWORD GetCurrentThreadId(void)
{
    return (DWORD)(uintptr_t)pthread_self();
}

#define MOVEFILE_REPLACE_EXISTING 0x00000001u
#define MOVEFILE_COPY_ALLOWED      0x00000002u
#define MOVEFILE_DELAY_UNTIL_REBOOT 0x00000004u
#define MOVEFILE_WRITE_THROUGH     0x00000008u

/* The MSVCRT underscore spellings. config_test redirects a FILE* back to its
 * descriptor so it can seed the settings file from one it already holds, and
 * rumble_model_test walks out of the directory it made before deleting it. */
#define _dup    dup
#define _dup2   dup2
#define _fileno fileno
#define _close  close
#define _chdir  chdir

/* rename(2) is the move. REPLACE_EXISTING is the default POSIX behaviour and
 * is only spelled out here because the callers pass it explicitly; a move onto
 * an existing target with that flag clear is refused rather than silently
 * overwritten, which rename() cannot express, so that case returns FALSE. */
static inline BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags)
{
    char frm[MAX_PATH], dst[MAX_PATH];
    (void)MOVEFILE_DELAY_UNTIL_REBOOT;
    if (!from || !to)
        return FALSE;
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && FileExistsA(to))
        return FALSE;
    if (!posix_norm(frm, sizeof frm, from) || !posix_norm(dst, sizeof dst, to))
        return FALSE;
    return rename(frm, dst) == 0;
}

/* FlushInstructionCache is a no-op by the architecture, not by convenience:
 * on x86 the instruction cache is coherent with the data caches, so a store
 * that is visible to the fetch unit needs no explicit fence. It would be a
 * real call on ARM or MIPS. */
static inline BOOL FlushInstructionCache(HANDLE h, LPCVOID base, SIZE_T n)
{
    (void)h; (void)base; (void)n;
    return TRUE;
}
DWORD posix_profile_get_section(const char *section, char *out, DWORD n,
                                const char *file);
DWORD posix_profile_section_names(char *out, DWORD n, const char *file);
int   posix_profile_get_int(const char *section, const char *key, int dflt,
                            const char *file);
BOOL  posix_profile_put_string(const char *section, const char *key,
                               const char *val, const char *file);

/* __declspec(dllexport) on shim_run is the only use in the tree, and ELF
 * exports every non-static symbol by default, so defining it away is correct
 * rather than a stub. If a memory-layout or calling-convention __declspec ever
 * appears here this will silently drop it -- check before adding more. */
#define __declspec(x) /* dropped on ELF; see the note above */

/* ---- time -------------------------------------------------------------- */

/* QueryPerformanceCounter as a nanosecond monotonic counter, and a matching
 * frequency, so a caller dividing by the frequency gets seconds. Only elapsed
 * time is ever taken, so the epoch is irrelevant. */
static inline BOOL QueryPerformanceCounter(LARGE_INTEGER *out)
{
    struct timespec ts;
    if (!out)
        return FALSE;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    out->QuadPart = (LONGLONG)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return TRUE;
}

static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *out)
{
    if (!out)
        return FALSE;
    out->QuadPart = 1000000000LL;
    return TRUE;
}

static inline DWORD GetTickCount(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)((unsigned long long)ts.tv_sec * 1000u +
                   (unsigned long long)ts.tv_nsec / 1000000u);
}

static inline void Sleep(DWORD ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000u;
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

#endif /* VCTHUNDER_POSIX_COMPAT_H */
