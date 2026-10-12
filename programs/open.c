#include "api.h"

/* Home «Открыть» is driven by the kernel UI; this program is the on-disk
 * opener entry that prints a marker and waits for Назад when run alone. */
void viewer_main(struct genesis_api *api) {
    api->fill(40, 90, api->width - 80, api->height - 120, 0x00243044u);
    api->text(48, 100, "Открыть", 0x00f0c14au, 2);
    api->text(48, 150, "choose a file on home", 0x00f4f1eau, 2);
    api->puts("open: ready\n");
    while (!api->poll_back()) {
        api->yield();
    }
}
