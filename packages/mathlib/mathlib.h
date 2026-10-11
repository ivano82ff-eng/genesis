/* mathlib 1.0: look up with sym("mathlib", "pow") etc. */
typedef unsigned int (*mathlib_pow)(unsigned int base, unsigned int exp);
typedef unsigned int (*mathlib_gcd)(unsigned int a, unsigned int b);
typedef unsigned int (*mathlib_isqrt)(unsigned int n);
typedef unsigned int (*mathlib_udivmod)(unsigned int n, unsigned int d, unsigned int *rem);
