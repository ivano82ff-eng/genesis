/* Full-screen text editor for the VGA text console: `edit FILE`. */
#include "fs_limits.h"
#include "editor.h"
#include "utf8.h"

static inline void ed_outb(unsigned short port, unsigned char value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

int fs_get(const char *name, const unsigned char **data, unsigned int *length);
int fs_put(const char *name, const char *data, unsigned int length);
void console_puts(const char *text);
void editor_closed(void);
int kbd_layout_ru(void);

enum { COLS = 80, ROWS = 25, TEXT_ROWS = 24, GUTTER = 5, TEXT_COLS = COLS - GUTTER };
enum { LANG_NONE = 0, LANG_C, LANG_ASM, LANG_SH };
enum {
    C_TEXT = 0x07, C_KEYWORD = 0x0e, C_COMMENT = 0x0a, C_STRING = 0x0c,
    C_NUMBER = 0x0b, C_GUTTER = 0x08, C_STATUS = 0x70, C_EMPTY = 0x01,
};

/* On the desktop (framebuffer mode) the editor draws into a shadow screen
 * that desktop.c paints with its own font; otherwise into VGA text memory. */
static unsigned short shadow[80 * 25];
static volatile unsigned short *VGA = (volatile unsigned short *)0xb8000;
static int cur_row, cur_col;
int desktop_active(void);
void desktop_editor_paint(void);

const unsigned short *editor_screen(void) {
    return shadow;
}

void editor_cursor(int *row, int *col) {
    *row = cur_row;
    *col = cur_col;
}

static int active;
static char name[FS_NAME];
static char *const buf = (char *)EDITOR_BUF_ADDR;
static unsigned int len;
static unsigned int cur;      /* byte offset of the cursor */
static int top;               /* first visible line */
static int left;              /* first visible column */
static int want_col = -1;     /* column kept while moving up/down */
static int dirty;
static int quit_armed;
static int lang;
static const char *note = "";

int editor_active(void) {
    return active;
}

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.' || c == '#';
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int ends_with(const char *s, const char *suffix) {
    int a = 0, b = 0;
    while (s[a]) a++;
    while (suffix[b]) b++;
    if (b > a) return 0;
    for (int i = 0; i < b; i++) {
        if (s[a - b + i] != suffix[i]) return 0;
    }
    return 1;
}

static const char *const C_WORDS[] = {
    "int", "char", "void", "unsigned", "signed", "long", "short", "static", "const",
    "struct", "enum", "union", "typedef", "return", "if", "else", "for", "while", "do",
    "break", "continue", "switch", "case", "default", "sizeof", "volatile", "extern",
    "goto", "inline", "#include", "#define", "#pragma", "#if", "#ifdef", "#ifndef",
    "#endif", "#else", 0,
};
static const char *const ASM_WORDS[] = {
    "mov", "add", "sub", "push", "pop", "call", "ret", "jmp", "je", "jne", "jz", "jnz",
    "cmp", "xor", "and", "or", "inc", "dec", "lea", "int", "hlt", "cli", "sti", "test",
    "shl", "shr", "in", "out", "loop", "nop", "section", "global", "extern", "bits",
    "db", "dw", "dd", "dq", "resb", "resq", "times", "org", "equ", ".text", ".data",
    ".globl", 0,
};
static const char *const SH_WORDS[] = {
    "if", "then", "else", "elif", "fi", "for", "in", "do", "done", "while", "case",
    "esac", "function", "echo", "export", "set", "cat", "ls", "write", "rm", "run",
    "spawn", "sh", 0,
};

static int is_keyword(const char *word, int n) {
    const char *const *list = lang == LANG_C ? C_WORDS : lang == LANG_ASM ? ASM_WORDS : SH_WORDS;
    for (int i = 0; list[i]; i++) {
        int k = 0;
        while (k < n && list[i][k] == word[k]) k++;
        if (k == n && list[i][k] == 0) return 1;
    }
    return 0;
}

/* Color every byte of one line; returns the block-comment state after it. */
static int color_line(const char *s, int n, unsigned char *attr, int in_block) {
    int i = 0;
    for (int k = 0; k < n; k++) attr[k] = C_TEXT;
    if (lang == LANG_NONE) return 0;
    while (i < n) {
        char c = s[i];
        if (in_block) {
            attr[i] = C_COMMENT;
            if (c == '*' && i + 1 < n && s[i + 1] == '/') {
                attr[i + 1] = C_COMMENT;
                i += 2;
                in_block = 0;
                continue;
            }
            i++;
            continue;
        }
        int line_comment = (lang == LANG_C && c == '/' && i + 1 < n && s[i + 1] == '/') ||
                           (lang == LANG_ASM && (c == ';' || (c == '#' && ends_with(name, ".s")))) ||
                           (lang == LANG_SH && c == '#');
        if (line_comment) {
            while (i < n) attr[i++] = C_COMMENT;
            break;
        }
        if (lang == LANG_C && c == '/' && i + 1 < n && s[i + 1] == '*') {
            attr[i] = attr[i + 1] = C_COMMENT;
            i += 2;
            in_block = 1;
            continue;
        }
        if (c == '"' || c == '\'') {
            attr[i++] = C_STRING;
            while (i < n) {
                attr[i] = C_STRING;
                if (s[i] == '\\' && i + 1 < n) {
                    attr[++i] = C_STRING;
                } else if (s[i] == c) {
                    i++;
                    break;
                }
                i++;
            }
            continue;
        }
        if (is_digit(c)) {
            while (i < n && (is_digit(s[i]) || is_alpha(s[i]))) attr[i++] = C_NUMBER;
            continue;
        }
        if (is_alpha(c)) {
            int start = i;
            while (i < n && (is_alpha(s[i]) || is_digit(s[i]))) i++;
            if (is_keyword(s + start, i - start)) {
                for (int k = start; k < i; k++) attr[k] = C_KEYWORD;
            }
            continue;
        }
        i++;
    }
    return in_block;
}

static void put(int row, int col, char ch, unsigned char attr) {
    VGA[row * COLS + col] = (unsigned short)((attr << 8) | (unsigned char)ch);
}

static void put_str(int row, int col, const char *s, unsigned char attr) {
    while (*s && col < COLS) put(row, col++, *s++, attr);
}

static int num_str(char *out, unsigned int v) {
    char tmp[12];
    int n = 0, k = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) out[k++] = tmp[--n];
    out[k] = 0;
    return k;
}

