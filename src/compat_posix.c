// POSIX (Linux reference) implementation of compat.h.
#define _DEFAULT_SOURCE // struct ip_mreq, IFF_* with -std=c11

#include "compat.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

bool compat_net_init(void)
{
    return true;
}

void compat_net_quit(void)
{
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
    if (!inet_ntop(AF_INET, &a, out, (socklen_t)cap) && cap)
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
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return COMPAT_BAD_SOCK;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    if (reuse)
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {.sin_family = AF_INET,
                             .sin_addr.s_addr = htonl(INADDR_ANY),
                             .sin_port = htons(port)};
    if (bind(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
        int saved = errno;
        close(s);
        errno = saved;
        return COMPAT_BAD_SOCK;
    }
    return s;
}

bool compat_mcast_join(compat_sock s, const char *group, const char *ifip)
{
    struct ip_mreq m = {0};
    if (inet_pton(AF_INET, group, &m.imr_multiaddr) != 1)
        return false;
    if (ifip)
        inet_pton(AF_INET, ifip, &m.imr_interface);
    return setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) == 0;
}

compat_sock compat_tcp_connect(const char *ip, uint16_t port, int timeout_s)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return COMPAT_BAD_SOCK;
    // SO_SNDTIMEO bounds connect() as well as later sends on Linux
    struct timeval tv = {.tv_sec = timeout_s};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in sa;
    if (!sockaddr_from(ip, port, &sa) ||
        connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return COMPAT_BAD_SOCK;
    }
    return fd;
}

int compat_send(compat_sock s, const void *buf, size_t len)
{
    ssize_t n = send(s, buf, len, MSG_NOSIGNAL);
    return n < 0 ? -1 : (int)n;
}

int compat_sendto(compat_sock s, const void *buf, size_t len, const char *ip,
                  uint16_t port)
{
    struct sockaddr_in sa;
    if (!sockaddr_from(ip, port, &sa))
        return -1;
    ssize_t n = sendto(s, buf, len, 0, (struct sockaddr *)&sa, sizeof sa);
    return n < 0 ? -1 : (int)n;
}

int compat_recv_nowait(compat_sock s, void *buf, size_t cap)
{
    ssize_t n = recv(s, buf, cap, MSG_DONTWAIT);
    return n < 0 ? -1 : (int)n;
}

int compat_wait_readable(const compat_sock *socks, int n, int timeout_ms)
{
    struct pollfd pfd[8];
    if (n > 8)
        n = 8;
    for (int i = 0; i < n; i++)
        pfd[i] = (struct pollfd){.fd = socks[i], .events = POLLIN};
    return poll(pfd, (nfds_t)n, timeout_ms); // negative fds are ignored
}

void compat_close(compat_sock s)
{
    if (s >= 0)
        close(s);
}

const char *compat_neterr(void)
{
    return strerror(errno);
}

bool compat_route_source_ip(const char *ip, char *out, size_t cap)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return false;
    struct sockaddr_in sa;
    bool ok = sockaddr_from(ip, 80, &sa) &&
              connect(s, (struct sockaddr *)&sa, sizeof sa) == 0;
    if (ok) {
        struct sockaddr_in local;
        socklen_t len = sizeof local;
        ok = getsockname(s, (struct sockaddr *)&local, &len) == 0 &&
             inet_ntop(AF_INET, &local.sin_addr, out, (socklen_t)cap) != NULL;
    }
    close(s);
    return ok;
}

int compat_ifaces(struct compat_iface *out, int max)
{
    struct ifaddrs *ifs;
    if (getifaddrs(&ifs) != 0)
        return 0;
    int n = 0;
    for (struct ifaddrs *i = ifs; i && n < max; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET)
            continue;
        struct compat_iface *o = &out[n++];
        memset(o, 0, sizeof *o);
        snprintf(o->name, sizeof o->name, "%s", i->ifa_name);
        o->addr = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
        if (i->ifa_netmask)
            o->mask = ntohl(
                ((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
        o->up = (i->ifa_flags & IFF_UP) != 0;
        o->loopback = (i->ifa_flags & IFF_LOOPBACK) != 0;
        // predictable names (wlp3s0, wlan0) or the sysfs wireless node
        char path[80];
        struct stat st;
        snprintf(path, sizeof path, "/sys/class/net/%s/wireless", o->name);
        o->wireless = strncmp(o->name, "wl", 2) == 0 ||
                      stat(path, &st) == 0;
    }
    freeifaddrs(ifs);
    return n;
}

bool compat_neighbor_mac(const char *ip, char *out, size_t cap)
{
    const char *path = getenv("C64U_ARP_TABLE");
    FILE *f = fopen(path ? path : "/proc/net/arp", "r");
    if (!f)
        return false;
    char line[256];
    bool found = false;
    while (!found && fgets(line, sizeof line, f)) {
        char a[46], hw[64];
        // /proc/net/arp: IP  HWtype  Flags  HWaddress  Mask  Device
        if (sscanf(line, "%45s %*s %*s %63s", a, hw) == 2 && !strcmp(a, ip)) {
            snprintf(out, cap, "%s", hw);
            found = true;
        }
    }
    fclose(f);
    return found;
}

void compat_arp_prime(compat_sock s, const char *ip, const char *ifname)
{
    uint32_t a;
    if (!compat_ipv4_parse(ip, &a))
        return; // hostnames never reach a shell
    if (ifname && ifname[0]) {
        // ping may force the egress device without privileges
        char cmd[160];
        snprintf(cmd, sizeof cmd,
                 "ping -n -q -c 1 -W 1 -I '%s' '%s' >/dev/null 2>&1", ifname,
                 ip);
        (void)!system(cmd);
    } else {
        compat_sendto(s, "", 1, ip, 11000);
    }
}
