/* Executes the series-produced posix_spawn C, not a second interpreter.
 * Mock only native handles/spawn/open/close. The host's POSIX ABI is not the
 * target ABI; the separate Pouch cross-compile checks that translation unit. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sys/types.h>
#include <signal.h>

typedef struct { int __flags; } posix_spawnattr_t;
typedef struct { void *__actions; } posix_spawn_file_actions_t;
#include "fdop-test.h"
#define hidden
#include "_pouch_process.h"

static int deny_probe, query_error;
#define SYS_thyla_handle_flags_get 129
#ifndef QUERY_MODE
#define QUERY_MODE 1
#endif
static int handles[256], spawned[16], nspawned, calls, opens, fails;
static int alloc_handle(int object) {
    for (int i = 0; i < 256; ++i) if (!handles[i]) {
        handles[i] = object; return i;
    }
    return -1;
}
static int mock_close(int fd) {
    assert(fd >= 0 && fd < 256 && handles[fd]);
    handles[fd] = 0; return 0;
}
static int mock_open(const char *path, int flags, ...) {
    (void)flags; ++opens;
    if (!strcmp(path, "fail")) { errno = EACCES; return -1; }
    return alloc_handle(900);
}
static long __attribute__((unused)) mock_syscall2(long number, long fd, long rights) {
    assert(number == SYS_thyla_dup);
    assert(rights == POUCH_RIGHT_READ || rights == POUCH_RIGHT_WRITE);
    if (fd < 0 || fd >= 256 || !handles[fd] || fd == deny_probe) return -1;
    return alloc_handle(handles[fd]);
}
static long mock_syscall1(long number, long arg) {
    if (number == SYS_thyla_handle_flags_get) {
        if (query_error) return -query_error;
        return arg < 0 || arg >= 256 || !handles[arg] ? -EBADF : 0;
    }
    if (number == SYS_thyla_close) return mock_close((int)arg);
    assert(number == SYS_thyla_spawn_full_argv);
    struct pouch_spawn_args *sa = (void *)(uintptr_t)arg;
    unsigned int *fds = (void *)(uintptr_t)sa->fd_list_va;
    ++calls; nspawned = sa->fd_count;
    assert(nspawned <= 16);
    for (int i = 0; i < nspawned; ++i) {
        if (fds[i] >= 256 || !handles[fds[i]]) return -1;
        spawned[i] = handles[fds[i]];
    }
    return 42;
}
#define posix_spawn pouch_spawn_under_test
#define open mock_open
#define close mock_close
#define __syscall1 mock_syscall1
#define __syscall2 mock_syscall2
#include "spawn-under-test.c"
#undef posix_spawn
#undef open
#undef close

static struct fdop *head;
static void add(int cmd, int dst, int src) {
    struct fdop *op = calloc(1, sizeof(*op) + 16);
    assert(op); op->cmd = cmd; op->fd = dst; op->srcfd = src;
    strcpy(op->path, "fixture");
    op->next = head;
    if (head) head->prev = op;
    head = op;
}
static void reset(void) {
    while (head) { struct fdop *next = head->next; free(head); head = next; }
    memset(handles, 0, sizeof handles);
    memset(spawned, 0, sizeof spawned);
    for (int i = 0; i < 3; ++i) handles[i] = 100 + i;
    handles[8] = 108; handles[100] = 200;
    calls = opens = nspawned = 0; deny_probe = -1; query_error = 0;
}
static void check(const char *name, int want, int stdout_object, int want_opens) {
    int before[256]; memcpy(before, handles, sizeof before);
    posix_spawn_file_actions_t fa = { .__actions = head };
    pid_t pid = -123;
    char *argv[] = { "child", NULL };
    int r = pouch_spawn_under_test(&pid, "/child", &fa, NULL, argv, NULL);
    int ok = r == want && opens == want_opens && !memcmp(before, handles, sizeof before);
    if (!want) ok &= calls == 1 && pid == 42 && nspawned == 3 && spawned[1] == stdout_object;
    else ok &= calls == (want == ENOENT ? 1 : 0) && pid == -123;
    printf("%s %s: result=%d spawn=%d opens=%d stdout=%d\n", ok ? "PASS" : "FAIL", name, r, calls, opens, spawned[1]);
    fails += !ok;
}
int main(void) {
    reset(); add(FDOP_CLOSE, 8, 0); add(FDOP_DUP2, 1, 8);
    check("closed-low-source", EBADF, 0, 0);
    reset(); add(FDOP_CLOSE, 100, 0); add(FDOP_DUP2, 1, 100);
    check("closed-high-source", EBADF, 0, 0);
    reset(); add(FDOP_CLOSE, 1, 0); add(FDOP_DUP2, 1, 1);
    check("closed-self-dup", EBADF, 0, 0);
    reset(); add(FDOP_OPEN, 1, 0); add(FDOP_DUP2, 2, 3);
    check("temporary-does-not-create-child-source", EBADF, 0, QUERY_MODE ? 0 : 1);
    reset(); handles[2] = 0; add(FDOP_OPEN, 1, 0); add(FDOP_DUP2, 2, 2);
    check("temporary-does-not-resurrect-missing-stderr", EBADF, 0, QUERY_MODE ? 0 : 1);
    reset(); add(FDOP_DUP2, 1, 99);
    check("missing-original-source", QUERY_MODE ? EBADF : ENOENT, 0, 0);
    reset(); add(FDOP_DUP2, 1, -1);
    check("negative-source", EBADF, 0, 0);
    reset(); add(FDOP_DUP2, 1, 8); add(FDOP_CLOSE, 8, 0);
    check("dup-then-close", 0, 108, 0);
    reset(); add(FDOP_DUP2, 1, 100); add(FDOP_CLOSE, 100, 0);
    check("high-dup-then-close", 0, 200, 0);
    reset(); add(FDOP_CLOSE, 8, 0); add(FDOP_OPEN, 8, 0);
    add(FDOP_DUP2, 1, 8); add(FDOP_CLOSE, 8, 0);
    check("close-open-dup-close", 0, 900, 1);
    reset(); add(FDOP_DUP2, 4, 8); add(FDOP_CLOSE, 8, 0);
    add(FDOP_DUP2, 1, 4); add(FDOP_CLOSE, 4, 0);
    check("chain-survives-original-close", 0, 108, 0);
    reset(); add(FDOP_DUP2, 4, 8); add(FDOP_CLOSE, 4, 0);
    add(FDOP_DUP2, 1, 4);
    check("chain-close-invalidates", EBADF, 0, 0);
    reset(); add(FDOP_CLOSE, 8, 0); add(FDOP_DUP2, 8, 100);
    add(FDOP_DUP2, 1, 8); add(FDOP_CLOSE, 8, 0);
    check("close-dup-reestablishes", 0, 200, 0);
    reset(); add(FDOP_OPEN, 1, 0); strcpy(head->path, "fail");
    add(FDOP_DUP2, 2, 1);
    check("open-failure-no-spawn", EACCES, 0, 1);
    reset(); add(FDOP_DUP2, 4, 3); add(FDOP_OPEN, 1, 0);
    add(FDOP_DUP2, 2, 4); add(FDOP_CLOSE, 4, 0);
    check("temporary-cannot-capture-earlier-alias", EBADF, 0, QUERY_MODE ? 0 : 1);
    reset(); add(FDOP_DUP2, 4, 3); add(FDOP_CLOSE, 4, 0);
    add(FDOP_OPEN, 1, 0);
    check("temporary-cannot-capture-discarded-alias", EBADF, 0, QUERY_MODE ? 0 : 1);
    reset(); add(FDOP_OPEN, 3, 0); add(FDOP_DUP2, 1, 3); add(FDOP_CLOSE, 3, 0);
    check("child-open-may-establish-same-number", 0, 900, 1);
    reset(); deny_probe = 8; add(FDOP_DUP2, 1, 8);
    check("original-source-not-reprobed", 0, 108, 0);
    reset(); check("standard-streams", 0, 101, 0);
    reset(); add(FDOP_DUP2, 4, 99); add(FDOP_CLOSE, 4, 0);
    check("discarded-invalid-source-no-open", EBADF, 0, 0);
    reset(); add(FDOP_DUP2, 4, 99); add(FDOP_DUP2, 4, 8); add(FDOP_CLOSE, 4, 0);
    check("overwritten-invalid-source-no-open", EBADF, 0, 0);
    reset(); for (int i=3;i<256;i++) handles[i]=100+i;
    add(FDOP_DUP2, 1, 8);
    check("full-parent-table-needs-no-probes", 0, 108, 0);
    reset(); query_error = ENOSYS; add(FDOP_DUP2, 1, 8);
    check("query-unavailable-fails-before-spawn", ENOSYS, 0, 0);
    reset(); query_error = 1; add(FDOP_DUP2, 1, 8);
    check("legacy-query-unavailable-fails-before-spawn", ENOSYS, 0, 0);
    reset(); query_error = 1;
    check("legacy-query-unavailable-standard-streams", ENOSYS, 0, 0);
    reset(); printf("failures=%d\n", fails); return !!fails;
}
