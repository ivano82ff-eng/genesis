#include "api.h"

void viewer_main(struct genesis_api *api) {
    api->fill(40, 90, api->width - 80, api->height - 120, 0x00243044u);
    api->text(48, 100, api->file_name, 0x00f0c14au, 2);
    char line[96];
    /* Wrap to the screen: 18 px per character at text scale 2. */
    int cols = (api->width - 96) / 18;
    if (cols > 90) {
        cols = 90;
    }
    unsigned int at = 0;
    int y = 150;
    api->puts("txt: ");
    while (at < api->file_len && y < api->height - 40) {
        unsigned int n = 0;
        int chars = 0;
        while (at < api->file_len && n + 1 < sizeof line) {
            unsigned char ch = api->file[at];
            if ((ch & 0xc0) != 0x80 && chars >= cols) {
                break; /* wrap: the rest goes on the next line */
            }
            /* UTF-8: never split a character across two screen lines. */
            if (ch >= 0xc0 && n + 5 >= sizeof line) {
                break;
            }
            at++;
            if (ch == '\n') {
                break;
            }
            if ((ch >= 32 && ch < 127) || ch >= 0x80) {
                line[n++] = (char)ch;
                if ((ch & 0xc0) != 0x80) {
                    chars++;
                }
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
