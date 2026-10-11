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
void desktop_mouse(int x, int y, int left);
void mouse_arm(void);
int disk_load(void);
int disk_save(void);
int install_system(void);
void disk_request_home(void);
int disk_consume_home(void);
int disk_consume_seed(void);
int grid_unlink(const char *name);
void machine_poweroff(void);
void machine_reboot(void);
void link_init(void);
void link_poll(void);
void clock_service(void);
int link_up(void);
int link_send_text(const char *text);
int link_ping(void);
void timer_start(void);
int task_current(void);
int task_spawn(const char *name, void *code);
void task_sleep(unsigned int ticks);
void cmd_ps(void);

#include "editor.h"

static volatile unsigned short *const VGA = (volatile unsigned short *)0xb8000;

static void vga_hide_cursor(void) {
    outb(0x3d4, 0x0a);
    outb(0x3d5, 0x20);
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

static int type_row = 1;
static int type_col = 0;

static void screen_scroll(void) {
    const unsigned short blank = (unsigned short)((VGA_ATTR << 8) | ' ');
    for (int i = VGA_COLS; i < VGA_COLS * VGA_ROWS; i++) {
        VGA[i - VGA_COLS] = VGA[i];
    }
    for (int i = (VGA_ROWS - 1) * VGA_COLS; i < VGA_COLS * VGA_ROWS; i++) {
        VGA[i] = blank;
    }
}

static void screen_advance_row(void) {
    if (type_row + 1 >= VGA_ROWS) {
        screen_scroll();
    } else {
        type_row++;
    }
}

static void screen_putc(char ch) {
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
    int dx = (int)(signed char)mouse_bytes[1];
    int dy = (int)(signed char)mouse_bytes[2];
    mouse_x += dx;
    mouse_y -= dy;
    if (mouse_x < 0) {
        mouse_x = 0;
    }
    if (mouse_y < 0) {
        mouse_y = 0;
    }
    if (mouse_x > 1023) {
        mouse_x = 1023;
    }
    if (mouse_y > 767) {
        mouse_y = 767;
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
#define KEY_UP ((char)1)
#define KEY_DOWN ((char)2)
static int kbd_count;

static void ps2_lock(unsigned long *flags) {
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(*flags)::"memory");
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
                    editor_key(key);
                }
                continue;
            }
            /* Outside the editor: arrow up/down drive shell history. */
            if ((b == 0x48 || b == 0x50) && !desktop_active()) {
                shell_input(b == 0x48 ? KEY_UP : KEY_DOWN);
            }
            continue;
        }
        if (b == 0x2a || b == 0x36) {
            kbd_shift = 1;
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
        if ((b & 0x80) != 0) {
            continue;
        }
        char ch = decode_set1(b, kbd_shift, kbd_caps);
        if (ch == 0) {
            continue;
        }
        if (editor_active()) {
            if (kbd_ctrl && (ch == 's' || ch == 'S')) {
                editor_key(ED_SAVE);
            } else if (kbd_ctrl && (ch == 'q' || ch == 'Q')) {
                editor_key(ED_QUIT);
            } else if (!kbd_ctrl) {
                editor_key((unsigned char)ch);
            }
            continue;
        }
        if (desktop_active()) {
            desktop_key(ch);
            continue;
        }
        shell_input(ch);
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

void mouse_arm(void) {
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

static char program_text[64];
static int program_len = 0;
static int program_capture = 0;

enum { SH_OUT = 4096 };
static char sh_bufs[2][SH_OUT];
static char *cap_buf;
static unsigned int cap_len;
static int cap_on;

void console_putc(char ch) {
    if (cap_on && task_current() == 0) {
        /* Shell pipe or redirect: collect output instead of printing it. */
        if (cap_len < SH_OUT) {
            cap_buf[cap_len++] = ch;
        }
        return;
    }
    serial_putc(ch);
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

static void print_hex64(unsigned long value) {
    console_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned int nibble = (unsigned int)((value >> shift) & 0xful);
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
static unsigned char *const HEAP = (unsigned char *)0x200000;
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

static void cmd_mem(void) {
    console_puts("usable ");
    print_u32((unsigned int)(usable_bytes >> 20));
    console_puts(" MiB\n");
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

struct ram_file {
    char name[FS_NAME];
    unsigned char *data;
    unsigned int length;
    int used;
};

static struct ram_file ramfs[FS_MAX];
static unsigned char *const FS_POOL = (unsigned char *)FS_POOL_ADDR;

static unsigned char *fs_slot_data(int index) {
    return FS_POOL + (unsigned int)index * FS_DATA;
}

static void fs_copy_name(char *dest, const char *src) {
    unsigned int i = 0;
    while (src[i] != 0 && i + 1 < FS_NAME) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = 0;
}

static int fs_add(const char *name, const char *data, unsigned int length) {
    struct ram_file *slot = 0;
    for (int i = 0; i < FS_MAX; i++) {
        if (ramfs[i].used && str_eq(ramfs[i].name, name)) {
            slot = &ramfs[i];
            break;
        }
        if (!ramfs[i].used && slot == 0) {
            slot = &ramfs[i];
        }
    }
    if (slot == 0 || name[0] == 0) {
        return 0;
    }
    if (length > FS_DATA) {
        length = FS_DATA;
    }
    int index = (int)(slot - ramfs);
    slot->data = fs_slot_data(index);
    fs_copy_name(slot->name, name);
    for (unsigned int i = 0; i < length; i++) {
        slot->data[i] = (unsigned char)data[i];
    }
    slot->length = length;
    slot->used = 1;
    return 1;
}

static struct ram_file *fs_find(const char *name) {
    for (int i = 0; i < FS_MAX; i++) {
        if (ramfs[i].used && str_eq(ramfs[i].name, name)) {
            return &ramfs[i];
        }
    }
    return 0;
}

extern const unsigned char hello_bin[];
extern const unsigned char hello_bin_end[];
extern const unsigned char ticker_bin[];
extern const unsigned char ticker_bin_end[];
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

/* Set while refreshing an existing disk: add built-in files only where missing. */
static int seed_only_missing;

static void fs_seed_blob(const char *name, const unsigned char *start, const unsigned char *end) {
    if (seed_only_missing && fs_find(name)) {
        return;
    }
    fs_add(name, (const char *)start, (unsigned int)(end - start));
}

static void fs_seed(void) {
    if (!seed_only_missing || !fs_find("readme")) {
        fs_add("readme", "welcome to genesis", 18);
    }
    fs_seed_blob("hello", hello_bin, hello_bin_end);
    fs_seed_blob("open", open_bin, open_bin_end);
    fs_seed_blob("vtxt", vtxt_bin, vtxt_bin_end);
    fs_seed_blob("vdocx", vdocx_bin, vdocx_bin_end);
    fs_seed_blob("vimg", vimg_bin, vimg_bin_end);
    fs_seed_blob("vpdf", vpdf_bin, vpdf_bin_end);
    fs_seed_blob("vmp4", vmp4_bin, vmp4_bin_end);
    fs_seed_blob("note.txt", sample_note_txt, sample_note_txt_end);
    fs_seed_blob("note.docx", sample_note_docx, sample_note_docx_end);
    fs_seed_blob("pic.png", sample_pic_png, sample_pic_png_end);
    fs_seed_blob("pic.jpg", sample_pic_jpg, sample_pic_jpg_end);
    fs_seed_blob("note.pdf", sample_note_pdf, sample_note_pdf_end);
    fs_seed_blob("clip.mp4", sample_clip_mp4, sample_clip_mp4_end);
    fs_seed_blob("ticker", ticker_bin, ticker_bin_end);
}

void fs_clear_all(void) {
    for (int i = 0; i < FS_MAX; i++) {
        ramfs[i].used = 0;
        ramfs[i].length = 0;
        ramfs[i].name[0] = 0;
    }
}

int fs_export(int index, char *name, unsigned int name_cap, unsigned char *data, unsigned int *length) {
    if (index < 0 || index >= FS_MAX || !ramfs[index].used || name_cap == 0) {
        return 0;
    }
    unsigned int n = 0;
    while (ramfs[index].name[n] != 0 && n + 1 < name_cap) {
        name[n] = ramfs[index].name[n];
        n++;
    }
    name[n] = 0;
    unsigned int count = ramfs[index].length;
    if (count > FS_DATA) {
        count = FS_DATA;
    }
    for (unsigned int i = 0; i < count; i++) {
        data[i] = ramfs[index].data[i];
    }
    *length = count;
    return 1;
}

void fs_import(const char *name, const unsigned char *data, unsigned int length) {
    fs_add(name, (const char *)data, length);
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

int fs_entry(int index, char *name, unsigned int name_cap, unsigned int *length) {
    if (index < 0 || index >= FS_MAX || !ramfs[index].used || name_cap == 0) {
        return 0;
    }
    unsigned int n = 0;
    while (ramfs[index].name[n] != 0 && n + 1 < name_cap) {
        name[n] = ramfs[index].name[n];
        n++;
    }
    name[n] = 0;
    if (length) {
        *length = ramfs[index].length;
    }
    return 1;
}

int fs_store_readme(const char *text) {
    unsigned int length = 0;
    while (text[length] != 0) {
        length++;
    }
    if (!fs_add("readme", text, length)) {
        return 0;
    }
    return disk_save();
}

void preview_readme(char *dest, unsigned int cap) {
    struct ram_file *file = fs_find("readme");
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

static void cmd_ls(void) {
    console_puts("ls:");
    for (int i = 0; i < FS_MAX; i++) {
        if (!ramfs[i].used) {
            continue;
        }
        console_putc(' ');
        console_puts(ramfs[i].name);
    }
    console_putc('\n');
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

/* Scratch buffer for appends (>>), one Grid Store file in size. */
static char edit_buf[FS_DATA];

static void prompt(void);

int fs_get(const char *name, const unsigned char **data, unsigned int *length) {
    struct ram_file *file = fs_find(name);
    if (file == 0) {
        return 0;
    }
    *data = file->data;
    *length = file->length;
    return 1;
}

int fs_put(const char *name, const char *data, unsigned int length) {
    return fs_add(name, data, length) && disk_save();
}

void editor_closed(void) {
    vga_hide_cursor();
    vga_clear();
    type_row = 0;
    type_col = 0;
    prompt();
}

static void cmd_edit(const char *name) {
    editor_open(name);
}

static void cmd_write(const char *args) {
    char name[FS_NAME];
    unsigned int i = 0;
    while (args[i] != 0 && args[i] != ' ' && i + 1 < FS_NAME) {
        name[i] = args[i];
        i++;
    }
    name[i] = 0;
    if (name[0] == 0 || args[i] != ' ') {
        console_puts("write: need a name and text\n");
        return;
    }
    const char *text = args + i + 1;
    unsigned int length = 0;
    while (text[length] != 0) {
        length++;
    }
    if (!fs_add(name, text, length)) {
        console_puts("write: full\n");
        return;
    }
    if (!disk_save()) {
        console_puts("disk: io failed\n");
    }
    console_puts("write: ");
    console_puts(name);
    console_putc('\n');
}

static int fs_remove(const char *name) {
    for (int i = 0; i < FS_MAX; i++) {
        if (ramfs[i].used && str_eq(ramfs[i].name, name)) {
            ramfs[i].used = 0;
            ramfs[i].length = 0;
            ramfs[i].name[0] = 0;
            return 1;
        }
    }
    return 0;
}

int fs_delete(const char *name) {
    if (fs_find(name) == 0) {
        return 0;
    }
    fs_remove(name);
    grid_unlink(name);
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
    if (name[0] == 0 || fs_find(name) == 0) {
        console_puts("rm: no such file\n");
        return;
    }
    fs_remove(name);
    grid_unlink(name);
    console_puts("rm: ");
    console_puts(name);
    console_putc('\n');
}

typedef void (*print_fn)(const char *);
typedef void (*sleep_fn)(unsigned int ticks);
typedef void *(*pkg_sym_fn)(const char *lib, const char *sym);
void *pkg_sym(const char *lib, const char *sym);
void cmd_pkg(const char *args);
typedef void (*prog_fn)(print_fn print, sleep_fn sleep, pkg_sym_fn sym);

/* background: start the program as a task (`spawn NAME`) and return. */
static void cmd_run(const char *name, int background) {
    struct ram_file *file = fs_find(name);
    if (file == 0) {
        console_puts("run: no such file\n");
        return;
    }
    unsigned char *dest = heap_alloc(file->length == 0 ? 1u : file->length);
    if (dest == 0) {
        console_puts("run: no memory\n");
        return;
    }
    for (unsigned int i = 0; i < file->length; i++) {
        dest[i] = (unsigned char)file->data[i];
    }
    if (background) {
        int id = task_spawn(name, dest);
        if (id < 0) {
            console_puts("spawn: too many tasks\n");
            return;
        }
        console_puts("spawn: task ");
        print_u32((unsigned int)id);
        console_putc('\n');
        return;
    }
    program_len = 0;
    program_text[0] = 0;
    program_capture = 1;
    /* Programs are machine code run in ring 0: no isolation, a bad program
     * can hang the whole kernel. Fine for a demo, not a userspace. */
    prog_fn program = (prog_fn)(void *)dest;
    program(console_puts, task_sleep, pkg_sym);
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
        if (!ramfs[i].used) {
            continue;
        }
        if (used != 0 && used + 1 < cap) {
            dest[used++] = ' ';
        }
        for (unsigned int n = 0; ramfs[i].name[n] != 0 && used + 1 < cap; n++) {
            dest[used++] = ramfs[i].name[n];
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

static void execute(char *input) {
    trim(input);
    if (input[0] == 0) {
        return;
    }
    if (str_eq(input, "help")) {
        console_puts("commands: help echo clear mem alloc heap ls cat write rm run spawn ps desktop\n");
        console_puts("shell: sh set grep wc head  NAME=v $NAME \"..\" '..' | > >> ; & #\n");
        console_puts("packages: pkg list | installed | install NAME | remove NAME\n");
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
    if (str_eq(input, "ls")) {
        cmd_ls();
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
    if (str_eq(input, "ps")) {
        cmd_ps();
        return;
    }
    if (str_eq(input, "desktop")) {
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
    console_puts("genesis> ");
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

static void sh_script(const char *name) {
    struct ram_file *f = fs_find(name);
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
        if (stages == 1 && redir == 0 && argc[0] == 1) {
            cmd_run(a[0], 1);
        } else if (stages == 1 && redir == 0 && argc[0] == 2 && (str_eq(a[0], "run") || str_eq(a[0], "spawn"))) {
            cmd_run(a[1], 1);
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
        sh_run_line(copy);
        if (!editor_active()) {
            prompt();
        }
        return;
    }
    if (ch == '\b') {
        if (line_len > 0) {
            line_len--;
            screen_putc('\b');
            serial_putc('\b');
            serial_putc(' ');
            serial_putc('\b');
        }
        return;
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
    } else if (disk_consume_seed()) {
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
    vga_hide_cursor();
    vga_clear();
    type_row = 1;
    type_col = 0;
    vga_puts(0, 0, "genesis: kernel booted");
    serial_puts("genesis: kernel booted\n");
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
