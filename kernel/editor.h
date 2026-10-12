#pragma once

/* Editor keys beyond plain bytes ('\n', '\b', printable ASCII and UTF-8 bytes). */
enum {
    ED_UP = 0x100, ED_DOWN, ED_LEFT, ED_RIGHT, ED_HOME, ED_END, ED_PGUP, ED_PGDN,
    ED_DELETE, ED_TAB, ED_SAVE, ED_QUIT, ED_REDRAW, ED_COPY, ED_CUT, ED_PASTE,
    ED_SHIFT = 0x1000, /* or-ed onto a movement key: extend the selection */
};

int editor_active(void);
int editor_open(const char *file);
void editor_key(int key);

/* Internal clipboard (editor ^C/^X/^V, shell ^V). */
const char *clipboard_text(unsigned int *length);
