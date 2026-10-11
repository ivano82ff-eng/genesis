#include "../genesis_pkg.h"
#include "../mathlib/mathlib.h"

GENESIS_LIBRARY

static pkg_sym_fn imports;

static unsigned int len(const char *s) {
    unsigned int n = 0;
    while (s[n] != 0) {
        n++;
    }
    return n;
}

static char *upper(char *s) {
    for (char *p = s; *p != 0; p++) {
        if (*p >= 'a' && *p <= 'z') {
            *p = (char)(*p - 'a' + 'A');
        }
    }
    return s;
}

static char *reverse(char *s) {
    unsigned int n = len(s);
    for (unsigned int i = 0; i < n / 2; i++) {
        char t = s[i];
        s[i] = s[n - 1 - i];
        s[n - 1 - i] = t;
    }
    return s;
}

/* Uses mathlib.udivmod: the dependency is real, not decorative. */
static char *utoa(unsigned int n, char *buf) {
    mathlib_udivmod divmod = imports ? (mathlib_udivmod)imports("mathlib", "udivmod") : 0;
    unsigned int i = 0;
    if (divmod == 0) {
        buf[0] = '?';
        buf[1] = 0;
        return buf;
    }
    do {
        unsigned int rem;
        n = divmod(n, 10, &rem);
        buf[i++] = (char)('0' + rem);
    } while (n != 0 && i < 11);
    buf[i] = 0;
    return reverse(buf);
}

void *lib_lookup(const char *sym, pkg_sym_fn import) {
    imports = import;
    PKG_EXPORT(len)
    PKG_EXPORT(upper)
    PKG_EXPORT(reverse)
    PKG_EXPORT(utoa)
    return 0;
}
