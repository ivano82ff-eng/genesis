#include "font8x8.h"
#include "utf8.h"

#include "../programs/api.h"
#include "fs_limits.h"

/* Framebuffer size: 1024x768 by default, `desktop WxH` or the boot option
 * size=WxH picks another (portrait phones, 1920x1080 TVs). */
enum { FB_MAX_W = 2048, FB_MAX_H = 2048 };
static int FB_W = 1024;
static int FB_H = 768;
enum { VIEW_HOME = 0, VIEW_VIEWER };
enum { BACK_X = 24, BACK_Y = 16, BACK_W = 300, BACK_H = 52 };

static unsigned int *fb;
static int desktop_on;
static int view;
static int pointer_down;
static int cursor_x;
static int cursor_y;
static int cursor_bg_ok;
static int cursor_bg_x;
static int cursor_bg_y;
static unsigned int cursor_bg[16 * 16];
static char clock_text[] = "time 00:00:00";

extern void console_puts(const char *text);
extern void console_putc(char ch);
extern void memory_summary(char *dest, unsigned int cap);
extern void fs_summary(char *dest, unsigned int cap);
extern void preview_readme(char *dest, unsigned int cap);
extern int fs_store_readme(const char *text);
extern int fs_lookup(const char *name, const unsigned char **data, unsigned int *length);
extern int fs_dir_entry(int index, const char *dir, char *name, unsigned int name_cap, int *is_dir);
extern void guest_yield(void);
extern void disk_request_home(void);
extern void machine_poweroff(void);
extern void machine_reboot(void);
extern const char *program_output(void);
extern void run_program(const char *name);
extern void mouse_arm(void);
extern void ps2_drain(void);
unsigned int clock_read(void);
void clock_service(void);

static inline void outw(unsigned short port, unsigned short value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outl(unsigned short port, unsigned int value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline unsigned int inl(unsigned short port) {
    unsigned int value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static unsigned int pci_read(unsigned slot, unsigned offset) {
    unsigned int addr = 0x80000000u | (slot << 11) | (offset & 0xfcu);
    outl(0xcf8, addr);
    return inl(0xcfc);
}

static void pci_write(unsigned slot, unsigned offset, unsigned int value) {
    unsigned int addr = 0x80000000u | (slot << 11) | (offset & 0xfcu);
    outl(0xcf8, addr);
    outl(0xcfc, value);
}

static unsigned int find_framebuffer(void) {
    for (unsigned slot = 0; slot < 32; slot++) {
        unsigned int id = pci_read(slot, 0);
        if (id == 0xffffffffu || id == 0) {
            continue;
        }
        unsigned int class_reg = pci_read(slot, 0x08);
        unsigned int class_code = (class_reg >> 24) & 0xffu;
        unsigned short vendor = (unsigned short)id;
        unsigned short device = (unsigned short)(id >> 16);
        if (class_code != 0x03 && !(vendor == 0x1234 && device == 0x1111)) {
            continue;
        }
        unsigned int bar = pci_read(slot, 0x10);
        if ((bar & 0x1u) != 0) {
            continue;
        }
        unsigned int command = pci_read(slot, 0x04);
        pci_write(slot, 0x04, command | 0x06u);
        return bar & ~0xfu;
    }
    return 0;
}

static void vbe_write(unsigned short index, unsigned short value) {
    outw(0x1ce, index);
    outw(0x1cf, value);
}

static int enable_linear_framebuffer(void) {
    unsigned int lfb = find_framebuffer();
    if (lfb == 0) {
        return 0;
    }
    vbe_write(4, 0);
    vbe_write(1, FB_W);
    vbe_write(2, FB_H);
    vbe_write(3, 32);
    vbe_write(6, FB_W);
    vbe_write(7, FB_H);
    vbe_write(4, 0x41);
    fb = (unsigned int *)(unsigned long)lfb;
    return 1;
}

static void pixel(int x, int y, unsigned int color);

static void cursor_forget(void) {
    cursor_bg_ok = 0;
}

static void cursor_erase(void) {
    if (!cursor_bg_ok || fb == 0) {
        return;
    }
    for (int row = 0; row < 16; row++) {
        int y = cursor_bg_y + row;
        if (y < 0 || y >= FB_H) {
            continue;
        }
        for (int col = 0; col < 16; col++) {
            int x = cursor_bg_x + col;
            if (x < 0 || x >= FB_W) {
                continue;
            }
            fb[(unsigned)y * FB_W + (unsigned)x] = cursor_bg[row * 16 + col];
        }
    }
    cursor_bg_ok = 0;
}

static void cursor_paint(void) {
    if (fb == 0) {
        return;
    }
    int x0 = cursor_x;
    int y0 = cursor_y;
    if (x0 > FB_W - 16) {
        x0 = FB_W - 16;
    }
    if (y0 > FB_H - 16) {
        y0 = FB_H - 16;
    }
    cursor_bg_x = x0;
    cursor_bg_y = y0;
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 16; col++) {
            cursor_bg[row * 16 + col] = fb[(unsigned)(y0 + row) * FB_W + (unsigned)(x0 + col)];
        }
    }
    cursor_bg_ok = 1;
    for (int i = 0; i < 12; i++) {
        pixel(cursor_x, cursor_y + i, 0x00fffff0u);
        pixel(cursor_x + 1, cursor_y + i, 0x00101820u);
    }
    for (int i = 0; i < 8; i++) {
        pixel(cursor_x + i, cursor_y + i, 0x00fffff0u);
        pixel(cursor_x + i, cursor_y + i + 1, 0x00101820u);
    }
}

static void cursor_move(void) {
    cursor_erase();
    cursor_paint();
}

static void pixel(int x, int y, unsigned int color) {
    if (x < 0 || y < 0 || x >= FB_W || y >= FB_H) {
        return;
    }
    fb[(unsigned)y * FB_W + (unsigned)x] = color;
}

static void fill(int x, int y, int width, int height, unsigned int color) {
    if (fb == 0 || width <= 0 || height <= 0) {
        return;
    }
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x >= FB_W || y >= FB_H) {
        return;
    }
    if (x + width > FB_W) {
        width = FB_W - x;
    }
    if (y + height > FB_H) {
        height = FB_H - y;
    }
    for (int row = 0; row < height; row++) {
        if ((row & 31) == 0) {
            ps2_drain();
        }
        unsigned int *dst = fb + (unsigned)(y + row) * FB_W + (unsigned)x;
        for (int col = 0; col < width; col++) {
            dst[col] = color;
        }
    }
}

static void blit_rows(int x, int y, const unsigned char *rows, unsigned int color, int scale) {
    for (int row = 0; row < 8; row++) {
        unsigned char bits = rows[row];
        for (int col = 0; col < 8; col++) {
            if ((bits & (0x80u >> col)) == 0) {
                continue;
            }
            int px = x + col * scale;
            int py = y + row * scale;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    pixel(px + sx, py + sy, color);
                }
            }
        }
    }
}

static void glyph_code(int x, int y, unsigned int code, unsigned int color, int scale) {
    const unsigned char *rows = 0;
    if (code >= 32 && code <= 127) {
        rows = FONT8X8[code - 32];
    } else {
        const struct cyr_glyph *g = cyr_glyph(code);
        rows = g ? g->rows : 0;
    }
    if (rows == 0) {
        rows = FONT8X8[' ' - 32];
    }
    blit_rows(x, y, rows, color, scale);
}

static int utf8_next(const char **message, unsigned int *code) {
    const unsigned char *s = (const unsigned char *)*message;
    if (*s == 0) {
        return 0;
    }
    /* The terminating 0 is not a continuation byte, so 4 is a safe bound. */
    *message += utf8_decode(s, 4, code);
    return 1;
}

static void text(int x, int y, const char *message, unsigned int color, int scale) {
    int cursor = x;
    unsigned int code = 0;
    while (utf8_next(&message, &code)) {
        glyph_code(cursor, y, code, color, scale);
        cursor += 8 * scale + 2;
    }
}


/* ---------------------------------------------------------------------------
 * «Ядро» (Core) desktop: a glowing hexagon in the middle shows memory and
 * tasks, hexagonal cells around it open apps, a docked panel on the right
 * shows the open app and a command bar runs any shell line.
 * ------------------------------------------------------------------------- */
enum { BG = 0x00101820u, PAPER = 0x00243044u, FRAME = 0x004c6a8au, YELLOW = 0x00f0c14au, INK = 0x00f4f1eau };
enum {
    CYAN = 0x003fe0ffu, CYAN_MID = 0x001f8fb8u, CYAN_DIM = 0x00174a6eu, CYAN_FAINT = 0x000f2a48u,
    AMBER = 0x00ffb84au, AMBER_MID = 0x00a8742au, AMBER_DIM = 0x004a3418u, AMBER_FAINT = 0x00261c16u,
    RED = 0x00ff5060u, RED_MID = 0x00a02838u, RED_DIM = 0x00481420u,
    CELL = 0x000d1a36u, CELL_HI = 0x00183260u, CORE_FILL = 0x00091530u,
    MUTED = 0x007d93b8u, WHITE = 0x00e8f4ffu, PANEL = 0x000c1830u, PANEL_BAR = 0x00132a50u,
};
enum { CELL_MAX = 12, CELL_CORE = 100, CELL_PEER = 101 };
/* Layout, computed from the framebuffer size by layout_init(). U is the UI
 * scale in 1/256 (256 at 1024x768); P() scales a 1024x768 design length. */
static int U = 256;
static int portrait;
static int CORE_X, CORE_Y, CORE_R, CELL_R;
static int PANEL_X, PANEL_Y, PANEL_W, PANEL_H, TITLE_H;
static int CLOSE_X, CLOSE_Y, CLOSE_W, CLOSE_H;
static int BAR_X, BAR_Y, BAR_W, BAR_H, BAR_TEXT_X, CHAR_W;
static int cell_cx[CELL_MAX], cell_cy[CELL_MAX];
#define P(v) ((int)(v) * U / 256)

static int ts(int scale) {
    int t = (scale * U + 200) / 256;
    return t < 1 ? 1 : t;
}

static void ui_text(int x, int y, const char *s, unsigned int color, int scale) {
    text(x, y, s, color, ts(scale));
}

static int min_i(int a, int b) {
    return a < b ? a : b;
}

static void layout_cells(void);
static int docked; /* landscape: a panel is docked on the right */
static int wide;   /* landscape: the panel is a full-width sheet (Лента) */

static void layout_init(void) {
    portrait = FB_H > FB_W;
    U = portrait ? min_i(FB_W * 256 / 480, FB_H * 256 / 800) : min_i(FB_W * 256 / 1024, FB_H * 256 / 768);
    if (U < 160) {
        U = 160;
    }
    CHAR_W = 8 * ts(2) + 2;
    BAR_X = P(16);
    BAR_W = FB_W - 2 * BAR_X;
    BAR_H = P(76);
    BAR_Y = FB_H - BAR_H - P(8);
    BAR_TEXT_X = BAR_X + P(16);
    TITLE_H = P(52);
    int top = P(56);
    if (portrait) {
        /* Phone: core on top, honeycomb below, the panel is a full sheet. */
        PANEL_X = P(8);
        PANEL_W = FB_W - 2 * PANEL_X;
        PANEL_Y = top;
        PANEL_H = BAR_Y - P(8) - PANEL_Y;
        CLOSE_X = PANEL_X + P(8);
        CLOSE_W = P(140);
        CORE_R = P(96);
        CORE_X = FB_W / 2;
        CORE_Y = top + CORE_R + P(16);
        CELL_R = P(52);
    } else {
        PANEL_W = P(412);
        PANEL_X = FB_W - PANEL_W - P(16);
        PANEL_Y = P(68);
        PANEL_H = BAR_Y - P(16) - PANEL_Y;
        CLOSE_W = P(48);
        CLOSE_X = PANEL_X + PANEL_W - CLOSE_W - P(8);
        int avail = (BAR_Y - top) / 2;
        /* Centred above the command bar; with a panel docked on the right the
         * whole ring slides into the free space on the left. */
        CORE_X = docked ? (PANEL_X + P(4)) / 2 : FB_W / 2;
        CORE_Y = top + avail;
        CORE_R = avail * 112 / 314;
        CELL_R = avail * 52 / 314;
        if (ts(1) >= 2) {
            /* TV (1080p): labels are 2x for the sofa, so cells grow to hold them. */
            CELL_R = avail * 74 / 314;
        }
    }
    if (!portrait && wide) {
        PANEL_X = P(16);
        PANEL_W = FB_W - 2 * PANEL_X;
        CLOSE_X = PANEL_X + PANEL_W - CLOSE_W - P(8);
    }
    CLOSE_Y = PANEL_Y + P(6);
    CLOSE_H = TITLE_H - P(12);
    layout_cells();
}
enum { P_NONE = 0, P_FILES, P_TERM, P_PKG, P_MEM, P_FEED, P_SET, P_TASKS };
enum { K_FILES = 0, K_EDITOR, K_TERM, K_PKG, K_MEM, K_FEED, K_SET, K_APP, K_PIN };
static void open_feed(int filter);
static int s_starts(const char *s, const char *prefix);
enum { DESK_UP = 1, DESK_DOWN = 2, DESK_LEFT = 3, DESK_RIGHT = 4, DESK_PGUP = 5, DESK_PGDN = 6, DESK_ESC = 27 };

extern int pkg_info(int index, char *name, unsigned int cap, int *installed, int *app);
extern int pins_get(int index, char *out, unsigned int cap);
extern int heap_percent(void);
extern unsigned int heap_used_kib(void);
extern int fs_file_count(void);
extern const char *shell_cwd(void);
extern void kbd_toggle_layout(void);
extern int kbd_layout_ru(void);
extern int task_count(void);
extern int task_info(int i, char *name, unsigned int cap, unsigned int *ticks);
extern int task_max(void);
extern int sh_complete(char *text, int *len, int cap, char *cands, unsigned int cands_cap);
extern int editor_active(void);
extern int editor_open(const char *file);
extern const unsigned short *editor_screen(void);
extern void editor_cursor(int *row, int *col);
void desktop_run(const char *cmd, char *out, unsigned int size);
void journal_add(char type, const char *what);
int journal_count(void);
int link_peer(char *out);
void link_hello(void);
int journal_get(int i, char *time, char *type, char *text, unsigned int cap);



static const unsigned char ICONS[9][8] = {
    {0x00, 0x70, 0x88, 0xff, 0x81, 0x81, 0x81, 0xff}, /* files: folder */
    {0xfc, 0x86, 0x85, 0xbf, 0x81, 0xbd, 0x81, 0xff}, /* editor: page */
    {0xff, 0x81, 0xa1, 0x91, 0xa1, 0x8d, 0x81, 0xff}, /* terminal: >_ */
    {0x3c, 0x42, 0xff, 0x99, 0x99, 0x81, 0x81, 0xff}, /* packages: box */
    {0x54, 0xfe, 0x82, 0xba, 0xba, 0x82, 0xfe, 0x54}, /* memory: chip */
    {0x00, 0x00, 0x24, 0x00, 0x49, 0xff, 0x49, 0x00}, /* feed: timeline */
    {0x18, 0x5a, 0x3c, 0xe7, 0xe7, 0x3c, 0x5a, 0x18}, /* settings: gear */
    {0x18, 0x3c, 0x7e, 0xff, 0xff, 0x7e, 0x3c, 0x18}, /* program */
    {0x00, 0x70, 0xff, 0x81, 0x99, 0x99, 0x81, 0xff}, /* pinned folder */
};
static const char *const fixed_labels[7] = {"Файлы", "Редактор", "Терминал", "Пакеты", "Память", "Лента", "Настройки"};

