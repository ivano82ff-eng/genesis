#include "font8x8.h"
#include "utf8.h"

#include "../programs/api.h"
#include "fs_limits.h"

enum { FB_W = 1024, FB_H = 768 };
enum { VIEW_HOME = 0, VIEW_MEMORY, VIEW_FILES, VIEW_EDITOR, VIEW_FILE, VIEW_VIEWER };
enum { BACK_X = 24, BACK_Y = 16, BACK_W = 300, BACK_H = 52 };
enum { ROW_Y0 = 120, ROW_H = 56, ROW_STRIDE = 60 };

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
static char editor_text[80];
static int editor_len;
static int file_slot[8];
static int file_rows;
static char shown_name[16];
static char shown_text[96];

extern void console_puts(const char *text);
extern void console_putc(char ch);
extern void memory_summary(char *dest, unsigned int cap);
extern void fs_summary(char *dest, unsigned int cap);
extern void preview_readme(char *dest, unsigned int cap);
extern int fs_store_readme(const char *text);
extern int fs_export(int index, char *name, unsigned int name_cap, unsigned char *data,
                     unsigned int *length);
extern int fs_entry(int index, char *name, unsigned int name_cap, unsigned int *length);
extern int fs_lookup(const char *name, const unsigned char **data, unsigned int *length);
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


enum { BG = 0x00101820u, PAPER = 0x00243044u, FRAME = 0x004c6a8au, YELLOW = 0x00f0c14au, INK = 0x00f4f1eau };

static const int home_tops[6] = {64, 168, 272, 376, 480, 584};
static const int home_band = 96;

static const char *home_label(int index) {
    static const char *labels[6] = {
        "Память",
        "Файлы",
        "Редактор",
        "Открыть",
        "Перезагрузка",
        "Выключить",
    };
    return labels[index];
}

static void draw_back(void) {
    fill(BACK_X, BACK_Y, BACK_W, BACK_H, FRAME);
    fill(BACK_X + 4, BACK_Y + 4, BACK_W - 8, BACK_H - 8, PAPER);
    text(BACK_X + 16, BACK_Y + 14, "Назад", INK, 2);
}

static void draw_clock_chip(void) {
    fill(620, 28, 13 * 16, 16, BG);
    text(620, 28, clock_text, INK, 2);
}

static int home_tool_at(int y) {
    for (int i = 0; i < 6; i++) {
        if (y >= home_tops[i] && y < home_tops[i] + home_band) {
            return i;
        }
    }
    return -1;
}

static int back_hit(int x, int y) {
    return x >= BACK_X && x < BACK_X + BACK_W && y >= BACK_Y && y < BACK_Y + BACK_H;
}

static int row_hit(int y) {
    for (int i = 0; i < file_rows; i++) {
        int top = ROW_Y0 + i * ROW_STRIDE;
        if (y >= top && y < top + ROW_H) {
            return i;
        }
    }
    return -1;
}

static int is_hidden_name(const char *name) {
    static const char *const hidden[] = {
        "open", "hello", "vtxt", "vdocx", "vpdf", "vimg", "vmp4",
    };
    for (unsigned int i = 0; i < sizeof hidden / sizeof hidden[0]; i++) {
        const char *a = name;
        const char *b = hidden[i];
        while (*a != 0 && *a == *b) {
            a++;
            b++;
        }
        if (*a == 0 && *b == 0) {
            return 1;
        }
    }
    return 0;
}

static void collect_files(void) {
    file_rows = 0;
    for (int i = 0; i < FS_MAX && file_rows < 8; i++) {
        char name[16];
        unsigned int length = 0;
        if (!fs_entry(i, name, sizeof name, &length)) {
            continue;
        }
        if (is_hidden_name(name)) {
            continue;
        }
        file_slot[file_rows++] = i;
    }
}

static void draw_bar(void);

