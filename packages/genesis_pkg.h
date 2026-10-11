/* Shared header for package sources (built position-independent, see src/packages.ts). */
#pragma once

typedef void (*print_fn)(const char *text);
typedef void (*sleep_fn)(unsigned int ticks);
/* Returns the address of SYM exported by installed library LIB, or 0. */
typedef void *(*pkg_sym_fn)(const char *lib, const char *sym);

static inline int pkg_streq(const char *a, const char *b) {
    while (*a != 0 && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* A library defines `void *lib_lookup(const char *sym, pkg_sym_fn import)`
 * and puts GENESIS_LIBRARY once in its source: magic + jump at offset 0. */
#define GENESIS_LIBRARY \
    __asm__(".pushsection .text.entry,\"ax\"\n.ascii \"GLIB\"\njmp lib_lookup\n.popsection");
#define PKG_EXPORT(fn) \
    if (pkg_streq(sym, #fn)) return (void *)fn;

/* A program marks its entry function with GENESIS_PROGRAM:
 * GENESIS_PROGRAM void main_(print_fn print, sleep_fn sleep, pkg_sym_fn sym) */
#define GENESIS_PROGRAM __attribute__((section(".text.entry"), used))
