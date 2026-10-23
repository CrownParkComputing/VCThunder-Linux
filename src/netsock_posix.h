/* netsock_posix.h -- the Winsock 2 and IP-Helper surface src/net_link.c and
 * tests/net_link_loopback_test.c are written against, on Linux.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * net_link.c is an unchanged upstream source that speaks Winsock: it resolves
 * socket/bind/sendto/recvfrom/setsockopt/WSASetLastError/WSAIoctl out of
 * ws2_32.dll and GetAdaptersInfo out of iphlpapi.dll, and it never links a
 * socket import directly (docs/building.md's rule about the host DLL's import
 * table). vcthunder.h keeps those winsock types inside net_link.c on purpose,
 * so this header is included ONLY from net_link.c and stays just as local.
 *
 * This is the vocabulary, exactly as dinput_posix.h and xinput_posix.h are for
 * their tiers: it declares the types, constants and function names net_link.c
 * references, and netsock_posix.c implements them. It is NOT a shim that fakes
 * an answer -- every call below lands on a real BSD socket, a real errno, or a
 * real getifaddrs() walk, because net_link_loopback_test asserts on behaviour
 * over live sockets and a header that quietly lied would turn that test into
 * decoration.
 *
 * WHY BSD SOCKETS ARE A FAITHFUL BACKING HERE AND NOT A APPROXIMATION: the four
 * entry points net_link overrides (socket/bind/sendto/recvfrom) mean the same
 * thing on both platforms, and the two things Windows does that Linux does not
 * are handled where they belong rather than faked at this boundary:
 *
 *   1. WSAEWOULDBLOCK == EWOULDBLOCK == EAGAIN on glibc, so the receive
 *      budget's refusal and the games' drain loops read the same code. The
 *      error codes below are the errno values, not invented numbers, so a
 *      Winsock error that has a POSIX twin IS that twin and the two stay in
 *      agreement for the whole set the host branches on.
 *
 *   2. Windows reports an ICMP port-unreachable provoked by a datagram WE SENT
 *      as WSAECONNRESET on the next receive (hence SIO_UDP_CONNRESET). Linux
 *      only surfaces ICMP errors on a CONNECTED socket, and both titles send
 *      with sendto on an unconnected socket, so there is genuinely no reset to
 *      suppress. posix_wsa_ioctl() answers that request with success rather
 *      than a fake: the behaviour it asks for is already the Linux default.
 */
#ifndef VCTH_NETSOCK_POSIX_H
#define VCTH_NETSOCK_POSIX_H


#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <errno.h>
#include <string.h>

#include "posix_compat.h"

/* ---- the socket handle --------------------------------------------------- *
/
 * Winsock's SOCKET is an unsigned handle whose sentinel is ~0; a BSD socket is
 * a signed fd whose error return is -1. They are made the same object here:
 * INVALID_SOCKET is -1, which is exactly what a failed socket() returns, so the
 * `s == INVALID_SOCKET` test net_link.c writes behaves without a translation.
 * Printing a handle as `(unsigned long long)s` (which net_link.c does in its
 * trace) still shows the fd, and only ever shows -1 for a refused socket. */
typedef int SOCKET;

#define INVALID_SOCKET  ((SOCKET)-1)
#define SOCKET_ERROR    (-1)

/* ---- address families, protocols, options -------------------------------- *
/
 * Where a POSIX twin exists it is used verbatim, so a value that leaks into the
 * wire or into a setsockopt call is the kernel's own and cannot drift. Only the
 * Winsock spellings that have no POSIX name (WSA*) are given numbers here. */
#ifndef AF_INET
#define AF_INET  2
#endif
#ifndef SOCK_DGRAM
#define SOCK_DGRAM 2
#endif
#ifndef IPPROTO_UDP
#define IPPROTO_UDP 17
#endif
#ifndef SOL_SOCKET
#define SOL_SOCKET 1
#endif
#ifndef SO_BROADCAST
#define SO_BROADCAST 6
#endif

#ifndef INADDR_ANY
#define INADDR_ANY ((unsigned int)0x00000000u)
#endif

/* ---- Winsock error codes the host branches on ---------------------------- *
/
 * WSAEWOULDBLOCK is the one that changes control flow (the receive budget and
 * both titles' drain loops). It is EWOULDBLOCK so the code a real EAGAIN recv
 * maps to, and the code the budget synthesises, are the SAME number and equal
 * the value the games compare against. The rest are carried for completeness
 * and set from errno where a twin exists. */
