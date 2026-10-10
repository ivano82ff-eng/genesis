#include "api.h"

static void show_page_text(struct genesis_api *api) {
    char out[120];
    unsigned int n = 0;
    unsigned int i = 0;
    while (i + 1 < api->file_len && n + 1 < sizeof out) {
        if (api->file[i] == '(' ) {
            i++;
            while (i < api->file_len && api->file[i] != ')' && n + 1 < sizeof out) {
                unsigned char ch = api->file[i++];
                if (ch == '\\' && i < api->file_len) {
                    ch = api->file[i++];
                }
                if (ch >= 32 && ch < 127) {
                    out[n++] = (char)ch;
                }
            }
            continue;
        }
        i++;
    }
    out[n] = 0;
    if (n == 0) {
        api->text(48, 150, "pdf empty", 0x00f4f1eau, 2);
        api->puts("pdf: empty\n");
        return;
    }
    api->text(48, 150, out, 0x00f4f1eau, 2);
    api->puts("pdf: ");
    api->puts(out);
    api->puts("\n");
}

void viewer_main(struct genesis_api *api) {
    api->fill(40, 90, GENESIS_FB_W - 80, GENESIS_FB_H - 120, 0x00243044u);
    api->text(48, 100, "PDF", 0x00f0c14au, 2);
    show_page_text(api);
    while (!api->poll_back()) {
        api->yield();
    }
}
