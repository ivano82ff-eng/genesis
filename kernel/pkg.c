/* pkg: a tiny package manager. The package repository is baked into the
 * kernel image by the host build (build/pkgrepo.bin, see src/packages.ts);
 * there is no TCP/IP, so "download" means copying from that repository onto
 * Grid Store. Libraries are position-independent flat binaries that start
 * with "GLIB" and a jump to `void *lookup(const char *sym, pkg_sym_fn import)`.
 * Programs get pkg_sym as their third argument and resolve symbols at run time. */
#include "fs_limits.h"

void console_puts(const char *text);
void console_putc(char ch);
int fs_get(const char *name, const unsigned char **data, unsigned int *length);
int fs_put(const char *name, const char *data, unsigned int length);
int fs_delete(const char *name);
int fs_free_slots(void);
void *kheap_alloc(unsigned int size);

extern const unsigned char pkg_repo[];
extern const unsigned char pkg_repo_end[];

typedef void *(*pkg_sym_fn)(const char *lib, const char *sym);
typedef void *(*lib_lookup_fn)(const char *sym, pkg_sym_fn import);

enum { LINE_CAP = 160, TOK_MAX = 6, DEPTH_MAX = 8, CACHE_MAX = 8 };

/* Installed files live in /lib (GLIB libraries) and /bin (programs). */
static void pkg_path(char *out, const char *dir, const char *name) {
    unsigned int n = 0;
    while (*dir != 0) {
        out[n++] = *dir++;
    }
    while (*name != 0 && n + 1 < FS_NAME) {
        out[n++] = *name++;
    }
    out[n] = 0;
}

static int pkg_lib(const char *name, const unsigned char **data, unsigned int *length) {
    char p[FS_NAME];
    pkg_path(p, "/lib/", name);
    return fs_get(p, data, length);
}

static int pkg_get(const char *name, const unsigned char **data, unsigned int *length) {
    char p[FS_NAME];
    if (pkg_lib(name, data, length)) {
        return 1;
    }
    pkg_path(p, "/bin/", name);
    return fs_get(p, data, length);
}

static int pkg_put(const char *name, const char *data, unsigned int length) {
    char p[FS_NAME];
    int lib = length >= 4 && data[0] == 'G' && data[1] == 'L' && data[2] == 'I' && data[3] == 'B';
    pkg_path(p, lib ? "/lib/" : "/bin/", name);
    return fs_put(p, data, length);
}

static void pkg_del(const char *name) {
    char p[FS_NAME];
    pkg_path(p, "/lib/", name);
    if (!fs_delete(p)) {
        pkg_path(p, "/bin/", name);
        fs_delete(p);
    }
}

