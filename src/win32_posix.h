/* win32_posix.h -- the Linux half of the ETS kernel boundary (win32_bind.c).
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * On Windows, win32_bind.c's resolve() answers the game's ETS import table from
 * kernel32/user32/ws2_32/ntdll via GetProcAddress. On Linux there is no
 * kernel32 to bind to, so the same names resolve against win32_posix.c, which
 * implements the Win32 surface the two titles actually import (handover item
 * 10). The Winsock names are already resolved through net_link.c's seam; the
 * rest are here.
 *
 * The functions are __stdcall (callee-cleans) so a pointer installed into an
 * __imp__ slot is entered with the convention the game's code expects -- the
 * same coupling between item 10 and item 12 that the handover records.
 */
#ifndef VCTHUNDER_WIN32_POSIX_H
#define VCTHUNDER_WIN32_POSIX_H

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve an UNDECORATED name ("CreateThread") to a host function, or NULL if
 * this module does not implement it yet. win32_bind.c's resolve() calls this
 * on Linux in place of GetProcAddress. */
void *win32_posix_resolve(const char *base);

#ifdef __cplusplus
}
#endif

#endif /* VCTHUNDER_WIN32_POSIX_H */
