#include "api.h"

#define STBI_NO_STDIO
#define STBI_NO_SIMD /* no SSE state is set up for programs: the SIMD JPEG path faulted */
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"

void viewer_main(struct genesis_api *api) {
    api->fill(40, 90, api->width - 80, api->height - 120, 0x00243044u);
    api->text(48, 100, "Рисунок", 0x00f0c14au, 2);
    int w = 0;
    int h = 0;
    int comp = 0;
    unsigned char *pixels = stbi_load_from_memory(api->file, (int)api->file_len, &w, &h, &comp, 3);
    if (pixels == 0 || w <= 0 || h <= 0) {
        api->text(48, 150, "image failed", 0x00f4f1eau, 2);
        api->puts("image: failed\n");
    } else {
        int x0 = 48;
        int y0 = 140;
        int max_w = api->width - 96;
        int max_h = api->height - 180;
        int step = 1;
        while ((w / step) > max_w || (h / step) > max_h) {
            step++;
        }
        x0 = (api->width - w / step) / 2; /* centred on any screen */
        if (x0 < 48) {
            x0 = 48;
        }
        for (int y = 0; y < h; y += step) {
            for (int x = 0; x < w; x += step) {
                unsigned int i = ((unsigned int)y * (unsigned int)w + (unsigned int)x) * 3u;
                unsigned int color = ((unsigned int)pixels[i] << 16) | ((unsigned int)pixels[i + 1] << 8) |
                                    (unsigned int)pixels[i + 2];
                api->pixel(x0 + x / step, y0 + y / step, color);
            }
        }
        api->puts("image: ok\n");
        stbi_image_free(pixels);
    }
    while (!api->poll_back()) {
        api->yield();
    }
}