static int s_eq(const char *a, const char *b) {
    while (*a != 0 && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void s_copy(char *d, const char *s, unsigned int cap) {
    unsigned int i = 0;
    while (s[i] != 0 && i + 1 < cap) {
        d[i] = s[i];
        i++;
    }
    d[i] = 0;
}

static unsigned int s_num(const char *s) {
    unsigned int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (unsigned int)(*s++ - '0');
    }
    return v;
}

static void put_num(unsigned int v) {
    char d[12];
    int n = 0;
    do {
        d[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0);
    while (n > 0) {
        console_putc(d[--n]);
    }
}

/* Index: text lines up to a NUL, then the file blobs.
 *   P name version deps|- description...
 *   F package file offset length          (offset into the blob area) */
static unsigned int index_len(void) {
    unsigned int n = 0;
    unsigned int total = (unsigned int)(pkg_repo_end - pkg_repo);
    while (n < total && pkg_repo[n] != 0) {
        n++;
    }
    return n;
}

static const unsigned char *blob_base(void) {
    return pkg_repo + index_len() + 1;
}

/* Reads the next line into buf and splits it; the last token keeps spaces. */
static int next_line(unsigned int *pos, char *buf, char **tok, int *count) {
    unsigned int end = index_len();
    if (*pos >= end) {
        return 0;
    }
    unsigned int n = 0;
    while (*pos < end && pkg_repo[*pos] != '\n') {
        if (n + 1 < LINE_CAP) {
            buf[n++] = (char)pkg_repo[*pos];
        }
        (*pos)++;
    }
    (*pos)++;
    buf[n] = 0;
    int c = 0;
    char *p = buf;
    while (*p != 0 && c < TOK_MAX) {
        tok[c++] = p;
        if (c == TOK_MAX - 1 && buf[0] == 'P') {
            break; /* description: rest of the line */
        }
        while (*p != 0 && *p != ' ') {
            p++;
        }
        if (*p == ' ') {
            *p++ = 0;
        }
    }
    *count = c;
    return 1;
}

static int pkg_exists(const char *name) {
    char buf[LINE_CAP];
    char *t[TOK_MAX];
    int c;
    unsigned int pos = 0;
    while (next_line(&pos, buf, t, &c)) {
        if (c >= 4 && t[0][0] == 'P' && s_eq(t[1], name)) {
            return 1;
        }
    }
    return 0;
}

/* Copies the deps field of NAME into out ("-" when none). */
static int pkg_field(const char *name, int field, char *out, unsigned int cap) {
    char buf[LINE_CAP];
    char *t[TOK_MAX];
    int c;
    unsigned int pos = 0;
    while (next_line(&pos, buf, t, &c)) {
        if (c > field && t[0][0] == 'P' && s_eq(t[1], name)) {
            s_copy(out, t[field], cap);
            return 1;
        }
    }
    out[0] = 0;
    return 0;
}

/* Number of NAME's files missing from Grid Store; -1 if it has none listed. */
static int pkg_missing(const char *name) {
    char buf[LINE_CAP];
    char *t[TOK_MAX];
    int c;
    int files = 0;
    int missing = 0;
    unsigned int pos = 0;
    while (next_line(&pos, buf, t, &c)) {
        if (c >= 5 && t[0][0] == 'F' && s_eq(t[1], name)) {
            const unsigned char *data;
            unsigned int length;
            files++;
            if (!pkg_get(t[2], &data, &length)) {
                missing++;
            }
        }
    }
    return files == 0 ? -1 : missing;
}

static int pkg_installed(const char *name) {
    return pkg_missing(name) == 0;
}

/* Calls fn for each comma-separated dependency of NAME. */
static int each_dep(const char *name, int (*fn)(const char *, int), int depth) {
    char deps[LINE_CAP];
    pkg_field(name, 3, deps, sizeof deps);
    if (s_eq(deps, "-")) {
        return 1;
    }
    char *p = deps;
    while (*p != 0) {
        char *start = p;
        while (*p != 0 && *p != ',') {
            p++;
        }
        int more = *p == ',';
        *p = 0;
        if (!fn(start, depth + 1)) {
            return 0;
        }
        if (more) {
            p++;
        }
    }
    return 1;
}

static int need_total;

static int count_need(const char *name, int depth) {
    if (depth > DEPTH_MAX) {
        console_puts("pkg: dependency loop at ");
        console_puts(name);
        console_putc('\n');
        return 0;
    }
    if (!pkg_exists(name)) {
        console_puts("pkg: no package ");
        console_puts(name);
        console_putc('\n');
        return 0;
    }
    int missing = pkg_missing(name);
    if (missing > 0) {
        need_total += missing;
    }
    return each_dep(name, count_need, depth);
}

static int install_one(const char *name, int depth) {
    if (!each_dep(name, install_one, depth)) {
        return 0;
    }
    if (pkg_installed(name)) {
        return 1;
    }
    char buf[LINE_CAP];
    char *t[TOK_MAX];
    int c;
    unsigned int pos = 0;
    const unsigned char *blobs = blob_base();
    while (next_line(&pos, buf, t, &c)) {
        if (c >= 5 && t[0][0] == 'F' && s_eq(t[1], name)) {
            if (!pkg_put(t[2], (const char *)(blobs + s_num(t[3])), s_num(t[4]))) {
                console_puts("pkg: cannot write ");
                console_puts(t[2]);
                console_putc('\n');
                return 0;
            }
        }
    }
    char version[32];
    pkg_field(name, 2, version, sizeof version);
    console_puts("pkg: installed ");
    console_puts(name);
    console_putc(' ');
    console_puts(version);
    console_putc('\n');
    return 1;
}

static void pkg_install(const char *name) {
    if (name[0] == 0) {
        console_puts("pkg: install NAME\n");
        return;
    }
    if (pkg_exists(name) && pkg_installed(name)) {
        console_puts("pkg: ");
        console_puts(name);
        console_puts(" already installed\n");
        return;
    }
    need_total = 0;
    if (!count_need(name, 0)) {
        return;
    }
    int free_slots = fs_free_slots();
    if (need_total > free_slots) {
        console_puts("pkg: need ");
        put_num((unsigned int)need_total);
        console_puts(" free file slots, have ");
        put_num((unsigned int)free_slots);
        console_puts(" (rm something first)\n");
        return;
    }
    install_one(name, 0);
}

static void pkg_list(int only_installed) {
    char buf[LINE_CAP];
    char *t[TOK_MAX];
    int c;
    unsigned int pos = 0;
    int any = 0;
    if (only_installed) {
        console_puts("pkg: installed:");
    }
    while (next_line(&pos, buf, t, &c)) {
        if (c < 4 || t[0][0] != 'P') {
            continue;
        }
        char name[FS_NAME];
        s_copy(name, t[1], sizeof name);
        int installed = pkg_installed(name);
        if (only_installed) {
            if (installed) {
                console_putc(' ');
                console_puts(name);
                any = 1;
            }
            continue;
        }
        console_puts("pkg: ");
        console_puts(name);
        console_putc(' ');
        char field[LINE_CAP];
        pkg_field(name, 2, field, sizeof field);
        console_puts(field);
        console_puts(installed ? " [installed]" : "");
        pkg_field(name, 3, field, sizeof field);
        if (!s_eq(field, "-")) {
            console_puts(" deps ");
            console_puts(field);
        }
        pkg_field(name, 4, field, sizeof field);
        if (field[0] != 0) {
            console_puts(" - ");
            console_puts(field);
        }
        console_putc('\n');
    }
    if (only_installed) {
        console_puts(any ? "\n" : " none\n");
    }
}

static const char *remove_target;
static int dep_hit;

static int dep_is_target(const char *dep, int depth) {
    (void)depth;
    if (s_eq(dep, remove_target)) {
        dep_hit = 1;
    }
    return 1;
}

static void cache_drop(const char *name);

static void pkg_remove(const char *name) {
    if (!pkg_exists(name) || pkg_missing(name) > 0) {
        console_puts("pkg: ");
        console_puts(name);
        console_puts(" not installed\n");
        return;
    }
    char buf[LINE_CAP];
    char *t[TOK_MAX];
    int c;
    unsigned int pos = 0;
    while (next_line(&pos, buf, t, &c)) {
        if (c < 4 || t[0][0] != 'P' || s_eq(t[1], name)) {
            continue;
        }
        char other[FS_NAME];
        s_copy(other, t[1], sizeof other);
        remove_target = name;
        dep_hit = 0;
        each_dep(other, dep_is_target, 0);
        if (dep_hit && pkg_installed(other)) {
            console_puts("pkg: ");
            console_puts(name);
            console_puts(" needed by ");
            console_puts(other);
            console_putc('\n');
            return;
        }
    }
    pos = 0;
    while (next_line(&pos, buf, t, &c)) {
        if (c >= 5 && t[0][0] == 'F' && s_eq(t[1], name)) {
            pkg_del(t[2]);
            cache_drop(t[2]);
        }
    }
    console_puts("pkg: removed ");
    console_puts(name);
    console_putc('\n');
}

void cmd_pkg(const char *args) {
    while (*args == ' ') {
        args++;
    }
    if (s_eq(args, "list")) {
        pkg_list(0);
        return;
    }
    if (s_eq(args, "installed")) {
        pkg_list(1);
        return;
    }
    if (args[0] == 'i' && args[1] == 'n' && args[2] == 's' && args[3] == 't' && args[4] == 'a' &&
        args[5] == 'l' && args[6] == 'l' && (args[7] == ' ' || args[7] == 0)) {
        pkg_install(args[7] == ' ' ? args + 8 : "");
        return;
    }
    if (args[0] == 'r' && args[1] == 'e' && args[2] == 'm' && args[3] == 'o' && args[4] == 'v' &&
        args[5] == 'e' && args[6] == ' ') {
        pkg_remove(args + 7);
        return;
    }
    console_puts("pkg: list | installed | install NAME | remove NAME\n");
}

/* Runtime linking. Loaded libraries are cached by name; the cache entry is
 * dropped as soon as the file is gone from Grid Store. */
static struct {
    char name[FS_NAME];
    const unsigned char *src;
    unsigned char *code;
} cache[CACHE_MAX];

static void cache_drop(const char *name) {
    for (int i = 0; i < CACHE_MAX; i++) {
        if (cache[i].code != 0 && s_eq(cache[i].name, name)) {
            cache[i].code = 0;
        }
    }
}

void *pkg_sym(const char *lib, const char *sym) {
    const unsigned char *data;
    unsigned int length;
    if (lib == 0 || sym == 0 || !pkg_lib(lib, &data, &length)) {
        if (lib != 0) {
            cache_drop(lib);
        }
        return 0;
    }
    if (length < 6 || data[0] != 'G' || data[1] != 'L' || data[2] != 'I' || data[3] != 'B') {
        return 0;
    }
    unsigned char *code = 0;
    int slot = -1;
    for (int i = 0; i < CACHE_MAX; i++) {
        if (cache[i].code != 0 && s_eq(cache[i].name, lib) && cache[i].src == data) {
            code = cache[i].code;
        } else if (cache[i].code == 0 && slot < 0) {
            slot = i;
        }
    }
    if (code == 0) {
        code = kheap_alloc(length);
        if (code == 0) {
            return 0;
        }
        for (unsigned int i = 0; i < length; i++) {
            code[i] = data[i];
        }
        if (slot >= 0) {
            s_copy(cache[slot].name, lib, FS_NAME);
            cache[slot].src = data;
            cache[slot].code = code;
        }
    }
    lib_lookup_fn lookup = (lib_lookup_fn)(void *)(code + 4);
    return lookup(sym, pkg_sym);
}

/* For the desktop: the INDEX-th package, whether it is installed and
 * whether it is a program (its blob is not a GLIB library). */
int pkg_info(int index, char *name, unsigned int cap, int *installed, int *app) {
    char buf[LINE_CAP];
    char *t[TOK_MAX];
    int c;
    unsigned int pos = 0;
    int seen = 0;
    while (next_line(&pos, buf, t, &c)) {
        if (c < 4 || t[0][0] != 'P') {
            continue;
        }
        if (seen++ != index) {
            continue;
        }
        s_copy(name, t[1], cap);
        *installed = pkg_installed(name);
        *app = 0;
        unsigned int p2 = 0;
        while (next_line(&p2, buf, t, &c)) {
            if (c >= 5 && t[0][0] == 'F' && s_eq(t[1], name)) {
                const unsigned char *b = blob_base() + s_num(t[3]);
                if (!(s_num(t[4]) >= 4 && b[0] == 'G' && b[1] == 'L' && b[2] == 'I' && b[3] == 'B')) {
                    *app = 1;
                }
            }
        }
        return 1;
    }
    return 0;
}
