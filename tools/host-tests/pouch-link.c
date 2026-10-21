#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>
/* The AT_ values and SYS_linkat are the musl tree's, from the driver: the host's differ. */
static int calls, failures, checks;
/* The seam's answer for the 0xFFFF sentinel (P-3), counted so a trap shows; only a trapping libc calls it. */
__attribute__((unused)) static long sys_any(long n, ...) { (void)n; calls++; return -ENOSYS; }
#define __syscall(...) sys_any(__VA_ARGS__)
static long __syscall_ret(unsigned long r) { if (r > -4096UL) { errno = -(long)r; return -1; } return (long)r; }
#define syscall(...) __syscall_ret(__syscall(__VA_ARGS__))
#define link tested_link
#define linkat tested_linkat
#include "link-under-test.c"
#include "linkat-under-test.c"
#undef link
#undef linkat
#define CASE(name, expr) do { ++checks; int ok = (expr); printf("%s %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; } while (0)
#define RESET() do { calls = 0; errno = EDOM; } while (0)
static int refused(int r, int e) { return r == -1 && errno == e && calls == 0; }
int main(void) {
 /* No hard link is made and nothing traps: EPERM, Linux's answer for a filesystem without them. */
 RESET(); CASE("link-eperm", refused(tested_link("a", "b"), EPERM));
 /* No path is read: null paths are EPERM, not a fault (ASan would see the read). */
 RESET(); CASE("link-no-path-read", refused(tested_link(0, 0), EPERM));
 RESET(); CASE("linkat-eperm", refused(tested_linkat(AT_FDCWD, "a", AT_FDCWD, "b", 0), EPERM));
 RESET(); CASE("linkat-follow-eperm", refused(tested_linkat(AT_FDCWD, "a", AT_FDCWD, "b", AT_SYMLINK_FOLLOW), EPERM));
 RESET(); CASE("linkat-empty-path-eperm", refused(tested_linkat(3, "", AT_FDCWD, "b", AT_EMPTY_PATH), EPERM));
 RESET(); CASE("linkat-both-flags-eperm", refused(tested_linkat(AT_FDCWD, "a", AT_FDCWD, "b", AT_SYMLINK_FOLLOW | AT_EMPTY_PATH), EPERM));
 RESET(); CASE("linkat-no-fd-or-path-read", refused(tested_linkat(-1, 0, INT_MAX, 0, 0), EPERM));
 /* Any other flag is EINVAL first, as Linux: AT_SYMLINK_NOFOLLOW too, and beside an allowed one. */
 RESET(); CASE("linkat-nofollow-einval", refused(tested_linkat(AT_FDCWD, "a", AT_FDCWD, "b", AT_SYMLINK_NOFOLLOW), EINVAL));
 RESET(); CASE("linkat-mixed-einval", refused(tested_linkat(AT_FDCWD, "a", AT_FDCWD, "b", AT_SYMLINK_FOLLOW | AT_NO_AUTOMOUNT), EINVAL));
 RESET(); CASE("linkat-einval-before-eperm", refused(tested_linkat(-1, 0, -1, 0, AT_REMOVEDIR), EINVAL));
 /* Each single bit: the two Linux takes are EPERM, every other EINVAL (bit 31 is a negative flag). */
 int bad = -1;
 for (int b = 0; b < 32 && bad < 0; b++) {
  int flag = (int)(1u << b), want = (flag & (AT_SYMLINK_FOLLOW | AT_EMPTY_PATH)) ? EPERM : EINVAL;
  RESET(); if (!refused(tested_linkat(AT_FDCWD, "a", AT_FDCWD, "b", flag), want)) bad = b;
 }
 CASE("linkat-every-bit", bad < 0);
 if (bad >= 0) printf("  first wrong bit: %d\n", bad);
 RESET(); CASE("linkat-all-bits-einval", refused(tested_linkat(AT_FDCWD, "a", AT_FDCWD, "b", -1), EINVAL));
 printf("tests=%d failures=%d\n", checks, failures); return !!failures;
}
