#include "api.h"

static int same_n(const char *a, const char *b, unsigned int n) {
    for (unsigned int i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static unsigned int read_u32(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) |
           ((unsigned int)p[3] << 24);
}

static unsigned int read_u16(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

/* Stored (method 0) ZIP only — enough for our sample docx. */
static int zip_find(const unsigned char *zip, unsigned int zip_len, const char *path,
                    const unsigned char **out, unsigned int *out_len) {
    unsigned int path_len = 0;
    while (path[path_len] != 0) {
        path_len++;
    }
    unsigned int at = 0;
    while (at + 30 < zip_len) {
        if (zip[at] != 'P' || zip[at + 1] != 'K' || zip[at + 2] != 3 || zip[at + 3] != 4) {
            at++;
            continue;
        }
        unsigned int method = read_u16(zip + at + 8);
        unsigned int comp = read_u32(zip + at + 18);
        unsigned int name_len = read_u16(zip + at + 26);
        unsigned int extra = read_u16(zip + at + 28);
        const unsigned char *name = zip + at + 30;
        const unsigned char *data = name + name_len + extra;
        if (data + comp > zip + zip_len) {
            return 0;
        }
        if (method == 0 && name_len == path_len && same_n((const char *)name, path, path_len)) {
            *out = data;
            *out_len = comp;
            return 1;
        }
        at = (unsigned int)(data - zip) + comp;
    }
    return 0;
}

static void extract_text(struct genesis_api *api, const unsigned char *xml, unsigned int len) {
    char out[120];
    unsigned int n = 0;
    unsigned int i = 0;
    while (i + 4 < len && n + 1 < sizeof out) {
        if (xml[i] == '<' && xml[i + 1] == 'w' && xml[i + 2] == ':' && xml[i + 3] == 't') {
            while (i < len && xml[i] != '>') {
                i++;
            }
            if (i < len) {
                i++;
            }
            while (i < len && xml[i] != '<' && n + 1 < sizeof out) {
                unsigned char ch = xml[i++];
                if (ch >= 32 && ch < 127) {
                    out[n++] = (char)ch;
                }
            }
            continue;
        }
        i++;
    }
    out[n] = 0;
    api->text(48, 150, out, 0x00f4f1eau, 2);
    api->puts("docx: ");
    api->puts(out);
    api->puts("\n");
}

void viewer_main(struct genesis_api *api) {
    api->fill(40, 90, GENESIS_FB_W - 80, GENESIS_FB_H - 120, 0x00243044u);
    api->text(48, 100, "Документ", 0x00f0c14au, 2);
    const unsigned char *xml = 0;
    unsigned int xml_len = 0;
    if (!zip_find(api->file, api->file_len, "word/document.xml", &xml, &xml_len)) {
        api->text(48, 150, "docx read failed", 0x00f4f1eau, 2);
        api->puts("docx: failed\n");
    } else {
        extract_text(api, xml, xml_len);
    }
    while (!api->poll_back()) {
        api->yield();
    }
}