struct cell {
    int kind;
    char label[32];
    char arg[FS_NAME];
};
static struct cell cells[CELL_MAX];
static int cell_count;
static char cells_line[512];

static int panel;
static int focus = -1;     /* keyboard focus: cell index, CELL_CORE or -1 */
static int hint = -1;      /* cell matching what is typed in the bar */
static int core_mem;
static int core_tasks;
static int core_alarm; /* seconds the core stays red after a task crash */
extern int task_crash_take(char *out, unsigned int cap);
static int core_phase;
static unsigned int row_color[FB_MAX_H];
static short star_x[80];
static short star_y[80];
static unsigned int star_c[80];

static int utf8_chars(const char *s) {
    int n = 0;
    for (; *s; s++) {
        if (!utf8_is_cont((unsigned char)*s)) {
            n++;
        }
    }
    return n;
}

/* Cut s to at most max characters, adding "..." when it was longer. */
static void utf8_clip(char *s, int max) {
    int chars = 0;
    for (unsigned int k = 0; s[k] != 0; k++) {
        if (!utf8_is_cont((unsigned char)s[k]) && ++chars > max) {
            if (max > 3) {
                unsigned int j = k;
                for (int back = 0; back < 2 && j > 0; back++) {
                    j--;
                    while (j > 0 && utf8_is_cont((unsigned char)s[j])) {
                        j--;
                    }
                }
                s[j] = '.';
                s[j + 1] = '.';
                s[j + 2] = 0;
            } else {
                s[k] = 0;
            }
            return;
        }
    }
}

static void s_copy(char *d, const char *s, unsigned int cap) {
    unsigned int n = 0;
    while (s[n] != 0 && n + 1 < cap) {
        d[n] = s[n];
        n++;
    }
    d[n] = 0;
}

static int s_eq(const char *a, const char *b) {
    while (*a != 0 && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static unsigned int s_cat(char *d, unsigned int used, const char *s, unsigned int cap) {
    while (*s != 0 && used + 1 < cap) {
        d[used++] = *s++;
    }
    d[used] = 0;
    return used;
}

static unsigned int u_cat(char *d, unsigned int used, unsigned int v, unsigned int cap) {
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n > 0 && used + 1 < cap) {
        d[used++] = tmp[--n];
    }
    d[used] = 0;
    return used;
}

static unsigned int blend(unsigned int a, unsigned int b, unsigned int b_parts) {
    unsigned int out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        unsigned int ca = (a >> sh) & 0xffu;
        unsigned int cb = (b >> sh) & 0xffu;
        out |= ((ca * (4u - b_parts) + cb * b_parts) / 4u) << sh;
    }
    return out;
}

static void scene_init(void) {
    for (int y = 0; y < FB_H; y++) {
        unsigned int r = 0x06u + (0x12u - 0x06u) * (unsigned)y / FB_H;
        unsigned int g = 0x0au + (0x1cu - 0x0au) * (unsigned)y / FB_H;
        unsigned int b = 0x1au + (0x40u - 0x1au) * (unsigned)y / FB_H;
        row_color[y] = (r << 16) | (g << 8) | b;
    }
    unsigned int seed = 0x9e3779b9u;
    for (int i = 0; i < 80; i++) {
        seed = seed * 1103515245u + 12345u;
        star_x[i] = (short)((seed >> 8) % FB_W);
        seed = seed * 1103515245u + 12345u;
        star_y[i] = (short)(P(56) + (seed >> 8) % (unsigned)(BAR_Y - P(64)));
        seed = seed * 1103515245u + 12345u;
        unsigned int v = 0x60u + (seed >> 9) % 0x90u;
        star_c[i] = (v << 16) | (v << 8) | (v + 0x20u > 0xffu ? 0xffu : v + 0x20u);
    }
}

static void span(int x, int y, int w, unsigned int color) {
    if (y < 0 || y >= FB_H) {
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (x + w > FB_W) {
        w = FB_W - x;
    }
    unsigned int *dst = fb + (unsigned)y * FB_W + (unsigned)x;
    for (int i = 0; i < w; i++) {
        dst[i] = color;
    }
}

/* Background (gradient + stars) for a rectangle. */
static void bg_fill(int x, int y, int w, int h) {
    if (fb == 0) {
        return;
    }
    for (int row = 0; row < h; row++) {
        if ((row & 31) == 0) {
            ps2_drain();
        }
        int yy = y + row;
        if (yy >= 0 && yy < FB_H) {
            span(x, yy, w, row_color[yy]);
        }
    }
    for (int i = 0; i < 80; i++) {
        int sx = star_x[i];
        int sy = star_y[i];
        if (sx >= x && sx < x + w && sy >= y && sy < y + h) {
            pixel(sx, sy, star_c[i]);
            if ((i & 7) == 0) {
                unsigned int dim = blend(star_c[i], row_color[sy], 2);
                if (sx > x) pixel(sx - 1, sy, dim);
                if (sx + 1 < x + w) pixel(sx + 1, sy, dim);
                if (sy > y) pixel(sx, sy - 1, dim);
                if (sy + 1 < y + h) pixel(sx, sy + 1, dim);
            }
        }
    }
}

static int hex_half(int r, int dy) {
    int a = dy < 0 ? -dy : dy;
    if (a > r) {
        return -1;
    }
    if (2 * a <= r) {
        return r * 887 / 1024;
    }
    return (r - a) * 887 * 2 / 1024;
}

static void hex_fill(int cx, int cy, int r, unsigned int color) {
    for (int dy = -r; dy <= r; dy++) {
        if (((dy + r) & 31) == 0) {
            ps2_drain();
        }
        int hw = hex_half(r, dy);
        if (hw >= 0) {
            span(cx - hw, cy + dy, 2 * hw + 1, color);
        }
    }
}

static void text_c(int cx, int y, const char *s, unsigned int color, int scale) {
    int w = utf8_chars(s) * (8 * ts(scale) + 2) - 2;
    ui_text(cx - w / 2, y, s, color, scale);
}

static void frame_rect(int x, int y, int w, int h, unsigned int line, unsigned int inside) {
    fill(x, y, w, h, line);
    fill(x + 2, y + 2, w - 4, h - 4, inside);
}

static void button(int x, int y, int w, int h, const char *label, unsigned int line, unsigned int inside) {
    frame_rect(x, y, w, h, line, inside);
    text_c(x + w / 2, y + (h - 16) / 2, label, WHITE, 2);
}

static int in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* ---- cells ---- */
static void cell_add(int kind, const char *label, const char *arg) {
    if (cell_count >= CELL_MAX) {
        return;
    }
    struct cell *c = &cells[cell_count++];
    c->kind = kind;
    /* At most 9 characters fit inside a cell. */
    unsigned int n = 0;
    int chars = 0;
    while (label[n] != 0 && n + 4 < sizeof c->label) {
        if (!utf8_is_cont((unsigned char)label[n]) && ++chars > 9) {
            break;
        }
        c->label[n] = label[n];
        n++;
    }
    c->label[n] = 0;
    s_copy(c->arg, arg, sizeof c->arg);
}

/* Fixed cells, then installed programs, then pinned folders. Prints
 * "cells: ..." on the serial line whenever the set changes. */
static int rebuild_cells(void) {
    cell_count = 0;
    for (int i = 0; i < 7; i++) {
        cell_add(i, fixed_labels[i], "");
    }
    char name[FS_NAME];
    int installed = 0;
    int app = 0;
    for (int i = 0; pkg_info(i, name, sizeof name, &installed, &app); i++) {
        if (installed && app) {
            cell_add(K_APP, name, name);
        }
    }
    for (int i = 0; pins_get(i, name, sizeof name); i++) {
        const char *base = name;
        for (const char *c = name; *c != 0; c++) {
            if (*c == '/' && c[1] != 0) {
                base = c + 1;
            }
        }
        cell_add(K_PIN, base, name);
    }
    char line[512];
    unsigned int used = s_cat(line, 0, "cells:", sizeof line);
    for (int i = 0; i < cell_count; i++) {
        used = s_cat(line, used, " ", sizeof line);
        used = s_cat(line, used, cells[i].kind == K_PIN ? cells[i].arg : cells[i].label, sizeof line);
    }
    if (focus >= cell_count && focus != CELL_CORE) {
        focus = -1;
    }
    layout_cells();
    if (s_eq(line, cells_line)) {
        return 0;
    }
    s_copy(cells_line, line, sizeof cells_line);
    console_puts(line);
    console_putc('\n');
    return 1;
}

static const short sin_tab[91] = {0,18,36,54,71,89,107,125,143,160,178,195,213,230,248,265,282,299,316,333,350,367,384,400,416,433,449,465,481,496,512,527,543,558,573,587,602,616,630,644,658,672,685,698,711,724,737,749,761,773,784,796,807,818,828,839,849,859,868,878,887,896,904,912,920,928,935,943,949,956,962,968,974,979,984,989,994,998,1002,1005,1008,1011,1014,1016,1018,1020,1022,1023,1023,1024,1024};

/* sin(deg) * 1024 for any integer degree. */
static int isin(int deg) {
    deg %= 360;
    if (deg < 0) {
        deg += 360;
    }
    if (deg <= 90) {
        return sin_tab[deg];
    }
    if (deg <= 180) {
        return sin_tab[180 - deg];
    }
    return -isin(deg - 180);
}

static void layout_cells(void) {
    if (!portrait) {
        /* Cells evenly on a ring around the core, the first one straight up;
         * cell i and cell n-i are exact mirror images. */
        int ring = (BAR_Y - P(56)) / 2 * 215 / 314;
        int n = cell_count > 0 ? cell_count : 1;
        for (int i = 0; i < CELL_MAX; i++) {
            int k = i < n ? i : n - 1;
            int mirror = k * 2 > n;
            int kk = mirror ? n - k : k;
            int deg = (kk * 360 + n / 2) / n;
            int dx = isin(deg) * ring / 1024;
            int dy = -isin(deg + 90) * ring / 1024;
            cell_cx[i] = CORE_X + (mirror ? -dx : dx);
            cell_cy[i] = CORE_Y + dy;
        }
        return;
    }
    int hw = CELL_R * 887 / 1024;
    int pitch_x = 2 * hw + P(10);
    int cols = (FB_W - P(16) - pitch_x / 2) / pitch_x;
    cols = cols < 2 ? 2 : cols > 6 ? 6 : cols;
    int pitch_y = CELL_R * 3 / 2 + P(10);
    int left = (FB_W - (cols * pitch_x + pitch_x / 2)) / 2 + pitch_x / 2;
    int y0 = CORE_Y + CORE_R + P(24) + CELL_R;
    for (int i = 0; i < CELL_MAX; i++) {
        int row = i / cols;
        int col = i % cols;
        cell_cx[i] = left + col * pitch_x + ((row & 1) ? pitch_x / 2 : 0);
        cell_cy[i] = y0 + row * pitch_y;
    }
}

static int highlighted(void) {
    return hint >= 0 ? hint : focus;
}

static int sheet_open(void) {
    return panel != P_NONE && (portrait || wide);
}

static void draw_cell(int i) {
    if (sheet_open()) {
        return;
    }
    if (i < 0 || i >= cell_count) {
        return;
    }
    struct cell *c = &cells[i];
    int cx = cell_cx[i];
    int cy = cell_cy[i];
    int amber = c->kind == K_FEED || c->kind == K_SET || c->kind == K_APP;
    unsigned int line = amber ? AMBER : CYAN;
    unsigned int mid = amber ? AMBER_DIM : CYAN_DIM;
    unsigned int outer = amber ? AMBER_FAINT : CYAN_FAINT;
    unsigned int inside = CELL;
    if (highlighted() == i) {
        outer = mid;
        mid = amber ? AMBER_MID : CYAN_MID;
        line = YELLOW;
        inside = CELL_HI;
    }
    hex_fill(cx, cy, CELL_R + 6, outer);
    hex_fill(cx, cy, CELL_R + 3, mid);
    hex_fill(cx, cy, CELL_R, line);
    hex_fill(cx, cy, CELL_R - 3, inside);
    blit_rows(cx - 4 * ts(3), cy - CELL_R * 30 / 52, ICONS[c->kind], line, ts(3));
    {
        /* Shrink the label until it fits inside the hexagon. */
        int room = CELL_R * 887 / 512 - 8;
        int sc = ts(1);
        while (sc > 1 && utf8_chars(c->label) * (8 * sc + 2) - 2 > room) {
            sc--;
        }
        int w = utf8_chars(c->label) * (8 * sc + 2) - 2;
        text(cx - w / 2, cy + CELL_R * 6 / 52, c->label, WHITE, sc);
    }
}

/* ---- the Genesis Link peer: a second, smaller core ---- */
static char peer_text[24];
static int send_mode; /* Files panel picks a file to send to the peer */

static int peer_r(void) {
    return CORE_R * 45 / 100;
}

static void peer_pos(int *x, int *y) {
    if (portrait) {
        *x = FB_W - peer_r() - P(14);
        *y = CORE_Y - CORE_R / 2;
    } else {
        *x = P(24) + peer_r();
        *y = P(76) + peer_r();
    }
}

static int highlighted(void);

static void draw_peer(void) {
    if (peer_text[0] == 0 || sheet_open()) {
        return;
    }
    int x, y;
    peer_pos(&x, &y);
    int r = peer_r();
    int on = highlighted() == CELL_PEER;
    hex_fill(x, y, r + 6, on ? AMBER_MID : AMBER_FAINT);
    hex_fill(x, y, r + 2, on ? YELLOW : AMBER);
    hex_fill(x, y, r - 2, on ? CELL_HI : CORE_FILL);
    text_c(x, y - 8 * ts(1) - P(4), "СОСЕД", AMBER, 1);
    text_c(x, y + P(4), peer_text + 9, WHITE, 1); /* the last three MAC bytes */
}

static int cell_at(int x, int y) {
    if (peer_text[0] != 0) {
        int px, py;
        peer_pos(&px, &py);
        if ((x - px) * (x - px) + (y - py) * (y - py) < peer_r() * peer_r()) {
            return CELL_PEER;
        }
    }
    int dx = x - CORE_X;
    int dy = y - CORE_Y;
    if (dx * dx + dy * dy < (CORE_R - P(8)) * (CORE_R - P(8))) {
        return CELL_CORE;
    }
    for (int i = 0; i < cell_count; i++) {
        int ex = x - cell_cx[i];
        int ey = y - cell_cy[i];
        int hit = CELL_R * 88 / 100;
        if (ex * ex + ey * ey < hit * hit) {
            return i;
        }
    }
    return -1;
}

/* ---- core ---- */
static void core_bar(int y, int percent, unsigned int line, unsigned int fillc) {
    int w = CORE_R * 150 / 112;
    int h = P(12);
    int x = CORE_X - w / 2;
    frame_rect(x, y, w, h, line, CORE_FILL);
    int inner = (w - 4) * (percent > 100 ? 100 : percent) / 100;
    fill(x + 2, y + 2, inner, h - 4, fillc);
}

static void draw_core(void) {
    if (sheet_open()) {
        return;
    }
    int warn = core_mem >= 85 || core_alarm > 0;
    int pulse = core_tasks > 0 && core_phase;
    unsigned int line = warn ? RED : CYAN;
    unsigned int mid = warn ? RED_MID : CYAN_MID;
    unsigned int dim = warn ? RED_DIM : CYAN_DIM;
    unsigned int faint = warn ? RED_DIM : CYAN_FAINT;
    int half_w = (CORE_R + 14) * 887 / 1024 + 2;
    bg_fill(CORE_X - half_w, CORE_Y - CORE_R - 14, 2 * half_w + 1, 2 * CORE_R + 29);
    if (pulse) {
        hex_fill(CORE_X, CORE_Y, CORE_R + 13, faint);
        hex_fill(CORE_X, CORE_Y, CORE_R + 9, dim);
        hex_fill(CORE_X, CORE_Y, CORE_R + 5, mid);
        hex_fill(CORE_X, CORE_Y, CORE_R + 1, WHITE);
    } else {
        hex_fill(CORE_X, CORE_Y, CORE_R + 8, faint);
        hex_fill(CORE_X, CORE_Y, CORE_R + 4, dim);
        hex_fill(CORE_X, CORE_Y, CORE_R + 1, line);
    }
    hex_fill(CORE_X, CORE_Y, CORE_R - 3, CORE_FILL);
    hex_fill(CORE_X, CORE_Y, CORE_R - 14, dim);
    hex_fill(CORE_X, CORE_Y, CORE_R - 16, CORE_FILL);
    if (highlighted() == CELL_CORE) {
        hex_fill(CORE_X, CORE_Y, CORE_R - 16, CELL_HI);
    }
    char s[40];
    unsigned int n = s_cat(s, 0, "ПАМЯТЬ ", sizeof s);
    n = u_cat(s, n, (unsigned int)core_mem, sizeof s);
    s_cat(s, n, "%", sizeof s);
    text_c(CORE_X, CORE_Y - CORE_R * 76 / 112, "ЯДРО", warn ? RED : MUTED, 1);
    text_c(CORE_X, CORE_Y - CORE_R * 54 / 112, s, warn ? RED : WHITE, 2);
    core_bar(CORE_Y - CORE_R * 30 / 112, core_mem, mid, warn ? RED : CYAN);
    n = s_cat(s, 0, "ЗАДАЧ ", sizeof s);
    u_cat(s, n, (unsigned int)core_tasks, sizeof s);
    text_c(CORE_X, CORE_Y, s, AMBER, 2);
    int max_bg = task_max() - 1;
    core_bar(CORE_Y + CORE_R * 24 / 112, max_bg > 0 ? core_tasks * 100 / max_bg : 0, AMBER_MID, AMBER);
    text_c(CORE_X, CORE_Y + CORE_R * 50 / 112,
           core_alarm > 0 ? "сбой задачи!" : warn ? "мало памяти!" : "щёлкни: задачи", warn ? RED : MUTED, 1);
}

static int core_sample(void) {
    int mem = heap_percent();
    int tasks = task_count();
    if (mem == core_mem && tasks == core_tasks) {
        return 0;
    }
    core_mem = mem;
    core_tasks = tasks;
    char s[48];
    unsigned int n = s_cat(s, 0, "core: mem ", sizeof s);
    n = u_cat(s, n, (unsigned int)mem, sizeof s);
    n = s_cat(s, n, "% tasks ", sizeof s);
    n = u_cat(s, n, (unsigned int)tasks, sizeof s);
    s_cat(s, n, "\n", sizeof s);
    console_puts(s);
    return 1;
}

/* ---- top line ---- */
void clock_now(char *out);

/* The clock beside «Назад» in full-screen viewers; HH:MM on narrow phones. */
static void viewer_clock(void) {
    int full = FB_W - 13 * 18 - 24 > BACK_X + BACK_W + 12;
    int x = full ? FB_W - 13 * 18 - 24 : FB_W - 5 * 18 - 16;
    char t[16];
    unsigned int n = 0;
    for (const char *c = full ? clock_text : clock_text + 5; *c != 0 && n < (full ? 13u : 5u); c++) {
        t[n++] = *c;
    }
    t[n] = 0;
    fill(x, 28, (int)n * 18, 16, BG);
    text(x, 28, t, INK, 2);
}

static void draw_clock_chip(void) {
    clock_now(clock_text + 5);
    if (view == VIEW_VIEWER) {
        viewer_clock();
        return;
    }
    int x = FB_W - P(98) - 8 * CHAR_W;
    bg_fill(x, P(16), 8 * CHAR_W, 8 * ts(2) + 2);
    ui_text(x, P(18), clock_text + 5, WHITE, 2);
}

static void draw_badge(void) {
    frame_rect(FB_W - P(80), P(10), P(64), P(34), AMBER_MID, CELL);
    text_c(FB_W - P(48), P(10) + (P(34) - 8 * ts(2)) / 2, kbd_layout_ru() ? "RU" : "EN", AMBER, 2);
}

static void draw_top(void) {
    ui_text(P(24), P(12), "genesis", CYAN, 3);
    if (!portrait) {
        ui_text(P(24) + 7 * (8 * ts(3) + 2) + P(10), P(24), "ядро", MUTED, 1);
    }
    draw_clock_chip();
    draw_badge();
}

/* ---- files ---- */
enum { FROWS_MAX = 96 };
#define BODY_Y (PANEL_Y + TITLE_H + P(10))
#define FROW_Y0 (BODY_Y + 8 * ts(1) + P(10))
#define FROW_H P(48)
#define SCROLL_Y (PANEL_Y + PANEL_H - P(60))
#define FROW_VIS ((SCROLL_Y - P(8) - FROW_Y0) / FROW_H)
#define BTN_H P(48)
static char desk_dir[FS_NAME] = "/home";
static int file_slot[FROWS_MAX];
static char row_name[FROWS_MAX][FS_PART + 1];
static int row_dir[FROWS_MAX];
static int file_rows;
static int file_top;

static int is_hidden_name(const char *name) {
    static const char *const hidden[] = {
        "open", "hello", "vtxt", "vdocx", "vpdf", "vimg", "vmp4",
    };
    if (name[0] == '.') {
        return 1;
    }
    for (unsigned int i = 0; i < sizeof hidden / sizeof hidden[0]; i++) {
        if (s_eq(name, hidden[i])) {
            return 1;
        }
    }
    return 0;
}

static void collect_files(void) {
    file_rows = 0;
    file_top = 0;
    if (!(desk_dir[0] == '/' && desk_dir[1] == 0)) {
        file_slot[0] = -1;
        s_copy(row_name[0], "..", sizeof row_name[0]);
        row_dir[0] = 1;
        file_rows = 1;
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < FS_MAX && file_rows < FROWS_MAX; i++) {
            char name[FS_PART + 1];
            int is_dir = 0;
            if (!fs_dir_entry(i, desk_dir, name, sizeof name, &is_dir) || is_dir != (pass == 0)) {
                continue;
            }
            if (is_hidden_name(name)) {
                continue;
            }
            file_slot[file_rows] = i;
            row_dir[file_rows] = is_dir;
            s_copy(row_name[file_rows], name, sizeof row_name[file_rows]);
            file_rows++;
        }
    }
}