static void cursor_pos(int *line, int *col) {
    int l = 0, c = 0;
    for (unsigned int i = 0; i < cur; i++) {
        if (buf[i] == '\n') {
            l++;
            c = 0;
        } else if (!utf8_is_cont((unsigned char)buf[i])) {
            c++;
        }
    }
    *line = l;
    *col = c;
}

static unsigned int line_start(unsigned int at) {
    while (at > 0 && buf[at - 1] != '\n') at--;
    return at;
}

static unsigned int line_end(unsigned int at) {
    while (at < len && buf[at] != '\n') at++;
    return at;
}

static void hw_cursor(int row, int col) {
    cur_row = row;
    cur_col = col;
    if (VGA == shadow) {
        return;
    }
    unsigned short pos = (unsigned short)(row * COLS + col);
    ed_outb(0x3d4, 0x0a);
    ed_outb(0x3d5, 0x0d);
    ed_outb(0x3d4, 0x0b);
    ed_outb(0x3d5, 0x0e);
    ed_outb(0x3d4, 0x0f);
    ed_outb(0x3d5, (unsigned char)(pos & 0xff));
    ed_outb(0x3d4, 0x0e);
    ed_outb(0x3d5, (unsigned char)(pos >> 8));
}

static void render(void) {
    int cline, ccol;
    cursor_pos(&cline, &ccol);
    if (cline < top) top = cline;
    if (cline >= top + TEXT_ROWS) top = cline - TEXT_ROWS + 1;
    if (ccol < left) left = ccol;
    if (ccol >= left + TEXT_COLS) left = ccol - TEXT_COLS + 1;

    unsigned char attr[COLS * 4];
    int lines = 1;
    for (unsigned int i = 0; i < len; i++) {
        if (buf[i] == '\n') lines++;
    }
    int block = 0;
    int row = 0;
    unsigned int at = 0;
    for (int line = 0; line < lines && row < TEXT_ROWS; line++) {
        unsigned int end = line_end(at);
        int n = (int)(end - at);
        int span = n < (int)sizeof attr ? n : (int)sizeof attr;
        block = color_line(buf + at, span, attr, block);
        if (line >= top) {
            char num[12];
            int w = num_str(num, (unsigned int)line + 1);
            for (int c = 0; c < GUTTER; c++) put(row, c, ' ', C_GUTTER);
            put_str(row, GUTTER - 1 - w, num, C_GUTTER);
            /* Walk characters, not bytes: one cell per UTF-8 character. */
            int k = 0, col = 0;
            while (k < n && col < left) {
                unsigned int cp;
                k += utf8_decode((const unsigned char *)buf + at + k, (unsigned int)(n - k), &cp);
                col++;
            }
            for (int c = 0; c < TEXT_COLS; c++) {
                if (k < n) {
                    unsigned int cp;
                    int a = k < span ? attr[k] : C_TEXT;
                    k += utf8_decode((const unsigned char *)buf + at + k, (unsigned int)(n - k), &cp);
                    put(row, GUTTER + c, (char)vga_cell(cp), (unsigned char)a);
                } else {
                    put(row, GUTTER + c, ' ', C_TEXT);
                }
            }
            row++;
        }
        at = end + 1;
    }
    for (; row < TEXT_ROWS; row++) {
        put(row, 0, '~', C_EMPTY);
        for (int c = 1; c < COLS; c++) put(row, c, ' ', C_TEXT);
    }

    char status[COLS + 1];
    int k = 0;
    const char *parts[] = {" ", name, dirty ? " [+]" : "", "  Ln ", 0};
    for (int p = 0; parts[p]; p++) {
        for (const char *s = parts[p]; *s && k < COLS; s++) status[k++] = *s;
    }
    k += num_str(status + k, (unsigned int)cline + 1);
    status[k++] = ':';
    k += num_str(status + k, (unsigned int)ccol + 1);
    const char *tail = kbd_layout_ru() ? "  RU  ^S save  ^Q/Esc quit  " : "  EN  ^S save  ^Q/Esc quit  ";
    for (const char *s = tail; *s && k < COLS; s++) status[k++] = *s;
    for (const char *s = note; *s && k < COLS; s++) status[k++] = *s;
    while (k < COLS) status[k++] = ' ';
    status[k] = 0;
    put_str(ROWS - 1, 0, status, C_STATUS);
    hw_cursor(cline - top, GUTTER + ccol - left);
    if (VGA == shadow) {
        desktop_editor_paint();
    }
}

