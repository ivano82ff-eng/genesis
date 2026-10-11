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
static char decode_set1(unsigned char scancode) {
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
        [0x34] = '.', [0x35] = '/', [0x39] = ' ',
    };
    if (scancode >= 128) {
        return 0;
    }
    return map[scancode];
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
        if ((bytes[i] & 0x80) != 0) {
            continue;
        }
        char ch = decode_set1(bytes[i]);
        if (ch == 0) {
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

void console_putc(char ch) {
    serial_putc(ch);
    if (!desktop_active()) {
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

enum { LINE_MAX = 72 };

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

static int editing;
static char edit_name[FS_NAME];
static char edit_buf[FS_DATA];
static unsigned int edit_len;

static void prompt(void);

static void cmd_edit(const char *name) {
    struct ram_file *file = fs_find(name);
    if (file == 0 || name[0] == 0) {
        console_puts("edit: no such file\n");
        return;
    }
    unsigned int n = 0;
    while (name[n] != 0 && n + 1 < FS_NAME) {
        edit_name[n] = name[n];
        n++;
    }
    edit_name[n] = 0;
    edit_len = 0;
    editing = 1;
    console_puts("edit: ");
    for (unsigned int i = 0; i < file->length; i++) {
        console_putc(file->data[i]);
    }
    console_putc('\n');
}

static void edit_input(char ch) {
    if (ch == '\n') {
        console_putc('\n');
        if (!fs_add(edit_name, edit_buf, edit_len) || !disk_save()) {
            console_puts("edit: failed\n");
        } else {
            console_puts("edit: saved\n");
        }
        editing = 0;
        edit_len = 0;
        prompt();
        return;
    }
    if (ch == '\b') {
        if (edit_len > 0) {
            edit_len--;
            screen_putc('\b');
            serial_putc('\b');
            serial_putc(' ');
            serial_putc('\b');
        }
        return;
    }
    if (edit_len + 1 >= FS_DATA) {
        return;
    }
    edit_buf[edit_len++] = ch;
    console_putc(ch);
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
typedef void (*prog_fn)(print_fn print, sleep_fn sleep);

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
    program(console_puts, task_sleep);
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

static void shell_input(char ch) {
    if (editing) {
        edit_input(ch);
        return;
    }
    if (ch == '\n') {
        console_putc('\n');
        line[line_len] = 0;
        execute(line);
        line_len = 0;
        if (!editing) {
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