/* "ls: a b/ c" for the shown folder, so the serial log mirrors the screen. */
static void rows_summary(char *dest, unsigned int cap) {
    unsigned int used = 0;
    dest[0] = 0;
    for (int r = 0; r < file_rows; r++) {
        if (file_slot[r] < 0) {
            continue;
        }
        if (used != 0) {
            used = s_cat(dest, used, " ", cap);
        }
        used = s_cat(dest, used, row_name[r], cap);
        if (row_dir[r]) {
            used = s_cat(dest, used, "/", cap);
        }
    }
}

/* ---- terminal scrollback ---- */
enum { TERM_CAP = 4096 };
#define TERM_COLS ((PANEL_W - P(32)) / (8 * ts(1) + 2))
#define TERM_LINE_H (8 * ts(1) + P(6))
static char term[TERM_CAP];
static unsigned int term_len;

static void term_add(const char *s) {
    for (; *s != 0; s++) {
        if (term_len + 1 >= TERM_CAP) {
            unsigned int cut = TERM_CAP / 2;
            while (cut < term_len && term[cut - 1] != '\n') {
                cut++;
            }
            for (unsigned int i = cut; i < term_len; i++) {
                term[i - cut] = term[i];
            }
            term_len -= cut;
        }
        term[term_len++] = *s;
    }
    term[term_len] = 0;
}

/* ---- bar ---- */
enum { BAR_CAP = 96, BAR_HIST = 10 };
static char bar_cmd[BAR_CAP];
static int bar_len;
static char bar_out[96];
static char bar_hist[BAR_HIST][BAR_CAP];
static int bar_hist_n;
static int bar_hist_pos;
static int blink_on = 1;
static unsigned int blink_ticks;
static volatile int blink_due;

static int prompt_chars(void) {
    return 8 + utf8_chars(shell_cwd()) + 2; /* "genesis:" cwd "> " */
}

static void draw_bar_cursor(void) {
    int x = BAR_TEXT_X + (prompt_chars() + utf8_chars(bar_cmd)) * CHAR_W;
    fill(x, BAR_Y + P(10), CHAR_W - 4, 8 * ts(2) + 2, blink_on ? YELLOW : PANEL_BAR);
}

static void draw_bar(void) {
    frame_rect(BAR_X, BAR_Y, BAR_W, BAR_H, CYAN_MID, PANEL_BAR);
    fill(BAR_X + 2, BAR_Y + BAR_H - 4, BAR_W - 4, 2, CYAN_DIM);
    char p[FS_NAME + 16];
    unsigned int n = s_cat(p, 0, "genesis:", sizeof p);
    n = s_cat(p, n, shell_cwd(), sizeof p);
    s_cat(p, n, "> ", sizeof p);
    ui_text(BAR_TEXT_X, BAR_Y + P(12), p, YELLOW, 2);
    ui_text(BAR_TEXT_X + prompt_chars() * CHAR_W, BAR_Y + P(12), bar_cmd, WHITE, 2);
    const char *out = bar_out[0] ? bar_out : "Введите команду и нажмите Enter (help, Tab дополняет)";
    int room = BAR_W - (BAR_TEXT_X - BAR_X) * 2;
    if (!bar_out[0] && utf8_chars(out) * (8 * ts(1) + 2) > room) {
        out = "Команда + Enter, Tab дополняет";
    }
    int big = bar_out[0] != 0 && utf8_chars(bar_out) * CHAR_W <= room;
    char clip[96];
    int max_chars = room / (8 * ts(1 + big) + 2);
    unsigned int k = 0;
    int chars = 0;
    while (out[k] != 0 && k + 4 < sizeof clip) {
        if (!utf8_is_cont((unsigned char)out[k]) && ++chars > max_chars) {
            break;
        }
        clip[k] = out[k];
        k++;
    }
    clip[k] = 0;
    ui_text(BAR_TEXT_X, BAR_Y + BAR_H - P(12) - 8 * ts(1 + big), clip, MUTED, 1 + big);
    draw_bar_cursor();
}

/* ---- panel ---- */
static const char *panel_title(void) {
    switch (panel) {
    case P_FILES: return send_mode ? "Отправить файл" : "Файлы";
    case P_TERM: return "Терминал";
    case P_PKG: return "Пакеты";
    case P_MEM: return "Память";
    case P_FEED: return "Лента";
    case P_SET: return "Настройки";
    case P_TASKS: return "Задачи";
    default: return "";
    }
}

static char pkg_rows[8][FS_NAME];
static int pkg_rows_installed[8];
static int pkg_row_count;

/* Everything in the panel that can be clicked, tapped or picked with the
 * arrows + Enter (TV remote) is an item; the same code runs either way. */
enum { A_CLOSE = 0, A_ROW, A_UP, A_DOWN, A_PKG, A_LAYOUT, A_REBOOT, A_POWEROFF, A_KILL, A_CHIP, A_CARD, A_FEED_PREV,
       A_FEED_NEXT, A_TO_FEED, A_BACK };
enum { ITEM_MAX = 32 };
static int it_n;
static int it_x[ITEM_MAX], it_y[ITEM_MAX], it_w[ITEM_MAX], it_h[ITEM_MAX], it_act[ITEM_MAX], it_arg[ITEM_MAX];
/* Navigation history of the open window: every step inside it (a folder, a
 * viewer, a Лента filter, «В ленту») pushes where we were. Back pops exactly
 * one step; with nothing left it closes the window. Esc / X close it at once. */
struct nav_step { int panel; char dir[FS_NAME]; int focus, top, filter; };
enum { NAV_MAX = 24 };
static struct nav_step nav_stack[NAV_MAX];
static int nav_n;
static void nav_push(void);
static void nav_back(void);
static int pfocus = -1; /* focused item, -1 = none (mouse use) */
static int pfocus_pending; /* the panel is being opened with Enter */

static void item(int x, int y, int w, int h, int act, int arg) {
    if (it_n >= ITEM_MAX) {
        return;
    }
    it_x[it_n] = x;
    it_y[it_n] = y;
    it_w[it_n] = w;
    it_h[it_n] = h;
    it_act[it_n] = act;
    it_arg[it_n] = arg;
    it_n++;
}

/* ---- «Лента»: the journal as a timeline ----
 * Chips filter by kind; cards sit on a line (horizontal in landscape,
 * vertical on a phone) that ends at a glowing «Сейчас» marker. */
enum { F_ALL = 0, F_FILES, F_EDITOR, F_TERM, F_PKG, F_PROG, F_COUNT };
static const char *const feed_chip[F_COUNT] = {"Все", "Файлы", "Редактор", "Терминал", "Пакеты", "Программы"};
enum { FEED_MAX = 40 };
static int feed_filter;
static int feed_ids[FEED_MAX]; /* journal indexes passing the filter, oldest first */
static int feed_n;
static int feed_first;   /* first visible card */
static int feed_confirm = -1; /* journal index waiting for a second Enter */

static int feed_match(char type, int filter) {
    switch (filter) {
    case F_FILES: return type == 'o';
    case F_EDITOR: return type == 'e' || type == 's';
    case F_TERM: return type == 'c';
    case F_PKG: return type == 'i' || type == 'r';
    case F_PROG: return type == 'p' || type == 'k' || type == 'x';
    default: return 1;
    }
}

static const char *feed_type_name(char type) {
    switch (type) {
    case 'o': return "открыт";
    case 'e': return "правка";
    case 's': return "сохранён";
    case 'i': return "установка";
    case 'r': return "удаление";
    case 'p': return "запуск";
    case 'k': return "остановка";
    case 'x': return "сбой";
    default: return "команда";
    }
}

static int feed_visible(void);

static void feed_collect(void) {
    feed_n = 0;
    char time[9];
    char type;
    char text[80];
    for (int i = 0; journal_get(i, time, &type, text, sizeof text) && feed_n < FEED_MAX; i++) {
        if (feed_match(type, feed_filter)) {
            feed_ids[feed_n++] = i;
        }
    }
    int vis = feed_visible();
    if (feed_first > feed_n - vis) {
        feed_first = feed_n - vis;
    }
    if (feed_first < 0) {
        feed_first = 0;
    }
}

#define FEED_CHIP_H (8 * ts(1) + P(18))
#define FEED_CARD_W (portrait ? PANEL_W - P(72) : P(208))
#define FEED_CARD_H (portrait ? 2 * (8 * ts(1)) + 8 * ts(2) + P(30) : 2 * (8 * ts(1)) + 8 * ts(2) + P(40))
#define FEED_GAP P(14)