static void draw_home_ui(void) {
    fill(0, 0, FB_W, FB_H, BG);
    text(48, 28, "genesis", YELLOW, 2);
    text(620, 28, clock_text, INK, 2);
    for (int i = 0; i < 6; i++) {
        int y = home_tops[i];
        unsigned int frame = i >= 4 ? YELLOW : FRAME;
        fill(24, y, FB_W - 48, home_band - 8, frame);
        fill(28, y + 4, FB_W - 56, home_band - 16, PAPER);
        text(48, y + 28, home_label(i), INK, 2);
    }
    draw_bar();
}

/* Command bar under the buttons: prompt, typed text, blinking cursor. */
enum { BAR_X = 24, BAR_Y = 680, BAR_H = 80, BAR_TEXT_X = 40, CHAR_W = 18 };
void desktop_run(const char *cmd, char *out, unsigned int size);
static char bar_cmd[64];
static int bar_len;
static char bar_out[96];
static int blink_on = 1;
static unsigned int blink_ticks;
static volatile int blink_due;

static int utf8_chars(const char *s) {
    int n = 0;
    for (; *s; s++) {
        if (!utf8_is_cont((unsigned char)*s)) {
            n++;
        }
    }
    return n;
}

static void draw_bar_cursor(void) {
    int x = BAR_TEXT_X + (9 + utf8_chars(bar_cmd)) * CHAR_W;
    fill(x, BAR_Y + 10, 14, 18, blink_on ? YELLOW : PAPER);
}

static void draw_bar(void) {
    fill(BAR_X, BAR_Y, FB_W - 2 * BAR_X, BAR_H, YELLOW);
    fill(BAR_X + 4, BAR_Y + 4, FB_W - 2 * BAR_X - 8, BAR_H - 8, PAPER);
    text(BAR_TEXT_X, BAR_Y + 12, "genesis> ", YELLOW, 2);
    text(BAR_TEXT_X + 9 * CHAR_W, BAR_Y + 12, bar_cmd, INK, 2);
    text(BAR_TEXT_X, BAR_Y + 48, bar_out[0] ? bar_out : "Введите команду и нажмите Enter (help)", FRAME, 2);
    draw_bar_cursor();
}