static void insert(char ch) {
    if (len + 1 > FS_DATA) {
        note = "file full (64 KB)";
        return;
    }
    for (unsigned int i = len; i > cur; i--) buf[i] = buf[i - 1];
    buf[cur++] = ch;
    len++;
    dirty = 1;
}

static void erase(unsigned int at) {
    if (at >= len) return;
    for (unsigned int i = at; i + 1 < len; i++) buf[i] = buf[i + 1];
    len--;
    dirty = 1;
}

static void move_vertical(int lines) {
    int line, col;
    cursor_pos(&line, &col);
    if (want_col < 0) want_col = col;
    unsigned int at = line_start(cur);
    while (lines < 0 && at > 0) {
        at = line_start(at - 1);
        lines++;
    }
    while (lines > 0) {
        unsigned int end = line_end(at);
        if (end >= len) break;
        at = end + 1;
        lines--;
    }
    unsigned int end = line_end(at);
    cur = at;
    for (int c = 0; c < want_col && cur < end; c++) {
        cur++;
        while (cur < end && utf8_is_cont((unsigned char)buf[cur])) cur++;
    }
}

void journal_add(char type, const char *what);

static void save(void) {
    if (!fs_put(name, buf, len)) {
        note = "save failed";
        console_puts("edit: failed\n");
        return;
    }
    char msg[64] = "edit: saved ";
    int k = 12;
    for (const char *s = name; *s; s++) msg[k++] = *s;
    msg[k++] = ' ';
    msg[k++] = '(';
    k += num_str(msg + k, len);
    for (const char *s = " bytes)\n"; *s; s++) msg[k++] = *s;
    msg[k] = 0;
    console_puts(msg);
    journal_add('s', name);
    dirty = 0;
    note = "saved";
}

