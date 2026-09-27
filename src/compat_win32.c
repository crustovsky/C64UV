// Windows (Winsock) implementation of compat.h. Built with MinGW
// (make TARGET=win32); libraries: ws2_32, iphlpapi.
#define _WIN32_WINNT 0x0600 // Vista+: inet_pton, WSAPoll, OnLinkPrefixLength

#include "compat.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <stdio.h>
#include <string.h>

bool compat_net_init(void)
{
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

void compat_net_quit(void)
{
    WSACleanup();
}

bool compat_ipv4_parse(const char *s, uint32_t *addr)
{
    struct in_addr a;
    if (!s || inet_pton(AF_INET, s, &a) != 1)
        return false;
    *addr = ntohl(a.s_addr);
    return true;
}

const char *compat_ipv4_format(uint32_t addr, char *out, size_t cap)
{
    struct in_addr a = {.s_addr = htonl(addr)};
    if (!inet_ntop(AF_INET, &a, out, cap) && cap)
        out[0] = '\0';
    return out;
}

static bool sockaddr_from(const char *ip, uint16_t port,
                          struct sockaddr_in *sa)
{
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_port = htons(port);
    return ip && inet_pton(AF_INET, ip, &sa->sin_addr) == 1;
}

compat_sock compat_udp_bind(uint16_t port, int rcvbuf, bool reuse)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET)
        return COMPAT_BAD_SOCK;
    BOOL one = TRUE;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof rcvbuf);
    if (reuse)
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in sa = {.sin_family = AF_INET,
                             .sin_addr.s_addr = htonl(INADDR_ANY),
                             .sin_port = htons(port)};
    if (bind(s, (struct sockaddr *)&sa, sizeof sa) != 0) {
        int saved = WSAGetLastError();
        closesocket(s);
        WSASetLastError(saved);
        return COMPAT_BAD_SOCK;
    }
    return (compat_sock)s;
}

bool compat_mcast_join(compat_sock s, const char *group, const char *ifip)
{
    struct ip_mreq m = {0};
    if (inet_pton(AF_INET, group, &m.imr_multiaddr) != 1)
        return false;
    if (ifip)
        inet_pton(AF_INET, ifip, &m.imr_interface);
    return setsockopt((SOCKET)s, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                      (const char *)&m, sizeof m) == 0;
}

// Winsock's SO_SNDTIMEO does not bound connect(), so the connect runs
// non-blocking against a select() timeout; the socket then goes back to
// blocking with a send timeout for the later sends.
compat_sock compat_tcp_connect(const char *ip, uint16_t port, int timeout_s)
{
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET)
        return COMPAT_BAD_SOCK;
    struct sockaddr_in sa;
    u_long nb = 1;
    bool ok = sockaddr_from(ip, port, &sa) && ioctlsocket(fd, FIONBIO, &nb) == 0;
    if (ok) {
        connect(fd, (struct sockaddr *)&sa, sizeof sa); // WSAEWOULDBLOCK
        fd_set w, e;
        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET(fd, &w);
        FD_SET(fd, &e);
        struct timeval tv = {.tv_sec = timeout_s};
        ok = select(0, NULL, &w, &e, &tv) == 1 && FD_ISSET(fd, &w);
        if (!ok)
            WSASetLastError(WSAETIMEDOUT);
    }
    if (ok) {
        nb = 0;
        ioctlsocket(fd, FIONBIO, &nb);
        DWORD ms = (DWORD)timeout_s * 1000;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof ms);
        return (compat_sock)fd;
    }
    int saved = WSAGetLastError();
    closesocket(fd);
    WSASetLastError(saved);
    return COMPAT_BAD_SOCK;
}

int compat_send(compat_sock s, const void *buf, size_t len)
{
    int n = send((SOCKET)s, buf, (int)len, 0); // no SIGPIPE on Windows
    return n == SOCKET_ERROR ? -1 : n;
}

int compat_sendto(compat_sock s, const void *buf, size_t len, const char *ip,
                  uint16_t port)
{
    struct sockaddr_in sa;
    if (!sockaddr_from(ip, port, &sa))
        return -1;
    int n = sendto((SOCKET)s, buf, (int)len, 0, (struct sockaddr *)&sa,
                   sizeof sa);
    return n == SOCKET_ERROR ? -1 : n;
}

int compat_recv_nowait(compat_sock s, void *buf, size_t cap)
{
    // no MSG_DONTWAIT: a zero-timeout readiness check stands in for it
    if (compat_wait_readable(&s, 1, 0) != 1) {
        WSASetLastError(WSAEWOULDBLOCK);
        return -1;
    }
    int n = recv((SOCKET)s, buf, (int)cap, 0);
    return n == SOCKET_ERROR ? -1 : n;
}

