#include "api.h"

#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_JPEG
#define STBI_NO_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"

static unsigned int be32(const unsigned char *p) {
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) |
           (unsigned int)p[3];
}

static int same4(const unsigned char *p, const char *tag) {
    return p[0] == (unsigned char)tag[0] && p[1] == (unsigned char)tag[1] &&
           p[2] == (unsigned char)tag[2] && p[3] == (unsigned char)tag[3];
}

static void blit_jpeg(struct genesis_api *api, const unsigned char *jpg, unsigned int len, int x0,
                      int y0) {
    int w = 0;
    int h = 0;
    int comp = 0;
    unsigned char *pixels = stbi_load_from_memory(jpg, (int)len, &w, &h, &comp, 3);
    if (pixels == 0) {
        return;
    }
    int step = 1;
    while ((w / step) > 400 || (h / step) > 300) {
        step++;
    }
    for (int y = 0; y < h; y += step) {
        for (int x = 0; x < w; x += step) {
            unsigned int i = ((unsigned int)y * (unsigned int)w + (unsigned int)x) * 3u;
            unsigned int color = ((unsigned int)pixels[i] << 16) | ((unsigned int)pixels[i + 1] << 8) |
                                (unsigned int)pixels[i + 2];
            api->pixel(x0 + x / step, y0 + y / step, color);
        }
    }
    stbi_image_free(pixels);
}

/* Real MP4 (isom) with MJPEG frames in mdat. */
void viewer_main(struct genesis_api *api) {
    api->fill(40, 90, api->width - 80, api->height - 120, 0x00243044u);
    api->text(48, 100, "Видео", 0x00f0c14au, 2);
    const unsigned char *mdat = 0;
    unsigned int mdat_len = 0;
    unsigned int at = 0;
    while (at + 8 <= api->file_len) {
        unsigned int size = be32(api->file + at);
        if (size < 8 || at + size > api->file_len) {
            break;
        }
        if (same4(api->file + at + 4, "mdat")) {
            mdat = api->file + at + 8;
            mdat_len = size - 8;
            break;
        }
        at += size;
    }
    if (mdat == 0 || mdat_len < 4) {
        api->text(48, 150, "mp4 failed", 0x00f4f1eau, 2);
        api->puts("mp4: failed\n");
        while (!api->poll_back()) {
            api->yield();
        }
        return;
    }
    api->puts("mp4: play\n");
    unsigned int frames = 0;
    unsigned int pos = 0;
    while (!api->poll_back()) {
        if (pos + 2 >= mdat_len) {
            pos = 0;
        }
        while (pos + 2 < mdat_len && !(mdat[pos] == 0xff && mdat[pos + 1] == 0xd8)) {
            pos++;
        }
        unsigned int start = pos;
        pos += 2;
        while (pos + 1 < mdat_len && !(mdat[pos] == 0xff && mdat[pos + 1] == 0xd9)) {
            pos++;
        }
        if (pos + 1 >= mdat_len) {
            pos = 0;
            continue;
        }
        pos += 2;
        unsigned int jpg_len = pos - start;
        frames++;
        if (frames == 1) {
            api->puts("mp4: frame\n");
        }
        api->fill(48, 140, 420, 320, 0x00101820u);
        if ((frames & 1u) != 0) {
            api->fill(500, 140, 80, 80, 0x00e04040u);
        } else {
            api->fill(500, 140, 80, 80, 0x0040e040u);
        }
        if (jpg_len > 0 && jpg_len < 8000u) {
            blit_jpeg(api, mdat + start, jpg_len, 48, 140);
        }
        for (int wait = 0; wait < 20 && !api->poll_back(); wait++) {
            api->yield();
        }
    }
}
