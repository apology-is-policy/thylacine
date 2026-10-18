/* Pouch native guest witness for ordered spawn descriptor actions.
 * Run from a writable user directory. argv[0] must resolve in child namespace.
 * tools/interactive/ls-descriptors.exp runs it as uid 1000 from a fresh dir. */
#define _GNU_SOURCE
#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int failures, tests;
static const char *self;
static const char payload[] = "SPAWN-CHILD\n";
static void require(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "SETUP-FAIL %s errno=%d\n", what, errno); exit(2); }
}
static int free_slot(void) {
    int fd = open("/dev/null", O_RDONLY);
    require(fd >= 0, "free slot"); require(close(fd) == 0, "close slot"); return fd;
}
static int target(void) {
    int fd = open("spawn-target", O_CREAT|O_TRUNC|O_RDWR, 0600);
    require(fd >= 0, "target open"); return fd;
}
static void action(int rc) { require(rc == 0, "file action"); }
/* gone: the slots the child must find closed, ended by -1 (NULL for none). */
static void run(const char *name, posix_spawn_file_actions_t *fa, int want, const int *gone) {
    int before = free_slot(), status = -1;
    pid_t pid = -123;
    char slot[2][12];
    char *argv[5] = {(char *)self, "child"};
    for (int i = 0; gone && gone[i] >= 0; ++i) {
        require(i < 2, "gone bound");
        snprintf(slot[i], sizeof slot[i], "%d", gone[i]);
        argv[2 + i] = slot[i];
    }
    int rc = posix_spawn(&pid, self, fa, NULL, argv, NULL);
    int ok = rc == want;
    if (rc == 0) {
        pid_t got = waitpid(pid, &status, 0);
        ok &= got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    } else ok &= pid == -123;
    if (rc == 0 && want == 0) {
        int fd = open("spawn-target", O_RDONLY);
        require(fd >= 0, "read target");
        char got[sizeof payload + 8]; ssize_t n = read(fd, got, sizeof got);
        require(close(fd) == 0, "close target");
        ok &= n == sizeof payload-1 && !memcmp(got, payload, sizeof payload-1);
    }
    ok &= free_slot() == before;
    printf("%s %s rc=%d status=%d\n", ok ? "PASS" : "FAIL", name, rc, status);
    ++tests; failures += !ok;
    action(posix_spawn_file_actions_destroy(fa));
}
#define INIT() action(posix_spawn_file_actions_init(&fa))
#define CLOSE(fd) action(posix_spawn_file_actions_addclose(&fa, (fd)))
#define DUP(src,dst) action(posix_spawn_file_actions_adddup2(&fa, (src), (dst)))
#define OPEN(fd) action(posix_spawn_file_actions_addopen(&fa, (fd), "spawn-target", O_CREAT|O_TRUNC|O_WRONLY, 0600))
#define GONE(...) ((const int[]){__VA_ARGS__, -1})
int main(int argc, char **argv) {
    self = argv[0];
    if (argc >= 2 && !strcmp(argv[1], "child")) {
        if (getuid() != 1000) return 3;
        /* An addclose that leaves the child's table dense passes every parent-side check. */
        for (int i = 2; i < argc; ++i) {
            errno = 0;
            if (fcntl(atoi(argv[i]), F_GETFD) != -1 || errno != EBADF) return 4;
        }
        return write(1, payload, sizeof payload-1) != sizeof payload-1;
    }
    require(getuid() == 1000, "ordinary uid1000");
    printf("SPAWN-ACTIONS-BEGIN uid=%u\n", (unsigned)getuid());
    posix_spawn_file_actions_t fa;
    int src = target();
    INIT(); CLOSE(src); DUP(src,1);
    run("closed-source", &fa, EBADF, NULL);
    INIT(); CLOSE(1); DUP(1,1);
    run("closed-self-dup", &fa, EBADF, NULL);
    INIT(); DUP(src,1); CLOSE(src);
    run("dup-then-close", &fa, 0, GONE(src));
    require(close(src) == 0, "close original");

    /* The child gets only stdio and the slots its actions establish, so a
     * skipped close of one directly above stdio leaves its table dense, which
     * nothing on the parent's side can see. */
    INIT(); OPEN(3); DUP(3,1); CLOSE(3);
    run("established-close-dense", &fa, 0, GONE(3));

    INIT(); CLOSE(8); OPEN(8); DUP(8,1); CLOSE(8);
    run("close-open-dup-close", &fa, 0, GONE(8));
    src = target();
    INIT(); DUP(src,8); CLOSE(src); DUP(8,1); CLOSE(8);
    run("chain-survives-original-close", &fa, 0, GONE(src, 8));
    INIT(); DUP(src,8); CLOSE(8); DUP(8,1);
    run("chain-close-invalidates", &fa, EBADF, NULL);
    INIT(); CLOSE(8); DUP(src,8); DUP(8,1); CLOSE(8);
    require(ftruncate(src, 0) == 0 && lseek(src, 0, SEEK_SET) == 0, "reset target");
    run("close-dup-reestablishes", &fa, 0, GONE(8));
    require(close(src) == 0, "close chain original");

    /* Match FDOP_OPEN exactly: O_CREAT holds the parent-directory handle
     * while opening the leaf, so /dev/null's lowest slot is not its result. */
    int missing = open("spawn-target", O_CREAT|O_TRUNC|O_WRONLY, 0600);
    require(missing > 2 && missing < 8, "known temporary slot");
    require(close(missing) == 0, "close measured temporary slot");
    printf("TEMPORARY-SLOT %d lowest-slot %d\n", missing, free_slot());
    INIT(); OPEN(1); DUP(missing,2);
    run("temporary-cannot-create-source", &fa, EBADF, NULL);
    INIT(); DUP(missing,8); OPEN(1); DUP(8,2); CLOSE(8);
    run("temporary-cannot-capture-earlier-alias", &fa, EBADF, NULL);
    INIT(); DUP(missing,8); CLOSE(8); OPEN(1);
    run("temporary-cannot-capture-discarded-alias", &fa, EBADF, NULL);
    INIT(); OPEN(missing); DUP(missing,1); CLOSE(missing);
    run("child-open-establishes-same-number", &fa, 0, GONE(missing));

    int held[128], count = 0, high;
    do {
        require(count < 128, "high source bound");
        high = open("spawn-target", O_RDWR);
        require(high >= 0, "high source open"); held[count++] = high;
    } while (high < 64);
    require(ftruncate(high, 0) == 0, "high target truncate");
    INIT(); CLOSE(high); DUP(high,1);
    run("closed-high-source", &fa, EBADF, NULL);
    INIT(); DUP(high,1); CLOSE(high);
    run("high-dup-then-close", &fa, 0, GONE(high));
    while (count) require(close(held[--count]) == 0, "high parent cleanup");

    INIT(); DUP(100,8); CLOSE(8);
    run("discarded-invalid-source-no-open", &fa, EBADF, NULL);
    INIT(); DUP(100,8); DUP(1,8); CLOSE(8);
    run("overwritten-invalid-source-no-open", &fa, EBADF, NULL);

    INIT(); action(posix_spawn_file_actions_addopen(&fa,1,"/no-such-spawn-parent/file",O_WRONLY,0)); DUP(1,2);
    run("open-failure", &fa, ENOENT, NULL);
    require(unlink("spawn-target") == 0, "target cleanup");
    printf("SPAWN-ACTIONS-END tests=%d failures=%d\n", tests, failures);
    return !!failures;
}
