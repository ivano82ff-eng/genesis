#include "api.h"

static unsigned char prog_heap[GENESIS_PROG_HEAP];
static unsigned int prog_used;

void *prog_malloc(unsigned int size) {
    if (size == 0) {
        size = 1;
    }
    size = (size + 15u) & ~15u;
    if (size > GENESIS_PROG_HEAP - prog_used) {
        return 0;
    }
    void *block = prog_heap + prog_used;
    prog_used += size;
    return block;
}

void prog_free(void *ptr) {
    (void)ptr;
}

void *memset(void *s, int c, unsigned long n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}

void *memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *D = (unsigned char *)d;
    const unsigned char *S = (const unsigned char *)s;
    while (n--) {
        *D++ = *S++;
    }
    return d;
}

void *memmove(void *d, const void *s, unsigned long n) {
    unsigned char *D = (unsigned char *)d;
    const unsigned char *S = (const unsigned char *)s;
    if (D == S || n == 0) {
        return d;
    }
    if (D < S) {
        while (n--) {
            *D++ = *S++;
        }
    } else {
        D += n;
        S += n;
        while (n--) {
            *--D = *--S;
        }
    }
    return d;
}

int abs(int x) {
    return x < 0 ? -x : x;
}

void *malloc(unsigned long size) {
    return prog_malloc((unsigned int)size);
}

void free(void *ptr) {
    prog_free(ptr);
}

void *realloc(void *ptr, unsigned long size) {
    if (ptr == 0) {
        return malloc(size);
    }
    if (size == 0) {
        return 0;
    }
    void *block = malloc(size);
    if (block == 0) {
        return 0;
    }
    memcpy(block, ptr, size);
    return block;
}
