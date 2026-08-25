// Ultimate discovery: one-shot GET /v1/info sweep of the local /24 subnets.
// SDL-free (libcurl only) so tests can link it standalone.
#ifndef DISCOVER_H
#define DISCOVER_H

#include <stdbool.h>
#include <stddef.h>

#define DISCOVER_MAX 8

struct discovered {
    char ip[46];
    char product[64];
    char hostname[64];
    char fw[32];
    char uid[32]; // unique_id: one machine can answer on WiFi and wired
};

// Sweeps every local /24 (one request per address, bounded concurrency,
// no retries) and fills `out` with the Ultimates that answered /v1/info.
// Returns the number found (capped at `max`).
// Test hooks: C64U_DISCOVER_NET=<a.b.c.0> sweeps only that /24 (loopback
// allowed), C64U_DISCOVER_PORT overrides port 80.
int discover_scan(struct discovered *out, int max, bool verbose);

// Minimal flat-JSON string lookup: copies the value of "key":"value" into
// out. Returns false if the key is missing or not a string.
bool json_find_str(const char *json, const char *key, char *out, size_t cap);

#endif
