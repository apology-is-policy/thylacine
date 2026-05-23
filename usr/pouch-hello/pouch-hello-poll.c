// /pouch-hello-poll — the polling pouch hello (Phase 6 sub-chunk 10).
//
// Closes POUCH-DESIGN.md §6.3 / §14 sub-chunk 10's exit criterion:
//   "POSIX poll / select work end-to-end against Thylacine pollable
//    objects (devpipe at v1.0)."
//
// First POSIX C program Thylacine runs that exercises:
//   - poll(2)    : musl's src/select/poll.c routed through SYS_POLL (=29)
//                  via the __NR_poll 29 entry the boundary-line patch adds.
//   - select(2)  : musl's src/select/select.c rewritten to translate
//                  fd_set <-> pollfd[] in userspace and call SYS_POLL.
//   - the same kernel poll primitive audited in P5-poll-a (see
//                  docs/reference/72-poll.md).
//
// Design: create a pipe pair via SYS_PIPE (musl's pipe(2) calls SYS_pipe2
// which the seam sentinels to ENOSYS — there is no native pipe-into-array
// shape on the kernel side, so we use inline asm to capture the kernel's
// rd-in-x0 / wr-in-x1 two-register return directly).
//
// Then four poll-shape probes:
//   1. poll the read end with timeout=100ms, no data       -> expect 0
//   2. write one byte to the write end, poll the read end  -> expect 1
//      (POLLIN set)
//   3. read the byte, build an fd_set, call select         -> expect 1
//      after re-writing the byte (POLLIN's fd_set re-set)
//   4. select(0, NULL, NULL, NULL, {0,0}) — the zero-timeout no-fds
//      portable-sleep edge case                            -> expect 0
//
// fd 1 is the pipe joey relays to the boot-log UART. Output:
//   pouch-hello-poll: pipe rd=N wr=M
//   pouch-hello-poll: poll empty -> 0 (ok)
//   pouch-hello-poll: poll with byte -> 1 POLLIN (ok)
//   pouch-hello-poll: select with byte -> 1 rfds (ok)
//   pouch-hello-poll: select(0,...) zero-tv -> 0 (ok)
//   pouch-hello-poll: exit 0
//
// Return non-zero on any failed assertion — joey treats it as a regression.

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

// SYS_PIPE = 8. No args. Returns:
//   rc <  0  -> error
//   rc >= 0  -> rd in x0, wr in x1.
// musl's pipe(2) speaks SYS_pipe2(fd_array, 0) which the pouch seam
// sentinels to ENOSYS — Thylacine's kernel doesn't write fd[2] from a
// user pointer; the registers ARE the return. Capture both directly.
static int sys_pipe_pair(int fds[2]) {
    register long x8 asm("x8") = 8;
    register long x0 asm("x0");
    register long x1 asm("x1");
    asm volatile ("svc 0"
                  : "=r"(x0), "=r"(x1)
                  : "r"(x8)
                  : "memory", "cc");
    if (x0 < 0) {
        errno = (int)-x0;
        return -1;
    }
    fds[0] = (int)x0;
    fds[1] = (int)x1;
    return 0;
}

int main(void) {
    int pipefd[2];
    if (sys_pipe_pair(pipefd) != 0) {
        printf("pouch-hello-poll: pipe failed: errno=%d\n", errno);
        fflush(stdout);
        return 1;
    }
    printf("pouch-hello-poll: pipe rd=%d wr=%d\n", pipefd[0], pipefd[1]);
    fflush(stdout);

    // 1. poll empty read end with a real (non-zero) timeout.
    //    Exercises tsleep + the per-pipe poll_waiter list. 50 ms is small
    //    enough to keep the boot-log latency budget but big enough that
    //    sub-ms host-timer jitter doesn't false-positive a wakeup.
    {
        struct pollfd p = { .fd = pipefd[0], .events = POLLIN, .revents = 0 };
        int rc = poll(&p, 1, 50);
        if (rc != 0 || p.revents != 0) {
            printf("pouch-hello-poll: FAIL poll-empty rc=%d revents=0x%x errno=%d\n",
                   rc, (unsigned)p.revents, errno);
            fflush(stdout);
            return 2;
        }
        printf("pouch-hello-poll: poll empty -> 0 (ok)\n");
        fflush(stdout);
    }

    // 2. write a byte, poll again with timeout=0 (the non-blocking
    //    fast-path arm — readiness sampled in the first scan, no sleep).
    {
        const char byte = 'P';
        if (write(pipefd[1], &byte, 1) != 1) {
            printf("pouch-hello-poll: FAIL write\n");
            fflush(stdout);
            return 3;
        }
        struct pollfd p = { .fd = pipefd[0], .events = POLLIN, .revents = 0 };
        int rc = poll(&p, 1, 0);
        if (rc != 1 || !(p.revents & POLLIN)) {
            printf("pouch-hello-poll: FAIL poll-byte rc=%d revents=0x%x\n",
                   rc, (unsigned)p.revents);
            fflush(stdout);
            return 4;
        }
        printf("pouch-hello-poll: poll with byte -> 1 POLLIN (ok)\n");
        fflush(stdout);
    }

    // 3. drain the byte, write another, exercise select() over the same
    //    pipe. select internally translates the fd_set to a pollfd[] of
    //    one entry, calls SYS_POLL, and translates revents back.
    {
        char drain;
        if (read(pipefd[0], &drain, 1) != 1) {
            printf("pouch-hello-poll: FAIL drain read\n");
            fflush(stdout);
            return 5;
        }
        const char byte = 'S';
        if (write(pipefd[1], &byte, 1) != 1) {
            printf("pouch-hello-poll: FAIL write 2\n");
            fflush(stdout);
            return 6;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pipefd[0], &rfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
        int rc = select(pipefd[0] + 1, &rfds, NULL, NULL, &tv);
        if (rc != 1 || !FD_ISSET(pipefd[0], &rfds)) {
            printf("pouch-hello-poll: FAIL select-byte rc=%d isset=%d\n",
                   rc, FD_ISSET(pipefd[0], &rfds));
            fflush(stdout);
            return 7;
        }
        printf("pouch-hello-poll: select with byte -> 1 rfds (ok)\n");
        fflush(stdout);
        // drain so close doesn't strand a byte.
        if (read(pipefd[0], &drain, 1) != 1) {
            printf("pouch-hello-poll: FAIL drain read 2\n");
            fflush(stdout);
            return 8;
        }
    }

    // 4. the zero-fds, zero-timeout edge case. POSIX says return 0
    //    immediately. The pouch select wrapper short-circuits before
    //    SYS_POLL (which itself rejects nfds=0). This proves the
    //    short-circuit arm.
    {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
        int rc = select(0, NULL, NULL, NULL, &tv);
        if (rc != 0) {
            printf("pouch-hello-poll: FAIL select(0,..., {0,0}) rc=%d errno=%d\n",
                   rc, errno);
            fflush(stdout);
            return 9;
        }
        printf("pouch-hello-poll: select(0,...) zero-tv -> 0 (ok)\n");
        fflush(stdout);
    }

    // Don't close — pouch programs at v1.0 exit via SYS_EXITS, which
    // tears down the handle table. close(2) on the pipe ends would be
    // additional noise.

    printf("pouch-hello-poll: exit 0\n");
    fflush(stdout);
    return 0;
}