#define WSA_SUCCESS            0
#define WSAEWOULDBLOCK         10035
#define WSAECONNRESET          10054
#define WSAEACCES              10013
#define WSAEINVAL              10022
#define WSAENOTCONN            10057
#define WSAEADDRINUSE          10048
#define WSAECONNREFUSED        10061
#define WSAHOST_NOT_FOUND      11001

/* ---- the ioctl socket makes non-blocking --------------------------------- *
/
 * FIONBIO is the request ioctlsocket() carries; it is the same number as the
 * kernel's, obtained from the system if available. Winsock's ioctlsocket(fd,
 * FIONBIO, &u_long) is reproduced by posix_ioctlsocket() over fcntl(F_SETFL,
 * O_NONBLOCK), which is the operation Windows itself performs underneath. */
#ifndef FIONBIO
#define FIONBIO 0x5421u
#endif

/* ---- SIO_UDP_CONNRESET, and the macros that build its id ----------------- *
/
 * net_link.c defines SIO_UDP_CONNRESET itself via _WSAIOW(IOC_VENDOR, 12) when
 * the platform header has not. It supplies both halves here so that definition
 * compiles unchanged; the resulting number is only ever passed back into
 * posix_wsa_ioctl(), which recognises it and answers success. */
#ifndef IOC_VENDOR
#define IOC_VENDOR  0x18000000u
#endif
#ifndef _WSAIOW
#define _WSAIOW(x, n)   ((unsigned long)(((x) | (n)) + 0x80000000u))
#endif
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

/* ---- WSADATA, WSAStartup, WSA(Async)Event -------------------------------- *
/
 * The Win32 startup handshake has no POSIX meaning, so WSADATA is reported as
 * version 2.2 and startup succeeds unconditionally: there is no socket library
 * to initialise on glibc. WSAStartup returning 0 is what makes net_link's
 * caller treat the link as available, which is honest because the sockets are
 * genuinely available. */
typedef struct {
    unsigned short wVersion;
    unsigned short wHighVersion;
    unsigned short iSocketStatus;
    unsigned short wMaxSockets;
    char szSysStatus[70];
    char szDescription[128];
    char szDriver[128];
} WSADATA;

#ifndef MAKEWORD
#define MAKEWORD(a, b) \
    ((unsigned short)(((unsigned char)(a)) | ((unsigned short)((unsigned char)(b))) << 8))
#endif

int WINAPI  WSAStartup(unsigned short wVersionRequested, WSADATA *wsaData);
int WINAPI  WSACleanup(void);

/* The thread's last Winsock error. posix_wsa_set_last_error() is the same
 * function under the name net_link.c assigns to its s_wsa_set_last_error
 * pointer, so the budget's synthesised WSAEWOULDBLOCK and a real recv's mapped
 * errno land in one place; WSASetLastError is the ordinary spelling. */
int WINAPI  WSAGetLastError(void);
void WINAPI WSASetLastError(int error);
void WINAPI posix_wsa_set_last_error(int error);

/* ---- the socket entry points net_link resolves and calls through --------- *
/
 * Declared with WINAPI (which posix_compat defines empty, i.e. cdecl on this
 * target -- matching the calling convention of the real libc calls and of the
 * ws2_32 exports net_link resolves on Windows). Each is a thin wrapper around
 * the libc call that additionally records the resulting errno as a Winsock
 * code into the thread's last-error, so WSAGetLastError() after a failed recv
 * returns WSAEWOULDBLOCK for EAGAIN exactly as Winsock would. net_link.c
 * assigns these straight to its function pointers, mirroring dinput.c's
 * `s_create = posix_di_create` -- the indirection is kept so the failure paths
 * stay identical to Windows'. */
SOCKET WINAPI posix_wsa_socket(int af, int type, int protocol);
int WINAPI    posix_wsa_bind(SOCKET s, const struct sockaddr *name, int namelen);
int WINAPI    posix_wsa_sendto(SOCKET s, const char *buf, int len, int flags,
                        const struct sockaddr *to, int tolen);
int WINAPI    posix_wsa_recvfrom(SOCKET s, char *buf, int len, int flags,
                          struct sockaddr *from, int *fromlen);
int WINAPI    posix_wsa_setsockopt(SOCKET s, int level, int optname,
                            const char *optval, int optlen);
