#pragma once

enum { GENESIS_FB_W = 1024, GENESIS_FB_H = 768 };

struct genesis_api {
    void (*puts)(const char *text);
    void (*text)(int x, int y, const char *message, unsigned int color, int scale);
    void (*fill)(int x, int y, int width, int height, unsigned int color);
    void (*pixel)(int x, int y, unsigned int color);
    unsigned int *fb;
    const unsigned char *file;
    unsigned int file_len;
    const char *file_name;
    /* 1 when the user pressed «Назад». */
    int (*poll_back)(void);
    /* Drain input and advance the clock while a viewer loops. */
    void (*yield)(void);
    void *(*malloc)(unsigned int size);
    void (*free)(void *ptr);
};

typedef void (*viewer_entry)(struct genesis_api *api);