static void bar_key(char ch) {
    unsigned char u = (unsigned char)ch;
    if (ch == '\n') {
        if (bar_len == 0) {
            return;
        }
        if ((bar_cmd[0] == 'e' && bar_cmd[1] == 'd' && bar_cmd[2] == 'i' && bar_cmd[3] == 't') ||
            (bar_cmd[0] == 'd' && bar_cmd[1] == 'e' && bar_cmd[2] == 's' && bar_cmd[3] == 'k')) {
            const char *msg = "edit/desktop: кнопки выше";
            int k = 0;
            while (msg[k] && k + 1 < (int)sizeof bar_out) {
                bar_out[k] = msg[k];
                k++;
            }
            bar_out[k] = 0;
        } else {
            desktop_run(bar_cmd, bar_out, sizeof bar_out);
        }
        bar_len = 0;
        bar_cmd[0] = 0;
    } else if (ch == '\b') {
        if (bar_len == 0) {
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
    }
    if (desktop_on && view == VIEW_HOME) {
        blink_on = 1;
        cursor_erase();
        draw_bar();
        cursor_paint();
    }
}

static void draw_section_ui(void) {
    fill(0, 0, FB_W, FB_H, BG);
    draw_back();
    text(620, 28, clock_text, INK, 2);
    if (view == VIEW_MEMORY) {
        char line[48];
        memory_summary(line, sizeof line);
        text(48, 100, "Память", YELLOW, 2);
        text(48, 160, line, INK, 2);
        return;
    }
    if (view == VIEW_FILES) {
        text(48, 84, shown_name[0] ? shown_name : "Файлы", YELLOW, 2);
        for (int i = 0; i < file_rows; i++) {
            char name[16];
            unsigned int length = 0;
            int top = ROW_Y0 + i * ROW_STRIDE;
            fs_entry(file_slot[i], name, sizeof name, &length);
            fill(24, top, FB_W - 48, ROW_H, FRAME);
            fill(28, top + 4, FB_W - 56, ROW_H - 8, PAPER);
            text(48, top + 16, name, INK, 2);
        }
        return;
    }
    if (view == VIEW_EDITOR) {
        text(48, 100, "Редактор", YELLOW, 2);
        text(48, 160, editor_text, INK, 2);
        return;
    }
    text(48, 100, shown_name, YELLOW, 2);
    text(48, 160, shown_text, INK, 2);
}

static void redraw_all(void) {
    cursor_forget();
    if (view == VIEW_HOME) {
        draw_home_ui();
    } else {
        draw_section_ui();
    }
    cursor_paint();
}

static void redraw_editor_line(void) {
    cursor_erase();
    fill(48, 160, 900, 24, BG);
    text(48, 160, editor_text, INK, 2);
    cursor_paint();
}

static void show_home(void);

static void say_back(void) {
    console_puts("Назад\n");
}

static int viewer_back;

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
    if (!fs_lookup(viewer, &prog, &prog_len) || prog_len == 0) {
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
    view = VIEW_VIEWER;
    viewer_back = 0;
    viewer_arena_used = 0;
    mouse_arm();
    cursor_forget();
    fill(0, 0, FB_W, FB_H, BG);
    draw_back();
    text(620, 28, clock_text, INK, 2);
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
    viewer_entry entry = (viewer_entry)(void *)PROG_LOAD_ADDR;
    {
        unsigned long stack_top = 0x900000ul + 512ul * 1024ul;
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
    }
    show_home();
}


static void show_home(void) {
    view = VIEW_HOME;
    redraw_all();
    console_puts("home: ready\n");
}

static void show_memory(void) {
    char line[48];
    view = VIEW_MEMORY;
    mouse_arm();
    memory_summary(line, sizeof line);
    redraw_all();
    say_back();
    console_puts(line);
    console_putc('\n');
}

static void show_files(void) {
    char line[160];
    shown_name[0] = 0;
    collect_files();
    mouse_arm();
    view = VIEW_FILES;
    fs_summary(line, sizeof line);
    redraw_all();
    say_back();
    console_puts("ls: ");
    console_puts(line);
    console_putc('\n');
}

static void show_open(void) {
    char line[160];
    const char *title = "Открыть";
    unsigned int n = 0;
    while (title[n] != 0 && n + 1 < sizeof shown_name) {
        shown_name[n] = title[n];
        n++;
    }
    shown_name[n] = 0;
    collect_files();
    mouse_arm();
    view = VIEW_FILES;
    fs_summary(line, sizeof line);
    redraw_all();
    say_back();
    console_puts("open: ");
    console_puts(line);
    console_putc('\n');
}

static void show_editor(void) {
    view = VIEW_EDITOR;
    mouse_arm();
    preview_readme(editor_text, sizeof editor_text);
    editor_len = 0;
    while (editor_text[editor_len] != 0) {
        editor_len++;
    }
    redraw_all();
    say_back();
    console_puts("edit: ");
    console_puts(editor_text);
    console_putc('\n');
}

static void show_file(int row) {
    char name[16];
    unsigned int length = 0;
    const unsigned char *bytes = 0;
    if (row < 0 || row >= file_rows) {
        return;
    }
    if (!fs_entry(file_slot[row], name, sizeof name, &length)) {
        return;
    }
    if (!fs_lookup(name, &bytes, &length)) {
        return;
    }
    const char *viewer = viewer_for_name(name);
    launch_viewer(viewer, bytes, length, name);
}

static void editor_key(char ch) {
    if (ch == '\n') {
        console_putc('\n');
        if (!fs_store_readme(editor_text)) {
            console_puts("edit: failed\n");
        } else {
            console_puts("edit: saved\n");
        }
        return;
    }
    if (ch == '\b') {
        if (editor_len > 0) {
            editor_len--;
            while (editor_len > 0 && utf8_is_cont((unsigned char)editor_text[editor_len])) {
                editor_len--;
            }
            editor_text[editor_len] = 0;
            console_putc('\b');
            redraw_editor_line();
        }
        return;
    }
    unsigned char u = (unsigned char)ch;
    if (u < 32 || u == 127) {
        return; /* bytes >= 0x80 are UTF-8 (Russian letters) */
    }
    if (u >= 0xc0 && editor_len + 5 >= (int)sizeof editor_text) {
        return;
    }
    if (editor_len + 1 >= (int)sizeof editor_text) {
        return;
    }
    editor_text[editor_len++] = ch;
    editor_text[editor_len] = 0;
    console_putc(ch);
    redraw_editor_line();
}

void desktop_mouse(int x, int y, int left) {
    if (!desktop_on) {
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
    if (view == VIEW_HOME) {
        int tool = home_tool_at(y);
        if (tool == 0) {
            show_memory();
            return;
        }
        if (tool == 1) {
            show_files();
            return;
        }
        if (tool == 2) {
            show_editor();
            return;
        }
        if (tool == 3) {
            show_open();
            return;
        }
        if (tool == 4) {
            disk_request_home();
            machine_reboot();
        }
        if (tool == 5) {
            machine_poweroff();
        }
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
    if (back_hit(x, y)) {
        show_home();
        return;
    }
    if (view == VIEW_FILES) {
        int row = row_hit(y);
        if (row >= 0) {
            show_file(row);
            return;
        }
    }
    cursor_move();
}

void desktop_key(char ch) {
    if (view == VIEW_EDITOR) {
        editor_key(ch);
    }
    if (view == VIEW_HOME) {
        bar_key(ch);
    }
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
    unsigned int offset_high;
    unsigned int zero;
} __attribute__((packed));

struct idt_ptr {
    unsigned short limit;
    unsigned long base;
} __attribute__((packed));

static struct idt_entry idt[256];
static unsigned int clock_ticks;
static unsigned int clock_seconds;
static volatile int clock_due;

extern void isr_irq0(void);

static void set_gate(int number, void *handler) {
    unsigned long addr = (unsigned long)handler;
    idt[number].offset_low = (unsigned short)addr;
    idt[number].selector = 0x08;
    idt[number].ist = 0;
    idt[number].type_attr = 0x8e;
    idt[number].offset_mid = (unsigned short)(addr >> 16);
    idt[number].offset_high = (unsigned int)(addr >> 32);
    idt[number].zero = 0;
}

static void print_u2(unsigned int value, char *dest) {
    dest[0] = (char)('0' + (value / 10u) % 10u);
    dest[1] = (char)('0' + value % 10u);
}

static void redraw_clock(void) {
    cursor_erase();
    draw_clock_chip();
    cursor_paint();
}

int desktop_active(void) {
    return desktop_on;
}

static void publish_clock(void) {
    unsigned int hours = (clock_seconds / 3600u) % 100u;
    unsigned int minutes = (clock_seconds / 60u) % 60u;
    unsigned int seconds = clock_seconds % 60u;
    print_u2(hours, clock_text + 5);
    print_u2(minutes, clock_text + 8);
    print_u2(seconds, clock_text + 11);
    if (desktop_on) {
        redraw_clock();
    }
    console_puts("time ");
    console_puts(clock_text + 5);
    console_putc('\n');
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
        if (desktop_on && view == VIEW_HOME) {
            cursor_erase();
            draw_bar_cursor();
            cursor_paint();
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

void cmd_desktop(void) {
    if (program_output()[0] == 0) {
        run_program("hello");
    }
    if (!enable_linear_framebuffer()) {
        console_puts("desktop: no framebuffer\n");
        return;
    }
    mouse_arm();
    desktop_on = 1;
    view = VIEW_HOME;
    redraw_all();
    console_puts("labels: Память Файлы Редактор Открыть Перезагрузка Выключить\n");
    console_puts("home: ready\n");
    timer_start();
}