int compat_wait_readable(const compat_sock *socks, int n, int timeout_ms)
{
    WSAPOLLFD pfd[8];
    if (n > 8)
        n = 8;
    for (int i = 0; i < n; i++)
        pfd[i] = (WSAPOLLFD){.fd = (SOCKET)socks[i], .events = POLLRDNORM};
    // INVALID_SOCKET entries read as negative and are ignored, like poll()
    return WSAPoll(pfd, (ULONG)n, timeout_ms);
}

int compat_wait_writable(compat_sock s, int timeout_ms)
{
    WSAPOLLFD pfd = {.fd = (SOCKET)s, .events = POLLWRNORM};
    return WSAPoll(&pfd, 1, timeout_ms);
}

void compat_close(compat_sock s)
{
    if (s != COMPAT_BAD_SOCK)
        closesocket((SOCKET)s);
}

const char *compat_neterr(void)
{
    static char msg[256];
    int err = WSAGetLastError();
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, (DWORD)err, 0, msg, sizeof msg, NULL);
    if (n == 0)
        snprintf(msg, sizeof msg, "Winsock error %d", err);
    else
        while (n > 0 && (msg[n - 1] == '\r' || msg[n - 1] == '\n'))
            msg[--n] = '\0';
    return msg;
}

bool compat_neterr_transient(void)
{
    int err = WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAETIMEDOUT || err == WSAEINTR;
}

bool compat_route_source_ip(const char *ip, char *out, size_t cap)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET)
        return false;
    struct sockaddr_in sa;
    bool ok = sockaddr_from(ip, 80, &sa) &&
              connect(s, (struct sockaddr *)&sa, sizeof sa) == 0;
    if (ok) {
        struct sockaddr_in local;
        int len = sizeof local;
        ok = getsockname(s, (struct sockaddr *)&local, &len) == 0 &&
             inet_ntop(AF_INET, &local.sin_addr, out, cap) != NULL;
    }
    closesocket(s);
    return ok;
}

int compat_ifaces(struct compat_iface *out, int max)
{
    ULONG size = 16384;
    IP_ADAPTER_ADDRESSES *list = malloc(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                  GAA_FLAG_SKIP_DNS_SERVER;
    if (list && GetAdaptersAddresses(AF_INET, flags, NULL, list, &size) ==
                    ERROR_BUFFER_OVERFLOW) {
        free(list);
        list = malloc(size);
    }
    if (!list || GetAdaptersAddresses(AF_INET, flags, NULL, list, &size) !=
                     NO_ERROR) {
        free(list);
        return 0;
    }
    int n = 0;
    for (IP_ADAPTER_ADDRESSES *a = list; a && n < max; a = a->Next) {
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress;
             u && n < max; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            struct compat_iface *o = &out[n++];
            memset(o, 0, sizeof *o);
            WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, o->name,
                                sizeof o->name - 1, NULL, NULL);
            o->addr = ntohl(((struct sockaddr_in *)u->Address.lpSockaddr)
                                ->sin_addr.s_addr);
            int pl = u->OnLinkPrefixLength;
            o->mask = pl > 0 && pl <= 32 ? ~0u << (32 - pl) : 0;
            o->up = a->OperStatus == IfOperStatusUp;
            o->loopback = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
            o->wireless = a->IfType == IF_TYPE_IEEE80211;
        }
    }
    free(list);
    return n;
}

bool compat_neighbor_mac(const char *ip, char *out, size_t cap)
{
    uint32_t addr;
    if (!compat_ipv4_parse(ip, &addr))
        return false;
    ULONG size = 0;
    if (GetIpNetTable(NULL, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER)
        return false;
    MIB_IPNETTABLE *t = malloc(size);
    bool found = false;
    if (t && GetIpNetTable(t, &size, FALSE) == NO_ERROR) {
        for (DWORD i = 0; i < t->dwNumEntries && !found; i++) {
            MIB_IPNETROW *r = &t->table[i];
            if (ntohl(r->dwAddr) != addr || r->dwPhysAddrLen != 6 ||
                r->dwType == MIB_IPNET_TYPE_INVALID)
                continue;
            snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x",
                     r->bPhysAddr[0], r->bPhysAddr[1], r->bPhysAddr[2],
                     r->bPhysAddr[3], r->bPhysAddr[4], r->bPhysAddr[5]);
            found = true;
        }
    }
    free(t);
    return found;
}

// One datagram is enough here: the Linux `ping -I` exists for policy
// routing that detours LAN traffic, which Windows VPN clients do not do
// in the same way (unverified; revisit if a Windows tester hits "Network
// Host Resolve Error" with a VPN up).
void compat_arp_prime(compat_sock s, const char *ip, const char *ifname)
{
    (void)ifname;
    compat_sendto(s, "", 1, ip, 11000);
}