static int feed_line_top(void) {
    int below_chips = BODY_Y + 2 * (FEED_CHIP_H + P(8)) + P(10);
    if (portrait) {
        return below_chips;
    }
    /* Landscape: the cards and the line sit in the middle of the sheet. */
    int mid = PANEL_Y + PANEL_H / 2 - FEED_CARD_H;
    return mid > below_chips ? mid : below_chips;
}

/* How many cards fit along the line. */
static int feed_visible(void) {
    int along = portrait ? PANEL_Y + PANEL_H - P(60) - feed_line_top() : PANEL_W - P(32) - 2 * P(44) - P(70);
    int step = portrait ? FEED_CARD_H + FEED_GAP : FEED_CARD_W + FEED_GAP;
    int n = along / step;
    return n < 1 ? 1 : n;
}

static void feed_say(void) {
    char s[160];
    unsigned int n = s_cat(s, 0, "feed: ", sizeof s);
    n = s_cat(s, n, feed_chip[feed_filter], sizeof s);
    n = s_cat(s, n, " ", sizeof s);
    n = u_cat(s, n, (unsigned int)feed_n, sizeof s);
    s_cat(s, n, "\n", sizeof s);
    console_puts(s);
}

static void draw_feed(int x0, int y0, int inner_w) {
    (void)inner_w;
    int t1 = 8 * ts(1);
    int t2 = 8 * ts(2);
    /* Chips: two rows on a phone, one row otherwise. */
    int cx = x0;
    int cy = y0;
    for (int f = 0; f < F_COUNT; f++) {
        int w = utf8_chars(feed_chip[f]) * (t1 + 2) + P(24);
        if (cx + w > PANEL_X + PANEL_W - P(16)) {
            cx = x0;
            cy += FEED_CHIP_H + P(8);
        }
        int on = f == feed_filter;
        frame_rect(cx, cy, w, FEED_CHIP_H, on ? AMBER : CYAN_DIM, on ? AMBER_DIM : CELL);
        ui_text(cx + P(12), cy + (FEED_CHIP_H - t1) / 2, feed_chip[f], on ? WHITE : MUTED, 1);
        item(cx, cy, w, FEED_CHIP_H, A_CHIP, f);
        cx += w + P(8);
    }
    int vis = feed_visible();
    int top = feed_line_top();
    char time[9];
    char type;
    char text[80];
    if (feed_n == 0) {
        ui_text(x0, top + P(20), "Пока пусто: выполните команду,", MUTED, 1);
        ui_text(x0, top + P(26) + t1, "откройте или сохраните файл.", MUTED, 1);
    }
    if (portrait) {
        int lx = PANEL_X + P(30);
        int bottom = PANEL_Y + PANEL_H - P(56);
        fill(lx - 1, top, 3, bottom - top, CYAN_DIM);
        for (int k = 0; k < vis && feed_first + k < feed_n; k++) {
            int id = feed_ids[feed_first + k];
            journal_get(id, time, &type, text, sizeof text);
            int y = top + k * (FEED_CARD_H + FEED_GAP);
            int x = lx + P(26);
            fill(lx - P(5), y + FEED_CARD_H / 2 - P(5), P(11), P(11), type == 'c' ? CYAN : type == 'x' ? RED : AMBER);
            frame_rect(x, y, FEED_CARD_W, FEED_CARD_H, id == feed_confirm ? RED : CYAN_MID, CELL);
            time[5] = 0;
            ui_text(x + P(10), y + P(8), time, MUTED, 1);
            ui_text(x + P(10) + 6 * (t1 + 2), y + P(8), feed_type_name(type), type == 'c' ? CYAN : type == 'x' ? RED : AMBER, 1);
            int max_chars = (FEED_CARD_W - P(20)) / (t2 + 2);
            char clip[80];
            s_copy(clip, text, sizeof clip);
            utf8_clip(clip, max_chars);
            ui_text(x + P(10), y + P(14) + t1, id == feed_confirm ? "Enter: повторить?" : clip, WHITE, 2);
            item(x, y, FEED_CARD_W, FEED_CARD_H, A_CARD, feed_first + k);
        }
        /* «Сейчас»: a glowing bar under the newest card. */
        int ny = top + (feed_n - feed_first < vis ? feed_n - feed_first : vis) * (FEED_CARD_H + FEED_GAP);
        if (feed_first + vis >= feed_n) {
            fill(PANEL_X + P(10), ny - P(2), PANEL_W - P(20), P(7), AMBER_DIM);
            fill(PANEL_X + P(10), ny, PANEL_W - P(20), P(3), YELLOW);
            ui_text(PANEL_X + P(16), ny + P(8), "Сейчас", YELLOW, 1);
        }
        int bw = P(120);
        if (feed_first > 0) {
            button(PANEL_X + P(16), bottom + P(4), bw, P(44), "Выше", CYAN_MID, CELL);
            item(PANEL_X + P(16), bottom + P(4), bw, P(44), A_FEED_PREV, 0);
        }
        if (feed_first + vis < feed_n) {
            button(PANEL_X + PANEL_W - bw - P(16), bottom + P(4), bw, P(44), "Ниже", CYAN_MID, CELL);
            item(PANEL_X + PANEL_W - bw - P(16), bottom + P(4), bw, P(44), A_FEED_NEXT, 0);
        }
        return;
    }
    int ly = top + FEED_CARD_H + P(40);
    int left = PANEL_X + P(16) + P(44);
    fill(PANEL_X + P(16), ly - 1, PANEL_W - P(32), 3, CYAN_DIM);
    int k = 0;
    for (; k < vis && feed_first + k < feed_n; k++) {
        int id = feed_ids[feed_first + k];
        journal_get(id, time, &type, text, sizeof text);
        int x = left + k * (FEED_CARD_W + FEED_GAP);
        unsigned int dot = type == 'c' ? CYAN : type == 'x' ? RED : AMBER;
        fill(x + FEED_CARD_W / 2 - 1, top + FEED_CARD_H, 3, ly - top - FEED_CARD_H, CYAN_DIM);
        fill(x + FEED_CARD_W / 2 - P(6), ly - P(6), P(13), P(13), dot);
        frame_rect(x, top, FEED_CARD_W, FEED_CARD_H, id == feed_confirm ? RED : CYAN_MID, CELL);
        ui_text(x + P(10), top + P(10), time, MUTED, 1);
        ui_text(x + P(10), top + P(16) + t1, feed_type_name(type), dot, 1);
        int big = utf8_chars(text) * (t2 + 2) <= FEED_CARD_W - P(20);
        int max_chars = (FEED_CARD_W - P(20)) / ((big ? t2 : t1) + 2);
        char clip[80];
        s_copy(clip, text, sizeof clip);
        utf8_clip(clip, max_chars);
        ui_text(x + P(10), top + P(24) + 2 * t1, id == feed_confirm ? "Повторить?" : clip, WHITE, big ? 2 : 1);
        item(x, top, FEED_CARD_W, FEED_CARD_H, A_CARD, feed_first + k);
        time[5] = 0;
        ui_text(x + FEED_CARD_W / 2 - 2 * (t1 + 2), ly + P(14), time, MUTED, 1);
    }
    if (feed_first + vis >= feed_n) {
        /* «Сейчас»: a glowing vertical marker after the newest card. */
        int nx = left + k * (FEED_CARD_W + FEED_GAP) + P(10);
        fill(nx - P(4), top - P(6), P(11), ly - top + P(30), AMBER_DIM);
        fill(nx - 1, top - P(6), P(3), ly - top + P(30), YELLOW);
        ui_text(nx + P(10), ly - P(30), "Сейчас", YELLOW, 1);
    }
    int bh = FEED_CARD_H;
    if (feed_first > 0) {
        button(PANEL_X + P(16), top, P(36), bh, "<", CYAN_MID, CELL);
        item(PANEL_X + P(16), top, P(36), bh, A_FEED_PREV, 0);
    }
    if (feed_first + vis < feed_n) {
        int bx = PANEL_X + PANEL_W - P(16) - P(36);
        button(bx, top, P(36), bh, ">", CYAN_MID, CELL);
        item(bx, top, P(36), bh, A_FEED_NEXT, 0);
    }
    if (feed_confirm >= 0) {
        ui_text(x0, ly + P(44) + t1, "Опасная команда: Enter ещё раз — выполнить, Esc — отмена", RED, 1);
    } else {
        ui_text(x0, ly + P(44) + t1, "Стрелки — по ленте, Enter — открыть снова или повторить", MUTED, 1);
    }
}

static void draw_panel_body(void) {
    int x0 = PANEL_X + P(16);
    int y0 = BODY_Y;
    int inner_w = PANEL_W - P(32);
    int t1 = 8 * ts(1);
    int t2 = 8 * ts(2);
    if (panel == P_FILES) {
        ui_text(x0, y0, desk_dir, MUTED, 1);
        for (int i = 0; i < FROW_VIS && file_top + i < file_rows; i++) {
            int r = file_top + i;
            int top = FROW_Y0 + i * FROW_H;
            int ty = top + (FROW_H - P(4) - t2) / 2;
            fill(PANEL_X + P(8), top, PANEL_W - P(16), FROW_H - P(4), (i & 1) ? CELL : CELL_HI);
            item(PANEL_X + P(8), top, PANEL_W - P(16), FROW_H - P(4), A_ROW, r);
            if (file_slot[r] < 0) {
                ui_text(x0, ty, ".. Назад", MUTED, 2);
                continue;
            }
            char name[FS_PART + 16];
            int max_chars = (inner_w - t2 - P(12)) / CHAR_W;
            unsigned int n = s_cat(name, 0, row_name[r], (unsigned int)(max_chars > 2 ? max_chars : 2));
            if (row_dir[r]) {
                s_cat(name, n, "/", sizeof name);
            }
            blit_rows(x0, ty, ICONS[row_dir[r] ? K_FILES : K_EDITOR], row_dir[r] ? AMBER : CYAN_MID, ts(2));
            ui_text(x0 + t2 + P(12), ty, name, row_dir[r] ? AMBER : WHITE, 2);
        }
        int bw = P(124);
        button(x0, SCROLL_Y, bw, BTN_H, "Выше", CYAN_MID, CELL);
        item(x0, SCROLL_Y, bw, BTN_H, A_UP, 0);
        button(x0 + bw + P(12), SCROLL_Y, bw, BTN_H, "Ниже", CYAN_MID, CELL);
        item(x0 + bw + P(12), SCROLL_Y, bw, BTN_H, A_DOWN, 0);
        char s[32];
        unsigned int n = u_cat(s, 0, (unsigned int)(file_rows == 0 ? 0 : file_top + 1), sizeof s);
        n = s_cat(s, n, "-", sizeof s);
        int last = file_top + FROW_VIS < file_rows ? file_top + FROW_VIS : file_rows;
        n = u_cat(s, n, (unsigned int)last, sizeof s);
        n = s_cat(s, n, "/", sizeof s);
        u_cat(s, n, (unsigned int)file_rows, sizeof s);
        ui_text(x0 + 2 * bw + P(24), SCROLL_Y + (BTN_H - t1) / 2, s, MUTED, 1);
        return;
    }
    if (panel == P_TERM) {
        /* Wrap the scrollback and show its last lines. */
        static unsigned short starts[600];
        static unsigned char lens[600];
        int cols = TERM_COLS;
        int lines = 0;
        unsigned int i = 0;
        while (i < term_len) {
            unsigned int s = i;
            int chars = 0;
            while (i < term_len && term[i] != '\n' && chars < cols && i - s < 200) {
                i++;
                while (i < term_len && utf8_is_cont((unsigned char)term[i])) {
                    i++;
                }
                chars++;
            }
            if (lines == 600) {
                for (int k = 1; k < 600; k++) {
                    starts[k - 1] = starts[k];
                    lens[k - 1] = lens[k];
                }
                lines--;
            }
            starts[lines] = (unsigned short)s;
            lens[lines] = (unsigned char)(i - s);
            lines++;
            if (i < term_len && term[i] == '\n') {
                i++;
            }
        }
        int fit = (PANEL_Y + PANEL_H - P(8) - y0) / TERM_LINE_H;
        int first = lines > fit ? lines - fit : 0;
        for (int l = first; l < lines; l++) {
            char buf[256];
            unsigned int n = lens[l];
            for (unsigned int k = 0; k < n; k++) {
                buf[k] = term[starts[l] + k];
            }
            buf[n] = 0;
            int prompt_line = n > 8 && buf[0] == 'g' && buf[7] == ':';
            ui_text(x0, y0 + (l - first) * TERM_LINE_H, buf, prompt_line ? YELLOW : WHITE, 1);
        }
        if (lines == 0) {
            ui_text(x0, y0, "Команды из строки внизу", MUTED, 1);
            ui_text(x0, y0 + TERM_LINE_H, "пишут сюда.", MUTED, 1);
        }
        return;
    }
    if (panel == P_PKG) {
        char name[FS_NAME];
        int installed = 0;
        int app = 0;
        int row_h = BTN_H + P(20);
        int bw = P(150);
        pkg_row_count = 0;
        for (int i = 0; pkg_row_count < 8 && pkg_info(i, name, sizeof name, &installed, &app); i++) {
            int top = y0 + pkg_row_count * (row_h + P(6));
            fill(PANEL_X + P(8), top, PANEL_W - P(16), row_h, CELL);
            blit_rows(x0, top + P(8), ICONS[app ? 7 : K_PKG], app ? AMBER : CYAN, ts(2));
            ui_text(x0 + t2 + P(10), top + P(8), name, WHITE, 2);
            ui_text(x0 + t2 + P(10), top + P(14) + t2, app ? (installed ? "программа +" : "программа")
                                                         : (installed ? "библиотека +" : "библиотека"),
                 MUTED, 1);
            int bx = PANEL_X + PANEL_W - bw - P(16);
            button(bx, top + P(10), bw, BTN_H, installed ? "Удалить" : "Установить",
                   installed ? RED_MID : CYAN_MID, installed ? RED_DIM : CYAN_DIM);
            item(bx, top + P(10), bw, BTN_H, A_PKG, pkg_row_count);
            s_copy(pkg_rows[pkg_row_count], name, sizeof pkg_rows[0]);
            pkg_rows_installed[pkg_row_count] = installed;
            pkg_row_count++;
        }
        return;
    }
    if (panel == P_MEM) {
        char s[64];
        int line = t2 + P(14);
        memory_summary(s, sizeof s);
        ui_text(x0, y0, "ОЗУ машины:", MUTED, 1);
        ui_text(x0, y0 + t1 + P(6), s, WHITE, 2);
        int y = y0 + t1 + P(6) + line + P(10);
        unsigned int n = s_cat(s, 0, "Куча: ", sizeof s);
        n = u_cat(s, n, heap_used_kib(), sizeof s);
        n = s_cat(s, n, "/1024 КиБ ", sizeof s);
        n = u_cat(s, n, (unsigned int)core_mem, sizeof s);
        s_cat(s, n, "%", sizeof s);
        ui_text(x0, y, "Память программ и пакетов:", MUTED, 1);
        ui_text(x0, y + t1 + P(6), s, core_mem >= 85 ? RED : WHITE, 2);
        y += t1 + P(6) + line;
        frame_rect(x0, y, inner_w, P(16), CYAN_MID, CORE_FILL);
        fill(x0 + 2, y + 2, (inner_w - 4) * (core_mem > 100 ? 100 : core_mem) / 100, P(16) - 4, core_mem >= 85 ? RED : CYAN);
        y += P(36);
        n = s_cat(s, 0, "Задач в фоне: ", sizeof s);
        n = u_cat(s, n, (unsigned int)core_tasks, sizeof s);
        n = s_cat(s, n, "/", sizeof s);
        u_cat(s, n, (unsigned int)(task_max() - 1), sizeof s);
        ui_text(x0, y, s, AMBER, 2);
        y += line;
        n = s_cat(s, 0, "Файлов: ", sizeof s);
        n = u_cat(s, n, (unsigned int)fs_file_count(), sizeof s);
        n = s_cat(s, n, "/", sizeof s);
        u_cat(s, n, FS_MAX, sizeof s);
        ui_text(x0, y, s, WHITE, 2);
        y += line + P(10);
        ui_text(x0, y, "Ядро краснеет, когда куча", MUTED, 1);
        ui_text(x0, y + t1 + P(6), "заполнена больше чем на 85%.", MUTED, 1);
        return;
    }
    if (panel == P_FEED) {
        draw_feed(x0, y0, inner_w);
        return;
    }
    if (panel == P_SET) {
        int h = BTN_H + P(4);
        int step = h + P(14);
        button(x0, y0 + P(10), inner_w, h, kbd_layout_ru() ? "Раскладка: RU" : "Раскладка: EN", CYAN_MID, CELL);
        item(x0, y0 + P(10), inner_w, h, A_LAYOUT, 0);
        button(x0, y0 + P(10) + step, inner_w, h, "Перезагрузка", AMBER_MID, CELL);
        item(x0, y0 + P(10) + step, inner_w, h, A_REBOOT, 0);
        button(x0, y0 + P(10) + 2 * step, inner_w, h, "Выключить", RED_MID, RED_DIM);
        item(x0, y0 + P(10) + 2 * step, inner_w, h, A_POWEROFF, 0);
        ui_text(x0, y0 + P(30) + 3 * step, "Ctrl+Пробел или Alt+Shift", MUTED, 1);
        return;
    }
    if (panel == P_TASKS) {
        int rows = 0;
        int row_h = BTN_H + P(16);
        int bw = P(120);
        for (int id = 0; id < task_max() && rows < 8; id++) {
            char name[FS_NAME];
            unsigned int ticks = 0;
            if (!task_info(id, name, sizeof name, &ticks)) {
                continue;
            }
            int top = y0 + rows * (row_h + P(6));
            fill(PANEL_X + P(8), top, PANEL_W - P(16), row_h, CELL);
            char s[64];
            unsigned int n = u_cat(s, 0, (unsigned int)id, sizeof s);
            n = s_cat(s, n, "  ", sizeof s);
            s_cat(s, n, name, sizeof s);
            ui_text(x0, top + P(8), s, id == 0 ? MUTED : WHITE, 2);
            n = s_cat(s, 0, "тиков: ", sizeof s);
            u_cat(s, n, ticks, sizeof s);
            ui_text(x0, top + P(14) + t2, s, MUTED, 1);
            if (id != 0) {
                int bx = PANEL_X + PANEL_W - bw - P(16);
                button(bx, top + P(8), bw, BTN_H, "стоп", RED_MID, RED_DIM);
                item(bx, top + P(8), bw, BTN_H, A_KILL, id);
            }
            rows++;
        }
        if (rows <= 1) {
            ui_text(x0, y0 + row_h + P(20), "Фоновых задач нет. Запустите:", MUTED, 1);
            ui_text(x0, y0 + row_h + P(26) + t1, "spawn pulse", YELLOW, 1);
        }
    }
}

