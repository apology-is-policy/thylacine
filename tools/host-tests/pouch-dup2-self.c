#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>
#define POUCH_SOCK_TAG 0x40000000
#define SYS_thyla_handle_flags_get 129
static long reply;
static int queries, resolves, failures;
struct pouch_sock_slot { int in_use; };
static struct pouch_sock_slot slot;
static __attribute__((unused)) struct pouch_sock_slot *pouch_sock_resolve(int fd) {
    ++resolves;
    return fd == POUCH_SOCK_TAG && slot.in_use ? &slot : 0;
}
static __attribute__((unused)) long __syscall1(long number, long fd) {
    assert(number == SYS_thyla_handle_flags_get && fd >= 0 && !(fd & POUCH_SOCK_TAG));
    ++queries;
    return reply;
}
#define dup2 tested_dup2
#include "dup2-under-test.c"
#undef dup2
static void check(const char *name, int old, int new, long result,
                  int want, int error, int query_count, int resolve_count) {
    queries = resolves = 0; reply = result; errno = EDOM;
    int got = tested_dup2(old, new);
    int ok = got == want && errno == error && queries == query_count && resolves == resolve_count;
    printf("%s %s result=%d errno=%d query=%d resolve=%d\n", ok ? "PASS" : "FAIL", name, got, errno, queries, resolves);
    failures += !ok;
}
int main(void) {
    check("live-native", 8, 8, 0, 8, EDOM, 1, 0);
    check("cloexec-native-preserved", 8, 8, 1, 8, EDOM, 1, 0);
    check("absent-native", 8, 8, -EBADF, -1, EBADF, 1, 0);
    check("negative", -1, -1, 0, -1, EBADF, 0, 0);
    check("out-of-range-native", 1024, 1024, -EBADF, -1, EBADF, 1, 0);
    check("legacy-kernel", 8, 8, -1, -1, ENOSYS, 1, 0);
    check("explicit-enosys", 8, 8, -ENOSYS, -1, ENOSYS, 1, 0);
    check("query-error", 8, 8, -EINVAL, -1, EINVAL, 1, 0);
    slot.in_use = 1;
    check("fresh-socket-no-kernel-handle", POUCH_SOCK_TAG, POUCH_SOCK_TAG, -EBADF, POUCH_SOCK_TAG, EDOM, 0, 1);
    slot.in_use = 0;
    check("closed-socket", POUCH_SOCK_TAG, POUCH_SOCK_TAG, 0, -1, EBADF, 0, 1);
    check("malformed-socket-tag", INT_MAX, INT_MAX, 0, -1, EBADF, 0, 1);
    check("distinct-still-unsupported", 8, 9, 0, -1, ENOSYS, 0, 0);
    printf("tests=12 failures=%d\n", failures);
    return !!failures;
}
