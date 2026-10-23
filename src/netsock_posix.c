/* netsock_posix.c -- Winsock and IP Helper on Linux, over BSD sockets and
 * getifaddrs(). The implementation behind netsock_posix.h's vocabulary.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Port code, so it is held to the same -Wall -Wextra -Wshadow -Werror as the
 * rest of the Linux tier with nothing waived. Every entry point here is a real
 * POSIX implementation rather than a plausible answer: net_link.c drives them
 * over live sockets in net_link_loopback_test, and a wrapper that quietly lied
 * would make that test prove nothing.
 */
#include "netsock_posix.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <fcntl.h>
#include <netpacket/packet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static unsigned long WINAPI game_htonl(unsigned long v) { return htonl(v); }
static unsigned short WINAPI game_htons(unsigned short v) { return htons(v); }
static unsigned long WINAPI game_inet_addr(const char *s) { return inet_addr(s); }

void *netsock_posix_resolve(const char *name)
{
    static const struct { const char *name; void *fn; } bindings[] = {
        { "WSAStartup", WSAStartup }, { "WSACleanup", WSACleanup },
        { "WSAGetLastError", WSAGetLastError },
        { "socket", posix_wsa_socket }, { "bind", posix_wsa_bind },
        { "closesocket", posix_closesocket }, { "ioctlsocket", posix_ioctlsocket },
        { "sendto", posix_wsa_sendto }, { "recvfrom", posix_wsa_recvfrom },
        { "htonl", game_htonl }, { "htons", game_htons },
        { "ntohl", game_htonl }, { "ntohs", game_htons },
        { "inet_addr", game_inet_addr },
    };
    unsigned i;
    for (i = 0; i < sizeof bindings / sizeof bindings[0]; i++)
        if (strcmp(name, bindings[i].name) == 0)
            return bindings[i].fn;
    return NULL;
}

/* The thread's last Winsock error. Win32 keeps it per thread; a failed call
 * records the mapped errno here and WSAGetLastError reads it back, so the one
 * value the host's control flow depends on (WSAEWOULDBLOCK) behaves the way the
 * games' drain loops expect. Success does not clear it, exactly as Win32. */
static __thread int s_wsa_last;

/* errno -> Winsock. The overlaps are given their Winsock twins so a code that
 * reaches the games means the same thing here as on Windows; WSAEWOULDBLOCK is
 * the one that changes behaviour, and EAGAIN/EWOULDBLOCK are the same value on
 * glibc anyway, so the mapping is a clarification rather than a translation. */
static int map_errno(int e)
{
    switch (e) {
    case 0:             return WSA_SUCCESS;
    /* EWOULDBLOCK and EAGAIN are the same value on glibc, so only one case is
     * written; it is the code the receive budget and the drain loops key on. */
    case EWOULDBLOCK:   return WSAEWOULDBLOCK;
    case EALREADY:      return WSAEWOULDBLOCK;
    case EINPROGRESS:   return WSAEWOULDBLOCK;
    case ECONNRESET:    return WSAECONNRESET;
    case ECONNREFUSED:  return WSAECONNREFUSED;
    case EACCES:        return WSAEACCES;
    case EPERM:         return WSAEACCES;
    case EINVAL:        return WSAEINVAL;
    case ENOTCONN:      return WSAENOTCONN;
    case EADDRINUSE:    return WSAEADDRINUSE;
    default:            return e;        /* nonzero, so it still reads as error */
    }
}

static void record_error(void)
{
    s_wsa_last = map_errno(errno);
}

/* ---- the socket entry points net_link calls through its pointers --------- */

SOCKET WINAPI posix_wsa_socket(int af, int type, int protocol)
{
    int s = socket(af, type, protocol);

    if (s < 0)
        record_error();
    return (SOCKET)s;
}

int WINAPI posix_wsa_bind(SOCKET s, const struct sockaddr *name, int namelen)
{
    int rc = bind(s, name, (socklen_t)namelen);

    if (rc < 0)
        record_error();
    return rc;
}

int WINAPI posix_wsa_sendto(SOCKET s, const char *buf, int len, int flags,
                     const struct sockaddr *to, int tolen)
{
    int rc = (int)sendto(s, buf, (size_t)len, flags, to, (socklen_t)tolen);

    if (rc < 0)
        record_error();
    return rc;
}