static void draw_focus_ring(int x, int y, int w, int h) {
    int t = P(3) < 3 ? 3 : P(3);
    fill(x - t, y - t, w + 2 * t, t, YELLOW);
    fill(x - t, y + h, w + 2 * t, t, YELLOW);
    fill(x - t, y, t, h, YELLOW);
    fill(x + w, y, t, h, YELLOW);
}

static void draw_panel(void) {
    if (panel == P_NONE) {
        return;
    }
    for (int y = PANEL_Y; y < PANEL_Y + PANEL_H; y++) {
        if ((y & 31) == 0) {
            ps2_drain();
        }
        span(PANEL_X, y, PANEL_W, blend(row_color[y], PANEL, 3));
    }
    fill(PANEL_X, PANEL_Y, PANEL_W, 2, CYAN_MID);
    fill(PANEL_X, PANEL_Y + PANEL_H - 2, PANEL_W, 2, CYAN_MID);
    fill(PANEL_X, PANEL_Y, 2, PANEL_H, CYAN_MID);
    fill(PANEL_X + PANEL_W - 2, PANEL_Y, 2, PANEL_H, CYAN_MID);
    fill(PANEL_X + 2, PANEL_Y + 2, PANEL_W - 4, TITLE_H - 2, PANEL_BAR);
    fill(PANEL_X + 2, PANEL_Y + TITLE_H, PANEL_W - 4, 1, CYAN_DIM);
    int title_y = PANEL_Y + (TITLE_H - 8 * ts(2)) / 2;
    it_n = 0;
    item(CLOSE_X, CLOSE_Y, CLOSE_W, CLOSE_H, A_CLOSE, 0);
    if (portrait) {
        /* Phone sheet: «Назад» on the left, like a mobile app bar. */
        button(CLOSE_X, CLOSE_Y, CLOSE_W, CLOSE_H, "< Назад", CYAN_MID, CELL);
        ui_text(CLOSE_X + CLOSE_W + P(14), title_y, panel_title(), CYAN, 2);
    } else {
        int tx = PANEL_X + P(16);
        if (nav_n > 0) {
            /* one step back inside this window */
            button(PANEL_X + P(8), CLOSE_Y, CLOSE_W, CLOSE_H, "<", CYAN_MID, CELL);
            item(PANEL_X + P(8), CLOSE_Y, CLOSE_W, CLOSE_H, A_BACK, 0);
            tx = PANEL_X + P(16) + CLOSE_W;
        }
        ui_text(tx, title_y, panel_title(), CYAN, 2);
        frame_rect(CLOSE_X, CLOSE_Y, CLOSE_W, CLOSE_H, RED_MID, RED_DIM);
        text_c(CLOSE_X + CLOSE_W / 2, CLOSE_Y + (CLOSE_H - 8 * ts(2)) / 2, "X", WHITE, 2);
    }
    draw_panel_body();
    if (panel != P_FEED) {
        /* «В ленту»: this panel's history on the timeline (tap / D-pad path). */
        int fw = utf8_chars("В ленту") * (8 * ts(1) + 2) + P(20);
        int fx = portrait ? PANEL_X + PANEL_W - fw - P(8) : CLOSE_X - fw - P(8);
        frame_rect(fx, CLOSE_Y, fw, CLOSE_H, AMBER_MID, CELL);
        ui_text(fx + P(10), CLOSE_Y + (CLOSE_H - 8 * ts(1)) / 2, "В ленту", AMBER, 1);
        item(fx, CLOSE_Y, fw, CLOSE_H, A_TO_FEED, 0);
    }
    if (pfocus >= it_n) {
        pfocus = it_n - 1;
    }
    if (pfocus >= 0) {
        draw_focus_ring(it_x[pfocus], it_y[pfocus], it_w[pfocus], it_h[pfocus]);
    }
}

/* ---- links: program -> library / core, glowing for a moment ---- */
enum { LINK_MAX = 6, LINK_SECONDS = 2 };
static char link_from[LINK_MAX][FS_NAME];
static char link_to[LINK_MAX][FS_NAME];
static int link_n;
static int link_age; /* seconds the current links have been shown */

static int cell_named(const char *name) {
    for (int i = 0; i < cell_count; i++) {
        if (cells[i].kind == K_APP && s_eq(cells[i].arg, name)) {
            return i;
        }
    }
    return -1;
}

static void link_end(const char *name, int *x, int *y) {
    if (s_eq(name, "peer")) {
        peer_pos(x, y);
        return;
    }
    if (s_eq(name, "core")) {
        *x = CORE_X;
        *y = CORE_Y;
        return;
    }
    int c = cell_named(name);
    if (c < 0) {
        c = K_PKG; /* libraries have no cell: the Пакеты cell stands in */
    }
    *x = cell_cx[c];
    *y = cell_cy[c];
}

static void glow_line(int x0, int y0, int x1, int y1) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    if (steps == 0) {
        return;
    }
    for (int k = 0; k <= steps; k++) {
        int x = x0 + dx * k / steps;
        int y = y0 + dy * k / steps;
        fill(x - 2, y - 2, 5, 5, AMBER_DIM);
    }
    for (int k = 0; k <= steps; k++) {
        fill(x0 + dx * k / steps - 1, y0 + dy * k / steps - 1, 2, 2, YELLOW);
    }
}

static void draw_links(void) {
    if (sheet_open()) {
        return;
    }
    for (int i = 0; i < link_n; i++) {
        int x0, y0, x1, y1;
        link_end(link_from[i], &x0, &y0);
        link_end(link_to[i], &x1, &y1);
        if (x0 != x1 || y0 != y1) {
            glow_line(x0, y0, x1, y1);
        }
    }
}

void desktop_link(const char *from, const char *to) {
    for (int i = 0; i < link_n; i++) {
        if (s_eq(link_from[i], from) && s_eq(link_to[i], to)) {
            link_age = 0;
            return;
        }
    }
    console_puts("link: ");
    console_puts(from);
    console_puts(" -> ");
    console_puts(to);
    console_putc('\n');
    if (link_n == LINK_MAX) {
        return;
    }
    s_copy(link_from[link_n], from, FS_NAME);
    s_copy(link_to[link_n], to, FS_NAME);
    link_n++;
    link_age = 0;
}

static void draw_home_ui(void) {
    bg_fill(0, 0, FB_W, FB_H);
    draw_top();
    draw_links();
    draw_peer();
    draw_core();
    for (int i = 0; i < cell_count; i++) {
        draw_cell(i);
    }
    draw_panel();
    draw_bar();
}

static void redraw_all(void) {
    cursor_forget();
    draw_home_ui();
    cursor_paint();
}

static void redraw_panel(void) {
    cursor_erase();
    draw_panel();
    cursor_paint();
}

static void redraw_mark(int i) {
    if (i == CELL_PEER) {
        draw_peer();
    } else if (i == CELL_CORE) {
        draw_core();
    } else if (i >= 0) {
        draw_cell(i);
    }
}

static void set_highlight(int new_hint, int new_focus) {
    int before = highlighted();
    hint = new_hint;
    focus = new_focus;
    int after = highlighted();
    if (before == after) {
        return;
    }
    cursor_erase();
    redraw_mark(before);
    redraw_mark(after);
    cursor_paint();
}

/* ---- viewers (full screen programs) ---- */
static void draw_back(void) {
    fill(BACK_X, BACK_Y, BACK_W, BACK_H, FRAME);
    fill(BACK_X + 4, BACK_Y + 4, BACK_W - 8, BACK_H - 8, PAPER);
    text(BACK_X + 16, BACK_Y + 14, "Назад", INK, 2);
}


static int back_hit(int x, int y) {
    return x >= BACK_X && x < BACK_X + BACK_W && y >= BACK_Y && y < BACK_Y + BACK_H;
}

static void show_home(void);
static void open_panel(int which);

static void say_back(void) {
    console_puts("Назад\n");
}

static int viewer_back; /* 1 = Back (one step), 2 = Esc (close the window) */


static void api_puts(const char *message) { console_puts(message); }
static void api_text(int x, int y, const char *message, unsigned int color, int scale) {
    text(x, y, message, color, scale);
}
static void api_fill(int x, int y, int width, int height, unsigned int color) {
    fill(x, y, width, height, color);
}
static void api_pixel(int x, int y, unsigned int color) { pixel(x, y, color); }
static int api_poll_back(void) { return viewer_back; }
static void api_yield(void) {
    guest_yield();
}
/* Bump arena shared by all viewer allocations; reset on every launch. */
static unsigned char viewer_arena[65536];
static unsigned int viewer_arena_used;

static void *api_malloc(unsigned int size) {
    if (size == 0) {
        size = 1;
    }
    unsigned int start = (viewer_arena_used + 15u) & ~15u;
    if (start > sizeof viewer_arena || size > sizeof viewer_arena - start) {
        return 0;
    }
    viewer_arena_used = start + size;
    return viewer_arena + start;
}

/* No per-block free: the arena resets when the next viewer launches. */
static void api_free(void *ptr) { (void)ptr; }

static const char *viewer_for_name(const char *name) {
    int dot = -1;
    for (int i = 0; name[i] != 0; i++) {
        if (name[i] == '.') {
            dot = i;
        }
    }
    if (dot < 0) {
        return "vtxt";
    }
    const char *ext = name + dot + 1;
    if ((ext[0] == 't' || ext[0] == 'T') && (ext[1] == 'x' || ext[1] == 'X') &&
        (ext[2] == 't' || ext[2] == 'T') && ext[3] == 0) {
        return "vtxt";
    }
    if ((ext[0] == 'd' || ext[0] == 'D') && (ext[1] == 'o' || ext[1] == 'O') &&
        (ext[2] == 'c' || ext[2] == 'C') && (ext[3] == 'x' || ext[3] == 'X') && ext[4] == 0) {
        return "vdocx";
    }
    if ((ext[0] == 'p' || ext[0] == 'P') && (ext[1] == 'n' || ext[1] == 'N') &&
        (ext[2] == 'g' || ext[2] == 'G') && ext[3] == 0) {
        return "vimg";
    }
    if ((ext[0] == 'j' || ext[0] == 'J') && (ext[1] == 'p' || ext[1] == 'P') &&
        (ext[2] == 'g' || ext[2] == 'G') && ext[3] == 0) {
        return "vimg";
    }
    if ((ext[0] == 'j' || ext[0] == 'J') && (ext[1] == 'p' || ext[1] == 'P') &&
        (ext[2] == 'e' || ext[2] == 'E') && (ext[3] == 'g' || ext[3] == 'G') && ext[4] == 0) {
        return "vimg";
    }
    if ((ext[0] == 'p' || ext[0] == 'P') && (ext[1] == 'd' || ext[1] == 'D') &&
        (ext[2] == 'f' || ext[2] == 'F') && ext[3] == 0) {
        return "vpdf";
    }
    if ((ext[0] == 'm' || ext[0] == 'M') && (ext[1] == 'p' || ext[1] == 'P') &&
        (ext[2] == '4') && ext[3] == 0) {
        return "vmp4";
    }
    return "vtxt";
}

