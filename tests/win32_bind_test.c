/* win32_bind_test.c -- the Linux ETS kernel boundary's pure half (item 10).
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * #includes win32_posix.c, so building this test IS building that half of the
 * boundary -- the same "building a test is porting that source" rule as the
 * rest of the suite. It proves two things:
 *
 *   - the resolution seam maps the pure names to functions (and, deliberately,
 *     that the not-yet-written ones -- threads, synchronisation, exceptions --
 *     still resolve to NULL, which win32_bind.c reports exactly as it reports an
 *     unbound name on Windows); and
 *   - the pure implementations behave, not merely exist: heap round-trips,
 *     TLS, interlocked atomics, a monotonic clock, a plausible wall-clock, the
 *     environment, last-error, the working directory, and codepage conversions.
 */
#include "win32_posix.c"

#include <stdio.h>

static int fails;

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (cond) {                                                   \
            ;                                                         \
        } else {                                                      \
            printf("FAIL: %s\n", msg);                                \
            fails++;                                                  \
        }                                                             \
    } while (0)

static unsigned long W32API test_thread_proc(void *p)
{
    *(volatile int *)p = 42;
    return 7;
}

int main(void)
{
    /* ---- resolution ----------------------------------------------------- */
    CHECK(win32_posix_resolve("VirtualAlloc") != NULL, "VirtualAlloc resolves");
    CHECK(win32_posix_resolve("HeapAlloc") != NULL, "HeapAlloc resolves");
    CHECK(win32_posix_resolve("GetTickCount") != NULL, "GetTickCount resolves");
    CHECK(win32_posix_resolve("CreateFileA") != NULL, "CreateFileA resolves");
    CHECK(win32_posix_resolve("GetSystemTime") != NULL, "GetSystemTime resolves");
    CHECK(win32_posix_resolve("InterlockedIncrement") != NULL,
          "InterlockedIncrement resolves");
    CHECK(win32_posix_resolve("CreateThread") != NULL, "CreateThread resolves");
    CHECK(win32_posix_resolve("WaitForSingleObject") != NULL,
          "WaitForSingleObject resolves");
    CHECK(win32_posix_resolve("CreateEventA") != NULL, "CreateEventA resolves");
    CHECK(win32_posix_resolve("InitializeCriticalSection") != NULL,
          "InitializeCriticalSection resolves");

    /* The CRT calls this API through a four-argument stdcall slot. A fifth
     * parameter silently moves its stack and corrupts the next conversion. */
    {
        int (WINAPI *classify)(DWORD, const WCHAR *, int, WORD *) = w32p_GetStringTypeW;
        const WCHAR text[] = { 'A', '1' };
        WORD types[2] = { 0, 0 };
        CHECK(classify(1, text, 2, types), "wide character classification ABI");
        CHECK((types[0] & 0x100) && (types[1] & 4), "wide character classes");
    }

    /* Games reserve arenas, commit pages in them, and retain many small
     * allocations at once. Exercise that workflow across the shared allocator. */
    {
        void *blocks[64] = { 0 };
        unsigned i;
        unsigned char *arena = VirtualAlloc(NULL, 65536, MEM_RESERVE, PAGE_NOACCESS);
        CHECK(arena != NULL, "reserve an arena");
        if (arena) {
            CHECK(((uintptr_t)arena & 65535u) == 0, "arena has 64 KiB alignment");
            CHECK(w32p_VirtualAlloc(arena + 4096, 4096, MEM_COMMIT, PAGE_READWRITE)
                  == arena + 4096, "commit an existing reservation across translation units");
            arena[4096] = 0x5a;
            CHECK(VirtualAlloc(arena + 4096, 4096, MEM_COMMIT, PAGE_READWRITE)
                  == arena + 4096 && arena[4096] == 0x5a, "recommit preserves data");
            CHECK(w32p_VirtualFree(arena, 0, MEM_RELEASE), "release the arena");
        }
        for (i = 0; i < 64; i++) {
            blocks[i] = VirtualAlloc(NULL, 128, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            CHECK(blocks[i] != NULL, "more than sixteen live allocations");
        }
        for (i = 0; i < 64; i++)
            if (blocks[i]) CHECK(VirtualFree(blocks[i], 0, MEM_RELEASE), "release small allocation");
    }

    /* ---- heap ----------------------------------------------------------- */
    {
        unsigned char *p = (unsigned char *)w32p_HeapAlloc(NULL,
                                                          HEAP_ZERO_MEMORY, 32);
        CHECK(p != NULL, "HeapAlloc returns memory");
        CHECK(p[0] == 0 && p[31] == 0, "HeapAlloc honours HEAP_ZERO_MEMORY");
        CHECK(w32p_HeapSize(NULL, 0, p) >= 32, "HeapSize reports the block");
        p = (unsigned char *)w32p_HeapReAlloc(NULL, 0, p, 64);
        CHECK(p != NULL, "HeapReAlloc grows the block");
        CHECK(w32p_HeapFree(NULL, 0, p), "HeapFree succeeds");
    }

    /* ---- TLS ------------------------------------------------------------ */
    {
        DWORD t = w32p_TlsAlloc();
        CHECK(t != 0xFFFFFFFFu, "TlsAlloc returns an index");
        CHECK(w32p_TlsSetValue(t, (void *)(intptr_t)0x1234), "TlsSetValue ok");
        CHECK(w32p_TlsGetValue(t) == (void *)(intptr_t)0x1234,
              "TlsGetValue returns what was set");
        CHECK(w32p_TlsFree(t), "TlsFree ok");
    }

    /* ---- interlocked ---------------------------------------------------- */
    {
        volatile LONG v = 5;
        CHECK(w32p_InterlockedIncrement(&v) == 6, "InterlockedIncrement");
        CHECK(w32p_InterlockedDecrement(&v) == 5, "InterlockedDecrement");
    }

    /* ---- time ----------------------------------------------------------- */
    {
        LARGE_INTEGER freq, c0, c1;
        SYSTEMTIME st;

        CHECK(w32p_QueryPerformanceFrequency(&freq) && freq.QuadPart > 0,
              "QueryPerformanceFrequency");
        CHECK(w32p_QueryPerformanceCounter(&c0), "QueryPerformanceCounter");
        CHECK(w32p_QueryPerformanceCounter(&c1) && c1.QuadPart >= c0.QuadPart,
              "QPC advances");
        w32p_GetSystemTime(&st);
        CHECK(st.wYear >= 2020 && st.wYear <= 2100, "GetSystemTime year plausible");
        CHECK(st.wMonth >= 1 && st.wMonth <= 12, "GetSystemTime month plausible");
    }

    /* ---- environment ---------------------------------------------------- */
    {
        char buf[16];
        CHECK(w32p_SetEnvironmentVariableA("VCTHUNDER_W32_TEST", "hello"),
              "SetEnvironmentVariableA");
        CHECK(w32p_GetEnvironmentVariableA("VCTHUNDER_W32_TEST", buf,
                                           sizeof buf) == 5,
              "GetEnvironmentVariableA length");
        CHECK(strcmp(buf, "hello") == 0, "GetEnvironmentVariableA value");
    }

    /* ---- last-error ----------------------------------------------------- */
    w32p_SetLastError(42);
    CHECK(w32p_GetLastError() == 42, "SetLastError/GetLastError round-trip");

    /* ---- directory ------------------------------------------------------ */
    {
        char cwd[512];
        CHECK(w32p_GetCurrentDirectoryA(sizeof cwd, cwd) > 0,
              "GetCurrentDirectoryA");
    }

    /* ---- file I/O ------------------------------------------------------- */
    {
        void *fh = w32p_CreateFileA("/tmp/vcthunder_w32_test.bin", GENERIC_WRITE,
                                    0, NULL, CREATE_ALWAYS, 0, NULL);
        DWORD wrote = 0;
        DWORD size;

        CHECK(fh != INVALID_HANDLE_VALUE, "CreateFileA creates a file");
        CHECK(w32p_WriteFile(fh, "hello", 5, &wrote, NULL) && wrote == 5,
              "WriteFile writes 5 bytes");
        CHECK(w32p_CloseHandle(fh), "CloseHandle");
        fh = w32p_CreateFileA("/tmp/vcthunder_w32_test.bin", GENERIC_READ, 0,
                              NULL, OPEN_EXISTING, 0, NULL);
        size = w32p_GetFileSize(fh, NULL);
        CHECK(size == 5, "GetFileSize reads the size back");
        w32p_CloseHandle(fh);
        CHECK(w32p_DeleteFileA("/tmp/vcthunder_w32_test.bin"), "DeleteFileA");
    }

    /* ---- codepage ------------------------------------------------------- */
    {
        WCHAR w[8];
        char m[8];

        CHECK(w32p_IsValidCodePage(1252), "1252 is a valid codepage");
        CHECK(!w32p_IsValidCodePage(999), "999 is not");
        CHECK(w32p_CompareStringA(0, 0, "abc", 3, "abc", 3) == 2,
              "CompareStringA equal");
        CHECK(w32p_LCMapStringA(0, 0x200, "hello", 5, m, sizeof m) == 5,
              "LCMapStringA length");
        CHECK(strncmp(m, "HELLO", 5) == 0, "LCMapStringA uppercases");
        CHECK(w32p_MultiByteToWideChar(0, 0, "AB", 2, w, 8) == 2,
              "MultiByteToWideChar count");
        CHECK(w32p_WideCharToMultiByte(0, 0, w, 2, m, 8, NULL, NULL) == 2,
              "WideCharToMultiByte count");
        CHECK(m[0] == 'A' && m[1] == 'B', "wide round-trip preserves bytes");
    }

    /* ---- threads and synchronisation ------------------------------------ */
    {
        static volatile int ran;
        void *h;
        DWORD tid = 0;
        void *ev, *mx;
        char cs[40];

        ran = 0;
        h = w32p_CreateThread(NULL, 0, test_thread_proc, (void *)&ran, 0, &tid);
        CHECK(h != NULL, "CreateThread returns a handle");
        CHECK(tid != 0, "CreateThread writes the thread id");
        CHECK(w32p_WaitForSingleObject(h, 1000) == WAIT_OBJECT_0, "wait for a native thread");
        CHECK(w32p_CloseHandle(h), "close a native thread");
        CHECK(ran == 42, "the thread ran its proc");

        ev = w32p_CreateEventA(NULL, 0, 0, NULL);
        CHECK(ev != NULL, "CreateEventA");
        CHECK(w32p_SetEvent(ev), "SetEvent");
        CHECK(w32p_WaitForSingleObject(ev, INFINITE) == WAIT_OBJECT_0,
              "wait on a signaled event returns immediately");
        CHECK(w32p_WaitForSingleObject(ev, 10) == WAIT_TIMEOUT,
              "an auto-reset event is consumed by one wait");
        CHECK(w32p_ResetEvent(ev), "ResetEvent");

        mx = w32p_CreateMutexA(NULL, 0, NULL);
        CHECK(mx != NULL, "CreateMutexA");
        CHECK(w32p_WaitForSingleObject(mx, INFINITE) == WAIT_OBJECT_0,
              "acquire a mutex");
        CHECK(w32p_ReleaseMutex(mx), "ReleaseMutex");

        memset(cs, 0, sizeof cs);
        w32p_InitializeCriticalSection(cs);
        w32p_EnterCriticalSection(cs);
        w32p_LeaveCriticalSection(cs);
        w32p_DeleteCriticalSection(cs);
        CHECK(*(void **)cs == NULL, "DeleteCriticalSection clears the slot");
    }

    if (fails) {
        printf("win32_bind_test: %d FAILURES\n", fails);
        return 1;
    }
    printf("win32_bind_test: all checks passed\n");
    return 0;
}