int WINAPI posix_wsa_recvfrom(SOCKET s, char *buf, int len, int flags,
                       struct sockaddr *from, int *fromlen)
{
    socklen_t fl = from ? (socklen_t)*fromlen : 0;
    int rc = (int)recvfrom(s, buf, (size_t)len, flags, from,
                           from ? &fl : NULL);

    if (rc < 0) {
        record_error();
        return SOCKET_ERROR;
    }
    if (from && fromlen)
        *fromlen = (int)fl;
    return rc;
}

int WINAPI posix_wsa_setsockopt(SOCKET s, int level, int optname,
                         const char *optval, int optlen)
{
    int rc = setsockopt(s, level, optname, optval, (socklen_t)optlen);

    if (rc < 0)
        record_error();
    return rc;
}

void WINAPI posix_wsa_set_last_error(int error)
{
    s_wsa_last = error;
}

void WINAPI WSASetLastError(int error)
{
    s_wsa_last = error;
}

int WINAPI WSAGetLastError(void)
{
    return s_wsa_last;
}

/* WSAIoctl, as net_link.c uses it: only ever SIO_UDP_CONNRESET, to stop a
 * datagram sent to an absent unit surfacing as WSAECONNRESET on the next
 * receive. On Linux an UNCONNECTED datagram socket does not receive ICMP
 * errors at all -- that behaviour requires connect() -- and both titles send
 * with sendto on an unconnected socket, so there is nothing to suppress and the
 * requested state already holds. Answering success is therefore the honest
 * reply, not a stub that papers over an unimplemented call. */
int WINAPI posix_wsa_ioctl(SOCKET s, unsigned long code, void *inbuf,
                    unsigned long inlen, void *outbuf, unsigned long outlen,
                    unsigned long *bytes_returned, void *reserved1,
                    void *reserved2)
{
    (void)s; (void)inbuf; (void)inlen; (void)outbuf; (void)outlen;
    (void)reserved1; (void)reserved2;

    if (bytes_returned)
        *bytes_returned = 0;
    /* Only SIO_UDP_CONNRESET is defined here; anything else is reported as
     * unhandled rather than silently "succeeded", so a future caller does not
     * mistake a no-op for work done. */
    return code == SIO_UDP_CONNRESET ? 0 : (int)SOCKET_ERROR;
}

int WINAPI posix_closesocket(SOCKET s)
{
    return close(s);
}

/* ioctlsocket(s, FIONBIO, &onoff): the non-blocking flag, which is what the
 * games set before their drain loops and what makes a would-block recv return
 * WSAEWOULDBLOCK rather than stop the frame. */
int WINAPI posix_ioctlsocket(SOCKET s, long cmd, unsigned long *argp)
{
    int flags;

    /* ETS calls with the Winsock request encoded in the original executable;
     * Linux callers use the native ioctl number. Both mean nonblocking I/O. */
    if (((uint32_t)cmd != 0x8004667eu && cmd != (long)FIONBIO) || !argp)
        return (int)SOCKET_ERROR;
    flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) {
        record_error();
        return (int)SOCKET_ERROR;
    }
    if (*argp)
        flags |= O_NONBLOCK;
    else
        flags &= ~O_NONBLOCK;
    if (fcntl(s, F_SETFL, flags) < 0) {
        record_error();
        return (int)SOCKET_ERROR;
    }
    return 0;
}

/* ---- WSAStartup / WSACleanup -------------------------------------------- *
/
 * No socket library to bring up on glibc, so startup always succeeds and
 * reports version 2.2 -- which is exactly the interface net_link asked for and
 * the reason its caller treats the link as available. */
int WINAPI WSAStartup(unsigned short wVersionRequested, WSADATA *wsaData)
{
    if (wsaData) {
        memset(wsaData, 0, sizeof *wsaData);
        wsaData->wVersion = wVersionRequested;
        wsaData->wHighVersion = wVersionRequested;
        snprintf(wsaData->szDescription, sizeof wsaData->szDescription,
                 "BSD sockets (vcthunder linux net tier)");
    }
    s_wsa_last = WSA_SUCCESS;
    return 0;
}

int WINAPI WSACleanup(void)
{
    return 0;
}

