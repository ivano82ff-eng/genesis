#include "../genesis_pkg.h"
#include "../mathlib/mathlib.h"
#include "../strlib/strlib.h"

GENESIS_PROGRAM void strdemo_main(print_fn print, sleep_fn sleep, pkg_sym_fn sym) {
    (void)sleep;
    strlib_upper upper = (strlib_upper)sym("strlib", "upper");
    strlib_reverse reverse = (strlib_reverse)sym("strlib", "reverse");
    strlib_utoa utoa = (strlib_utoa)sym("strlib", "utoa");
    mathlib_pow pow = (mathlib_pow)sym("mathlib", "pow");
    if (upper == 0 || reverse == 0 || utoa == 0) {
        print("strdemo: missing library strlib (pkg install strlib)\n");
        return;
    }
    if (pow == 0) {
        print("strdemo: missing library mathlib (pkg install mathlib)\n");
        return;
    }
    char text[] = "hello genesis";
    print("strdemo: ");
    print(upper(text));
    print("\nstrdemo: ");
    print(reverse(text));
    char num[12];
    print("\nstrdemo: 2^10=");
    print(utoa(pow(2, 10), num));
    print("\n");
}