void WINAPI   posix_wsa_set_last_error(int error);
int WINAPI    posix_wsa_ioctl(SOCKET s, unsigned long code, void *inbuf,
                       unsigned long inlen, void *outbuf, unsigned long outlen,
                       unsigned long *bytes_returned, void *reserved1,
                       void *reserved2);

/* Closes a socket. Winsock's closesocket() and the BSD close() are the same
 * call on this platform; named so net_link's test reads like Winsock. */
int WINAPI posix_closesocket(SOCKET s);
#define closesocket(s) posix_closesocket(s)

/* ioctlsocket(s, FIONBIO, &onoff): the only form the host and test use. */
int WINAPI posix_ioctlsocket(SOCKET s, long cmd, unsigned long *argp);
void *netsock_posix_resolve(const char *name);
#define ioctlsocket(s, c, a) posix_ioctlsocket((s), (c), (a))

/* ---- IP Helper: the adapter list net_link reads for its own address ------ *
/
 * GetAdaptersInfo is reproduced over getifaddrs(). The struct is the real
 * Win32 IP_ADAPTER_INFO, field for field for everything net_link.c reads:
 * Next, Type, AddressLength/Address, Description, and the embedded
 * IpAddressList / GatewayList string triples. Those strings are fixed arrays
 * inside the record on Windows too, so each adapter is exactly
 * sizeof(IP_ADAPTER_INFO) contiguous and the two-call size/fill protocol the
 * Windows code follows works unchanged. */
#define MAX_ADAPTER_ADDRESS_LENGTH      8
#define MAX_ADAPTER_DESCRIPTION_LENGTH  128
#define MAX_ADAPTER_NAME_LENGTH         256
#define MAX_DOMAIN_NAME_LENGTH          256

#define MIB_IF_TYPE_OTHER     1
#define MIB_IF_TYPE_ETHERNET  6
#define MIB_IF_TYPE_LOOPBACK  24

typedef unsigned long  ULONG;
typedef ULONG *PULONG;

typedef struct _IP_ADDRESS_STRING { char String[16]; } IP_ADDRESS_STRING;
typedef struct _IP_MASK_STRING    { char String[16]; } IP_MASK_STRING;
typedef IP_ADDRESS_STRING         IP_NODE_STRING;

typedef struct _IP_STRING {
    IP_ADDRESS_STRING IpAddress;
    IP_MASK_STRING    IpMask;
    unsigned long     Context;
} IP_STRING;

typedef struct _IP_ADAPTER_INFO {
    struct _IP_ADAPTER_INFO *Next;
    unsigned long      Context;
    unsigned int       Index;
    unsigned long      Type;
    unsigned long      DhcpEnabled;
    void              *AdapterId;               /* the GUID*, never read here */
    unsigned long      AddressLength;
    unsigned char      Address[MAX_ADAPTER_ADDRESS_LENGTH];
    unsigned long      Reserved[8];
    char               AdapterName[MAX_ADAPTER_NAME_LENGTH + 4];
    char               Description[MAX_ADAPTER_DESCRIPTION_LENGTH + 4];
    char               PrimaryDomainId[MAX_DOMAIN_NAME_LENGTH + 4];
    IP_STRING          IpAddressList;
    IP_STRING          GatewayList;
    IP_STRING          DhcpServer;
    unsigned char     *PhysicalAddress;          /* points at Address[] below */
    unsigned int       PhysicalAddressLength;
} IP_ADAPTER_INFO, *PIP_ADAPTER_INFO;

/* ERROR_BUFFER_OVERFLOW / ERROR_SUCCESS as GetAdaptersInfo reports them. These
 * are the Win32 codes net_link.c compares against, given their exact values so
 * the size-then-fill protocol branches the same way it does on Windows. */
#ifndef ERROR_SUCCESS
#define ERROR_SUCCESS         0L
#endif
#ifndef ERROR_BUFFER_OVERFLOW
#define ERROR_BUFFER_OVERFLOW 111L
#endif

/* The two-call GetAdaptersInfo, over getifaddrs() and the default route read
 * from /proc/net/route. Called via get_adapters_info_fn, whose signature this
 * matches: ULONG (WINAPI *)(PIP_ADAPTER_INFO, PULONG). */
unsigned long WINAPI posix_get_adapters_info(PIP_ADAPTER_INFO adapter_info,
                                      PULONG out_buffer_len);

#endif /* VCTH_NETSOCK_POSIX_H */
