#include "discover.h"

#include "compat.h"

#include <curl/curl.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool json_find_str(const char *json, const char *key, char *out, size_t cap)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p)
        return false;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p++ != ':')
        return false;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p++ != '"')
        return false;
    size_t n = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1])
            p++; // keep escaped char raw; /v1/info values have no escapes
        if (n + 1 < cap)
            out[n++] = *p;
        p++;
    }
    out[n] = '\0';
    return *p == '"';
}

// The sweep hits every address on the /24 once; random web servers answer on
// port 80 (routers, printers), so a responder counts only if it returns JSON
// whose "product" looks like an Ultimate.
static bool is_ultimate_info(long code, const char *resp, struct discovered *d)
{
    if (code != 200 || !json_find_str(resp, "product", d->product,
                                      sizeof d->product))
        return false;
    if (!strstr(d->product, "Ultimate") && !strstr(d->product, "U64"))
        return false;
    if (!json_find_str(resp, "hostname", d->hostname, sizeof d->hostname))
        d->hostname[0] = '\0';
    if (!json_find_str(resp, "firmware_version", d->fw, sizeof d->fw))
        d->fw[0] = '\0';
    if (!json_find_str(resp, "unique_id", d->uid, sizeof d->uid))
        d->uid[0] = '\0';
    return true;
}

bool discover_ip_is_wired(const char *ip)
{
    char mac[COMPAT_MAC_STRLEN];
    return compat_neighbor_mac(ip, mac, sizeof mac) &&
           strncmp(mac, "02:15:41", 8) == 0;
}

struct probe {
    char url[64];
    char ip[46];
    char resp[512];
    CURL *h;
};

static size_t probe_sink(char *data, size_t size, size_t nmemb, void *userp)
{
    char *buf = userp; // keep the first chunk only; /v1/info fits easily
    size_t n = size * nmemb;
    if (buf[0] == '\0') {
        size_t cap = 511 < n ? 511 : n;
        memcpy(buf, data, cap);
        buf[cap] = '\0';
    }
    return n;
}

#define DISC_CONC 64 // concurrent probes; a /24 sweeps in ~4 waves

int discover_scan(struct discovered *out, int max, bool verbose)
{
    long port = 80;
    const char *penv = getenv("C64U_DISCOVER_PORT");
    if (penv)
        port = atol(penv);
    // password-protected Ultimates refuse /v1/info without the header
    struct curl_slist *pwhdr = NULL;
    if (getenv("C64U_PASSWORD")) {
        char h[160];
        snprintf(h, sizeof h, "X-Password: %s", getenv("C64U_PASSWORD"));
        pwhdr = curl_slist_append(NULL, h);
    }

    // Collect the /24 of every usable IPv4 interface (deduplicated: wired and
    // wireless commonly share a subnet), remembering our own addresses so the
    // sweep skips them.
    uint32_t nets[8], self[16];
    int nnets = 0, nself = 0;
    const char *nenv = getenv("C64U_DISCOVER_NET");
    if (nenv) {
        uint32_t a;
        if (!compat_ipv4_parse(nenv, &a))
            return 0;
        nets[nnets++] = a & 0xFFFFFF00u;
    } else {
        struct compat_iface ifs[32];
        int n = compat_ifaces(ifs, 32);
        for (int k = 0; k < n; k++) {
            const struct compat_iface *i = &ifs[k];
            if (i->loopback || !i->up)
                continue;
            // The sweep covers the /24 around the address, so it only makes
            // sense on interfaces whose subnet is at least that big; this
            // skips /32 VPN endpoints (Tailscale, WireGuard).
            if (i->mask & 0xFF)
                continue;
            if (nself < 16)
                self[nself++] = i->addr;
            uint32_t net = i->addr & 0xFFFFFF00u;
            bool dup = false;
            for (int j = 0; j < nnets; j++)
                dup |= nets[j] == net;
            if (!dup && nnets < 8)
                nets[nnets++] = net;
        }
    }

    CURLM *multi = curl_multi_init();
    if (!multi)
        return 0;
    struct probe probes[DISC_CONC];
    memset(probes, 0, sizeof probes);
    int found = 0, next = 0, total = nnets * 254, running = 0;

    for (int k = 0; k < nnets && verbose; k++) {
        char s[COMPAT_IP_STRLEN];
        fprintf(stderr, "scanning %s/24 for /v1/info responders...\n",
                compat_ipv4_format(nets[k], s, sizeof s));
    }

    while ((next < total || running > 0) && found < max) {
        while (next < total && running < DISC_CONC) {
            uint32_t addr = nets[next / 254] | (uint32_t)(next % 254 + 1);
            next++;
            bool own = false;
            for (int k = 0; k < nself; k++)
                own |= self[k] == addr;
            if (own)
                continue;
            struct probe *pr = &probes[0]; // running < DISC_CONC: a slot is free
            for (int k = 0; k < DISC_CONC; k++)
                if (!probes[k].h) {
                    pr = &probes[k];
                    break;
                }
            compat_ipv4_format(addr, pr->ip, sizeof pr->ip);
            snprintf(pr->url, sizeof pr->url, "http://%s:%ld/v1/info", pr->ip,
                     port);
            pr->resp[0] = '\0';
            CURL *h = curl_easy_init();
            pr->h = h;
            curl_easy_setopt(h, CURLOPT_URL, pr->url);
            // Split budget: unreachable addresses must fail on the short
            // connect timeout, while the Ultimate's REST can take ~2.5 s to
            // answer once connected.
            curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 1500L);
            curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 4750L);
            curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, probe_sink);
            curl_easy_setopt(h, CURLOPT_WRITEDATA, pr->resp);
            curl_easy_setopt(h, CURLOPT_PRIVATE, pr);
            if (pwhdr)
                curl_easy_setopt(h, CURLOPT_HTTPHEADER, pwhdr);
            curl_multi_add_handle(multi, h);
            running++;
        }
        int still = 0;
        curl_multi_perform(multi, &still);
        if (still == running && running > 0)
            curl_multi_poll(multi, NULL, 0, 100, NULL);
        CURLMsg *msg;
        int left;
        while ((msg = curl_multi_info_read(multi, &left))) {
            if (msg->msg != CURLMSG_DONE)
                continue;
            CURL *h = msg->easy_handle;
            struct probe *pr;
            curl_easy_getinfo(h, CURLINFO_PRIVATE, &pr);
            long code = 0;
            curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
            if (msg->data.result == CURLE_OK && found < max &&
                is_ultimate_info(code, pr->resp, &out[found])) {
                snprintf(out[found].ip, sizeof out[found].ip, "%s", pr->ip);
                found++;
            }
            pr->h = NULL;
            running--;
            curl_multi_remove_handle(multi, h);
            curl_easy_cleanup(h);
        }
    }
    for (int k = 0; k < DISC_CONC; k++) { // found == max leaves probes in flight
        if (probes[k].h) {
            curl_multi_remove_handle(multi, probes[k].h);
            curl_easy_cleanup(probes[k].h);
        }
    }
    curl_multi_cleanup(multi);
    curl_slist_free_all(pwhdr);
    return found;
}
