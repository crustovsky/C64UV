// VIC stream frame assembly: 780-byte UDP packets -> ARGB frames.
// SDL-free so tests can use it standalone.
#ifndef VIDEO_H
#define VIDEO_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define VIDEO_MAX_W 384
#define VIDEO_MAX_H 312 // safety headroom; PAL uses 272, NTSC 240
#define VIDEO_HDR_LEN 12

// Pepto/colodore-style VIC-II palette, ARGB8888.
extern const uint32_t vic_palette[16];

struct frame_buf {
    uint32_t px[VIDEO_MAX_W * VIDEO_MAX_H]; // palette-expanded
    int width, height; // known once a last-flagged packet arrives
    uint16_t frame_no;
    int lines_got;
    bool ready;    // saw the last-packet flag for frame_no
    bool complete; // every line of the frame arrived
};

void video_init(struct frame_buf *fb);
// Feeds one UDP datagram; returns true when a frame became presentable.
bool video_handle_packet(const uint8_t *d, ssize_t len, struct frame_buf *fb);
bool video_dump_ppm(const char *path, const struct frame_buf *fb);

#endif
