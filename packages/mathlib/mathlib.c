#include "../genesis_pkg.h"

GENESIS_LIBRARY

static unsigned int pow(unsigned int base, unsigned int exp) {
    unsigned int r = 1;
    while (exp-- > 0) {
        r *= base;
    }
    return r;
}

static unsigned int gcd(unsigned int a, unsigned int b) {
    while (b != 0) {
        unsigned int t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static unsigned int isqrt(unsigned int n) {
    unsigned int r = 0;
    while ((r + 1) * (r + 1) <= n) {
        r++;
    }
    return r;
}

static unsigned int udivmod(unsigned int n, unsigned int d, unsigned int *rem) {
    *rem = d == 0 ? 0 : n % d;
    return d == 0 ? 0 : n / d;
}

void *lib_lookup(const char *sym, pkg_sym_fn import) {
    (void)import;
    PKG_EXPORT(pow)
    PKG_EXPORT(gcd)
    PKG_EXPORT(isqrt)
    PKG_EXPORT(udivmod)
    return 0;
}
