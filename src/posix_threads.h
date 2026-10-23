/* Native pthread-backed objects shared by the host and ETS bindings. */
/* SPDX-License-Identifier: MIT */
#ifndef VCT_POSIX_THREADS_H
#define VCT_POSIX_THREADS_H
#include "posix_compat.h"

HANDLE WINAPI w32p_CreateThread(void *, size_t, DWORD (WINAPI *)(void *), void *, DWORD, DWORD *);
HANDLE WINAPI w32p_CreateEventA(void *, int, int, const char *);
int WINAPI w32p_SetEvent(void *);
int WINAPI w32p_ResetEvent(void *);
DWORD WINAPI w32p_WaitForSingleObject(void *, DWORD);
DWORD WINAPI w32p_WaitForMultipleObjects(DWORD, const HANDLE *, BOOL, DWORD);
int WINAPI w32p_CloseHandle(void *);
DWORD WINAPI w32p_GetThreadId(void *);
HANDLE WINAPI w32p_CreateWaitableTimerA(void *, int, const char *);
int WINAPI w32p_SetWaitableTimer(void *, const LARGE_INTEGER *, LONG, void *, void *, int);
int WINAPI w32p_CancelWaitableTimer(void *);

#define CreateThread w32p_CreateThread
#define CreateEventA w32p_CreateEventA
#define SetEvent w32p_SetEvent
#define ResetEvent w32p_ResetEvent
#define WaitForSingleObject w32p_WaitForSingleObject
#define WaitForMultipleObjects w32p_WaitForMultipleObjects
#define CloseHandle w32p_CloseHandle
#define GetThreadId w32p_GetThreadId
#define SetWaitableTimer w32p_SetWaitableTimer
#define CancelWaitableTimer w32p_CancelWaitableTimer
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 2
#define TIMER_ALL_ACCESS 0x1f0003
#define CreateWaitableTimerExW(sa, name, flags, access) w32p_CreateWaitableTimerA(sa, 0, NULL)
#define CreateWaitableTimerW(sa, manual, name) w32p_CreateWaitableTimerA(sa, manual, NULL)
#define WAIT_FAILED 0xffffffffu
#endif
