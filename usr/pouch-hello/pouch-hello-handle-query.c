/* Guest ABI witness; tools/interactive/ls-descriptors.exp runs its default
 * mode as uid 1000. Pass "old-kernel" only when testing fail-closed
 * compatibility against a kernel predating the query (129 on main). The
 * "linux-collision" mode assumes a Linux 129 with no translation; on main
 * Linux 129 is kill, so that mode is not run here. */
#include <thyla/syscall.h>
#include <thyla/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
static void check(int ok, const char *name) {
    printf("%s QUERY %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) exit(1);
}
int main(int argc, char **argv) {
    check(getuid() == 1000, "ordinary-user");
    if (argc == 2 && !strcmp(argv[1], "linux-collision")) {
        /* Phenotype belongs to the namespace, never to ELF OSABI branding.
         * Mount only the Linux witness through the public declared boundary. */
        const char *path = "/home/michael/query-linux";
        const char *source = "/tmp/native/linux-collision";
        /* Mount sources must remain walkable; an already-open 9P fid
         * cannot be cloned by a later namespace crossing. */
        int src = (int)t_open(T_WALK_OPEN_FROM_ROOT, source, strlen(source), T_OPATH);
        check(src >= 0, "linux-source");
        int dst = open(path, O_CREAT|O_EXCL|O_WRONLY, 0700);
        check(dst >= 0 && close(dst) == 0, "linux-target");
        check(t_mount(path, strlen(path), src, T_MREPL|T_MPHENO_LINUX) == 0,
              "linux-declared-mount");
        check(close(src) == 0, "linux-source-close");
        pid_t pid = -1; int status = -1;
        char *args[] = {(char *)path, NULL};
        int rc = posix_spawn(&pid, path, NULL, NULL, args, NULL);
        pid_t got = rc == 0 ? waitpid(pid, &status, 0) : -1;
        printf("LINUX-CHILD rc=%d pid=%d got=%d status=%d errno=%d\n",
               rc, (int)pid, (int)got, status, errno);
        int child_ok = rc == 0 && got == pid && WIFEXITED(status)
                       && WEXITSTATUS(status) == 0;
        check(t_unmount(path, strlen(path)) == 0 && unlink(path) == 0,
              "linux-mount-cleanup");
        check(child_ok, "linux-collision-child");
        puts("QUERY-LINUX-END"); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "old-kernel")) {
        check(t_handle_flags_get(1) == -1, "old-kernel-legacy-unknown");
        pid_t pid = -123;
        char *args[] = {argv[0], "unexpected-child", NULL};
        check(posix_spawn(&pid, argv[0], NULL, NULL, args, NULL) == ENOSYS
              && pid == -123, "old-kernel-spawn-fails-closed");
        puts("QUERY-OLD-END"); return 0;
    }
    check(argc == 1, "no-unexpected-child");
    int fd = open("/dev/null", O_RDONLY);
    check(fd >= 3, "open");
    check(t_handle_flags_get(fd) == 0, "live-flags-zero");
    check(t_handle_flags_get(-1) == -EBADF, "negative-index");
    check(t_handle_flags_get(1024) == -EBADF, "out-of-range");
    check(t_handle_flags_get((1UL << 32) + fd) == -EBADF, "no-narrowed-alias");
    long narrow = t_dup(fd, T_RIGHT_TRANSFER);
    check(narrow >= 0, "transfer-only-handle");
    char byte;
    check(t_read(narrow, &byte, 1) < 0 && t_write(narrow, &byte, 1) < 0,
          "transfer-only-denies-io");
    check(t_handle_flags_get(narrow) == 0, "query-without-io-rights");
    /* Dup the same existing object, avoiding a thousand device opens. No
     * allocations/stdio while full; query before releasing any slot. */
    long held[1024]; int n = 0;
    while (n < 1024) {
        long next = t_dup(fd, T_RIGHT_TRANSFER);
        if (next < 0) break;
        held[n++] = next;
    }
    int live = 0;
    for (int i = 0; i < 1024; ++i) live += t_handle_flags_get(i) >= 0;
    long full_flags = t_handle_flags_get(narrow);
    long retry = t_dup(fd, T_RIGHT_TRANSFER);
    if (retry >= 0) t_close(retry);
    int close_failures = 0;
    for (int i = 0; i < n; ++i) close_failures += t_close(held[i]) != 0;
    check(live == 1024 && n > 900 && n < 1024 && retry < 0, "parent-table-saturated");
    check(close_failures == 0, "saturated-table-cleanup");
    check(full_flags == 0, "query-while-table-full");
    check(t_close(narrow) == 0 && t_handle_flags_get(narrow) == -EBADF,
          "closed-slot");
    check(close(fd) == 0, "cleanup");
    printf("QUERY-END filled=%d\n", n);
    return 0;
}