/* ---- GetAdaptersInfo over getifaddrs() ---------------------------------- *
/
 * The two-call protocol the Windows code follows: a first call with a NULL
 * buffer returns ERROR_BUFFER_OVERFLOW and the needed size, then a second call
 * fills a buffer of that size and returns ERROR_SUCCESS. Only interfaces that
 * carry an IPv4 address are listed, because net_link.c skips any adapter whose
 * address does not parse -- and a machine on which the only IPv4 address is on
 * loopback correctly finds no usable adapter and falls back to 127.0.0.1, the
 * same way it would on Windows. */

struct iface {
    char          name[IF_NAMESIZE];
    char          ip[16];
    char          mask[16];
    unsigned char mac[6];
    unsigned      mac_len;
    unsigned long flags;
    int           has_mac;
    int           is_default_gw;
};

#define MAX_IFACES 64

/* A bounded string copy; defined further down, used by both the route reader
 * and the adapter fill. */
static void copy_str(char *dst, size_t n, const char *src);

/* The dotted-quad form of an AF_INET sockaddr, or empty if the pointer is not
 * one. Written into a 16-byte buffer, which is INET_ADDRSTRLEN for IPv4. */
static void addr_to_string(const struct sockaddr *sa, char *out, size_t n)
{
    const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;

    out[0] = '\0';
    if (!sa || sa->sa_family != AF_INET)
        return;
    inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)n);
}

/* The default route's interface name and gateway, from /proc/net/route. The
 * Destination and Gateway columns are little-endian hex of the address as it
 * sits in network order, so "00000000" destination is the default route and a
 * gateway hex value can be handed straight to inet_ntop after splitting its
 * four bytes. On the first default route we win, which is what a single-homed
 * cabinet has; a machine with several has no single "the" gateway anyway. */
static void find_default_route(char *iface, size_t iface_n, char *gw, size_t gw_n)
{
    FILE *f = fopen("/proc/net/route", "r");
    char line[256];

    iface[0] = '\0';
    gw[0] = '\0';
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char   name[64];
        unsigned long dest, gate;

        if (sscanf(line, "%63s %lx %lx", name, &dest, &gate) != 3)
            continue;
        if (dest != 0 || gate == 0)
            continue;                       /* not the default route, or no gw */
        copy_str(iface, iface_n, name);
        {
            struct sockaddr_in sin;
            memset(&sin, 0, sizeof sin);
            sin.sin_family = AF_INET;
            sin.sin_addr.s_addr = (uint32_t)gate;   /* already network order */
            inet_ntop(AF_INET, &sin.sin_addr, gw, (socklen_t)gw_n);
        }
        break;
    }
    fclose(f);
}

/* One getifaddrs pass into a flat iface array. Returns the count, or -1 if
 * getifaddrs itself failed (which net_link treats as "cannot read this
 * machine's address" and refuses to answer ether0 with a guess). */
static int collect_ifaces(struct iface *arr, int cap)
{
    struct ifaddrs *ifap = NULL, *p;
    char gw_iface[64], gw_addr[16];
    int count = 0;

    find_default_route(gw_iface, sizeof gw_iface, gw_addr, sizeof gw_addr);

    if (getifaddrs(&ifap) != 0)
        return -1;

    /* First pass: every interface with an IPv4 address. */
    for (p = ifap; p && count < cap; p = p->ifa_next) {
        int i;

        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
            continue;
        /* Skip a second IPv4 on an interface already recorded. */
        for (i = 0; i < count; i++)
            if (strcmp(arr[i].name, p->ifa_name) == 0)
                break;
        if (i < count)
            continue;

        memset(&arr[count], 0, sizeof arr[count]);
        snprintf(arr[count].name, sizeof arr[count].name, "%s", p->ifa_name);
        arr[count].flags = (unsigned long)p->ifa_flags;
        addr_to_string(p->ifa_addr, arr[count].ip, sizeof arr[count].ip);
        addr_to_string(p->ifa_netmask, arr[count].mask, sizeof arr[count].mask);
        if (gw_iface[0] && strcmp(arr[count].name, gw_iface) == 0) {
            arr[count].is_default_gw = 1;
        }
        count++;
    }

    /* Second pass: attach each interface's hardware address (AF_PACKET entry),
     * which Windows reports inline and net_link wants in the same record. */
    for (p = ifap; p; p = p->ifa_next) {
        int i;

        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_PACKET)
            continue;
        for (i = 0; i < count; i++) {
            const struct sockaddr_ll *sll;

            if (strcmp(arr[i].name, p->ifa_name) != 0)
                continue;
            sll = (const struct sockaddr_ll *)p->ifa_addr;
            if (arr[i].has_mac)
                break;
            if (sll->sll_halen > 0 && sll->sll_halen <= (unsigned char)6) {
                memcpy(arr[i].mac, sll->sll_addr, sll->sll_halen);
                arr[i].mac_len = sll->sll_halen;
                arr[i].has_mac = 1;
            }
            break;
        }
    }

    freeifaddrs(ifap);
    return count;
}