static void launch_viewer(const char *viewer, const unsigned char *file, unsigned int len,
                          const char *fname) {
    const unsigned char *prog = 0;
    unsigned int prog_len = 0;
    char vpath[FS_NAME] = "/bin/";
    for (unsigned int k = 0; viewer[k] != 0 && k + 6 < FS_NAME; k++) {
        vpath[5 + k] = viewer[k];
        vpath[6 + k] = 0;
    }
    if (!fs_lookup(vpath, &prog, &prog_len) || prog_len == 0) {
        console_puts("viewer: missing\n");
        return;
    }
    unsigned char *dest = (unsigned char *)PROG_LOAD_ADDR;
    for (unsigned int i = 0; i < prog_len; i++) {
        dest[i] = prog[i];
    }
    for (unsigned int i = 0; i < GENESIS_PROG_HEAP; i++) {
        dest[prog_len + i] = 0;
    }
    nav_push();
    view = VIEW_VIEWER;
    viewer_back = 0;
    viewer_arena_used = 0;
    mouse_arm();
    cursor_forget();
    fill(0, 0, FB_W, FB_H, BG);
    draw_back();
    viewer_clock();
    struct genesis_api api;
    api.puts = api_puts;
    api.text = api_text;
    api.fill = api_fill;
    api.pixel = api_pixel;
    api.fb = fb;
    api.file = file;
    api.file_len = len;
    api.file_name = fname;
    api.poll_back = api_poll_back;
    api.yield = api_yield;
    api.malloc = api_malloc;
    api.free = api_free;
    api.width = FB_W;
    api.height = FB_H;
    viewer_entry entry = (viewer_entry)(void *)PROG_LOAD_ADDR;
    {
        unsigned long stack_top = 0x900000ul + 512ul * 1024ul;
#ifdef __x86_64__
        __asm__ volatile(
            "mov %%rsp, %%r12\n\t"
            "mov %[sp], %%rsp\n\t"
            "mov %[api], %%rdi\n\t"
            "call *%[fn]\n\t"
            "mov %%r12, %%rsp\n\t"
            :
            : [sp] "r"(stack_top), [api] "r"(&api), [fn] "r"(entry)
            : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12",
              "memory");
#else
        /* cdecl: the api pointer goes on the viewer's own stack; esi survives the call. */
        __asm__ volatile(
            "mov %%esp, %%esi\n\t"
            "mov %[sp], %%esp\n\t"
            "sub $12, %%esp\n\t"
            "push %[api]\n\t"
            "call *%[fn]\n\t"
            "mov %%esi, %%esp\n\t"
            :
            : [sp] "a"(stack_top), [api] "c"(&api), [fn] "d"(entry)
            : "esi", "memory");
#endif
    }
    if (viewer_back == 2) {
        show_home();
        return;
    }
    nav_back();
}


static void set_mode(int dock, int wide_sheet) {
    if (docked != dock || wide != wide_sheet) {
        docked = dock;
        wide = wide_sheet;
        layout_init();
    }
}

static void set_docked(int on) {
    set_mode(on, on ? wide : 0);
}

static void show_home(void) {
    nav_n = 0;
    view = VIEW_HOME;
    panel = P_NONE;
    send_mode = 0;
    set_docked(0);
    pfocus = -1;
    mouse_arm();
    rebuild_cells();
    redraw_all();
    link_hello(); /* a Genesis Link peer that answers appears as a small core */
    console_puts("home: ready\n");
}

static void open_panel(int which) {
    view = VIEW_HOME;
    panel = which;
    set_mode(1, which == P_FEED);
    pfocus = 1; /* arrows work at once: focus starts on the first row */
    mouse_arm();
    redraw_all();
    say_back();
}

static void open_files(const char *dir) {
    char line[512];
    s_copy(desk_dir, dir, sizeof desk_dir);
    collect_files();
    open_panel(P_FILES);
    rows_summary(line, sizeof line);
    console_puts("ls: ");
    console_puts(line);
    console_putc('\n');
}

static void open_tasks(void) {
    open_panel(P_TASKS);
    char s[160];
    unsigned int n = s_cat(s, 0, "tasks:", sizeof s);
    for (int id = 0; id < task_max(); id++) {
        char name[FS_NAME];
        unsigned int ticks = 0;
        if (task_info(id, name, sizeof name, &ticks)) {
            n = s_cat(s, n, " ", sizeof s);
            n = u_cat(s, n, (unsigned int)id, sizeof s);
            n = s_cat(s, n, ":", sizeof s);
            n = s_cat(s, n, name, sizeof s);
        }
    }
    s_cat(s, n, "\n", sizeof s);
    console_puts(s);
}

static void open_editor(const char *file) {
    cursor_erase();
    editor_open(file);
}

/* Runs one bar line: output goes to the terminal panel and the bar. */
static void bar_exec(const char *cmd, int show_term) {
    static char out[2048];
    char head[FS_NAME + BAR_CAP + 16];
    unsigned int n = s_cat(head, 0, "genesis:", sizeof head);
    n = s_cat(head, n, shell_cwd(), sizeof head);
    n = s_cat(head, n, "> ", sizeof head);
    n = s_cat(head, n, cmd, sizeof head);
    s_cat(head, n, "\n", sizeof head);
    term_add(head);
    if (bar_hist_n == BAR_HIST) {
        for (int i = 1; i < BAR_HIST; i++) {
            s_copy(bar_hist[i - 1], bar_hist[i], BAR_CAP);
        }
        bar_hist_n--;
    }
    s_copy(bar_hist[bar_hist_n++], cmd, BAR_CAP);
    bar_hist_pos = bar_hist_n;
    out[0] = 0;
    if (s_eq(cmd, "desktop")) {
        bar_out[0] = 0;
        show_home();
        return;
    }
    if (s_eq(cmd, "feed") || s_starts(cmd, "feed ")) {
        /* `feed [all|files|editor|term|pkg|prog]` opens Лента on the desktop. */
        static const char *const names[F_COUNT] = {"all", "files", "editor", "term", "pkg", "prog"};
        int f = F_ALL;
        for (int i = 0; i < F_COUNT; i++) {
            if (s_eq(cmd + (cmd[4] == ' ' ? 5 : 4), names[i])) {
                f = i;
            }
        }
        bar_out[0] = 0;
        open_feed(f);
        return;
    }
    if (s_eq(cmd, "clear")) {
        term_len = 0;
        term[0] = 0;
        bar_out[0] = 0;
    } else {
        cursor_erase();
        desktop_run(cmd, out, sizeof out);
        term_add(out);
        unsigned int k = 0;
        unsigned int chars = 0;
        while (out[k] != 0 && out[k] != '\n' && k + 4 < sizeof bar_out && chars < 54) {
            bar_out[k] = out[k];
            if (!utf8_is_cont((unsigned char)out[k])) {
                chars++;
            }
            k++;
        }
        while (k > 0 && utf8_is_cont((unsigned char)out[k]) ) {
            k--; /* never cut a UTF-8 character in half */
        }
        bar_out[k] = 0;
    }
    if (!desktop_on || editor_active()) {
        return;
    }
    rebuild_cells();
    core_sample();
    if (show_term && out[0] != 0) {
        if (panel != P_TERM) {
            pfocus = -1; /* command output: the arrows stay with the cells */
        }
        panel = P_TERM;
        set_docked(1);
    }
    if (panel == P_FILES) {
        collect_files();
    }
    redraw_all();
}

static void open_cell(int i) {
    nav_n = 0; /* another window: a fresh history */
    if (i == CELL_PEER) {
        /* «Отправить файл»: pick a file in /home, it goes to the peer. */
        send_mode = 1;
        open_files("/home");
        console_puts("send: pick a file\n");
        return;
    }
    if (i == CELL_CORE) {
        open_tasks();
        return;
    }
    if (i < 0 || i >= cell_count) {
        return;
    }
    struct cell *c = &cells[i];
    switch (c->kind) {
    case K_FILES: open_files("/home"); break;
    case K_EDITOR: open_editor("/home/readme"); break;
    case K_TERM: open_panel(P_TERM); break;
    case K_PKG: open_panel(P_PKG); break;
    case K_MEM: {
        char line[48];
        open_panel(P_MEM);
        memory_summary(line, sizeof line);
        console_puts(line);
        console_putc('\n');
        break;
    }
    case K_FEED: open_feed(0); break;
    case K_SET: open_panel(P_SET); break;
    case K_APP: {
        char cmd[FS_NAME + 8];
        s_cat(cmd, s_cat(cmd, 0, "run ", sizeof cmd), c->arg, sizeof cmd);
        bar_exec(cmd, 1);
        mouse_arm();
        break;
    }
    case K_PIN: open_files(c->arg); break;
    default: break;
    }
}

static void show_file(int row) {
    char path[FS_NAME];
    const unsigned char *bytes = 0;
    unsigned int length = 0;
    if (row < 0 || row >= file_rows) {
        return;
    }
    s_copy(path, desk_dir, sizeof path);
    unsigned int n = 0;
    while (path[n] != 0) {
        n++;
    }
    if (file_slot[row] < 0) {
        while (n > 1 && path[n - 1] != '/') {
            n--;
        }
        if (n > 1) {
            n--;
        }
        path[n] = 0;
    } else {
        if (n > 1) {
            n = s_cat(path, n, "/", sizeof path);
        }
        s_cat(path, n, row_name[row], sizeof path);
    }
    if (row_dir[row]) {
        char line[512];
        nav_push();
        s_copy(desk_dir, path, sizeof desk_dir);
        file_top = 0;
        collect_files();
        mouse_arm();
        rows_summary(line, sizeof line);
        redraw_panel();
        console_puts("dir: ");
        console_puts(desk_dir);
        console_puts(" ls: ");
        console_puts(line);
        console_putc('\n');
        return;
    }
    if (!fs_lookup(path, &bytes, &length)) {
        return;
    }
    if (send_mode) {
        char cmd[FS_NAME + 8];
        s_cat(cmd, s_cat(cmd, 0, "send ", sizeof cmd), path, sizeof cmd);
        send_mode = 0;
        bar_exec(cmd, 1);
        return;
    }
    journal_add('o', path);
    const char *viewer = viewer_for_name(row_name[row]);
    launch_viewer(viewer, bytes, length, row_name[row]);
}

