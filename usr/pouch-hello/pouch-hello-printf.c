// /pouch-hello-printf — the printf(3) pouch hello (Phase 6 sub-chunk 6c).
//
// The first POSIX C program Thylacine runs that exercises the *compiler
// runtime*. /pouch-hello uses the raw write(2) seam and /pouch-hello-stdio
// uses buffered stdio via puts()/fwrite(); this binary uses printf(3) —
// and printf pulls musl's vfprintf, whose format engine is the first
// thing in the project to need compiler-rt builtins.
//
// Why printf needs the compiler runtime: vfprintf formats floating point
// through fmt_fp(long double). On aarch64 `long double` is IEEE binary128,
// which has no hardware support — so the format math compiles to calls
// into the soft-float builtins __addtf3 / __subtf3 / __multf3 /
// __extenddftf2 / __fixtfdi / ... Those are what sub-chunk 6b's
// libclang_rt.builtins.a provides; tools/pouch-ld groups it with libc.
// Linking any printf pulls vfprintf.o, which references those builtins
// unconditionally — so this binary cannot link without the runtime.
//
// The binary is a self-test. It snprintf()s an integer and a
// floating-point value and strcmp()s the result against the
// known-correct string:
//   - the %d check proves the *link* — vfprintf.o, hence the runtime,
//     is pulled in.
//   - the %.2f check proves the runtime *runs*: vfprintf's fmt_fp does
//     its decimal-digit extraction in binary128 long double, so a broken
//     __subtf3 / __multf3 / __fixtfdi would format the wrong digits.
// main returns non-zero on any mismatch — joey treats that as a boot
// regression. (1936 is the year the last known thylacine died.)
//
// fd 1 is a pipe write-end joey relays to the boot-log UART and
// content-checks. Cross-compiled with tools/pouch-clang against the
// pouch sysroot.

#include <stdio.h>
#include <string.h>

int main(void) {
    printf("pouch-hello-printf: hello via printf(3) (pouch libc + compiler-rt)\n");

    char buf[64];

    // %d — proves the link: any printf pulls musl's vfprintf, and
    // vfprintf.o references the binary128 soft-float builtins
    // unconditionally, so this binary cannot link without the runtime.
    int n = snprintf(buf, sizeof(buf), "%d", 1936);
    if (n != 4 || strcmp(buf, "1936") != 0) {
        printf("pouch-hello-printf: FAIL %%d format -> \"%s\"\n", buf);
        return 1;
    }
    printf("pouch-hello-printf: int ok - snprintf gave \"%s\"\n", buf);

    // %.2f — proves the runtime runs: vfprintf's fmt_fp extracts decimal
    // digits in binary128 long double. 3.140625 is exact in binary, so
    // %.2f is the unambiguous "3.14"; a broken soft-float builtin would
    // print the wrong digits here.
    n = snprintf(buf, sizeof(buf), "%.2f", 3.140625);
    if (n != 4 || strcmp(buf, "3.14") != 0) {
        printf("pouch-hello-printf: FAIL %%.2f format -> \"%s\"\n", buf);
        return 1;
    }
    printf("pouch-hello-printf: float ok - snprintf gave \"%s\" (binary128 soft-float)\n", buf);

    printf("pouch-hello-printf: exit 0\n");
    return 0;
}