/* A bounded string copy. Used instead of snprintf("%s") so the strict port
 * build does not have to waive -Wformat-truncation: the source is a short,
 * NUL-terminated name that always fits, and copying it by hand states that as
 * plainly as any format precision. */
static void copy_str(char *dst, size_t n, const char *src)
{
    size_t i = 0;

    if (!n)
        return;
    for (; i + 1 < n && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Fill one record from a collected interface. The gateway string is written
 * only on the default-route adapter, so net_link's "one with a gateway wins"
 * branch behaves: every other adapter parses 0 and is passed over. */
static void fill_adapter(PIP_ADAPTER_INFO a, const struct iface *ifc,
                         const char *gw_addr)
{
    int loop = (ifc->flags & (unsigned long)IFF_LOOPBACK) != 0;

    memset(a, 0, sizeof *a);
    a->Next = NULL;
    a->Index = 0;
    a->Context = 0;
    a->Type = loop ? MIB_IF_TYPE_LOOPBACK
                   : (ifc->has_mac ? MIB_IF_TYPE_ETHERNET : MIB_IF_TYPE_OTHER);
    a->AddressLength = ifc->has_mac ? ifc->mac_len : 0;
    if (ifc->has_mac)
        memcpy(a->Address, ifc->mac, ifc->mac_len);
    a->PhysicalAddress = a->Address;
    a->PhysicalAddressLength = ifc->mac_len;
    copy_str(a->AdapterName, sizeof a->AdapterName, ifc->name);
    copy_str(a->Description, sizeof a->Description, ifc->name);
    copy_str(a->IpAddressList.IpAddress.String,
             sizeof a->IpAddressList.IpAddress.String, ifc->ip);
    copy_str(a->IpAddressList.IpMask.String,
             sizeof a->IpAddressList.IpMask.String, ifc->mask);
    if (ifc->is_default_gw && gw_addr && gw_addr[0])
        copy_str(a->GatewayList.IpAddress.String,
                 sizeof a->GatewayList.IpAddress.String, gw_addr);
    else
        copy_str(a->GatewayList.IpAddress.String,
                 sizeof a->GatewayList.IpAddress.String, "0.0.0.0");
}

unsigned long WINAPI posix_get_adapters_info(PIP_ADAPTER_INFO adapter_info,
                                      PULONG out_buffer_len)
{
    struct iface arr[MAX_IFACES];
    char gw_addr[16];
    int  count, i;
    unsigned long need;

    if (!out_buffer_len)
        return ERROR_BUFFER_OVERFLOW;

    count = collect_ifaces(arr, MAX_IFACES);
    if (count < 0)
        return ERROR_BUFFER_OVERFLOW;       /* getifaddrs failed: no adapter data */

    /* Re-derive the gateway string for the fill pass (find_default_route read
     * it for the marking pass; ask again so the two stay consistent). */
    {
        char dummy[64];
        find_default_route(dummy, sizeof dummy, gw_addr, sizeof gw_addr);
    }

    need = (unsigned long)count * (unsigned long)sizeof(IP_ADAPTER_INFO);

    /* The sizing call, or a buffer that is genuinely too small. */
    if (!adapter_info || *out_buffer_len < need) {
        *out_buffer_len = need;
        return count ? ERROR_BUFFER_OVERFLOW : ERROR_SUCCESS;
    }

    for (i = 0; i < count; i++) {
        PIP_ADAPTER_INFO a = adapter_info + i;
        fill_adapter(a, &arr[i], gw_addr);
        a->Next = (i + 1 < count) ? adapter_info + i + 1 : NULL;
    }
    *out_buffer_len = need;
    return ERROR_SUCCESS;
}
