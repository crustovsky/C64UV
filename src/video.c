#include "video.h"

#include <stdio.h>

const uint32_t vic_palette[16] = {
    0xFF000000, 0xFFFFFFFF, 0xFF813338, 0xFF75CEC8, 0xFF8E3C97, 0xFF56AC4D,
    0xFF2E2C9B, 0xFFEDF171, 0xFF8E5029, 0xFF553800, 0xFFC46C71, 0xFF4A4A4A,
    0xFF7B7B7B, 0xFFA9FF9F, 0xFF706DEB, 0xFFB2B2B2,
};

void video_init(struct frame_buf *fb)
{
    fb->frame_no = 0xFFFF;
    fb->width = VIDEO_MAX_W;
    fb->height = 272;
    fb->lines_got = 0;
    fb->ready = fb->complete = false;
}

bool video_handle_packet(const uint8_t *d, ssize_t len, struct frame_buf *fb)
{
    if (len < VIDEO_HDR_LEN)
        return false;
    uint16_t frame = (uint16_t)(d[2] | d[3] << 8);
    uint16_t rawline = (uint16_t)(d[4] | d[5] << 8);
    int line = rawline & 0x7FFF;
    bool last = rawline & 0x8000;
    int ppl = d[6] | d[7] << 8;
    int lpp = d[8];
    int bpp = d[9];
    int enc = d[10] | d[11] << 8;

    if (bpp != 4 || enc != 0 || ppl <= 0 || ppl > VIDEO_MAX_W || lpp <= 0 ||
        line + lpp > VIDEO_MAX_H ||
        len < VIDEO_HDR_LEN + (ssize_t)(ppl / 2 * lpp))
        return false; // not a stream layout we understand

    if (frame != fb->frame_no) { // new frame begins; keep old pixels as filler
        fb->frame_no = frame;
        fb->lines_got = 0;
        fb->ready = false;
    }
    const uint8_t *src = d + VIDEO_HDR_LEN;
    for (int l = 0; l < lpp; l++) {
        uint32_t *dst = fb->px + (size_t)(line + l) * VIDEO_MAX_W;
        for (int x = 0; x < ppl; x += 2) {
            uint8_t b = *src++;
            dst[x] = vic_palette[b & 0x0F]; // low nibble = left pixel
            dst[x + 1] = vic_palette[b >> 4];
        }
    }
    fb->lines_got += lpp;
    fb->width = ppl;
    if (last) {
        fb->height = line + lpp;
        fb->ready = true;
        fb->complete = fb->lines_got >= fb->height;
    }
    return fb->ready;
}

bool video_dump_ppm(const char *path, const struct frame_buf *fb)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    fprintf(f, "P6\n%d %d\n255\n", fb->width, fb->height);
    for (int y = 0; y < fb->height; y++)
        for (int x = 0; x < fb->width; x++) {
            uint32_t p = fb->px[(size_t)y * VIDEO_MAX_W + x];
            fputc(p >> 16 & 0xFF, f);
            fputc(p >> 8 & 0xFF, f);
            fputc(p & 0xFF, f);
        }
    return fclose(f) == 0;
}
