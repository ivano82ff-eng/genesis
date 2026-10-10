#include "api.h"

void viewer_main(struct genesis_api *api) {
    api->fill(40, 90, GENESIS_FB_W - 80, GENESIS_FB_H - 120, 0x00243044u);
    api->text(48, 100, api->file_name, 0x00f0c14au, 2);
    char line[64];
    unsigned int at = 0;
    int y = 150;
    api->puts("txt: ");
    while (at < api->file_len && y < GENESIS_FB_H - 40) {
        unsigned int n = 0;
        while (at < api->file_len && n + 1 < sizeof line) {
            unsigned char ch = api->file[at++];
            if (ch == '\n') {
                break;
            }
            if (ch >= 32 && ch < 127) {
                line[n++] = (char)ch;
            }
        }
        line[n] = 0;
        api->text(48, y, line, 0x00f4f1eau, 2);
        api->puts(line);
        api->puts("\n");
        y += 28;
    }
    while (!api->poll_back()) {
        api->yield();
    }
}