static int s_starts(const char *s, const char *prefix) {
    while (*prefix != 0) {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

static void open_feed(int filter) {
    feed_filter = filter;
    feed_confirm = -1;
    feed_first = 1 << 20; /* start at «Сейчас» */
    feed_collect();
    open_panel(P_FEED);
    feed_collect(); /* again: the sheet is wider than a docked panel */
    redraw_panel();
    /* Focus the newest card (or the first chip when the feed is empty). */
    pfocus = 1;
    for (int k = 0; k < it_n; k++) {
        if (it_act[k] == A_CARD) {
            pfocus = k;
        }
    }
    redraw_panel();
    feed_say();
}

/* The Лента filter that fits the panel (or cell) you came from. */
static int feed_filter_for_panel(int which) {
    switch (which) {
    case P_FILES: return F_FILES;
    case P_TERM: return F_TERM;
    case P_PKG: return F_PKG;
    case P_TASKS: return F_PROG;
    default: return F_ALL;
    }
}

static int destructive(const char *cmd) {
    return s_starts(cmd, "rm ") || s_starts(cmd, "rmdir ") || s_starts(cmd, "mv ") ||
           s_starts(cmd, "pkg remove ") || s_starts(cmd, "kill ") || s_starts(cmd, "write ") ||
           s_starts(cmd, "poweroff") || s_starts(cmd, "reboot") || s_starts(cmd, "unpin ");
}

static void bar_exec(const char *cmd, int show_term);

/* Enter on a card: open the file again or run the command again. */
static void feed_replay(int pos) {
    if (pos < 0 || pos >= feed_n) {
        return;
    }
    int id = feed_ids[pos];
    char time[9];
    char type;
    char text[80];
    if (!journal_get(id, time, &type, text, sizeof text)) {
        return;
    }
    if (type == 'x') {
        console_puts("feed: crash ");
        console_puts(text);
        console_putc('\n');
        return; /* a crash report, nothing to replay */
    }
    if (destructive(text) && feed_confirm != id) {
        feed_confirm = id;
        console_puts("feed: confirm ");
        console_puts(text);
        console_putc('\n');
        redraw_panel();
        return;
    }
    feed_confirm = -1;
    console_puts("feed: replay ");
    console_puts(text);
    console_putc('\n');
    char cmd[96];
    if ((type == 'o' || type == 's') && text[0] == '/') {
        const unsigned char *bytes = 0;
        unsigned int length = 0;
        if (type == 'o' && fs_lookup(text, &bytes, &length)) {
            const char *base = text;
            for (const char *c = text; *c != 0; c++) {
                if (*c == '/') {
                    base = c + 1;
                }
            }
            journal_add('o', text);
            launch_viewer(viewer_for_name(base), bytes, length, base);
            return;
        }
        unsigned int n = s_cat(cmd, 0, "edit ", sizeof cmd);
        s_cat(cmd, n, text, sizeof cmd);
    } else {
        s_copy(cmd, text, sizeof cmd);
    }
    bar_exec(cmd, 1);
}

/* Serial trace of the focused card, for tests and debugging. */
static void feed_at(void) {
    if (panel != P_FEED || pfocus < 0 || pfocus >= it_n || it_act[pfocus] != A_CARD) {
        return;
    }
    char time[9];
    char type;
    char text[80];
    if (journal_get(feed_ids[it_arg[pfocus]], time, &type, text, sizeof text)) {
        console_puts("feed: at ");
        console_puts(text);
        console_putc('\n');
    }
}

static void feed_scroll(int step) {
    int vis = feed_visible();
    int before = feed_first;
    feed_first += step;
    if (feed_first > feed_n - vis) {
        feed_first = feed_n - vis;
    }
    if (feed_first < 0) {
        feed_first = 0;
    }
    if (feed_first != before) {
        feed_confirm = -1;
        redraw_panel();
    }
}

static void nav_push(void) {
    if (panel == P_NONE) {
        return;
    }
    if (nav_n == NAV_MAX) {
        for (int i = 1; i < NAV_MAX; i++) {
            nav_stack[i - 1] = nav_stack[i];
        }
        nav_n--;
    }
    struct nav_step *e = &nav_stack[nav_n++];
    e->panel = panel;
    s_copy(e->dir, desk_dir, sizeof e->dir);
    e->focus = pfocus;
    e->top = file_top;
    e->filter = feed_filter;
}

static void nav_back(void) {
    if (nav_n == 0) {
        show_home();
        return;
    }
    struct nav_step e = nav_stack[--nav_n];
    view = VIEW_HOME;
    if (e.panel == P_FILES) {
        char line[512];
        s_copy(desk_dir, e.dir, sizeof desk_dir);
        collect_files();
        file_top = e.top < file_rows ? e.top : 0;
        if (panel != P_FILES) {
            open_panel(P_FILES);
        }
        pfocus = e.focus;
        redraw_all();
        rows_summary(line, sizeof line);
        console_puts("back: ");
        console_puts(desk_dir);
        console_puts(" ls: ");
        console_puts(line);
        console_putc('\n');
    } else if (e.panel == P_FEED) {
        open_feed(e.filter);
        console_puts("back: feed\n");
    } else {
        open_panel(e.panel);
        console_puts("back: panel\n");
    }
}

static void panel_act(int k) {
    int act = it_act[k];
    int arg = it_arg[k];
    if (act == A_CLOSE) {
        if (portrait) {
            nav_back(); /* the phone bar's «< Назад» */
        } else {
            show_home();
        }
        return;
    }
    if (act == A_BACK) {
        nav_back();
        return;
    }
    if (act == A_ROW) {
        show_file(arg);
        return;
    }
    if (act == A_CHIP && arg != feed_filter) {
        nav_push();
    }
    if (act == A_CHIP) {
        feed_filter = arg;
        feed_confirm = -1;
        feed_first = 1 << 20;
        feed_collect();
        mouse_arm();
        redraw_panel();
        feed_say();
        return;
    }
    if (act == A_CARD) {
        feed_replay(arg);
        mouse_arm();
        return;
    }
    if (act == A_FEED_PREV || act == A_FEED_NEXT) {
        feed_scroll(act == A_FEED_PREV ? -1 : 1);
        mouse_arm();
        return;
    }
    if (act == A_TO_FEED) {
        nav_push();
        open_feed(feed_filter_for_panel(panel));
        mouse_arm();
        return;
    }
    if (act == A_UP || act == A_DOWN) {
        if (act == A_UP) {
            file_top = file_top >= FROW_VIS ? file_top - FROW_VIS : 0;
        } else if (file_top + FROW_VIS < file_rows) {
            file_top += FROW_VIS;
        }
        mouse_arm();
        redraw_panel();
        return;
    }
    if (act == A_PKG) {
        char cmd[FS_NAME + 16];
        unsigned int n = s_cat(cmd, 0, pkg_rows_installed[arg] ? "pkg remove " : "pkg install ", sizeof cmd);
        s_cat(cmd, n, pkg_rows[arg], sizeof cmd);
        bar_exec(cmd, 0);
        mouse_arm();
        return;
    }
    if (act == A_LAYOUT) {
        kbd_toggle_layout();
        mouse_arm();
        redraw_panel();
        return;
    }
    if (act == A_REBOOT) {
        disk_request_home();
        machine_reboot();
    }
    if (act == A_POWEROFF) {
        machine_poweroff();
    }
    if (act == A_KILL) {
        char cmd[16];
        u_cat(cmd, s_cat(cmd, 0, "kill ", sizeof cmd), (unsigned int)arg, sizeof cmd);
        bar_exec(cmd, 0);
        mouse_arm();
    }
}

static void panel_click(int x, int y) {
    for (int k = 0; k < it_n; k++) {
        if (in_rect(x, y, it_x[k], it_y[k], it_w[k], it_h[k])) {
            pfocus = -1;
            panel_act(k);
            return;
        }
    }
    cursor_move();
}

/* Spatial arrow navigation (TV remote): the nearest target in direction
 * (dx, dy) from (fx, fy). Returns its index or -1. */
static int nearest(int n, const int *xs, const int *ys, int from, int dx, int dy) {
    int fx = xs[from];
    int fy = ys[from];
    int best = -1;
    long best_score = 0;
    for (int i = 0; i < n; i++) {
        if (i == from) {
            continue;
        }
        int ex = xs[i] - fx;
        int ey = ys[i] - fy;
        int along = ex * dx + ey * dy;
        int across = dx != 0 ? ey : ex;
        if (across < 0) {
            across = -across;
        }
        if (along <= 0) {
            continue;
        }
        long score = (long)along + 2L * across;
        if (best < 0 || score < best_score) {
            best = i;
            best_score = score;
        }
    }
    return best;
}

static void home_nav(int dx, int dy);

static void panel_nav(int dx, int dy) {
    int xs[ITEM_MAX];
    int ys[ITEM_MAX];
    for (int k = 0; k < it_n; k++) {
        xs[k] = it_x[k] + it_w[k] / 2;
        ys[k] = it_y[k] + it_h[k] / 2;
    }
    int next;
    if (pfocus < 0) {
        next = it_n > 1 ? 1 : 0;
    } else {
        next = nearest(it_n, xs, ys, pfocus, dx, dy);
        int along = portrait ? dy : dx;
        if (panel == P_FEED && it_act[pfocus] == A_CARD && along != 0) {
            /* Along the timeline: the neighbouring card, sliding at the ends. */
            int want = it_arg[pfocus] + along;
            next = -1;
            for (int k = 0; k < it_n; k++) {
                if (it_act[k] == A_CARD && it_arg[k] == want) {
                    next = k;
                }
            }
            if (next < 0) {
                if (want >= 0 && want < feed_n) {
                    feed_scroll(along);
                    console_puts("feed: scroll\n");
                    for (int k = 0; k < it_n; k++) {
                        if (it_act[k] == A_CARD && it_arg[k] == want) {
                            pfocus = k;
                        }
                    }
                    redraw_panel();
                    feed_at();
                }
                return;
            }
        }
        if (next < 0 && dy != 0 && panel == P_FILES) {
            /* Past the last visible row: scroll the list. */
            if (dy > 0 && file_top + FROW_VIS < file_rows) {
                file_top += FROW_VIS;
                pfocus = 1;
                redraw_panel();
            } else if (dy < 0 && file_top > 0) {
                file_top = file_top >= FROW_VIS ? file_top - FROW_VIS : 0;
                pfocus = 1;
                redraw_panel();
            }
            return;
        }
        if (next < 0 && !sheet_open() && (dx < 0 || (dy != 0 && it_n <= 2))) {
            /* Left past the panel's edge: back out to the cells. */
            pfocus = -1;
            redraw_panel();
            home_nav(dx, dy);
            return;
        }
        if (next < 0) {
            return;
        }
    }
    pfocus = next;
    if (focus >= 0) {
        set_highlight(hint, -1);
    }
    redraw_panel();
    feed_at();
    console_puts("focus: item ");
    char s[12];
    u_cat(s, 0, (unsigned int)pfocus, sizeof s);
    console_puts(s);
    console_putc('\n');
}

/* Which cell the typed command is about, for the hint highlight. */
static int hint_for(const char *cmd) {
    char w0[24];
    char w1[FS_NAME];
    unsigned int i = 0;
    unsigned int n = 0;
    while (cmd[i] == ' ') {
        i++;
    }
    while (cmd[i] != 0 && cmd[i] != ' ' && n + 1 < sizeof w0) {
        w0[n++] = cmd[i++];
    }
    w0[n] = 0;
    while (cmd[i] == ' ') {
        i++;
    }
    n = 0;
    while (cmd[i] != 0 && cmd[i] != ' ' && n + 1 < sizeof w1) {
        w1[n++] = cmd[i++];
    }
    w1[n] = 0;
    if (w0[0] == 0) {
        return -1;
    }
    for (int k = 0; k < cell_count; k++) {
        if (cells[k].kind == K_APP &&
            (s_eq(w0, cells[k].arg) || ((s_eq(w0, "run") || s_eq(w0, "spawn")) && s_eq(w1, cells[k].arg)))) {
            return k;
        }
        if (cells[k].kind == K_PIN && (s_eq(w0, "cd") || s_eq(w0, "ls") || s_eq(w0, "unpin")) && w1[0] != 0 &&
            (s_eq(w1, cells[k].arg) || s_eq(w1, cells[k].label))) {
            return k;
        }
    }
    /* While the first word is still being typed, a unique command prefix counts. */
    if (cmd[i] == 0 && w1[0] == 0) {
        static const char *const words[] = {"edit", "pkg", "mem", "heap", "alloc", "reboot", "poweroff", "kill", "spawn", "mkdir", "rmdir", "unpin"};
        static const char *const full[] = {"edit", "pkg", "mem", "heap", "alloc", "reboot", "poweroff", "kill", "spawn", "mkdir", "rmdir", "unpin"};
        unsigned int wl = 0;
        while (w0[wl] != 0) {
            wl++;
        }
        for (unsigned int k = 0; wl >= 2 && k < sizeof words / sizeof words[0]; k++) {
            unsigned int j = 0;
            while (j < wl && words[k][j] == w0[j]) {
                j++;
            }
            if (j == wl && words[k][j] != 0) {
                s_copy(w0, full[k], sizeof w0);
                break;
            }
        }
    }
    static const char *const files_words[] = {"ls", "cd", "cat", "mkdir", "rmdir", "mv", "cp", "rm", "pwd", "write", "pin", "unpin"};
    for (unsigned int k = 0; k < sizeof files_words / sizeof files_words[0]; k++) {
        if (s_eq(w0, files_words[k])) {
            return K_FILES;
        }
    }
    if (s_eq(w0, "edit")) return K_EDITOR;
    if (s_eq(w0, "pkg")) return K_PKG;
    if (s_eq(w0, "mem") || s_eq(w0, "heap") || s_eq(w0, "alloc")) return K_MEM;
    if (s_eq(w0, "reboot") || s_eq(w0, "poweroff")) return K_SET;
    if (s_eq(w0, "ps") || s_eq(w0, "kill") || s_eq(w0, "spawn")) return CELL_CORE;
    return K_TERM;
}

static void hint_update(void) {
    int h = hint_for(bar_cmd);
    if (h == hint) {
        return;
    }
    set_highlight(h, focus);
    if (h >= 0) {
        console_puts("hint: ");
        console_puts(h == CELL_CORE ? "Ядро" : cells[h].label);
        console_putc('\n');
    }
}

static void bar_set(const char *s) {
    s_copy(bar_cmd, s, sizeof bar_cmd);
    bar_len = 0;
    while (bar_cmd[bar_len] != 0) {
        bar_len++;
    }
}



static void focus_say(int f) {
    console_puts("focus: ");
    console_puts(f == CELL_CORE ? "Ядро" : f == CELL_PEER ? "Сосед" : cells[f].label);
    console_putc('\n');
}

/* Arrow keys on the home screen: move to the nearest cell (or the core). */
static void home_nav(int dx, int dy) {
    int xs[CELL_MAX + 2];
    int ys[CELL_MAX + 2];
    for (int i = 0; i < cell_count; i++) {
        xs[i] = cell_cx[i];
        ys[i] = cell_cy[i];
    }
    xs[cell_count] = CORE_X;
    ys[cell_count] = CORE_Y;
    int targets = cell_count + 1;
    if (peer_text[0] != 0 && !sheet_open()) {
        peer_pos(&xs[cell_count + 1], &ys[cell_count + 1]);
        targets++;
    }
    int at = focus == CELL_CORE ? cell_count : focus == CELL_PEER ? cell_count + 1 : focus;
    int next;
    if (at < 0) {
        next = cell_count; /* start on the core */
    } else {
        next = nearest(targets, xs, ys, at, dx, dy);
        if (next < 0 && dx > 0 && panel != P_NONE && it_n > 0) {
            /* Right past the last cell: into the docked panel. */
            set_highlight(hint, -1);
            panel_nav(0, 0);
            return;
        }
        if (next < 0) {
            return;
        }
    }
    int f = next == cell_count ? CELL_CORE : next == cell_count + 1 ? CELL_PEER : next;
    set_highlight(hint, f);
    focus_say(f);
}

static void focus_move(int step) {
    int ring = cell_count + 1; /* cells, then the core */
    int at = focus == CELL_CORE ? cell_count : focus;
    if (at < 0) {
        at = step > 0 ? -1 : ring;
    }
    at = (at + step + ring) % ring;
    int f = at == cell_count ? CELL_CORE : at;
    set_highlight(hint, f);
    focus_say(f);
}

static void bar_key(char ch) {
    unsigned char u = (unsigned char)ch;
    if (ch == '\n') {
        if (bar_len == 0) {
            if (panel != P_NONE && pfocus >= 0) {
                pfocus_pending = 1;
                panel_act(pfocus);
            } else if (panel == P_NONE && focus >= 0) {
                pfocus_pending = 1;
                open_cell(focus);
            }
            pfocus_pending = 0;
            return;
        }
        char cmd[BAR_CAP];
        s_copy(cmd, bar_cmd, sizeof cmd);
        bar_set("");
        hint = -1;
        bar_exec(cmd, 1);
        return;
    }
    if (ch == '\t') {
        if (bar_len == 0) {
            if (panel != P_NONE && it_n > 0) {
                pfocus = (pfocus + 1) % it_n;
                redraw_panel();
            } else {
                focus_move(1);
            }
            return;
        }
        char cands[256];
        int found = sh_complete(bar_cmd, &bar_len, BAR_CAP - 4, cands, sizeof cands);
        if (found > 1 && cands[0] != 0) {
            s_copy(bar_out, cands, 60);
        }
    } else if (bar_len == 0 && panel != P_NONE && it_n > 0 && pfocus >= 0 &&
               (ch == DESK_LEFT || ch == DESK_RIGHT || ch == DESK_UP || ch == DESK_DOWN)) {
        /* A docked panel owns the arrows while one of its items has focus. */
        panel_nav(ch == DESK_LEFT ? -1 : ch == DESK_RIGHT ? 1 : 0, ch == DESK_UP ? -1 : ch == DESK_DOWN ? 1 : 0);
        return;
    } else if (ch == DESK_LEFT || ch == DESK_RIGHT || (bar_len == 0 && (ch == DESK_UP || ch == DESK_DOWN))) {
        /* D-pad / arrows: spatial focus; the first arrow lands on the core.
         * Up/Down recall history while a line is being typed (or PgUp/PgDn). */
        home_nav(ch == DESK_LEFT ? -1 : ch == DESK_RIGHT ? 1 : 0, ch == DESK_UP ? -1 : ch == DESK_DOWN ? 1 : 0);
        return;
    } else if (ch == DESK_UP || ch == DESK_DOWN || ch == DESK_PGUP || ch == DESK_PGDN) {
        if (ch == DESK_PGUP) {
            ch = DESK_UP;
        } else if (ch == DESK_PGDN) {
            ch = DESK_DOWN;
        }
        if (ch == DESK_UP && bar_hist_pos > 0) {
            bar_hist_pos--;
            bar_set(bar_hist[bar_hist_pos]);
        } else if (ch == DESK_DOWN && bar_hist_pos < bar_hist_n) {
            bar_hist_pos++;
            bar_set(bar_hist_pos < bar_hist_n ? bar_hist[bar_hist_pos] : "");
        } else {
            return;
        }
    } else if (ch == DESK_ESC) {
        if (panel == P_FEED && feed_confirm >= 0) {
            feed_confirm = -1;
            console_puts("feed: cancel\n");
            redraw_panel();
        } else if (panel != P_NONE) {
            show_home();
        } else {
            set_highlight(-1, -1);
        }
        return;
    } else if (ch == '\b') {
        if (bar_len == 0) {
            if (panel != P_NONE) {
                nav_back(); /* Back (TV remote, phone pad): one step */
            }
            return;
        }
        bar_len--;
        while (bar_len > 0 && utf8_is_cont((unsigned char)bar_cmd[bar_len])) {
            bar_len--;
        }
        bar_cmd[bar_len] = 0;
    } else {
        if (u < 32 || u == 127 || bar_len + 1 >= (int)sizeof bar_cmd) {
            return;
        }
        if (u >= 0xc0 && bar_len + 5 >= (int)sizeof bar_cmd) {
            return;
        }
        bar_cmd[bar_len++] = ch;
        bar_cmd[bar_len] = 0;
        if (u >= 0xc0) {
            return; /* draw once the whole UTF-8 character is in */
        }
    }
    blink_on = 1;
    cursor_erase();
    draw_bar();
    cursor_paint();
    hint_update();
}

/* ---- full-screen editor inside the desktop ---- */
/* 80x25 cells fitted to the screen (12x30 px at 1024x768). */
#define ED_CW (FB_W / 80 < 12 || FB_W < 1024 ? FB_W / 80 : 12 * U / 256)
#define ED_CH (FB_H / 25 < 30 * U / 256 ? FB_H / 25 : 30 * U / 256)
#define ED_X0 ((FB_W - 80 * ED_CW) / 2)
#define ED_Y0 ((FB_H - 25 * ED_CH) / 2)
static const unsigned int vga_fg[16] = {
    0x000000u, 0x2a4a9au, 0x55d07au, 0x3fe0ffu, 0xd04a4au, 0xc060d0u, 0xc08a30u, 0xc8d4e4u,
    0x5a6a88u, 0x6a8affu, 0x7cff9au, 0x7ff0ffu, 0xff7070u, 0xff80ffu, 0xffd860u, 0xffffffu,
};
static const unsigned int vga_bg[8] = {
    0x0a1226u, 0x14204au, 0x103a20u, 0x0f4a60u, 0x4a1418u, 0x40184au, 0x4a3418u, 0x1f6f98u,
};
static unsigned short ed_painted[80 * 25];
static int ed_valid;
static int ed_cur;

static const unsigned char *cell_rows(unsigned char b) {
    if (b >= 32 && b < 128) {
        return FONT8X8[b - 32];
    }
    if (b >= 128) {
        for (int i = 0; i < CYR8X8_COUNT; i++) {
            if (CYR8X8[i].slot == b) {
                return CYR8X8[i].rows;
            }
        }
    }
    return FONT8X8[0];
}

static void ed_cell(int idx, unsigned short v, int under) {
    int x = ED_X0 + (idx % 80) * ED_CW;
    int y = ED_Y0 + (idx / 80) * ED_CH;
    unsigned int fg = vga_fg[(v >> 8) & 15u];
    unsigned int bgc = vga_bg[(v >> 12) & 7u];
    fill(x, y, ED_CW, ED_CH, bgc);
    const unsigned char *rows = cell_rows((unsigned char)v);
    int cw = ED_CW;
    int ys = (ED_CH - 6) / 8;
    ys = ys < 1 ? 1 : ys;
    int oy = (ED_CH - 8 * ys) / 2;
    for (int r = 0; r < 8; r++) {
        unsigned char bits = rows[r];
        if (bits == 0) {
            continue;
        }
        for (int c = 0; c < cw; c++) {
            if (bits & (0x80u >> (c * 8 / cw))) {
                for (int k = 0; k < ys; k++) {
                    pixel(x + c, y + oy + r * ys + k, fg);
                }
            }
        }
    }
    if (under) {
        fill(x, y + ED_CH - 4, cw, 3, YELLOW);
    }
}

void desktop_editor_paint(void) {
    if (!desktop_on || fb == 0) {
        return;
    }
    const unsigned short *scr = editor_screen();
    int row = 0;
    int col = 0;
    editor_cursor(&row, &col);
    int cur = row * 80 + col;
    if (!ed_valid) {
        cursor_forget();
        fill(0, 0, FB_W, FB_H, vga_bg[0]);
    }
    for (int i = 0; i < 80 * 25; i++) {
        if (!ed_valid || scr[i] != ed_painted[i] || i == cur || i == ed_cur) {
            ed_cell(i, scr[i], i == cur);
            ed_painted[i] = scr[i];
        }
    }
    ed_cur = cur;
    ed_valid = 1;
}

void desktop_editor_closed(void) {
    ed_valid = 0;
    show_home();
}

void desktop_layout_changed(void) {
    if (!desktop_on || view != VIEW_HOME || editor_active()) {
        return;
    }
    cursor_erase();
    draw_badge();
    if (panel == P_SET) {
        draw_panel();
    }
    cursor_paint();
}

void desktop_mouse(int x, int y, int left) {
    if (!desktop_on || editor_active()) {
        return;
    }
    cursor_x = x;
    cursor_y = y;
    int pressed = left && !pointer_down;
    pointer_down = left;
    if (!pressed) {
        cursor_move();
        return;
    }
    if (view == VIEW_VIEWER) {
        if (back_hit(x, y)) {
            viewer_back = 1;
        }
        cursor_move();
        return;
    }
    if (panel != P_NONE && in_rect(x, y, PANEL_X, PANEL_Y, PANEL_W, PANEL_H)) {
        if (in_rect(x, y, CLOSE_X, CLOSE_Y, CLOSE_W, CLOSE_H)) {
            if (portrait) {
                nav_back();
            } else {
                show_home();
            }
            return;
        }
        panel_click(x, y);
        return;
    }
    int hit = cell_at(x, y);
    if (hit >= 0) {
        open_cell(hit);
        return;
    }
    cursor_move();
}

void desktop_key(char ch) {
    if (view == VIEW_VIEWER && (ch == DESK_ESC || ch == '\b')) {
        viewer_back = ch == DESK_ESC ? 2 : 1; /* Esc closes, Back steps back */
        return;
    }
    if (view == VIEW_HOME) {
        bar_key(ch);
    }
}

/* Once a second: clock, core numbers, live panels. */
static void desktop_second(void) {
    char crash[128];
    if (task_crash_take(crash, sizeof crash)) {
        journal_add('x', crash); /* Лента: a red "сбой" card */
        core_alarm = 3;
        if (view == VIEW_HOME && !editor_active()) {
            redraw_all();
        }
    }
    if (view != VIEW_HOME || editor_active()) {
        return;
    }
    int changed = core_sample();
    if (core_alarm > 0 && --core_alarm == 0) {
        changed = 1; /* the red flash ends */
    }
    char peer[24];
    if (link_peer(peer) && !s_eq(peer, peer_text)) {
        s_copy(peer_text, peer, sizeof peer_text);
        redraw_all();
        return;
    }
    if (link_n > 0 && ++link_age > LINK_SECONDS) {
        link_n = 0; /* the glow fades: repaint without the lines */
        redraw_all();
        return;
    }
    cursor_erase();
    if (changed) {
        draw_core();
    }
    if (panel == P_TASKS || (changed && panel == P_MEM)) {
        draw_panel();
    }
    cursor_paint();
}

static void desktop_blink(void) {
    if (view != VIEW_HOME || editor_active()) {
        return;
    }
    cursor_erase();
    draw_bar_cursor();
    if (core_tasks > 0 || core_phase) {
        core_phase = core_tasks > 0 ? !core_phase : 0;
        draw_core();
    }
    cursor_paint();
}

static inline void outb(unsigned short port, unsigned char value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

struct idt_entry {
    unsigned short offset_low;
    unsigned short selector;
    unsigned char ist;
    unsigned char type_attr;
    unsigned short offset_mid;
#ifdef __x86_64__
    unsigned int offset_high;
    unsigned int zero;
#endif
} __attribute__((packed));

#ifdef __x86_64__
#define IDT_HIGH(e, addr) ((e).offset_high = (unsigned int)((addr) >> 32), (e).zero = 0)
#else
#define IDT_HIGH(e, addr) ((void)(addr)) /* i686: 8-byte gates, no IST */
#endif

struct idt_ptr {
    unsigned short limit;
    unsigned long base;
} __attribute__((packed));

static struct idt_entry idt[256];
static unsigned int clock_ticks;
static unsigned int clock_seconds;
static volatile int clock_due;

extern void isr_irq0(void);

void idt_gate(int number, void *handler, unsigned char attr, unsigned char ist) {
    unsigned long addr = (unsigned long)handler;
    idt[number].offset_low = (unsigned short)addr;
    idt[number].selector = 0x08;
#ifdef __x86_64__
    idt[number].ist = ist;
#else
    (void)ist;
    idt[number].ist = 0;
#endif
    idt[number].type_attr = attr;
    idt[number].offset_mid = (unsigned short)(addr >> 16);
    IDT_HIGH(idt[number], addr);
}

void user_init(void);

static void set_gate(int number, void *handler) {
    unsigned long addr = (unsigned long)handler;
    idt[number].offset_low = (unsigned short)addr;
    idt[number].selector = 0x08;
    idt[number].ist = 0;
    idt[number].type_attr = 0x8e;
    idt[number].offset_mid = (unsigned short)(addr >> 16);
    IDT_HIGH(idt[number], addr);
}

static void print_u2(unsigned int value, char *dest) {
    dest[0] = (char)('0' + (value / 10u) % 10u);
    dest[1] = (char)('0' + value % 10u);
}

static void redraw_clock(void) {
    if (editor_active()) {
        return;
    }
    cursor_erase();
    draw_clock_chip();
    cursor_paint();
}

int desktop_active(void) {
    return desktop_on;
}

static inline unsigned char inb(unsigned short port) {
    unsigned char value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static unsigned char cmos(unsigned char reg) {
    outb(0x70, reg);
    return inb(0x71);
}

/* Seconds since midnight UTC from the CMOS real-time clock, -1 if absent. */
static int rtc_day_seconds(void) {
    for (int spin = 0; spin < 100000 && (cmos(0x0a) & 0x80) != 0; spin++) {
    }
    unsigned char sec = cmos(0x00);
    unsigned char min = cmos(0x02);
    unsigned char hour = cmos(0x04);
    unsigned char b = cmos(0x0b);
    int pm = hour & 0x80;
    hour &= 0x7f;
    if ((b & 0x04) == 0) { /* BCD */
        sec = (unsigned char)((sec & 0x0f) + (sec >> 4) * 10);
        min = (unsigned char)((min & 0x0f) + (min >> 4) * 10);
        hour = (unsigned char)((hour & 0x0f) + (hour >> 4) * 10);
    }
    if ((b & 0x02) == 0) { /* 12-hour */
        hour = (unsigned char)(hour % 12 + (pm ? 12 : 0));
    }
    if (sec > 59 || min > 59 || hour > 23) {
        return -1;
    }
    return hour * 3600 + min * 60 + sec;
}

static int tz_minutes = 180; /* UTC+3 by default; `tz +5:30` changes it */
static int rtc_base = -1;    /* local seconds-of-day at clock_seconds == 0 */

static void rtc_sync(void) {
    int now = rtc_day_seconds();
    if (now < 0) {
        rtc_base = 0;
        return;
    }
    long local = (long)now + tz_minutes * 60L - (long)clock_seconds;
    local %= 86400L;
    if (local < 0) {
        local += 86400L;
    }
    rtc_base = (int)local;
}

/* `tz` shows the offset; `tz +3`, `tz -5`, `tz +5:30` set it. */
void clock_tz(const char *arg, char *out, unsigned int size) {
    if (arg != 0 && arg[0] != 0) {
        int sign = 1;
        const char *p = arg;
        if (*p == '+' || *p == '-') {
            sign = *p == '-' ? -1 : 1;
            p++;
        }
        int h = 0;
        int m = 0;
        while (*p >= '0' && *p <= '9') {
            h = h * 10 + (*p++ - '0');
        }
        if (*p == ':') {
            p++;
            while (*p >= '0' && *p <= '9') {
                m = m * 10 + (*p++ - '0');
            }
        }
        if (*p == 0 && h <= 14 && m < 60) {
            tz_minutes = sign * (h * 60 + m);
            rtc_sync();
        }
    }
    int t = tz_minutes < 0 ? -tz_minutes : tz_minutes;
    unsigned int n = s_cat(out, 0, "tz: UTC", size);
    n = s_cat(out, n, tz_minutes < 0 ? "-" : "+", size);
    n = u_cat(out, n, (unsigned int)(t / 60), size);
    if (t % 60 != 0) {
        n = s_cat(out, n, ":", size);
        char mm[3] = {(char)('0' + t % 60 / 10), (char)('0' + t % 10), 0};
        n = s_cat(out, n, mm, size);
    }
}

/* Local wall-clock time as "HH:MM:SS" (for the journal). */
void clock_now(char *out) {
    if (rtc_base < 0) {
        rtc_sync();
    }
    unsigned int local = ((unsigned int)rtc_base + clock_seconds) % 86400u;
    print_u2(local / 3600u, out);
    out[2] = ':';
    print_u2(local / 60u % 60u, out + 3);
    out[5] = ':';
    print_u2(local % 60u, out + 6);
    out[8] = 0;
}

static void publish_clock(void) {
    if (rtc_base < 0) {
        rtc_sync();
    }
    unsigned int local = ((unsigned int)rtc_base + clock_seconds) % 86400u;
    unsigned int hours = local / 3600u;
    unsigned int minutes = (local / 60u) % 60u;
    unsigned int seconds = local % 60u;
    print_u2(hours, clock_text + 5);
    print_u2(minutes, clock_text + 8);
    print_u2(seconds, clock_text + 11);
    if (desktop_on) {
        redraw_clock();
        desktop_second();
    }
    char up[24];
    unsigned int n = s_cat(up, 0, " up ", sizeof up);
    up[n++] = (char)('0' + clock_seconds / 36000u % 10u);
    up[n++] = (char)('0' + clock_seconds / 3600u % 10u);
    up[n++] = ':';
    up[n++] = (char)('0' + clock_seconds / 600u % 6u);
    up[n++] = (char)('0' + clock_seconds / 60u % 10u);
    up[n++] = ':';
    up[n++] = (char)('0' + clock_seconds % 60u / 10u);
    up[n++] = (char)('0' + clock_seconds % 10u);
    up[n++] = '\n';
    up[n] = 0;
    console_puts("time ");
    console_puts(clock_text + 5);
    console_puts(up);
}

void timer_tick(void) {
    ps2_drain();
    outb(0x20, 0x20);
    if (!desktop_on) {
        return; /* the PIT also drives the scheduler; the clock is a desktop thing */
    }
    if (++blink_ticks >= 50) {
        blink_ticks = 0;
        blink_due = 1;
    }
    clock_ticks++;
    if (clock_ticks < 100) {
        return;
    }
    clock_ticks = 0;
    clock_seconds++;
    clock_due = 1;
}

void clock_service(void) {
    if (blink_due) {
        blink_due = 0;
        blink_on ^= 1;
        if (desktop_on) {
            desktop_blink();
        }
    }
    if (!clock_due) {
        return;
    }
    clock_due = 0;
    publish_clock();
}

unsigned int clock_read(void) {
    return clock_seconds;
}

void timer_start(void) {
    static int started;
    if (started) {
        return;
    }
    started = 1;
    for (int i = 0; i < 256; i++) {
        idt[i].type_attr = 0;
    }
    set_gate(32, isr_irq0);
    user_init(); /* GDT user segments, TSS, exceptions, int 0x80 */
    struct idt_ptr pointer = {
        (unsigned short)(sizeof idt - 1),
        (unsigned long)idt,
    };
    __asm__ volatile("lidt %0" : : "m"(pointer));
    outb(0x20, 0x11);
    outb(0xa0, 0x11);
    outb(0x21, 0x20);
    outb(0xa1, 0x28);
    outb(0x21, 0x04);
    outb(0xa1, 0x02);
    outb(0x21, 0x01);
    outb(0xa1, 0x01);
    outb(0x21, 0xfe);
    outb(0xa1, 0xff);
    unsigned int divisor = 1193182u / 100u;
    outb(0x43, 0x36);
    outb(0x40, (unsigned char)divisor);
    outb(0x40, (unsigned char)(divisor >> 8));
    __asm__ volatile("sti");
}

/* Framebuffer size for the mouse clamp in kernel.c. */
int desktop_width(void) {
    return FB_W;
}

int desktop_height(void) {
    return FB_H;
}

/* `desktop WxH`: pick the framebuffer size (portrait phones, 1080p TVs). */
void desktop_set_size(int w, int h) {
    if (w >= 320 && h >= 320 && w <= FB_MAX_W && h <= FB_MAX_H) {
        FB_W = w & ~7;
        FB_H = h;
    }
}

void cmd_desktop(void) {
    if (program_output()[0] == 0) {
        run_program("hello");
    }
    if (!enable_linear_framebuffer()) {
        console_puts("desktop: no framebuffer\n");
        return;
    }
    layout_init();
    scene_init();
    desktop_on = 1;
    focus = -1;
    hint = -1;
    core_mem = -1;
    core_sample();
    console_puts("labels: Файлы Редактор Терминал Пакеты Память Лента Настройки\n");
    cells_line[0] = 0;
    show_home();
    timer_start();
}
