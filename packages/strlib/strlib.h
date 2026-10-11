/* strlib 1.0 (needs mathlib): look up with sym("strlib", "upper") etc. */
typedef unsigned int (*strlib_len)(const char *s);
typedef char *(*strlib_upper)(char *s);
typedef char *(*strlib_reverse)(char *s);
/* Writes the decimal form of n into buf (12 bytes), returns buf. */
typedef char *(*strlib_utoa)(unsigned int n, char *buf);
