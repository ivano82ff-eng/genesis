/* After long mode: the same line on COM1 and on the VGA text screen. */
static inline void outb(unsigned short port, unsigned char value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(unsigned short port, unsigned short value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline unsigned char inb(unsigned short port) {
    unsigned char value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

enum { COM1 = 0x3f8, VGA_COLS = 80, VGA_ROWS = 25, VGA_ATTR = 0x0f };

int desktop_active(void);
void desktop_key(char ch);
void cmd_desktop(void);
void desktop_set_size(int w, int h);
void desktop_mouse(int x, int y, int left);
void mouse_arm(void);
int disk_load(void);
int disk_save(void);
int install_system(void);
void disk_request_home(void);
int disk_consume_home(void);
int disk_consume_seed(void);
int disk_consume_migrated(void);
void machine_poweroff(void);
void machine_reboot(void);
void link_init(void);
void link_poll(void);
int net_command(const char *line);
void clock_service(void);
void clock_tz(const char *arg, char *out, unsigned int size);
int link_up(void);
int link_send_text(const char *text);
int link_ping(void);
int link_send_file(const char *name, const unsigned char *data, unsigned int length);
void timer_start(void);
int task_current(void);
void task_sleep(unsigned int ticks);
void cmd_ps(void);

#ifdef __x86_64__
#define PUSHF_POP "pushfq; popq"
#else
#define PUSHF_POP "pushfl; popl"
#endif
#include "fs_limits.h"
#include "editor.h"
#include "utf8.h"

static volatile unsigned short *const VGA = (volatile unsigned short *)0xb8000;

/* Thick blinking hardware cursor (VGA blinks it on its own timer). */
static void vga_show_cursor(void) {
    outb(0x3d4, 0x0a);
    outb(0x3d5, 0x0b);
    outb(0x3d4, 0x0b);
    outb(0x3d5, 0x0f);
}

static void vga_cursor_at(int row, int col) {
    unsigned short pos = (unsigned short)(row * VGA_COLS + (col < VGA_COLS ? col : VGA_COLS - 1));
    outb(0x3d4, 0x0f);
    outb(0x3d5, (unsigned char)(pos & 0xff));
    outb(0x3d4, 0x0e);
    outb(0x3d5, (unsigned char)(pos >> 8));
}

static void vga_clear(void) {
    const unsigned short blank = (unsigned short)((VGA_ATTR << 8) | ' ');
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        VGA[i] = blank;
    }
}

static void vga_puts(int row, int col, const char *text) {
    while (*text && col < VGA_COLS && row < VGA_ROWS) {
        VGA[row * VGA_COLS + col] = (unsigned short)((VGA_ATTR << 8) | (unsigned char)*text);
        text++;
        col++;
    }
}

/* Put the Cyrillic glyphs into the VGA text font (plane 2), in CP866 slots,
 * so text cells can hold Russian letters. Each 8x8 row is drawn twice. */
static void vga_load_cyrillic(void) {
    volatile unsigned char *font = (volatile unsigned char *)0xa0000;
    outb(0x3c4, 0x00); outb(0x3c5, 0x01);
    outb(0x3c4, 0x02); outb(0x3c5, 0x04);
    outb(0x3c4, 0x04); outb(0x3c5, 0x07);
    outb(0x3c4, 0x00); outb(0x3c5, 0x03);
    outb(0x3ce, 0x04); outb(0x3cf, 0x02);
    outb(0x3ce, 0x05); outb(0x3cf, 0x00);
    outb(0x3ce, 0x06); outb(0x3cf, 0x00);
    outb(0x3ce, 0x01); outb(0x3cf, 0x00);
    outb(0x3ce, 0x03); outb(0x3cf, 0x00);
    outb(0x3ce, 0x08); outb(0x3cf, 0xff);
    for (int i = 0; i < CYR8X8_COUNT; i++) {
        volatile unsigned char *cell = font + (unsigned int)CYR8X8[i].slot * 32u;
        for (int row = 0; row < 16; row++) {
            cell[row] = CYR8X8[i].rows[row / 2];
        }
    }
    outb(0x3c4, 0x00); outb(0x3c5, 0x01);
    outb(0x3c4, 0x02); outb(0x3c5, 0x03);
    outb(0x3c4, 0x04); outb(0x3c5, 0x03);
    outb(0x3c4, 0x00); outb(0x3c5, 0x03);
    outb(0x3ce, 0x04); outb(0x3cf, 0x00);
    outb(0x3ce, 0x05); outb(0x3cf, 0x10);
    outb(0x3ce, 0x06); outb(0x3cf, 0x0e);
}

static int kbd_ru;

/* EN/RU in the top-right corner of the text console. */
static void layout_indicator(void) {
    vga_puts(0, VGA_COLS - 2, kbd_ru ? "RU" : "EN");
}

int kbd_layout_ru(void) {
    return kbd_ru;
}

#define BOOT_HINT "Введите команду и нажмите Enter (help — список команд)"

static void vga_puts_utf8(int row, int col, const char *text, unsigned char attr) {
    const unsigned char *s = (const unsigned char *)text;
    unsigned int cp;
    while (*s && col < VGA_COLS) {
        s += utf8_decode(s, 4, &cp);
        VGA[row * VGA_COLS + col++] = (unsigned short)((attr << 8) | vga_cell(cp));
    }
}

static int type_row = 1;
static int type_col = 0;

static void screen_scroll(void) {
    const unsigned short blank = (unsigned short)((VGA_ATTR << 8) | ' ');
    for (int i = VGA_COLS; i < VGA_COLS * VGA_ROWS; i++) {
        VGA[i - VGA_COLS] = VGA[i];
    }
    for (int i = (VGA_ROWS - 1) * VGA_COLS; i < VGA_COLS * VGA_ROWS; i++) {
        VGA[i] = blank;
    }    if (kbd_ru) {
        layout_indicator();
    }
}

static void screen_advance_row(void) {
    if (type_row + 1 >= VGA_ROWS) {
        screen_scroll();
    } else {
        type_row++;
    }
}

static unsigned char utf8_pend[4];
static unsigned int utf8_have;

static void screen_cell(unsigned char cell);

/* Console bytes are UTF-8: gather a sequence, then draw one cell for it. */
static void screen_putc(char ch) {
    unsigned char b = (unsigned char)ch;
    if (utf8_have > 0 && utf8_is_cont(b)) {
        utf8_pend[utf8_have++] = b;
        unsigned int cp = 0;
        unsigned char lead = utf8_pend[0];
        unsigned int want = (lead & 0xe0u) == 0xc0u ? 2u : (lead & 0xf0u) == 0xe0u ? 3u : 4u;
        if (utf8_have < want && utf8_have < sizeof utf8_pend) {
            return;
        }
        utf8_decode(utf8_pend, utf8_have, &cp);
        utf8_have = 0;
        screen_cell(vga_cell(cp));
        return;
    }
    if (utf8_have > 0) {
        utf8_have = 0;
        screen_cell('?');
    }
    if (b >= 0xc0u && b < 0xf8u) {
        utf8_pend[utf8_have++] = b;
        return;
    }
    if (b >= 0x80u) {
        screen_cell('?');
        return;
    }
    screen_cell(b);
}

static void screen_cell_raw(unsigned char ch);

static void screen_cell(unsigned char ch) {
    screen_cell_raw(ch);
    if (!editor_active() && !desktop_active()) {
        vga_cursor_at(type_row, type_col);
    }
}

static void screen_cell_raw(unsigned char ch) {
    const unsigned short blank = (unsigned short)((VGA_ATTR << 8) | ' ');
    if (ch == '\b') {
        if (type_col > 0) {
            type_col--;
            VGA[type_row * VGA_COLS + type_col] = blank;
        }
        return;
    }
    if (ch == '\n') {
        type_col = 0;
        screen_advance_row();
        return;
    }
    if (type_col >= VGA_COLS) {
        type_col = 0;
        screen_advance_row();
    }
    VGA[type_row * VGA_COLS + type_col] = (unsigned short)((VGA_ATTR << 8) | (unsigned char)ch);
    type_col++;
}

/* Scancode set 1, make codes only. Unlisted codes are ignored. */
static char decode_set1(unsigned char scancode, int shift, int caps) {
    static const char map[128] = {
        [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
        [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0a] = '9', [0x0b] = '0',
        [0x0c] = '-', [0x0d] = '=', [0x0e] = '\b', [0x10] = 'q', [0x11] = 'w',
        [0x12] = 'e', [0x13] = 'r', [0x14] = 't', [0x15] = 'y', [0x16] = 'u',
        [0x17] = 'i', [0x18] = 'o', [0x19] = 'p', [0x1c] = '\n', [0x1e] = 'a',
        [0x1f] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g', [0x23] = 'h',
        [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';', [0x28] = '\'',
        [0x29] = '`', [0x2b] = '\\', [0x2c] = 'z', [0x2d] = 'x', [0x2e] = 'c',
        [0x2f] = 'v', [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',',
        [0x34] = '.', [0x35] = '/', [0x39] = ' ', [0x1a] = '[', [0x1b] = ']',
    };
    static const char shifted[128] = {
        [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
        [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0a] = '(', [0x0b] = ')',
        [0x0c] = '_', [0x0d] = '+', [0x1a] = '{', [0x1b] = '}', [0x27] = ':',
        [0x28] = '"', [0x29] = '~', [0x2b] = '|', [0x33] = '<', [0x34] = '>',
        [0x35] = '?',
    };
    if (scancode >= 128) {
        return 0;
    }
    char ch = map[scancode];
    if (ch >= 'a' && ch <= 'z') {
        return (shift ^ caps) ? (char)(ch - 'a' + 'A') : ch;
    }
    if (shift && shifted[scancode] != 0) {
        return shifted[scancode];
    }
    return ch;
}

/* ЙЦУКЕН on the same keys; 0 means "type the Latin character". */
static unsigned int decode_ru(unsigned char scancode, int shift, int caps) {
    static const unsigned short letters[128] = {
        [0x10] = 0x439, [0x11] = 0x446, [0x12] = 0x443, [0x13] = 0x43a, [0x14] = 0x435,
        [0x15] = 0x43d, [0x16] = 0x433, [0x17] = 0x448, [0x18] = 0x449, [0x19] = 0x437,
        [0x1a] = 0x445, [0x1b] = 0x44a, [0x1e] = 0x444, [0x1f] = 0x44b, [0x20] = 0x432,
        [0x21] = 0x430, [0x22] = 0x43f, [0x23] = 0x440, [0x24] = 0x43e, [0x25] = 0x43b,
        [0x26] = 0x434, [0x27] = 0x436, [0x28] = 0x44d, [0x2c] = 0x44f, [0x2d] = 0x447,
        [0x2e] = 0x441, [0x2f] = 0x43c, [0x30] = 0x438, [0x31] = 0x442, [0x32] = 0x44c,
        [0x33] = 0x431, [0x34] = 0x44e, [0x29] = 0x451,
    };
    if (scancode >= 128) {
        return 0;
    }
    unsigned int cp = letters[scancode];
    if (cp != 0) {
        if (shift ^ caps) {
            cp = cp == 0x451 ? 0x401 : cp - 0x20;
        }
        return cp;
    }
    switch (scancode) {
    case 0x35: return shift ? ',' : '.';
    case 0x03: return shift ? '"' : 0;
    case 0x04: return shift ? 0x2116 : 0;
    case 0x05: return shift ? ';' : 0;
    case 0x07: return shift ? ':' : 0;
    case 0x08: return shift ? '?' : 0;
    default: return 0;
    }
}

static void ps2_wait_write(void) {
    for (int i = 0; i < 2000000; i++) {
        if ((inb(0x64) & 0x02) == 0) {
            return;
        }
    }
}

static int ps2_wait_read(void) {
    for (int i = 0; i < 2000000; i++) {
        if ((inb(0x64) & 0x01) != 0) {
            return 1;
        }
    }
    return 0;
}

static void mouse_write(unsigned char value) {
    ps2_wait_write();
    outb(0x64, 0xd4);
    ps2_wait_write();
    outb(0x60, value);
}

static void mouse_init(void) {
    ps2_wait_write();
    outb(0x64, 0xa8);
    ps2_wait_write();
    outb(0x64, 0x60);
    ps2_wait_write();
    outb(0x60, 0x47);
    mouse_write(0xf6);
    ps2_wait_read();
    (void)inb(0x60);
    mouse_write(0xf4);
    ps2_wait_read();
    (void)inb(0x60);
}

int desktop_width(void);
int desktop_height(void);

static int mouse_cycle = 0;
static unsigned char mouse_bytes[3];
static int mouse_x = 0;
static int mouse_y = 0;
static int mouse_left = 0;
static int mouse_saw_down = 0;
static int mouse_dirty = 0;
static int mouse_down_x = 0;
static int mouse_down_y = 0;

static void mouse_packet(unsigned char data) {
    if (mouse_cycle == 0 && (data & 0x08) == 0) {
        return;
    }
    mouse_bytes[mouse_cycle++] = data;
    if (mouse_cycle < 3) {
        return;
    }
    mouse_cycle = 0;
    /* 9-bit deltas: the sign bits live in byte 0 (bit 4 = X, bit 5 = Y). */
    int dx = (int)mouse_bytes[1] - ((mouse_bytes[0] & 0x10) ? 256 : 0);
    int dy = (int)mouse_bytes[2] - ((mouse_bytes[0] & 0x20) ? 256 : 0);
    if (mouse_bytes[0] & 0xc0) {
        dx = dy = 0; /* overflow: the deltas are garbage */
    }
    mouse_x += dx;
    mouse_y -= dy;
    if (mouse_x < 0) {
        mouse_x = 0;
    }
    if (mouse_y < 0) {
        mouse_y = 0;
    }
    /* the real screen size: 480x800 phones and 1920x1080 TVs too */
    if (mouse_x > desktop_width() - 1) {
        mouse_x = desktop_width() - 1;
    }
    if (mouse_y > desktop_height() - 1) {
        mouse_y = desktop_height() - 1;
    }
    int was_left = mouse_left;
    mouse_left = (mouse_bytes[0] & 0x01) != 0;
    if (mouse_left && !was_left) {
        mouse_down_x = mouse_x;
        mouse_down_y = mouse_y;
    }
    if (mouse_left) {
        mouse_saw_down = 1;
    }
    mouse_dirty = 1;
}

static void keyboard_drain(void) {
    for (int i = 0; i < 32; i++) {
        if ((inb(0x64) & 0x01) == 0) {
            return;
        }
        (void)inb(0x60);
    }
}

static void shell_input(char ch);

enum { KBD_RING = 128 };

static unsigned char kbd_bytes[KBD_RING];
static int kbd_shift;
static int kbd_caps;
static int kbd_e0;
static int kbd_ctrl;
static int kbd_alt;
#define KEY_UP ((char)1)
#define KEY_DOWN ((char)2)
#define DESK_UP ((char)1)
#define DESK_DOWN ((char)2)
#define DESK_LEFT ((char)3)
#define DESK_RIGHT ((char)4)
#define DESK_ESC ((char)27)
#define DESK_PGUP ((char)5)
#define DESK_PGDN ((char)6)
static int kbd_count;

static void ps2_lock(unsigned long *flags) {
    __asm__ volatile(PUSHF_POP " %0; cli" : "=r"(*flags)::"memory");
}

static void ps2_unlock(unsigned long flags) {
    if ((flags & 0x200ul) != 0) {
        __asm__ volatile("sti" ::: "memory");
    }
}

void ps2_drain(void) {
    unsigned long flags;
    ps2_lock(&flags);
    for (;;) {
        unsigned char status = inb(0x64);
        if ((status & 0x01) == 0) {
            break;
        }
        unsigned char data = inb(0x60);
        if ((status & 0x20) != 0) {
            mouse_packet(data);
            continue;
        }
        if (kbd_count >= KBD_RING) {
            for (int i = 1; i < KBD_RING; i++) {
                kbd_bytes[i - 1] = kbd_bytes[i];
            }
            kbd_count = KBD_RING - 1;
        }
        kbd_bytes[kbd_count++] = data;
    }
    ps2_unlock(flags);
}

static void mouse_flush(void) {
    unsigned long flags;
    ps2_lock(&flags);
    int dirty = mouse_dirty;
    int saw = mouse_saw_down;
    int left = mouse_left;
    int x = mouse_x;
    int y = mouse_y;
    int down_x = mouse_down_x;
    int down_y = mouse_down_y;
    mouse_dirty = 0;
    mouse_saw_down = 0;
    ps2_unlock(flags);
    if (!dirty && !saw) {
        return;
    }
    if (saw) {
        desktop_mouse(down_x, down_y, 1);
        if (!left || x != down_x || y != down_y) {
            desktop_mouse(x, y, left);
        }
        return;
    }
    desktop_mouse(x, y, left);
}

void desktop_layout_changed(void);

static void layout_toggle(void) {
    kbd_ru ^= 1;
    if (desktop_active()) {
        desktop_layout_changed();
    }
    if (!desktop_active() && !editor_active()) {
        layout_indicator();
    }
    if (editor_active()) {
        editor_key(ED_REDRAW);
    }
}

/* One typed byte (UTF-8 bytes arrive one by one) to whoever has the focus. */
static void deliver(char ch) {
    if (editor_active()) {
        editor_key((unsigned char)ch);
    } else if (desktop_active()) {
        desktop_key(ch);
    } else {
        shell_input(ch);
    }
}

/* Navigation-cluster keys (arrows, Home/End, PgUp/PgDn, Delete). They come
 * E0-prefixed from a real keyboard, but some front-ends (QEMU's GTK UI on
 * Windows) deliver them as bare keypad scancodes, so both paths land here. */
static void nav_scancode(unsigned char b) {
    if (editor_active()) {
        int key = 0;
        switch (b) {
        case 0x48: key = ED_UP; break;
        case 0x50: key = ED_DOWN; break;
        case 0x4b: key = ED_LEFT; break;
        case 0x4d: key = ED_RIGHT; break;
        case 0x47: key = ED_HOME; break;
        case 0x4f: key = ED_END; break;
        case 0x49: key = ED_PGUP; break;
        case 0x51: key = ED_PGDN; break;
        case 0x53: key = ED_DELETE; break;
        default: break;
        }
        if (key != 0) {
            editor_key(key | (kbd_shift && key != ED_DELETE ? ED_SHIFT : 0)); /* Shift+arrows select */
        }
        return;
    }
    /* On the desktop the arrows move history (up/down) and cell focus. */
    if (desktop_active() && (b == 0x48 || b == 0x50 || b == 0x4b || b == 0x4d || b == 0x49 || b == 0x51)) {
        desktop_key(b == 0x48   ? DESK_UP
                    : b == 0x50 ? DESK_DOWN
                    : b == 0x4b ? DESK_LEFT
                    : b == 0x4d ? DESK_RIGHT
                    : b == 0x49 ? DESK_PGUP
                                : DESK_PGDN);
        return;
    }
    /* Outside the editor: arrow up/down drive shell history. */
    if ((b == 0x48 || b == 0x50) && !desktop_active()) {
        shell_input(b == 0x48 ? KEY_UP : KEY_DOWN);
    }
}

static void ps2_apply(void) {
    unsigned long flags;
    ps2_lock(&flags);
    unsigned char bytes[KBD_RING];
    int count = kbd_count;
    for (int i = 0; i < count; i++) {
        bytes[i] = kbd_bytes[i];
    }
    kbd_count = 0;
    ps2_unlock(flags);
    for (int i = 0; i < count; i++) {
        unsigned char b = bytes[i];
        if (b == 0xe0) {
            kbd_e0 = 1;
            continue;
        }
        if (kbd_e0) {
            kbd_e0 = 0;
            if (b == 0x1d || b == 0x9d) {
                kbd_ctrl = b == 0x1d;
                continue;
            }
            if (b == 0x38 || b == 0xb8) {
                kbd_alt = b == 0x38;
                if (kbd_alt && kbd_shift) {
                    layout_toggle();
                }
                continue;
            }
            nav_scancode(b);
            continue;
        }
        if (b == 0x2a || b == 0x36) {
            if (kbd_alt && !kbd_shift) {
                layout_toggle();
            }
            kbd_shift = 1;
            continue;
        }
        if (b == 0x38 || b == 0xb8) {
            kbd_alt = b == 0x38;
            if (kbd_alt && kbd_shift) {
                layout_toggle();
            }
            continue;
        }
        if (b == 0x39 && kbd_ctrl) {
            layout_toggle();
            continue;
        }
        if (b == 0xaa || b == 0xb6) {
            kbd_shift = 0;
            continue;
        }
        if (b == 0x3a) {
            kbd_caps ^= 1;
            continue;
        }
        if (b == 0x1d || b == 0x9d) {
            kbd_ctrl = b == 0x1d;
            continue;
        }
        if (editor_active() && (b == 0x01 || b == 0x0f)) {
            editor_key(b == 0x01 ? ED_QUIT : ED_TAB);
            continue;
        }
        if (b == 0x0f) {
            deliver('\t'); /* Tab: completion (shell, bar) or cell focus */
            continue;
        }
        if (b == 0x01 && desktop_active()) {
            desktop_key(DESK_ESC);
            continue;
        }
        if ((b & 0x80) != 0) {
            continue;
        }
        if (b == 0x47 || b == 0x48 || b == 0x49 || b == 0x4b || b == 0x4d || b == 0x4f || b == 0x50 ||
            b == 0x51 || b == 0x53) {
            nav_scancode(b); /* keypad arrows without E0 */
            continue;
        }
        char ch = decode_set1(b, kbd_shift, kbd_caps);
        if (ch == 0) {
            continue;
        }
        if (!editor_active() && kbd_ctrl && (ch == 'v' || ch == 'V')) {
            /* ^V in the shell or the command bar: the internal clipboard */
            unsigned int clen = 0;
            const char *ctext = clipboard_text(&clen);
            for (unsigned int k = 0; k < clen; k++) {
                if (ctext[k] != '\n' || k + 1 < clen) {
                    deliver(ctext[k] == '\t' ? ' ' : ctext[k]);
                }
            }
            continue;
        }
        if (editor_active() && kbd_ctrl) {
            if (ch == 'c' || ch == 'C') {
                editor_key(ED_COPY);
            } else if (ch == 'x' || ch == 'X') {
                editor_key(ED_CUT);
            } else if (ch == 'v' || ch == 'V') {
                editor_key(ED_PASTE);
            } else if (ch == 's' || ch == 'S') {
                editor_key(ED_SAVE);
            } else if (ch == 'q' || ch == 'Q') {
                editor_key(ED_QUIT);
            }
            continue;
        }
        unsigned int cp = kbd_ru ? decode_ru(b, kbd_shift, kbd_caps) : 0;
        if (cp != 0) {
            char bytes[4];
            int n = utf8_encode(cp, bytes);
            for (int k = 0; k < n; k++) {
                deliver(bytes[k]);
            }
            continue;
        }
        deliver(ch);
    }
}

static void keyboard_poll(void) {
    ps2_drain();
    ps2_apply();
    mouse_flush();
}

void guest_yield(void) {
    if (task_current() != 0) {
        return; /* input belongs to the shell task */
    }
    keyboard_poll();
    clock_service();
    link_poll();
}

/* Arms the PS/2 mouse once, on the first desktop screen. Later calls are
 * no-ops: re-arming used to reset the pointer to 0,0 (top-left) and drop
 * buffered keystrokes on every button press or panel switch. */
static int mouse_armed;

void mouse_arm(void) {
    if (mouse_armed) {
        return;
    }
    mouse_armed = 1;
    mouse_init();
    for (int i = 0; i < 16; i++) {
        if ((inb(0x64) & 0x01) == 0) {
            break;
        }
        (void)inb(0x60);
    }
    mouse_x = 0;
    mouse_y = 0;
    mouse_cycle = 0;
    mouse_left = 0;
    mouse_saw_down = 0;
    mouse_dirty = 0;
    mouse_down_x = 0;
    mouse_down_y = 0;
    kbd_count = 0;
}

static void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xc7);
    outb(COM1 + 4, 0x0b);
}

static void serial_putc(char ch) {
    for (int spin = 0; spin < 100000; spin++) {
        if (inb(COM1 + 5) & 0x20) {
            break;
        }
    }
    outb(COM1, (unsigned char)ch);
}

static void serial_puts(const char *text) {
    while (*text) {
        serial_putc(*text++);
    }
}

static volatile int fg_task = -1; /* the foreground program's task */
static char program_text[64];
static int program_len = 0;
static int program_capture = 0;

enum { SH_OUT = 4096 };
static char sh_bufs[2][SH_OUT];
static char *cap_buf;
static unsigned int cap_len;
static int cap_on;

static char desk_cap[2048];
static unsigned int desk_cap_len;
static int desk_cap_on;

void console_putc(char ch) {
    if (cap_on && (task_current() == 0 || task_current() == fg_task)) {
        /* Shell pipe or redirect: collect output instead of printing it. */
        if (cap_len < SH_OUT) {
            cap_buf[cap_len++] = ch;
        }
        return;
    }
    serial_putc(ch);
    if (desk_cap_on && desk_cap_len + 1 < sizeof desk_cap) {
        desk_cap[desk_cap_len++] = ch;
        desk_cap[desk_cap_len] = 0;
    }
    if (!desktop_active() && !editor_active()) {
        screen_putc(ch);
    }
    if (program_capture && ch != '\n' && program_len + 1 < (int)sizeof program_text) {
        program_text[program_len++] = ch;
        program_text[program_len] = 0;
    }
}

void console_puts(const char *text) {
    /* One string at a time, so a background task cannot split a line. */
    unsigned long flags;
    ps2_lock(&flags);
    while (*text) {
        console_putc(*text++);
    }
    ps2_unlock(flags);
}

enum { LINE_MAX = 128 };

static char line[LINE_MAX];
static int line_len = 0;

static int str_eq(const char *left, const char *right) {
    while (*left != 0 && *left == *right) {
        left++;
        right++;
    }
    return *left == 0 && *right == 0;
}

static void trim(char *text) {
    char *start = text;
    while (*start == ' ') {
        start++;
    }
    if (start != text) {
        char *dest = text;
        while (*start != 0) {
            *dest++ = *start++;
        }
        *dest = 0;
    }
    char *end = text;
    while (*end != 0) {
        end++;
    }
    while (end > text && end[-1] == ' ') {
        end--;
        *end = 0;
    }
}

static int starts_with(const char *text, const char *prefix) {
    while (*prefix != 0) {
        if (*text++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

static int parse_u32(const char *text, unsigned int *out) {
    unsigned int value = 0;
    if (*text < '0' || *text > '9') {
        return 0;
    }
    while (*text >= '0' && *text <= '9') {
        value = value * 10u + (unsigned int)(*text - '0');
        text++;
    }
    if (*text != 0) {
        return 0;
    }
    *out = value;
    return 1;
}

static void print_hex64(unsigned long long value) {
    console_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned int nibble = (unsigned int)((value >> shift) & 0xfull);
        console_putc((char)(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10)));
    }
}

static void print_u32(unsigned int value) {
    char digits[10];
    int count = 0;
    if (value == 0) {
        console_putc('0');
        return;
    }
    while (value > 0) {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (count > 0) {
        console_putc(digits[--count]);
    }
}

enum { MAP_MAX = 8, HEAP_SIZE = 1024u * 1024u };

struct mem_region {
    unsigned long addr;
    unsigned long length;
    unsigned int type;
};

static struct mem_region mem_map[MAP_MAX];
static int mem_map_count = 0;
static unsigned long usable_bytes = 0;
/* Was 0x200000, inside the kernel's own BSS once it grew past 1 MiB. */
static unsigned char *const HEAP = (unsigned char *)KERNEL_HEAP_ADDR;
static unsigned int heap_used = 0;

static void memory_init(unsigned int info_addr) {
    mem_map_count = 0;
    usable_bytes = 0;
    if (info_addr < 8) {
        return;
    }
    unsigned char *base = (unsigned char *)(unsigned long)info_addr;
    unsigned int total = *(unsigned int *)base;
    unsigned int offset = 8;
    while (offset + 8 <= total) {
        unsigned int type = *(unsigned int *)(base + offset);
        unsigned int size = *(unsigned int *)(base + offset + 4);
        if (size < 8) {
            break;
        }
        if (type == 0) {
            break;
        }
        if (type == 6 && size >= 16) {
            unsigned int entry_size = *(unsigned int *)(base + offset + 8);
            unsigned int cursor = offset + 16;
            if (entry_size < 24) {
                entry_size = 24;
            }
            while (cursor + 20 <= offset + size && mem_map_count < MAP_MAX) {
                unsigned long addr = *(unsigned long *)(base + cursor);
                unsigned long length = *(unsigned long *)(base + cursor + 8);
                unsigned int entry_type = *(unsigned int *)(base + cursor + 16);
                mem_map[mem_map_count].addr = addr;
                mem_map[mem_map_count].length = length;
                mem_map[mem_map_count].type = entry_type;
                if (entry_type == 1) {
                    usable_bytes += length;
                }
                mem_map_count++;
                cursor += entry_size;
            }
        }
        offset += (size + 7u) & ~7u;
    }
}

static void *heap_alloc(unsigned int size) {
    unsigned int start = (heap_used + 15u) & ~15u;
    if (size == 0 || start > HEAP_SIZE || size > HEAP_SIZE - start) {
        return 0;
    }
    heap_used = start + size;
    return HEAP + start;
}

void *kheap_alloc(unsigned int size) {
    return heap_alloc(size);
}

unsigned int user_pages_used(void);
unsigned int user_pages_total(void);

static void cmd_mem(void) {
    console_puts("usable ");
    print_u32((unsigned int)(usable_bytes >> 20));
    console_puts(" MiB\n");
    console_puts("user pages ");
    print_u32(user_pages_used());
    console_putc('/');
    print_u32(user_pages_total());
    console_putc('\n');
    if (mem_map_count == 0) {
        console_puts("map: none\n");
        return;
    }
    for (int i = 0; i < mem_map_count; i++) {
        console_puts("map ");
        print_hex64(mem_map[i].addr);
        console_putc(' ');
        print_hex64(mem_map[i].length);
        console_puts(" type ");
        print_u32(mem_map[i].type);
        console_putc('\n');
    }
}

static void cmd_alloc(const char *arg) {
    unsigned int size = 0;
    if (!parse_u32(arg, &size) || size == 0) {
        console_puts("alloc: need a size\n");
        return;
    }
    void *block = heap_alloc(size);
    if (block == 0) {
        console_puts("alloc: no memory\n");
        return;
    }
    console_puts("alloc: ");
    print_hex64((unsigned long)block);
    console_putc('\n');
}

#include "fs_limits.h"

/*
 * In-RAM view of Grid Store. Every entry is keyed by its absolute path
 * ("/home/notes.txt"); directories are entries with dir = 1. File bytes live
 * in a 48 MiB pool and are compacted when it fills up. disk.c persists the
 * table; `dirty` marks files whose bytes are not on disk yet.
 */
struct ram_file {
    char name[FS_NAME];
    unsigned char *data;
    unsigned int length;
    unsigned int cap;
    int used;
    int dir;
    int dirty;
    unsigned int blk;
    unsigned int nblk;
};

static struct ram_file ramfs[FS_MAX];
static unsigned char *const FS_POOL = (unsigned char *)FS_POOL_ADDR;
static unsigned long pool_top;
static char cwd[FS_NAME] = "/home";

static void mem_move(unsigned char *dest, const unsigned char *src, unsigned int n) {
    if (dest == src || n == 0) {
        return;
    }
    if (dest < src) {
        for (unsigned int i = 0; i < n; i++) {
            dest[i] = src[i];
        }
    } else {
        for (unsigned int i = n; i > 0; i--) {
            dest[i - 1] = src[i - 1];
        }
    }
}

static unsigned int str_len(const char *s) {
    unsigned int n = 0;
    while (s[n] != 0) {
        n++;
    }
    return n;
}

/* Turn PATH (absolute or relative to cwd; ".", ".." allowed) into a clean
 * absolute path. Returns 0 if it does not fit. */
int fs_resolve(const char *path, char *out) {
    char tmp[FS_NAME * 2];
    unsigned int t = 0;
    if (path[0] != '/') {
        for (const char *c = cwd; *c != 0 && t + 1 < sizeof tmp; c++) {
            tmp[t++] = *c;
        }
        if (t + 1 < sizeof tmp) {
            tmp[t++] = '/';
        }
    }
    for (const char *c = path; *c != 0; c++) {
        if (t + 1 >= sizeof tmp) {
            return 0;
        }
        tmp[t++] = *c;
    }
    tmp[t] = 0;
    unsigned int o = 0;
    out[o] = 0;
    unsigned int i = 0;
    while (tmp[i] != 0) {
        while (tmp[i] == '/') {
            i++;
        }
        if (tmp[i] == 0) {
            break;
        }
        unsigned int s = i;
        while (tmp[i] != 0 && tmp[i] != '/') {
            i++;
        }
        unsigned int n = i - s;
        if (n == 1 && tmp[s] == '.') {
            continue;
        }
        if (n == 2 && tmp[s] == '.' && tmp[s + 1] == '.') {
            while (o > 0 && out[o - 1] != '/') {
                o--;
            }
            if (o > 0) {
                o--;
            }
            out[o] = 0;
            continue;
        }
        if (n >= FS_PART || o + 1 + n + 1 > FS_NAME) {
            return 0;
        }
        out[o++] = '/';
        for (unsigned int k = 0; k < n; k++) {
            out[o++] = tmp[s + k];
        }
        out[o] = 0;
    }
    if (o == 0) {
        out[0] = '/';
        out[1] = 0;
    }
    return 1;
}

/* Is ABS directly inside directory DIR? */
static int fs_in_dir(const char *abs, const char *dir) {
    unsigned int d = str_len(dir);
    if (d == 1) {
        d = 0; /* root */
    }
    for (unsigned int i = 0; i < d; i++) {
        if (abs[i] != dir[i]) {
            return 0;
        }
    }
    if (abs[d] != '/' || abs[d + 1] == 0) {
        return 0;
    }
    for (const char *c = abs + d + 1; *c != 0; c++) {
        if (*c == '/') {
            return 0;
        }
    }
    return 1;
}

static const char *base_name(const char *abs) {
    const char *b = abs;
    for (const char *c = abs; *c != 0; c++) {
        if (*c == '/') {
            b = c + 1;
        }
    }
    return b;
}

static struct ram_file *fs_find_abs(const char *abs) {
    for (int i = 0; i < FS_MAX; i++) {
        if (ramfs[i].used && str_eq(ramfs[i].name, abs)) {
            return &ramfs[i];
        }
    }
    return 0;
}

static int fs_is_dir_abs(const char *abs) {
    if (abs[0] == '/' && abs[1] == 0) {
        return 1;
    }
    struct ram_file *f = fs_find_abs(abs);
    return f != 0 && f->dir;
}

static int fs_parent_ok(const char *abs) {
    char parent[FS_NAME];
    unsigned int n = 0;
    const char *b = base_name(abs);
    while (abs + n < b - 1 && n + 1 < FS_NAME) {
        parent[n] = abs[n];
        n++;
    }
    if (n == 0) {
        parent[n++] = '/';
    }
    parent[n] = 0;
    return fs_is_dir_abs(parent);
}

/* Files only: a path naming a directory is "no such file". */
static struct ram_file *fs_find(const char *name) {
    char abs[FS_NAME];
    if (name[0] == 0 || !fs_resolve(name, abs)) {
        return 0;
    }
    struct ram_file *f = fs_find_abs(abs);
    return f != 0 && !f->dir ? f : 0;
}

static struct ram_file *fs_free_slot(void) {
    for (int i = 0; i < FS_MAX; i++) {
        if (!ramfs[i].used) {
            return &ramfs[i];
        }
    }
    return 0;
}

static void fs_compact(void) {
    static unsigned char placed[FS_MAX];
    unsigned long cursor = 0;
    for (int i = 0; i < FS_MAX; i++) {
        placed[i] = !(ramfs[i].used && !ramfs[i].dir && ramfs[i].cap > 0);
    }
    for (;;) {
        int best = -1;
        for (int i = 0; i < FS_MAX; i++) {
            if (!placed[i] && (best < 0 || ramfs[i].data < ramfs[best].data)) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        placed[best] = 1;
        mem_move(FS_POOL + cursor, ramfs[best].data, ramfs[best].length);
        ramfs[best].data = FS_POOL + cursor;
        cursor += ramfs[best].cap;
    }
    pool_top = cursor;
}

static unsigned long fs_pool_live(const struct ram_file *skip) {
    unsigned long live = 0;
    for (int i = 0; i < FS_MAX; i++) {
        if (ramfs[i].used && !ramfs[i].dir && &ramfs[i] != skip) {
            live += ramfs[i].cap;
        }
    }
    return live;
}

/* Store a file at an absolute path. Fails (returns 0) when the parent
 * directory is missing, the path is a directory, or there is no room. */
static int fs_put_abs(const char *abs, const char *data, unsigned int length) {
    if (length > FS_DATA || abs[0] != '/' || abs[1] == 0) {
        return 0;
    }
    struct ram_file *slot = fs_find_abs(abs);
    if (slot != 0 && slot->dir) {
        return 0;
    }
    if (slot == 0 && (!fs_parent_ok(abs) || (slot = fs_free_slot()) == 0)) {
        return 0;
    }
    const unsigned char *src = (const unsigned char *)data;
    if (slot->used && length <= slot->cap) {
        mem_move(slot->data, src, length);
    } else {
        unsigned int cap = (length + 15u) & ~15u;
        if (fs_pool_live(slot->used ? slot : 0) + cap > FS_POOL_SIZE) {
            return 0;
        }
        if (src >= FS_POOL && src < FS_POOL + FS_POOL_SIZE) {
            unsigned char *scratch = (unsigned char *)FS_MOVE_ADDR;
            mem_move(scratch, src, length);
            src = scratch;
        }
        if (slot->used) {
            slot->used = 0; /* drop the old bytes; compaction reclaims them */
        }
        if (pool_top + cap > FS_POOL_SIZE) {
            fs_compact();
        }
        slot->data = FS_POOL + pool_top;
        pool_top += cap;
        slot->cap = cap;
        mem_move(slot->data, src, length);
    }
    if (!slot->used) {
        unsigned int n = 0;
        while (abs[n] != 0 && n + 1 < FS_NAME) {
            slot->name[n] = abs[n];
            n++;
        }
        slot->name[n] = 0;
        slot->nblk = 0;
    }
    slot->length = length;
    slot->used = 1;
    slot->dir = 0;
    slot->dirty = 1;
    return 1;
}

static int fs_add(const char *name, const char *data, unsigned int length) {
    char abs[FS_NAME];
    if (name[0] == 0 || !fs_resolve(name, abs)) {
        return 0;
    }
    return fs_put_abs(abs, data, length);
}

static int fs_mkdir_abs(const char *abs) {
    if (fs_find_abs(abs) != 0 || (abs[0] == '/' && abs[1] == 0)) {
        return fs_is_dir_abs(abs) ? 2 : 0;
    }
    struct ram_file *slot;
    if (!fs_parent_ok(abs) || (slot = fs_free_slot()) == 0) {
        return 0;
    }
    unsigned int n = 0;
    while (abs[n] != 0 && n + 1 < FS_NAME) {
        slot->name[n] = abs[n];
        n++;
    }
    slot->name[n] = 0;
    slot->used = 1;
    slot->dir = 1;
    slot->length = 0;
    slot->cap = 0;
    slot->data = 0;
    slot->dirty = 0;
    slot->nblk = 0;
    return 1;
}

static int fs_dir_empty(const char *abs) {
    for (int i = 0; i < FS_MAX; i++) {
        if (ramfs[i].used && fs_in_dir(ramfs[i].name, abs)) {
            return 0;
        }
    }
    return 1;
}

/* ---- disk.c interface ---- */
void fs_clear_all(void) {
    for (int i = 0; i < FS_MAX; i++) {
        ramfs[i].used = 0;
        ramfs[i].length = 0;
        ramfs[i].cap = 0;
        ramfs[i].name[0] = 0;
    }
    pool_top = 0;
}

/* Load one catalog entry as-is (already on disk, so clean). */
int fs_load_entry(const char *abs, int is_dir, const unsigned char *data, unsigned int length,
                  unsigned int blk, unsigned int nblk) {
    struct ram_file *slot = fs_find_abs(abs);
    if (slot == 0 && (slot = fs_free_slot()) == 0) {
        return 0;
    }
    if (is_dir) {
        if (slot->used) {
            return slot->dir;
        }
        unsigned int n = 0;
        while (abs[n] != 0 && n + 1 < FS_NAME) {
            slot->name[n] = abs[n];
            n++;
        }
        slot->name[n] = 0;
        slot->used = 1;
        slot->dir = 1;
        slot->length = 0;
        slot->cap = 0;
        slot->data = 0;
        slot->dirty = 0;
        slot->nblk = 0;
        return 1;
    }
    unsigned int cap = (length + 15u) & ~15u;
    if (slot->used || pool_top + cap > FS_POOL_SIZE) {
        return 0;
    }
    slot->data = FS_POOL + pool_top;
    pool_top += cap;
    slot->cap = cap;
    mem_move(slot->data, data, length);
    unsigned int n = 0;
    while (abs[n] != 0 && n + 1 < FS_NAME) {
        slot->name[n] = abs[n];
        n++;
    }
    slot->name[n] = 0;
    slot->length = length;
    slot->used = 1;
    slot->dir = 0;
    slot->dirty = 0;
    slot->blk = blk;
    slot->nblk = nblk;
    return 1;
}

int fs_slot(int i, const char **name, const unsigned char **data, unsigned int *length, int *is_dir,
            int *dirty, unsigned int *blk, unsigned int *nblk) {
    if (i < 0 || i >= FS_MAX || !ramfs[i].used) {
        return 0;
    }
    *name = ramfs[i].name;
    *data = ramfs[i].data;
    *length = ramfs[i].length;
    *is_dir = ramfs[i].dir;
    *dirty = ramfs[i].dirty;
    *blk = ramfs[i].blk;
    *nblk = ramfs[i].nblk;
    return 1;
}

void fs_slot_saved(int i, unsigned int blk, unsigned int nblk) {
    ramfs[i].blk = blk;
    ramfs[i].nblk = nblk;
    ramfs[i].dirty = 0;
}

/* After loading an older disk every file is new to this format. */
void fs_mark_all_dirty(void) {
    for (int i = 0; i < FS_MAX; i++) {
        if (ramfs[i].used && !ramfs[i].dir) {
            ramfs[i].dirty = 1;
        }
    }
}

extern const unsigned char hello_bin[];
extern const unsigned char hello_bin_end[];
extern const unsigned char ticker_bin[];
extern const unsigned char ticker_bin_end[];
extern const unsigned char pulse_bin[];
extern const unsigned char pulse_bin_end[];
extern const unsigned char crashy_bin[];
extern const unsigned char crashy_bin_end[];
extern const unsigned char open_bin[];
extern const unsigned char open_bin_end[];
extern const unsigned char vtxt_bin[];
extern const unsigned char vtxt_bin_end[];
extern const unsigned char vdocx_bin[];
extern const unsigned char vdocx_bin_end[];
extern const unsigned char vimg_bin[];
extern const unsigned char vimg_bin_end[];
extern const unsigned char vpdf_bin[];
extern const unsigned char vpdf_bin_end[];
extern const unsigned char vmp4_bin[];
extern const unsigned char vmp4_bin_end[];
extern const unsigned char sample_note_txt[];
extern const unsigned char sample_note_txt_end[];
extern const unsigned char sample_note_docx[];
extern const unsigned char sample_note_docx_end[];
extern const unsigned char sample_pic_png[];
extern const unsigned char sample_pic_png_end[];
extern const unsigned char sample_pic_jpg[];
extern const unsigned char sample_pic_jpg_end[];
extern const unsigned char sample_note_pdf[];
extern const unsigned char sample_note_pdf_end[];
extern const unsigned char sample_clip_mp4[];
extern const unsigned char sample_clip_mp4_end[];

extern const char cc_sample_hello[];
extern const char cc_sample_fib[];
extern const unsigned int cc_sample_hello_len;
extern const unsigned int cc_sample_fib_len;
int cc_run(const char *source, const char *name);

/* Set while refreshing an existing disk: add built-in files only where missing. */
static int seed_only_missing;

static void fs_seed_blob(const char *abs, const unsigned char *start, const unsigned char *end) {
    if (seed_only_missing && fs_find_abs(abs)) {
        return;
    }
    fs_put_abs(abs, (const char *)start, (unsigned int)(end - start));
}

/* Layout: /bin programs, /lib pkg libraries, /docs samples, /home your files. */
static void fs_seed(void) {
    fs_mkdir_abs("/bin");
    fs_mkdir_abs("/lib");
    fs_mkdir_abs("/docs");
    fs_mkdir_abs("/home");
    if (!seed_only_missing || !fs_find_abs("/home/readme")) {
        fs_put_abs("/home/readme", "welcome to genesis", 18);
    }
    fs_seed_blob("/bin/hello", hello_bin, hello_bin_end);
    fs_seed_blob("/bin/open", open_bin, open_bin_end);
    fs_seed_blob("/bin/vtxt", vtxt_bin, vtxt_bin_end);
    fs_seed_blob("/bin/vdocx", vdocx_bin, vdocx_bin_end);
    fs_seed_blob("/bin/vimg", vimg_bin, vimg_bin_end);
    fs_seed_blob("/bin/vpdf", vpdf_bin, vpdf_bin_end);
    fs_seed_blob("/bin/vmp4", vmp4_bin, vmp4_bin_end);
    fs_seed_blob("/bin/ticker", ticker_bin, ticker_bin_end);
    fs_seed_blob("/bin/pulse", pulse_bin, pulse_bin_end);
    fs_seed_blob("/bin/crashy", crashy_bin, crashy_bin_end);
    fs_seed_blob("/docs/note.txt", sample_note_txt, sample_note_txt_end);
    fs_seed_blob("/docs/note.docx", sample_note_docx, sample_note_docx_end);
    fs_seed_blob("/docs/pic.png", sample_pic_png, sample_pic_png_end);
    fs_seed_blob("/docs/pic.jpg", sample_pic_jpg, sample_pic_jpg_end);
    fs_seed_blob("/docs/note.pdf", sample_note_pdf, sample_note_pdf_end);
    fs_seed_blob("/docs/clip.mp4", sample_clip_mp4, sample_clip_mp4_end);
    fs_seed_blob("/docs/hello.c", (const unsigned char *)cc_sample_hello,
                 (const unsigned char *)cc_sample_hello + cc_sample_hello_len);
    fs_seed_blob("/docs/fib.c", (const unsigned char *)cc_sample_fib,
                 (const unsigned char *)cc_sample_fib + cc_sample_fib_len);
}

/* Built-in names an old flat (GBF1) disk may hold; they are re-seeded. */
int fs_builtin_name(const char *name) {
    static const char *const names[] = {"readme", "hello", "open", "vtxt", "vdocx", "vimg", "vpdf",
                                        "vmp4", "ticker", "pulse", "crashy", "note.txt", "note.docx", "pic.png",
                                        "pic.jpg", "note.pdf", "clip.mp4"};
    for (unsigned int i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (str_eq(name, names[i])) {
            return 1;
        }
    }
    return 0;
}

int fs_lookup(const char *name, const unsigned char **data, unsigned int *length) {
    struct ram_file *file = fs_find(name);
    if (file == 0) {
        return 0;
    }
    *data = file->data;
    *length = file->length;
    return 1;
}

/* Desktop browser: entry i if it sits directly in DIR. */
int fs_dir_entry(int index, const char *dir, char *name, unsigned int name_cap, int *is_dir) {
    if (index < 0 || index >= FS_MAX || !ramfs[index].used || name_cap == 0 ||
        !fs_in_dir(ramfs[index].name, dir)) {
        return 0;
    }
    const char *b = base_name(ramfs[index].name);
    unsigned int n = 0;
    while (b[n] != 0 && n + 1 < name_cap) {
        name[n] = b[n];
        n++;
    }
    name[n] = 0;
    *is_dir = ramfs[index].dir;
    return 1;
}

int fs_store_readme(const char *text) {
    if (!fs_put_abs("/home/readme", text, str_len(text))) {
        return 0;
    }
    return disk_save();
}

void preview_readme(char *dest, unsigned int cap) {
    struct ram_file *file = fs_find_abs("/home/readme");
    unsigned int n = 0;
    if (cap == 0) {
        return;
    }
    if (file != 0) {
        while (n + 1 < cap && n < file->length) {
            dest[n] = file->data[n];
            n++;
        }
    }
    dest[n] = 0;
}

static void cmd_ls(const char *arg) {
    char abs[FS_NAME];
    if (!fs_resolve(arg[0] ? arg : ".", abs)) {
        console_puts("ls: bad path\n");
        return;
    }
    if (!fs_is_dir_abs(abs)) {
        if (fs_find_abs(abs)) {
            console_puts("ls: ");
            console_puts(base_name(abs));
            console_putc('\n');
        } else {
            console_puts("ls: no such directory\n");
        }
        return;
    }
    console_puts("ls:");
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < FS_MAX; i++) {
            if (!ramfs[i].used || ramfs[i].dir != (pass == 0) || !fs_in_dir(ramfs[i].name, abs) ||
                base_name(ramfs[i].name)[0] == '.') {
                continue; /* dot files (.journal, .pins) stay hidden */
            }
            console_putc(' ');
            console_puts(base_name(ramfs[i].name));
            if (ramfs[i].dir) {
                console_putc('/');
            }
        }
    }
    console_putc('\n');
}

/* cc FILE.c: compile with the built-in C compiler (kernel/cc.c) and run. */
static char cc_source[64 * 1024];

static int ends_with_c(const char *line) {
    unsigned int n = 0;
    while (line[n] != 0) {
        n++;
    }
    while (n > 0 && line[n - 1] == ' ') {
        n--;
    }
    return n > 2 && line[n - 2] == '.' && line[n - 1] == 'c';
}

static void cmd_cc(const char *name) {
    while (*name == ' ') {
        name++;
    }
    if (*name == 0) {
        console_puts("cc: usage: cc FILE.c   (try cc /docs/hello.c)\n");
        return;
    }
    struct ram_file *file = fs_find(name);
    if (file == 0) {
        console_puts("cc: no such file\n");
        return;
    }
    if (file->length >= sizeof cc_source) {
        console_puts("cc: file too big (64 KiB max)\n");
        return;
    }
    for (unsigned int i = 0; i < file->length; i++) {
        cc_source[i] = file->data[i];
    }
    cc_source[file->length] = 0;
    int rc = cc_run(cc_source, name);
    if (rc > 0) { /* -1: the error is already printed */
        console_puts("cc: exit ");
        char digits[12];
        int n = 0;
        do {
            digits[n++] = (char)('0' + rc % 10);
            rc /= 10;
        } while (rc != 0);
        while (n > 0) {
            console_putc(digits[--n]);
        }
        console_putc('\n');
    }
}

static void cmd_cat(const char *name) {
    struct ram_file *file = fs_find(name);
    if (file == 0) {
        console_puts("cat: no such file\n");
        return;
    }
    console_puts("cat: ");
    for (unsigned int i = 0; i < file->length; i++) {
        console_putc(file->data[i]);
    }
    console_putc('\n');
}

/* Scratch buffer for appends (>>) and cp, one Grid Store file in size. */
static char *const edit_buf = (char *)FS_EDIT_ADDR;

static void prompt(void);

int fs_get(const char *name, const unsigned char **data, unsigned int *length) {
    return fs_lookup(name, data, length);
}

int fs_put(const char *name, const char *data, unsigned int length) {
    return fs_add(name, data, length) && disk_save();
}

void desktop_editor_closed(void);

void editor_closed(void) {
    if (desktop_active()) {
        desktop_editor_closed();
        return;
    }
    vga_show_cursor();
    vga_clear();
    type_row = 0;
    type_col = 0;
    prompt();
}

static void cmd_edit(const char *name) {
    editor_open(name);
}

static void cmd_write(const char *args) {
    char name[FS_NAME * 2];
    unsigned int i = 0;
    while (args[i] != 0 && args[i] != ' ' && i + 1 < sizeof name) {
        name[i] = args[i];
        i++;
    }
    name[i] = 0;
    if (name[0] == 0 || args[i] != ' ') {
        console_puts("write: need a name and text\n");
        return;
    }
    const char *text = args + i + 1;
    if (!fs_add(name, text, str_len(text))) {
        console_puts("write: full, or no such folder\n");
        return;
    }
    if (!disk_save()) {
        console_puts("disk: io failed\n");
    }
    console_puts("write: ");
    console_puts(name);
    console_putc('\n');
}

static int fs_remove_abs(const char *abs) {
    struct ram_file *f = fs_find_abs(abs);
    if (f == 0) {
        return 0;
    }
    f->used = 0;
    f->length = 0;
    f->cap = 0;
    f->name[0] = 0;
    return 1;
}

int fs_delete(const char *name) {
    struct ram_file *f = fs_find(name);
    if (f == 0) {
        return 0;
    }
    fs_remove_abs(f->name);
    disk_save();
    return 1;
}

int fs_free_slots(void) {
    int n = 0;
    for (int i = 0; i < FS_MAX; i++) {
        if (!ramfs[i].used) {
            n++;
        }
    }
    return n;
}

static void cmd_rm(const char *name) {
    struct ram_file *f = name[0] ? fs_find(name) : 0;
    if (f == 0) {
        console_puts("rm: no such file\n");
        return;
    }
    fs_remove_abs(f->name);
    disk_save();
    console_puts("rm: ");
    console_puts(name);
    console_putc('\n');
}

static void cmd_mkdir(const char *name) {
    char abs[FS_NAME];
    if (name[0] == 0 || !fs_resolve(name, abs)) {
        console_puts("mkdir: bad name\n");
        return;
    }
    int r = fs_mkdir_abs(abs);
    if (r == 2 || (r == 0 && fs_find_abs(abs))) {
        console_puts("mkdir: already exists\n");
        return;
    }
    if (r == 0) {
        console_puts("mkdir: no parent folder or full\n");
        return;
    }
    disk_save();
    console_puts("mkdir: ");
    console_puts(abs);
    console_putc('\n');
}

static void cmd_rmdir(const char *name) {
    char abs[FS_NAME];
    struct ram_file *f = 0;
    if (name[0] != 0 && fs_resolve(name, abs)) {
        f = fs_find_abs(abs);
    }
    if (f == 0 || !f->dir) {
        console_puts("rmdir: no such folder\n");
        return;
    }
    if (!fs_dir_empty(abs)) {
        console_puts("rmdir: not empty\n");
        return;
    }
    if (str_eq(abs, cwd)) {
        console_puts("rmdir: it is the current folder\n");
        return;
    }
    fs_remove_abs(abs);
    disk_save();
    console_puts("rmdir: ");
    console_puts(abs);
    console_putc('\n');
}

static void cmd_cd(const char *name) {
    char abs[FS_NAME];
    if (!fs_resolve(name[0] ? name : "/home", abs) || !fs_is_dir_abs(abs)) {
        console_puts("cd: no such folder\n");
        return;
    }
    for (unsigned int i = 0; i < FS_NAME; i++) {
        cwd[i] = abs[i];
        if (abs[i] == 0) {
            break;
        }
    }
}

/* Destination for mv/cp: into a folder keeps the source's name. */
static int fs_dest(const char *src_abs, const char *dst, char *out) {
    if (!fs_resolve(dst, out)) {
        return 0;
    }
    if (fs_is_dir_abs(out)) {
        const char *b = base_name(src_abs);
        unsigned int n = str_len(out);
        if (n == 1) {
            n = 0;
        }
        if (n + 1 + str_len(b) + 1 > FS_NAME) {
            return 0;
        }
        out[n++] = '/';
        for (unsigned int k = 0; b[k] != 0; k++) {
            out[n++] = b[k];
        }
        out[n] = 0;
    }
    return 1;
}

static void cmd_mv_cp(const char *args, int copy) {
    const char *who = copy ? "cp" : "mv";
    char a[LINE_MAX];
    unsigned int n = 0;
    while (args[n] != 0 && args[n] != ' ' && n + 1 < LINE_MAX) {
        a[n] = args[n];
        n++;
    }
    a[n] = 0;
    const char *b = args[n] == ' ' ? args + n + 1 : "";
    while (*b == ' ') {
        b++;
    }
    struct ram_file *f = a[0] ? fs_find(a) : 0;
    char dst[FS_NAME];
    if (f == 0 || b[0] == 0) {
        console_puts(who);
        console_puts(f == 0 ? ": no such file\n" : ": need a destination\n");
        return;
    }
    if (!fs_dest(f->name, b, dst) || str_eq(dst, f->name)) {
        console_puts(who);
        console_puts(": bad destination\n");
        return;
    }
    char src[FS_NAME];
    for (unsigned int i = 0; i < FS_NAME; i++) {
        src[i] = f->name[i];
    }
    unsigned int len = f->length;
    mem_move((unsigned char *)edit_buf, f->data, len);
    if (!fs_put_abs(dst, edit_buf, len)) {
        console_puts(who);
        console_puts(": cannot write (no folder or full)\n");
        return;
    }
    if (!copy) {
        fs_remove_abs(src);
    }
    if (!disk_save()) {
        console_puts("disk: io failed\n");
    }
    console_puts(who);
    console_puts(": ");
    console_puts(dst);
    console_putc('\n');
}

/* `run NAME`: a path, or a bare name found in the cwd, then /bin (PATH). */
static struct ram_file *fs_find_prog(const char *name) {
    struct ram_file *f = fs_find(name);
    if (f != 0 || name[0] == 0) {
        return f;
    }
    for (const char *c = name; *c != 0; c++) {
        if (*c == '/') {
            return 0;
        }
    }
    char abs[FS_NAME] = "/bin/";
    unsigned int n = 5;
    for (const char *c = name; *c != 0 && n + 1 < FS_NAME; c++) {
        abs[n++] = *c;
    }
    abs[n] = 0;
    f = fs_find_abs(abs);
    return f != 0 && !f->dir ? f : 0;
}

typedef void (*print_fn)(const char *);
typedef void (*sleep_fn)(unsigned int ticks);
typedef void *(*pkg_sym_fn)(const char *lib, const char *sym);
void cmd_pkg(const char *args);
typedef void (*prog_fn)(print_fn print, sleep_fn sleep, pkg_sym_fn sym);

/* background: start the program as a task (`spawn NAME`) and return. */
void desktop_link(const char *from, const char *to);
void journal_add(char type, const char *what);

/* A whole file arrived over Genesis Link: keep it in /home/inbox. */
void link_file_received(const char *name, const unsigned char *data, unsigned int length) {
    char abs[FS_NAME];
    unsigned int n = 0;
    for (const char *c = "/home/inbox/"; *c != 0; c++) {
        abs[n++] = *c;
    }
    for (const char *c = name; *c != 0 && n + 1 < FS_NAME; c++) {
        abs[n++] = *c;
    }
    abs[n] = 0;
    if (!fs_is_dir_abs("/home/inbox")) {
        fs_mkdir_abs("/home/inbox");
    }
    if (!fs_put_abs(abs, (const char *)data, length)) {
        console_puts("inbox: cannot save\n");
        return;
    }
    (void)disk_save();
    console_puts("inbox: ");
    console_puts(abs);
    console_puts(" (");
    print_u32(length);
    console_puts(" bytes)\n");
    journal_add('o', abs);
    desktop_link("peer", "core");
}

static const char *run_prog; /* the foreground program, while it runs */

int task_spawn_user(const char *name, const unsigned char *image, unsigned int len,
                    const char *args, int quiet);
int task_alive(int id);

/* A user task was stopped by a fault: say so now, tell the desktop later
 * (journal + red core) from the shell loop, see task_crash_take. */
static char crash_pending[128];
static volatile int crash_due;

void task_crash_note(int id, const char *name, const char *why) {
    console_puts("\ntask ");
    print_u32((unsigned int)id);
    console_puts(" (");
    console_puts(name);
    console_puts("): ");
    console_puts(why);
    console_puts(" - task stopped, system OK\n");
    unsigned int n = 0;
    for (const char *c = name; *c != 0 && n + 3 < sizeof crash_pending; c++) {
        crash_pending[n++] = *c;
    }
    crash_pending[n++] = ':';
    crash_pending[n++] = ' ';
    for (const char *c = why; *c != 0 && n + 1 < sizeof crash_pending; c++) {
        crash_pending[n++] = *c;
    }
    crash_pending[n] = 0;
    crash_due = 1;
}

/* For the desktop: copies the last crash text once; 0 if none is new. */
int task_crash_take(char *out, unsigned int cap) {
    if (!crash_due) {
        return 0;
    }
    unsigned long flags;
    __asm__ volatile(PUSHF_POP " %0; cli" : "=r"(flags)::"memory");
    unsigned int n = 0;
    while (crash_pending[n] != 0 && n + 1 < cap) {
        out[n] = crash_pending[n];
        n++;
    }
    out[n] = 0;
    crash_due = 0;
    if (flags & 0x200ul) {
        __asm__ volatile("sti" ::: "memory");
    }
    return 1;
}

/* `run NAME [ARGS]` / `spawn NAME [ARGS]`: the program runs in ring 3 in its
 * own address space; a crash stops only that task. `run` waits for it. */
static void cmd_run(const char *line, int background) {
    char name[FS_NAME];
    unsigned int n = 0;
    while (*line == ' ') {
        line++;
    }
    while (line[n] != 0 && line[n] != ' ' && n + 1 < FS_NAME) {
        name[n] = line[n];
        n++;
    }
    name[n] = 0;
    const char *args = line + n;
    while (*args != 0 && *args != ' ') {
        args++;
    }
    while (*args == ' ') {
        args++;
    }
    struct ram_file *file = fs_find_prog(name);
    if (file == 0) {
        console_puts("run: no such file\n");
        return;
    }
    timer_start(); /* the desktop may run a program while still booting */
    int id = task_spawn_user(name, (const unsigned char *)file->data, file->length, args, !background);
    if (id == -2) {
        console_puts("run: no memory\n");
        return;
    }
    if (id < 0) {
        console_puts("spawn: too many tasks\n");
        return;
    }
    if (background) {
        console_puts("spawn: task ");
        print_u32((unsigned int)id);
        console_putc('\n');
        return;
    }
    program_len = 0;
    program_text[0] = 0;
    program_capture = 1;
    run_prog = name;
    desktop_link(name, "core"); /* a running program is tied to the core */
    fg_task = id;
    while (task_alive(id)) {
        __asm__ volatile("hlt");
    }
    fg_task = -1;
    run_prog = 0;
    program_capture = 0;
    console_putc('\n');
}

const char *program_output(void) {
    return program_text;
}

void run_program(const char *name) {
    cmd_run(name, 0);
}

void memory_summary(char *dest, unsigned int cap) {
    const char *prefix = "usable ";
    unsigned int i = 0;
    unsigned int mib = (unsigned int)(usable_bytes >> 20);
    char digits[10];
    int count = 0;
    while (*prefix != 0 && i + 1 < cap) {
        dest[i++] = *prefix++;
    }
    if (mib == 0) {
        if (i + 1 < cap) {
            dest[i++] = '0';
        }
    } else {
        unsigned int value = mib;
        while (value > 0 && count < 10) {
            digits[count++] = (char)('0' + (value % 10u));
            value /= 10u;
        }
        while (count > 0 && i + 1 < cap) {
            dest[i++] = digits[--count];
        }
    }
    const char *suffix = " MiB";
    while (*suffix != 0 && i + 1 < cap) {
        dest[i++] = *suffix++;
    }
    dest[i] = 0;
}

void fs_summary(char *dest, unsigned int cap) {
    unsigned int used = 0;
    dest[0] = 0;
    for (int i = 0; i < FS_MAX; i++) {
        if (!ramfs[i].used || !fs_in_dir(ramfs[i].name, "/home")) {
            continue;
        }
        if (used != 0 && used + 1 < cap) {
            dest[used++] = ' ';
        }
        for (const char *c = base_name(ramfs[i].name); *c != 0 && used + 1 < cap; c++) {
            dest[used++] = *c;
        }
        dest[used] = 0;
    }
}

static void cmd_heap(void) {
    console_puts("heap: used ");
    print_u32(heap_used);
    console_puts(" left ");
    print_u32(HEAP_SIZE - heap_used);
    console_putc('\n');
}

int task_kill(int id);
int task_count(void);

static void cmd_kill(const char *arg) {
    unsigned int id = 0;
    while (*arg == ' ') {
        arg++;
    }
    if (!parse_u32(arg, &id)) {
        console_puts("kill: need a task number (see ps)\n");
        return;
    }
    int r = task_kill((int)id);
    if (r < 0) {
        console_puts("kill: the shell cannot be stopped\n");
        return;
    }
    console_puts(r ? "kill: stopped task " : "kill: no task ");
    print_u32(id);
    console_putc('\n');
}

/* Pinned folders: one absolute path per line in /home/.pins. */
#define PINS_FILE "/home/.pins"
enum { PINS_MAX = 5 };

int pins_get(int index, char *out, unsigned int cap) {
    struct ram_file *f = fs_find_abs(PINS_FILE);
    if (f == 0) {
        return 0;
    }
    int line_no = 0;
    unsigned int i = 0;
    while (i < f->length) {
        unsigned int s = i;
        while (i < f->length && f->data[i] != '\n') {
            i++;
        }
        if (i > s && line_no++ == index) {
            unsigned int n = 0;
            while (s + n < i && n + 1 < cap) {
                out[n] = (char)f->data[s + n];
                n++;
            }
            out[n] = 0;
            return 1;
        }
        i++;
    }
    return 0;
}

static void cmd_pin(const char *arg, int add) {
    char abs[FS_NAME];
    while (*arg == ' ') {
        arg++;
    }
    if (*arg == 0 || !fs_resolve(arg, abs)) {
        console_puts(add ? "pin: need a folder\n" : "unpin: need a folder\n");
        return;
    }
    char text[PINS_MAX * FS_NAME + PINS_MAX];
    unsigned int n = 0;
    int found = 0;
    int count = 0;
    char one[FS_NAME];
    for (int i = 0; pins_get(i, one, sizeof one); i++) {
        if (str_eq(one, abs)) {
            found = 1;
            if (!add) {
                continue;
            }
        }
        for (const char *c = one; *c != 0; c++) {
            text[n++] = *c;
        }
        text[n++] = '\n';
        count++;
    }
    if (add) {
        if (found) {
            console_puts("pin: already pinned\n");
            return;
        }
        if (!fs_is_dir_abs(abs)) {
            console_puts("pin: not a folder\n");
            return;
        }
        if (count >= PINS_MAX) {
            console_puts("pin: at most 5 pins\n");
            return;
        }
        for (const char *c = abs; *c != 0; c++) {
            text[n++] = *c;
        }
        text[n++] = '\n';
    } else if (!found) {
        console_puts("unpin: not pinned\n");
        return;
    }
    if (!fs_put_abs(PINS_FILE, text, n)) {
        console_puts("pin: cannot write " PINS_FILE "\n");
        return;
    }
    if (!disk_save()) {
        console_puts("disk: io failed\n");
    }
    console_puts(add ? "pin: " : "unpin: ");
    console_puts(abs);
    console_putc('\n');
}

/* ---- Journal: every user action, for the «Лента» timeline ----
 * /home/.journal holds one action per line: "HH:MM:SS<TAB>T<TAB>text",
 * newest last, capped at JOURNAL_MAX lines. T is the action type:
 * c command, o file opened, e file edited, s file saved, i pkg install,
 * r pkg remove, p program run, k program stopped. */
#define JOURNAL_FILE "/home/.journal"
enum { JOURNAL_MAX = 40, JOURNAL_TEXT = 72 };
static char j_time[JOURNAL_MAX][9];
static char j_type[JOURNAL_MAX];
static char j_text[JOURNAL_MAX][JOURNAL_TEXT];
static int j_count = -1; /* -1: not loaded from disk yet */
static int journal_quiet;  /* replaying a script: only the top line counts */

void clock_now(char *hhmmss);

static void journal_load(void) {
    j_count = 0;
    struct ram_file *f = fs_find_abs(JOURNAL_FILE);
    if (f == 0) {
        return;
    }
    unsigned int i = 0;
    while (i < f->length && j_count < JOURNAL_MAX) {
        unsigned int s = i;
        while (i < f->length && f->data[i] != '\n') {
            i++;
        }
        /* HH:MM:SS \t T \t text */
        if (i - s >= 12 && f->data[s + 8] == '\t' && f->data[s + 10] == '\t') {
            for (int k = 0; k < 8; k++) {
                j_time[j_count][k] = (char)f->data[s + k];
            }
            j_time[j_count][8] = 0;
            j_type[j_count] = (char)f->data[s + 9];
            unsigned int n = 0;
            for (unsigned int k = s + 11; k < i && n + 1 < JOURNAL_TEXT; k++) {
                j_text[j_count][n++] = (char)f->data[k];
            }
            j_text[j_count][n] = 0;
            j_count++;
        }
        i++;
    }
}

static void journal_save(void) {
    static char text[JOURNAL_MAX * (JOURNAL_TEXT + 12)];
    unsigned int n = 0;
    for (int e = 0; e < j_count; e++) {
        for (int k = 0; k < 8; k++) {
            text[n++] = j_time[e][k];
        }
        text[n++] = '\t';
        text[n++] = j_type[e];
        text[n++] = '\t';
        for (const char *c = j_text[e]; *c != 0; c++) {
            text[n++] = *c;
        }
        text[n++] = '\n';
    }
    /* The journal never takes the last free catalog slots from user files:
     * with few left it stays in memory until there is room again. */
    if (fs_find_abs(JOURNAL_FILE) == 0 && fs_free_slots() <= 8) {
        return;
    }
    if (fs_put_abs(JOURNAL_FILE, text, n)) {
        (void)disk_save();
    }
}

void journal_add(char type, const char *what) {
    if (journal_quiet || what == 0 || what[0] == 0) {
        return;
    }
    if (j_count < 0) {
        journal_load();
    }
    if (j_count == JOURNAL_MAX) {
        for (int e = 1; e < JOURNAL_MAX; e++) {
            for (int k = 0; k < 9; k++) {
                j_time[e - 1][k] = j_time[e][k];
            }
            j_type[e - 1] = j_type[e];
            for (int k = 0; k < JOURNAL_TEXT; k++) {
                j_text[e - 1][k] = j_text[e][k];
            }
        }
        j_count--;
    }
    clock_now(j_time[j_count]);
    j_type[j_count] = type;
    unsigned int n = 0;
    while (what[n] != 0 && n + 1 < JOURNAL_TEXT) {
        j_text[j_count][n] = what[n] == '\t' || what[n] == '\n' ? ' ' : what[n];
        n++;
    }
    while (n > 0 && utf8_is_cont((unsigned char)what[n])) {
        n--; /* never cut a UTF-8 character */
    }
    j_text[j_count][n] = 0;
    j_count++;
    journal_save();
    /* Serial only: the screen and the desktop's captured output stay clean. */
    char head[4] = {(char)type, ' ', 0, 0};
    serial_puts("journal: ");
    serial_puts(head);
    serial_puts(j_text[j_count - 1]);
    serial_puts("\n");
}

/* A typed command line: the type follows from its first word. */
static void journal_command(const char *line) {
    while (*line == ' ') {
        line++;
    }
    char type = 'c';
    if (starts_with(line, "pkg install ")) {
        type = 'i';
    } else if (starts_with(line, "pkg remove ")) {
        type = 'r';
    } else if (starts_with(line, "run ") || starts_with(line, "spawn ")) {
        type = 'p';
    } else if (starts_with(line, "kill ")) {
        type = 'k';
    } else if (starts_with(line, "edit ")) {
        type = 'e';
    } else if (starts_with(line, "cat ")) {
        type = 'o';
    }
    journal_add(type, line);
}

int journal_count(void) {
    if (j_count < 0) {
        journal_load();
    }
    return j_count;
}

/* Entry i (0 = oldest): time, type and text. */
int journal_get(int i, char *time, char *type, char *text, unsigned int cap) {
    if (i < 0 || i >= journal_count()) {
        return 0;
    }
    for (int k = 0; k < 9; k++) {
        time[k] = j_time[i][k];
    }
    *type = j_type[i];
    unsigned int n = 0;
    while (j_text[i][n] != 0 && n + 1 < cap) {
        text[n] = j_text[i][n];
        n++;
    }
    text[n] = 0;
    return 1;
}

extern unsigned char kernel_load_start[];
extern unsigned char kernel_load_end[];

/* Memory in use, in bytes: the loaded kernel (code, data, bss with the
 * framebuffer back-buffers and task stacks) plus the heap. */
unsigned long mem_used_bytes(void) {
    return (unsigned long)(kernel_load_end - kernel_load_start) + heap_used +
           (unsigned long)user_pages_used() * 4096ul;
}

/* Used share of the usable RAM from the Multiboot map, rounded up so that
 * any use shows at least 1%. Falls back to the heap alone without a map. */
int heap_percent(void) {
    unsigned long total = usable_bytes;
    unsigned long used = mem_used_bytes();
    if (total == 0) {
        total = HEAP_SIZE;
        used = heap_used;
    }
    return (int)((used * 100ul + total - 1) / total);
}

unsigned int heap_used_kib(void) {
    return heap_used / 1024u;
}

int fs_file_count(void) {
    int n = 0;
    for (int i = 0; i < FS_MAX; i++) {
        if (ramfs[i].used && !ramfs[i].dir) {
            n++;
        }
    }
    return n;
}

const char *shell_cwd(void) {
    return cwd;
}

void kbd_toggle_layout(void) {
    layout_toggle();
}

/* ---- Tab completion: commands for the first word, paths after it ---- */
static const char *const sh_words[] = {
    "help", "echo", "clear", "mem", "alloc", "heap", "ls", "cat", "write", "rm", "run", "spawn",
    "ps", "kill", "desktop", "pwd", "cd", "mkdir", "rmdir", "mv", "cp", "edit", "pkg", "pin",
    "unpin", "poweroff", "reboot", "ping", "say", "send", "sh", "set", "grep", "wc", "head", "tz", "cc", "net", "ifconfig", "dhcp", "dns", "http", "nc", "udp",
};

static unsigned int cand_used;

static void cand_add(char *cands, unsigned int cap, const char *word, int dir) {
    if (cand_used != 0 && cand_used + 1 < cap) {
        cands[cand_used++] = ' ';
    }
    for (const char *c = word; *c != 0 && cand_used + 1 < cap; c++) {
        cands[cand_used++] = *c;
    }
    if (dir && cand_used + 1 < cap) {
        cands[cand_used++] = '/';
    }
    cands[cand_used] = 0;
}

/* Completes the last word of TEXT (LEN bytes, CAP room) in place. Returns how
 * many names matched; CANDS gets them, space separated, when there are several. */
int sh_complete(char *text, int *len, int cap, char *cands, unsigned int cands_cap) {
    int n = *len;
    int start = n;
    while (start > 0 && text[start - 1] != ' ' && text[start - 1] != '|' && text[start - 1] != ';' &&
           text[start - 1] != '&' && text[start - 1] != '>') {
        start--;
    }
    int first = 1;
    for (int k = start - 1; k >= 0; k--) {
        char c = text[k];
        if (c == '|' || c == ';' || c == '&') {
            break;
        }
        if (c != ' ') {
            first = 0;
            break;
        }
    }
    const char *word = text + start;
    int wlen = n - start;
    char best[FS_NAME];
    int best_len = -1;
    int best_dir = 0;
    int matches = 0;
    cand_used = 0;
    cands[0] = 0;
    int slash = -1;
    for (int k = 0; k < wlen; k++) {
        if (word[k] == '/') {
            slash = k;
        }
    }
    const char *prefix = word + slash + 1;
    int plen = wlen - slash - 1;
    if (first && slash < 0) {
        for (unsigned int w = 0; w < sizeof sh_words / sizeof sh_words[0]; w++) {
            const char *cand = sh_words[w];
            int k = 0;
            while (k < plen && cand[k] == prefix[k]) {
                k++;
            }
            if (k < plen) {
                continue;
            }
            matches++;
            cand_add(cands, cands_cap, cand, 0);
            if (best_len < 0) {
                best_len = (int)str_len(cand);
                for (int j = 0; j <= best_len; j++) {
                    best[j] = cand[j];
                }
            } else {
                int j = 0;
                while (j < best_len && best[j] == cand[j]) {
                    j++;
                }
                best_len = j;
            }
        }
    } else {
        char dir_text[FS_NAME];
        char abs[FS_NAME];
        int d = 0;
        if (slash == 0) {
            dir_text[d++] = '/';
        }
        for (int k = 0; k < slash && d + 1 < FS_NAME; k++) {
            dir_text[d++] = word[k];
        }
        dir_text[d] = 0;
        if (!fs_resolve(d == 0 ? "." : dir_text, abs)) {
            return 0;
        }
        for (int i = 0; i < FS_MAX; i++) {
            char name[FS_PART + 1];
            int is_dir = 0;
            if (!fs_dir_entry(i, abs, name, sizeof name, &is_dir)) {
                continue;
            }
            int k = 0;
            while (k < plen && name[k] == prefix[k]) {
                k++;
            }
            if (k < plen || (name[0] == '.' && plen == 0)) {
                continue;
            }
            matches++;
            cand_add(cands, cands_cap, name, is_dir);
            if (best_len < 0) {
                best_len = (int)str_len(name);
                for (int j = 0; j <= best_len; j++) {
                    best[j] = name[j];
                }
                best_dir = is_dir;
            } else {
                int j = 0;
                while (j < best_len && best[j] == name[j]) {
                    j++;
                }
                best_len = j;
            }
        }
    }
    if (matches == 0) {
        return 0;
    }
    for (int j = plen; j < best_len && n + 2 < cap; j++) {
        text[n++] = best[j];
    }
    if (matches == 1 && n + 2 < cap) {
        text[n++] = best_dir ? '/' : ' ';
    }
    text[n] = 0;
    *len = n;
    if (matches == 1) {
        cands[0] = 0;
    }
    return matches;
}

static void execute(char *input) {
    trim(input);
    if (input[0] == 0) {
        return;
    }
    if (str_eq(input, "help")) {
        console_puts("commands: help echo clear mem alloc heap ls cat write rm run spawn ps desktop\n");
        console_puts("folders: pwd cd mkdir rmdir mv cp  paths: /docs/note.txt ../x ./y\n");
        console_puts("shell: sh set grep wc head  NAME=v $NAME \"..\" '..' | > >> ; & #\n");
        console_puts("packages: pkg list | installed | install NAME | remove NAME\n");
        console_puts("tasks: spawn NAME, ps, kill N  desktop: pin DIR, unpin DIR  Tab completes\n");
        console_puts("net: net, dhcp, ping HOST, dns NAME, http URL, nc HOST PORT [TEXT], udp HOST PORT TEXT\n");
        console_puts("C: cc FILE.c or run FILE.c  (cc /docs/hello.c, cc /docs/fib.c)\n");
        return;
    }
    if (str_eq(input, "clear")) {
        vga_clear();
        type_row = 0;
        type_col = 0;
        return;
    }
    if (str_eq(input, "echo") || starts_with(input, "echo ")) {
        console_puts(input[4] == ' ' ? input + 5 : "");
        console_putc('\n');
        return;
    }
    if (str_eq(input, "mem")) {
        cmd_mem();
        return;
    }
    if (str_eq(input, "heap")) {
        cmd_heap();
        return;
    }
    if (str_eq(input, "alloc") || starts_with(input, "alloc ")) {
        cmd_alloc(input[5] == ' ' ? input + 6 : "");
        return;
    }
    if (str_eq(input, "ls") || starts_with(input, "ls ")) {
        cmd_ls(input[2] == ' ' ? input + 3 : "");
        return;
    }
    if (str_eq(input, "pwd")) {
        console_puts(cwd);
        console_putc('\n');
        return;
    }
    if (str_eq(input, "cd") || starts_with(input, "cd ")) {
        cmd_cd(input[2] == ' ' ? input + 3 : "");
        return;
    }
    if (str_eq(input, "mkdir") || starts_with(input, "mkdir ")) {
        cmd_mkdir(input[5] == ' ' ? input + 6 : "");
        return;
    }
    if (str_eq(input, "rmdir") || starts_with(input, "rmdir ")) {
        cmd_rmdir(input[5] == ' ' ? input + 6 : "");
        return;
    }
    if (str_eq(input, "mv") || starts_with(input, "mv ")) {
        cmd_mv_cp(input[2] == ' ' ? input + 3 : "", 0);
        return;
    }
    if (str_eq(input, "cp") || starts_with(input, "cp ")) {
        cmd_mv_cp(input[2] == ' ' ? input + 3 : "", 1);
        return;
    }
    if (str_eq(input, "cat") || starts_with(input, "cat ")) {
        cmd_cat(input[3] == ' ' ? input + 4 : "");
        return;
    }
    if (str_eq(input, "write") || starts_with(input, "write ")) {
        cmd_write(input[5] == ' ' ? input + 6 : "");
        return;
    }
    if (str_eq(input, "rm") || starts_with(input, "rm ")) {
        cmd_rm(input[2] == ' ' ? input + 3 : "");
        return;
    }
    if (str_eq(input, "edit") || starts_with(input, "edit ")) {
        cmd_edit(input[4] == ' ' ? input + 5 : "");
        return;
    }
    if (str_eq(input, "cc") || starts_with(input, "cc ")) {
        cmd_cc(input[2] == ' ' ? input + 3 : "");
        return;
    }
    if (starts_with(input, "run ") && ends_with_c(input)) {
        cmd_cc(input + 4);
        return;
    }
    if (str_eq(input, "run") || starts_with(input, "run ")) {
        cmd_run(input[3] == ' ' ? input + 4 : "", 0);
        return;
    }
    if (str_eq(input, "spawn") || starts_with(input, "spawn ")) {
        cmd_run(input[5] == ' ' ? input + 6 : "", 1);
        return;
    }
    if (str_eq(input, "pkg") || starts_with(input, "pkg ")) {
        cmd_pkg(input[3] == ' ' ? input + 4 : "");
        return;
    }
    if (str_eq(input, "tz") || starts_with(input, "tz ")) {
        char out[48];
        clock_tz(input[2] == ' ' ? input + 3 : "", out, sizeof out);
        console_puts(out);
        console_putc('\n');
        return;
    }
    if (str_eq(input, "ps")) {
        cmd_ps();
        return;
    }
    if (str_eq(input, "kill") || starts_with(input, "kill ")) {
        cmd_kill(input[4] == ' ' ? input + 5 : "");
        return;
    }
    if (str_eq(input, "pin") || starts_with(input, "pin ")) {
        cmd_pin(input[3] == ' ' ? input + 4 : "", 1);
        return;
    }
    if (str_eq(input, "unpin") || starts_with(input, "unpin ")) {
        cmd_pin(input[5] == ' ' ? input + 6 : "", 0);
        return;
    }
    if (str_eq(input, "desktop") || starts_with(input, "desktop ")) {
        /* desktop WxH: e.g. 480x800 (phone), 1920x1080 (TV) */
        unsigned int w = 0;
        unsigned int h = 0;
        const char *arg = input + 7;
        while (*arg == ' ') {
            arg++;
        }
        while (*arg >= '0' && *arg <= '9') {
            w = w * 10u + (unsigned int)(*arg++ - '0');
        }
        if (*arg == 'x' || *arg == 'X') {
            arg++;
            while (*arg >= '0' && *arg <= '9') {
                h = h * 10u + (unsigned int)(*arg++ - '0');
            }
        }
        if (w != 0 || h != 0) {
            desktop_set_size((int)w, (int)h);
        }
        cmd_desktop();
        return;
    }
    if (str_eq(input, "poweroff")) {
        machine_poweroff();
        return;
    }
    if (str_eq(input, "reboot")) {
        machine_reboot();
        return;
    }
    if (net_command(input)) {
        return;
    }
    if (str_eq(input, "ping")) {
        if (!link_up()) {
            console_puts("ping: no link\n");
        } else if (link_ping()) {
            console_puts("pong\n");
        } else {
            console_puts("ping: timeout\n");
        }
        return;
    }
    if (str_eq(input, "send") || starts_with(input, "send ")) {
        /* send FILE: copy a file to the Genesis Link peer's /home/inbox. */
        char abs[FS_NAME];
        const char *arg = input[4] == ' ' ? input + 5 : "";
        struct ram_file *f = arg[0] != 0 && fs_resolve(arg, abs) ? fs_find_abs(abs) : 0;
        if (f == 0 || f->dir) {
            console_puts("send: no such file\n");
        } else if (!link_up()) {
            console_puts("send: no link\n");
        } else if (link_send_file(base_name(abs), (const unsigned char *)f->data, f->length)) {
            console_puts("send: done ");
            console_puts(abs);
            console_putc('\n');
            desktop_link("core", "peer");
        } else {
            console_puts("send: failed\n");
        }
        return;
    }
    if (str_eq(input, "say") || starts_with(input, "say ")) {
        const char *text = input[3] == ' ' ? input + 4 : "";
        if (text[0] == 0 || !link_up() || !link_send_text(text)) {
            console_puts("say: failed\n");
        } else {
            console_puts("say: sent\n");
        }
        return;
    }
    console_puts("unknown command\n");
}

static void prompt(void) {
    console_puts("genesis:");
    console_puts(cwd);
    console_puts("> ");
}

static void sh_run_line(const char *src);

/* The desktop command bar runs one line; output goes to COM1 and `out`. */
void desktop_run(const char *cmd, char *out, unsigned int size) {
    char copy[LINE_MAX];
    unsigned int n = 0;
    while (cmd[n] != 0 && n + 1 < LINE_MAX) {
        copy[n] = cmd[n];
        n++;
    }
    copy[n] = 0;
    prompt();
    console_puts(copy);
    console_putc('\n');
    desk_cap_len = 0;
    desk_cap[0] = 0;
    desk_cap_on = 1;
    sh_run_line(copy);
    journal_command(cmd);
    desk_cap_on = 0;
    unsigned int k = 0;
    while (k + 1 < size && desk_cap[k] != 0) {
        out[k] = desk_cap[k];
        k++;
    }
    out[k] = 0;
}

/* ---- developer shell: tokens, variables, pipes, redirects, scripts ---- */
enum { SH_TOK = 32, SH_STAGES = 4, SH_VARS = 16, SH_VNAME = 16, SH_VVAL = 64, SH_HIST = 16 };
enum { T_WORD, T_PIPE, T_SEMI, T_AMP, T_GT, T_GTGT };

struct sh_tok {
    int type;
    char *s;
};

static char var_names[SH_VARS][SH_VNAME];
static char var_vals[SH_VARS][SH_VVAL];
static const char *sh_in;
static unsigned int sh_in_len;
static int sh_has_in;
static int sh_depth;

static int is_name_ch(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static int name_eq_n(const char *stored, const char *name, unsigned int n) {
    for (unsigned int i = 0; i < n; i++) {
        if (stored[i] != name[i]) {
            return 0;
        }
    }
    return stored[n] == 0;
}

static const char *var_get(const char *name, unsigned int n) {
    for (int i = 0; i < SH_VARS; i++) {
        if (var_names[i][0] != 0 && name_eq_n(var_names[i], name, n)) {
            return var_vals[i];
        }
    }
    return "";
}

static int var_set(const char *name, unsigned int n, const char *value) {
    int slot = -1;
    if (n == 0 || n >= SH_VNAME) {
        return 0;
    }
    for (int i = 0; i < SH_VARS; i++) {
        if (var_names[i][0] != 0 && name_eq_n(var_names[i], name, n)) {
            slot = i;
            break;
        }
        if (var_names[i][0] == 0 && slot < 0) {
            slot = i;
        }
    }
    if (slot < 0) {
        return 0;
    }
    for (unsigned int i = 0; i < n; i++) {
        var_names[slot][i] = name[i];
    }
    var_names[slot][n] = 0;
    unsigned int k = 0;
    while (value[k] != 0 && k + 1 < SH_VVAL) {
        var_vals[slot][k] = value[k];
        k++;
    }
    var_vals[slot][k] = 0;
    return 1;
}

/* One statement (up to ; or &), so $VAR sees earlier assignments.
 * Returns token count, -1 if too long, -2 on an unclosed quote. */
static int sh_tokenize(const char **src, struct sh_tok *toks, char *store, unsigned int cap) {
    const char *p = *src;
    int n = 0;
    unsigned int used = 0;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r') {
            p++;
        }
        if (*p == 0 || *p == '#') {
            *src = 0;
            return n;
        }
        if (n >= SH_TOK) {
            return -1;
        }
        toks[n].s = 0;
        if (*p == '|' || *p == ';' || *p == '&') {
            toks[n++].type = *p == '|' ? T_PIPE : *p == ';' ? T_SEMI : T_AMP;
            p++;
            if (toks[n - 1].type != T_PIPE) {
                *src = p;
                return n;
            }
            continue;
        }
        if (*p == '>') {
            p++;
            toks[n].type = T_GT;
            if (*p == '>') {
                toks[n].type = T_GTGT;
                p++;
            }
            n++;
            continue;
        }
        char *word = store + used;
        char quote = 0;
        while (*p != 0) {
            char c = *p;
            if (!quote && (c == ' ' || c == '\t' || c == '\r' || c == '|' || c == ';' || c == '&' || c == '>')) {
                break;
            }
            if (!quote && (c == '"' || c == '\'')) {
                quote = c;
                p++;
                continue;
            }
            if (quote && c == quote) {
                quote = 0;
                p++;
                continue;
            }
            if (c == '$' && quote != '\'' && is_name_ch(p[1])) {
                const char *name = ++p;
                unsigned int k = 0;
                while (is_name_ch(name[k])) {
                    k++;
                }
                const char *value = var_get(name, k);
                p += k;
                while (*value != 0) {
                    if (used + 1 >= cap) {
                        return -1;
                    }
                    store[used++] = *value++;
                }
                continue;
            }
            if (used + 1 >= cap) {
                return -1;
            }
            store[used++] = c;
            p++;
        }
        if (quote) {
            return -2;
        }
        if (used + 1 >= cap) {
            return -1;
        }
        store[used++] = 0;
        toks[n].type = T_WORD;
        toks[n].s = word;
        n++;
    }
}

/* Input for a filter: the pipe if there is one, else the named file. */
static int sh_input(const char *file, const char *who, const char **data, unsigned int *len) {
    if (sh_has_in) {
        *data = sh_in;
        *len = sh_in_len;
        return 1;
    }
    struct ram_file *f = file ? fs_find(file) : 0;
    if (f == 0) {
        console_puts(who);
        console_puts(file ? ": no such file\n" : ": no input\n");
        return 0;
    }
    *data = (const char *)f->data;
    *len = f->length;
    return 1;
}

static int line_has(const char *line, unsigned int len, const char *word) {
    unsigned int w = 0;
    while (word[w] != 0) {
        w++;
    }
    if (w == 0) {
        return 1;
    }
    for (unsigned int i = 0; i + w <= len; i++) {
        unsigned int k = 0;
        while (k < w && line[i + k] == word[k]) {
            k++;
        }
        if (k == w) {
            return 1;
        }
    }
    return 0;
}

static void put_n(const char *text, unsigned int len) {
    for (unsigned int i = 0; i < len; i++) {
        console_putc(text[i]);
    }
}

static void sh_grep(char **argv, int argc) {
    const char *data;
    unsigned int len;
    if (argc < 2) {
        console_puts("grep: need a word\n");
        return;
    }
    if (!sh_input(argc > 2 ? argv[2] : 0, "grep", &data, &len)) {
        return;
    }
    unsigned int start = 0;
    for (unsigned int i = 0; i <= len; i++) {
        if (i == len || data[i] == '\n') {
            if (i > start && line_has(data + start, i - start, argv[1])) {
                put_n(data + start, i - start);
                console_putc('\n');
            }
            start = i + 1;
        }
    }
}

static void sh_wc(char **argv, int argc) {
    const char *data;
    unsigned int len;
    if (!sh_input(argc > 1 ? argv[1] : 0, "wc", &data, &len)) {
        return;
    }
    unsigned int lines = 0, words = 0;
    int in_word = 0;
    for (unsigned int i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\n') {
            lines++;
        }
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            words++;
        }
    }
    if (len > 0 && data[len - 1] != '\n') {
        lines++;
    }
    print_u32(lines);
    console_putc(' ');
    print_u32(words);
    console_putc(' ');
    print_u32(len);
    console_putc('\n');
}

static void sh_head(char **argv, int argc) {
    const char *data;
    unsigned int len;
    unsigned int count = 10;
    int arg = 1;
    if (argc > 1 && parse_u32(argv[1], &count)) {
        arg = 2;
    }
    if (!sh_input(argc > arg ? argv[arg] : 0, "head", &data, &len)) {
        return;
    }
    unsigned int i = 0;
    while (i < len && count > 0) {
        console_putc(data[i]);
        if (data[i] == '\n') {
            count--;
        }
        i++;
    }
    if (i > 0 && data[i - 1] != '\n') {
        console_putc('\n');
    }
}

static void sh_run_line(const char *src);

static void sh_script(const char *path) {
    char name[FS_NAME];
    if (!fs_resolve(path, name)) {
        name[0] = 0;
    }
    struct ram_file *f = name[0] ? fs_find(name) : 0;
    if (f == 0) {
        console_puts("sh: no such file\n");
        return;
    }
    if (sh_depth >= 4) {
        console_puts("sh: too deep\n");
        return;
    }
    sh_depth++;
    char buf[LINE_MAX];
    unsigned int n = 0;
    for (unsigned int i = 0; i <= f->length; i++) {
        if (i == f->length || f->data[i] == '\n') {
            buf[n] = 0;
            sh_run_line(buf);
            n = 0;
            f = fs_find(name);
            if (f == 0) {
                break;
            }
            continue;
        }
        if (n + 1 < LINE_MAX) {
            buf[n++] = (char)f->data[i];
        }
    }
    sh_depth--;
}

static void sh_set(void) {
    for (int i = 0; i < SH_VARS; i++) {
        if (var_names[i][0] != 0) {
            console_puts(var_names[i]);
            console_putc('=');
            console_puts(var_vals[i]);
            console_putc('\n');
        }
    }
}

static void sh_command(char **argv, int argc) {
    if (argc == 0) {
        return;
    }
    const char *eq = argv[0];
    while (is_name_ch(*eq)) {
        eq++;
    }
    if (*eq == '=' && eq != argv[0] && !(argv[0][0] >= '0' && argv[0][0] <= '9')) {
        if (!var_set(argv[0], (unsigned int)(eq - argv[0]), eq + 1)) {
            console_puts("sh: too many variables\n");
        }
        return;
    }
    if (str_eq(argv[0], "grep")) {
        sh_grep(argv, argc);
        return;
    }
    if (str_eq(argv[0], "wc")) {
        sh_wc(argv, argc);
        return;
    }
    if (str_eq(argv[0], "head")) {
        sh_head(argv, argc);
        return;
    }
    if (str_eq(argv[0], "set")) {
        sh_set();
        return;
    }
    if (str_eq(argv[0], "sh")) {
        if (argc < 2) {
            console_puts("sh: need a file\n");
            return;
        }
        sh_script(argv[1]);
        return;
    }
    if (sh_has_in && str_eq(argv[0], "cat") && argc == 1) {
        put_n(sh_in, sh_in_len);
        return;
    }
    /* Everything else: the classic one-line commands. */
    char cmd[LINE_MAX * 2];
    unsigned int n = 0;
    for (int i = 0; i < argc; i++) {
        for (const char *s = argv[i]; *s != 0 && n + 2 < sizeof cmd; s++) {
            cmd[n++] = *s;
        }
        if (i + 1 < argc && n + 2 < sizeof cmd) {
            cmd[n++] = ' ';
        }
    }
    cmd[n] = 0;
    execute(cmd);
}

static void sh_write_file(const char *name, const char *data, unsigned int len, int append) {
    unsigned int total = 0;
    struct ram_file *old = append ? fs_find(name) : 0;
    if (old != 0) {
        for (; total < old->length && total < FS_DATA; total++) {
            edit_buf[total] = (char)old->data[total];
        }
    }
    for (unsigned int i = 0; i < len && total < FS_DATA; i++) {
        edit_buf[total++] = data[i];
    }
    if (!fs_add(name, edit_buf, total)) {
        console_puts("sh: cannot write file\n");
        return;
    }
    if (!disk_save()) {
        console_puts("disk: io failed\n");
    }
}

static void sh_statement(struct sh_tok *t, int n, int background) {
    char *argv[SH_STAGES][SH_TOK];
    int argc[SH_STAGES];
    int stages = 1;
    char *redir = 0;
    int append = 0;
    if (n == 0) {
        return;
    }
    argc[0] = 0;
    for (int i = 0; i < n; i++) {
        if (t[i].type == T_PIPE) {
            if (argc[stages - 1] == 0 || stages >= SH_STAGES) {
                console_puts("sh: bad pipe\n");
                return;
            }
            argc[stages++] = 0;
        } else if (t[i].type == T_GT || t[i].type == T_GTGT) {
            if (i + 1 >= n || t[i + 1].type != T_WORD) {
                console_puts("sh: need a file after >\n");
                return;
            }
            append = t[i].type == T_GTGT;
            redir = t[++i].s;
        } else {
            argv[stages - 1][argc[stages - 1]++] = t[i].s;
        }
    }
    if (argc[stages - 1] == 0) {
        console_puts("sh: bad pipe\n");
        return;
    }
    if (background) {
        /* `ticker &` or `run ticker &`: a program from disk as a task. */
        char **a = argv[0];
        int skip = argc[0] >= 2 && (str_eq(a[0], "run") || str_eq(a[0], "spawn"));
        if (stages == 1 && redir == 0 && argc[0] > skip) {
            char line[96];
            unsigned int n = 0;
            for (int w = skip; w < argc[0]; w++) {
                for (const char *c = a[w]; *c != 0 && n + 2 < sizeof line; c++) {
                    line[n++] = *c;
                }
                line[n++] = ' ';
            }
            line[n] = 0;
            cmd_run(line, 1);
        } else {
            console_puts("sh: only a program can run with &\n");
        }
        return;
    }
    int outer_on = cap_on;
    char *outer_buf = cap_buf;
    unsigned int outer_len = cap_len;
    const char *in = 0;
    unsigned int in_len = 0;
    int has_in = 0;
    int which = 0;
    for (int s = 0; s < stages; s++) {
        int capture = s + 1 < stages || redir != 0;
        if (capture) {
            cap_buf = sh_bufs[which];
            cap_len = 0;
            cap_on = 1;
        } else {
            cap_on = outer_on;
            cap_buf = outer_buf;
            cap_len = outer_len;
        }
        sh_in = in;
        sh_in_len = in_len;
        sh_has_in = has_in;
        sh_command(argv[s], argc[s]);
        sh_has_in = 0;
        if (capture) {
            in = sh_bufs[which];
            in_len = cap_len;
            has_in = 1;
            which ^= 1;
        } else {
            outer_len = cap_len;
        }
    }
    cap_on = outer_on;
    cap_buf = outer_buf;
    cap_len = outer_len;
    if (redir != 0) {
        sh_write_file(redir, in, in_len, append);
    }
}

static void sh_run_line(const char *src) {
    struct sh_tok toks[SH_TOK];
    char store[LINE_MAX * 2];
    while (src != 0) {
        int n = sh_tokenize(&src, toks, store, sizeof store);
        if (n == -2) {
            console_puts("sh: unclosed quote\n");
            return;
        }
        if (n < 0) {
            console_puts("sh: line too long\n");
            return;
        }
        int amp = n > 0 && toks[n - 1].type == T_AMP;
        if (n > 0 && (toks[n - 1].type == T_SEMI || amp)) {
            n--;
        }
        sh_statement(toks, n, amp);
    }
}

static char hist[SH_HIST][LINE_MAX];
static int hist_count;
static int hist_pos;

static void line_erase(void) {
    while (line_len > 0) {
        line_len--;
        if (utf8_is_cont((unsigned char)line[line_len])) {
            continue; /* one cell per character, not per byte */
        }
        screen_putc('\b');
        serial_putc('\b');
        serial_putc(' ');
        serial_putc('\b');
    }
}

static void line_load(const char *text) {
    line_erase();
    while (*text != 0 && line_len + 1 < LINE_MAX) {
        line[line_len++] = *text;
        console_putc(*text++);
    }
}

static void hist_push(const char *text) {
    if (text[0] == 0) {
        return;
    }
    if (hist_count == SH_HIST) {
        for (int i = 1; i < SH_HIST; i++) {
            for (int k = 0; k < LINE_MAX; k++) {
                hist[i - 1][k] = hist[i][k];
            }
        }
        hist_count--;
    }
    int k = 0;
    while (text[k] != 0 && k + 1 < LINE_MAX) {
        hist[hist_count][k] = text[k];
        k++;
    }
    hist[hist_count][k] = 0;
    hist_count++;
}

static void shell_input(char ch) {
    if (ch == KEY_UP) {
        if (hist_pos > 0) {
            hist_pos--;
            line_load(hist[hist_pos]);
        }
        return;
    }
    if (ch == KEY_DOWN) {
        if (hist_pos < hist_count) {
            hist_pos++;
            line_load(hist_pos < hist_count ? hist[hist_pos] : "");
        }
        return;
    }
    if (ch == '\t') {
        char cands[256];
        int before = line_len;
        line[line_len] = 0;
        int found = sh_complete(line, &line_len, LINE_MAX, cands, sizeof cands);
        for (int k = before; k < line_len; k++) {
            console_putc(line[k]);
        }
        if (found > 1 && line_len == before) {
            console_putc('\n');
            console_puts(cands);
            console_putc('\n');
            prompt();
            line[line_len] = 0;
            console_puts(line);
        }
        return;
    }
    if (ch == '\n') {
        console_putc('\n');
        line[line_len] = 0;
        hist_push(line);
        hist_pos = hist_count;
        line_len = 0;
        char copy[LINE_MAX];
        for (int i = 0; i < LINE_MAX; i++) {
            copy[i] = line[i];
        }
        char typed[LINE_MAX];
        for (int i = 0; i < LINE_MAX; i++) {
            typed[i] = line[i];
        }
        sh_run_line(copy);
        journal_command(typed);
        if (!editor_active()) {
            prompt();
        }
        return;
    }
    if (ch == '\b') {
        if (line_len > 0) {
            line_len--;
            while (line_len > 0 && utf8_is_cont((unsigned char)line[line_len])) {
                line_len--;
            }
            screen_putc('\b');
            serial_putc('\b');
            serial_putc(' ');
            serial_putc('\b');
        }
        return;
    }
    if (line_len + 4 >= LINE_MAX && !utf8_is_cont((unsigned char)ch)) {
        return; /* keep room so a whole UTF-8 character always fits */
    }
    if (line_len + 1 >= LINE_MAX) {
        return;
    }
    line[line_len++] = ch;
    console_putc(ch);
}

void machine_poweroff(void) {
    console_puts("poweroff\n");
    outw(0x604, 0x2000);
    outw(0xb004, 0x2000);
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void machine_reboot(void) {
    console_puts("reboot\n");
    while ((inb(0x64) & 0x02) != 0) {
    }
    outb(0x64, 0xfe);
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void clock_service(void);

__attribute__((noreturn)) void kmain(unsigned int mb_info) {
    serial_init();
    memory_init(mb_info);
    if (!disk_load()) {
        fs_seed();
        if (!disk_save()) {
            serial_puts("disk: io failed\n");
        }
    } else if (disk_consume_seed() | disk_consume_migrated()) {
        /* Host imported files (npm run look): restore any missing built-ins. */
        seed_only_missing = 1;
        fs_seed();
        seed_only_missing = 0;
        disk_save();
    }
    if (mb_info >= 8) {
        install_system();
    }
    if (disk_consume_home()) {
        cmd_desktop();
    }
    vga_show_cursor();
    vga_load_cyrillic();
    vga_clear();
    type_row = 1;
    type_col = 0;
    vga_puts(0, 0, "genesis: kernel booted");
    vga_puts_utf8(0, 24, BOOT_HINT, 0x0e);
    serial_puts("genesis: kernel booted\n");
    serial_puts(BOOT_HINT "\n");
    link_init();
    keyboard_drain();
    timer_start();
    prompt();
    for (;;) {
        keyboard_poll();
        clock_service();
        link_poll();
        /* Idle until the next timer tick: input is drained on every tick,
         * and other tasks get the rest of this slice. */
        __asm__ volatile("hlt");
    }
}
