/* Explicit native guest witness, linked with the series-built Pouch libc. */
#include <thyla/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
static int checks;
static void check(int ok, const char *name) {
    printf("%s DUP2 %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) exit(1);
    ++checks;
}
int main(int argc, char **argv) {
    check(getuid() == 1000, "ordinary-user");
    int fd = open("/dev/null", O_RDONLY);
    check(fd >= 3, "open-native");
    if (argc == 2 && !strcmp(argv[1], "old-kernel")) {
        errno = 0;
        check(dup2(fd, fd) == -1 && errno == ENOSYS, "old-kernel-fails-closed");
        check(close(fd) == 0, "old-kernel-cleanup");
        printf("DUP2-OLD-END tests=%d\n", checks); return 0;
    }
    check(argc == 1, "expected-mode");
    long flags = t_handle_flags_get(fd);
    errno = EDOM;
    check(flags >= 0 && dup2(fd, fd) == fd && errno == EDOM &&
          t_handle_flags_get(fd) == flags, "live-native-preserves-flags");
    errno = 0;
    check(dup2(-1, -1) == -1 && errno == EBADF, "negative");
    errno = 0;
    check(dup2(1024, 1024) == -1 && errno == EBADF, "out-of-range-native");
    long narrow = t_dup(fd, T_RIGHT_TRANSFER);
    check(narrow >= 0 && dup2(narrow, narrow) == narrow, "transfer-only-self-dup");
    long held[1024]; int count = 0;
    while (count < 1024) {
        long h = t_dup(fd, T_RIGHT_TRANSFER);
        if (h < 0) break;
        held[count++] = h;
    }
    int live = 0;
    for (int i=0; i<1024; ++i) live += t_handle_flags_get(i) >= 0;
    int full_result = dup2(fd, fd), close_errors = 0;
    for (int i=0; i<count; ++i) close_errors += t_close(held[i]) != 0;
    check(live == 1024 && full_result == fd, "full-table-self-dup");
    check(close_errors == 0 && t_close(narrow) == 0, "full-table-cleanup");
    errno = 0;
    check(dup2(fd, fd+1) == fd+1 && t_handle_flags_get(fd+1) == 0 &&
          close(fd+1) == 0 && t_handle_flags_get(fd) == flags, "distinct-target-native");
    check(close(fd) == 0, "close-native");
    errno = 0;
    check(dup2(fd, fd) == -1 && errno == EBADF, "closed-native");
    int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    check(socket_fd >= 0, "fresh-socket");
    check(dup2(socket_fd, socket_fd) == socket_fd, "fresh-socket-self-dup");
    check(close(socket_fd) == 0, "close-socket");
    errno = 0;
    check(dup2(socket_fd, socket_fd) == -1 && errno == EBADF, "closed-socket");
    errno = 0;
    check(dup2(INT_MAX, INT_MAX) == -1 && errno == EBADF, "malformed-socket-tag");
    printf("DUP2-END tests=%d filled=%d\n", checks, count);
    return 0;
}
