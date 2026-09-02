// Platform compatibility layer.
//
// Everything OS-specific in c64uv that is not already covered by SDL3 or
// libcurl goes through this header: BSD sockets, interface enumeration,
// the neighbor (ARP) table, and the ARP-prime trick the keepalive relies
// on. The rest of the code is plain C11 + SDL3 + libcurl. Linux is the
// reference implementation (compat_posix.c); a port supplies the same
// functions with Winsock / GetAdaptersAddresses / GetIpNetTable behind them.
//
// Conventions: IPv4 only, addresses are host-order uint32_t or dotted
// strings, ports are host order. Nothing here logs; callers report failures
// with compat_neterr().
#ifndef COMPAT_H
#define COMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int compat_sock; // SOCKET on Winsock
#define COMPAT_BAD_SOCK (-1)

#define COMPAT_IP_STRLEN 16 // "255.255.255.255" + NUL
#define COMPAT_MAC_STRLEN 18 // "aa:bb:cc:dd:ee:ff" + NUL

// Process-wide socket setup (WSAStartup on Windows; no-op on POSIX). Call
// once before any other compat_* function; compat_net_quit at exit.
bool compat_net_init(void);
void compat_net_quit(void);

// ------------------------------------------------------------ IPv4 helpers

// Dotted quad -> host-order address. False if `s` is not an IPv4 literal
// (hostnames are rejected on purpose; the caller decides what to do).
bool compat_ipv4_parse(const char *s, uint32_t *addr);
// Host-order address -> dotted quad in out (>= COMPAT_IP_STRLEN bytes).
// Returns out.
const char *compat_ipv4_format(uint32_t addr, char *out, size_t cap);
static inline bool compat_ipv4_is_multicast(uint32_t addr)
{
    return (addr >> 28) == 0xE; // 224.0.0.0/4
}

// ------------------------------------------------------------------ sockets

// UDP socket bound to INADDR_ANY:port with the given receive buffer size
// (bytes). `reuse` sets SO_REUSEADDR so several viewers can share a
// multicast port. COMPAT_BAD_SOCK on failure.
compat_sock compat_udp_bind(uint16_t port, int rcvbuf, bool reuse);

// IP_ADD_MEMBERSHIP for `group`; `ifip` picks the local interface address
// to join on (NULL = kernel picks by route). A duplicate join fails, which
// callers treat as harmless.
bool compat_mcast_join(compat_sock s, const char *group, const char *ifip);

// Blocking TCP connect to an IPv4 literal, bounded by timeout_s. The
// socket stays blocking for sends. COMPAT_BAD_SOCK on failure.
compat_sock compat_tcp_connect(const char *ip, uint16_t port, int timeout_s);

// send() that never raises SIGPIPE; the peer closing shows up as -1.
int compat_send(compat_sock s, const void *buf, size_t len);

// One datagram to ip:port. -1 on failure (including a non-literal ip).
int compat_sendto(compat_sock s, const void *buf, size_t len, const char *ip,
                  uint16_t port);

// Non-blocking receive: > 0 bytes read, 0 when a stream peer closed, -1
// when nothing is pending (or on error).
int compat_recv_nowait(compat_sock s, void *buf, size_t cap);

// Waits until any of the sockets is readable or timeout_ms elapsed.
// COMPAT_BAD_SOCK entries are skipped. Returns the number ready, 0 on
// timeout, -1 on error.
int compat_wait_readable(const compat_sock *socks, int n, int timeout_ms);

void compat_close(compat_sock s);

// Text for the last socket error (errno / WSAGetLastError).
const char *compat_neterr(void);

// Source address the OS would use to reach `ip` (UDP connect +
// getsockname). Dotted quad in out (>= COMPAT_IP_STRLEN bytes).
bool compat_route_source_ip(const char *ip, char *out, size_t cap);

// --------------------------------------------------------------- interfaces

struct compat_iface {
    char name[32];
    uint32_t addr, mask; // host order; mask 0 when unknown
    bool up, loopback;
    bool wireless; // best-effort hint: wired is preferred for the stream
};

// Every IPv4-configured interface (including down and loopback ones;
// callers filter). Returns the count, capped at max.
int compat_ifaces(struct compat_iface *out, int max);

// ----------------------------------------------------------- neighbor table

// MAC of `ip` from the OS neighbor (ARP) table as "aa:bb:cc:dd:ee:ff"
// (out >= COMPAT_MAC_STRLEN bytes). False when the address is unknown.
// Linux reads /proc/net/arp; C64U_ARP_TABLE overrides the path for tests.
bool compat_neighbor_mac(const char *ip, char *out, size_t cap);

// Makes the Ultimate hear from us so `ip` lands in its ARP table (the
// firmware never ARPs on demand). With a non-empty `ifname` the traffic is
// forced out of that interface, which on Linux needs `ping -I` because
// policy routing (VPN accept-routes) can otherwise detour LAN traffic; an
// empty name sends one datagram from `s` and lets the OS route it.
void compat_arp_prime(compat_sock s, const char *ip, const char *ifname);

#endif