static void close_editor(void) {
    active = 0;
    console_puts("edit: closed\n");
    editor_closed();
}

void editor_key(int key) {
    if (!active) return;
    note = "";
    if (key != ED_QUIT) quit_armed = 0;
    if (key != ED_UP && key != ED_DOWN && key != ED_PGUP && key != ED_PGDN) want_col = -1;
    switch (key) {
    case ED_SAVE:
        save();
        break;
    case ED_QUIT:
        if (dirty && !quit_armed) {
            quit_armed = 1;
            note = "unsaved! ^S to save, ^Q/Esc again to drop";
            break;
        }
        close_editor();
        return;
    case ED_LEFT:
        if (cur > 0) cur--;
        while (cur > 0 && utf8_is_cont((unsigned char)buf[cur])) cur--;
        break;
    case ED_RIGHT:
        if (cur < len) cur++;
        while (cur < len && utf8_is_cont((unsigned char)buf[cur])) cur++;
        break;
    case ED_REDRAW:
        break;
    case ED_UP:
        move_vertical(-1);
        break;
    case ED_DOWN:
        move_vertical(1);
        break;
    case ED_PGUP:
        move_vertical(-(TEXT_ROWS - 1));
        break;
    case ED_PGDN:
        move_vertical(TEXT_ROWS - 1);
        break;
    case ED_HOME:
        cur = line_start(cur);
        break;
    case ED_END:
        cur = line_end(cur);
        break;
    case ED_DELETE:
        erase(cur);
        while (cur < len && utf8_is_cont((unsigned char)buf[cur])) erase(cur);
        break;
    case '\b':
        if (cur > 0) {
            cur--;
            while (cur > 0 && utf8_is_cont((unsigned char)buf[cur])) cur--;
            erase(cur);
            while (cur < len && utf8_is_cont((unsigned char)buf[cur])) erase(cur);
        }
        break;
    case ED_TAB:
        for (int i = 0; i < 4; i++) insert(' ');
        break;
    default:
        if (key == '\n' || (key >= 32 && key < 127) || (key >= 0x80 && key < 0x100)) insert((char)key);
        if (key >= 0xc0 && key < 0x100) return; /* rest of the UTF-8 character follows */
        break;
    }
    render();
}

int editor_open(const char *file) {
    int n = 0;
    while (file[n] == ' ') file++;
    while (file[n] && file[n] != ' ' && n + 1 < FS_NAME) {
        name[n] = file[n];
        n++;
    }
    name[n] = 0;
    if (n == 0) {
        console_puts("edit: need a file name\n");
        return 0;
    }
    const unsigned char *data = 0;
    unsigned int size = 0;
    int exists = fs_get(name, &data, &size);
    len = 0;
    if (exists) {
        for (unsigned int i = 0; i < size && i < FS_DATA; i++) buf[len++] = (char)data[i];
    }
    cur = 0;
    top = 0;
    left = 0;
    want_col = -1;
    dirty = 0;
    quit_armed = 0;
    lang = LANG_NONE;
    if (ends_with(name, ".c") || ends_with(name, ".h")) lang = LANG_C;
    if (ends_with(name, ".asm") || ends_with(name, ".s")) lang = LANG_ASM;
    if (ends_with(name, ".sh")) lang = LANG_SH;
    note = exists ? "" : "new file";
    /* Serial log keeps the old one-line form so scripts can see the text. */
    console_puts(exists ? "edit: " : "edit: new ");
    if (!exists) console_puts(name);
    {
        char line[128];
        unsigned int k = 0;
        for (unsigned int i = 0; i < len && k + 2 < sizeof line; i++) line[k++] = buf[i] == '\n' ? ' ' : buf[i];
        line[k++] = '\n';
        line[k] = 0;
        console_puts(line);
    }
    active = 1;
    VGA = desktop_active() ? shadow : (volatile unsigned short *)0xb8000;
    render();
    return 1;
}
