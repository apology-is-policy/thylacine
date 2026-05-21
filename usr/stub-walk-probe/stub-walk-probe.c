// /stub-walk-probe — t_attach_9p + t_walk_open + t_read round-trip against
// the stratumd-stub synthetic FS, exercising both the handle-source path
// (e1) and the territory-root-source path (e2).
//
// Where /stub-fs-probe drove the stub's wire handlers directly via raw
// 9P over pipes, this probe goes through the kernel's 9P client + the
// dev9p Dev vtable + the SYS_WALK_OPEN syscall (handle source + the
// FROM_ROOT sentinel) — testing the same end-to-end content path but
// exercising production kernel surfaces.
//
// Pre-installed transport fds (kernel test framework / joey thunk sets
// them up BEFORE exec_setup):
//   fd 0 = c2s_wr — write end of client→server pipe
//   fd 1 = s2c_rd — read end of server→client pipe
//
// Sequence:
//   1. t_attach_9p(0, 1, "/", 1, 0) → attach_fd
//   2. e1: t_walk_open(attach_fd, "hello", 5, T_OREAD) → hello_fd
//          + t_read + content match + t_read EOF (count 0) + close
//   3. e2: t_chroot(attach_fd) → 0
//          + t_walk_open(T_WALK_OPEN_FROM_ROOT, "hello", 5, T_OREAD) →
//          root_hello_fd
//          + t_read + content match + t_read EOF + close
//   4. t_close(attach_fd) — territory's root_spoor still holds an
//      independent kernel-side ref, so close doesn't tear down the
//      attach session.
//   5. exit 0 — Proc exit → territory_unref → spoor_clunk on root_spoor
//      → dev9p_close on the root → 9P session teardown.
//
// On any error: diagnostic + exit 1.

#include <thyla/syscall.h>

#define TX_FD     0
#define RX_FD     1

static const unsigned char HELLO_CONTENT[] = "hello from stratumd-stub\n";
#define HELLO_CONTENT_LEN ((unsigned)(sizeof(HELLO_CONTENT) - 1))

static int mem_eq(const unsigned char *a, const unsigned char *b, unsigned n) {
    for (unsigned i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

void _start(void) {
    t_putstr("stub-walk-probe: t_attach_9p + t_walk_open + t_read + t_chroot on fds 0/1\n");

    long attach_fd = t_attach_9p(TX_FD, RX_FD, "/", 1, 0);
    if (attach_fd < 0) {
        t_putstr("stub-walk-probe: FAIL t_attach_9p\n");
        t_exits(1);
    }

    // === e1: walk-via-fd ===
    long hello_fd = t_walk_open(attach_fd, "hello", 5, T_OREAD);
    if (hello_fd < 0) {
        t_putstr("stub-walk-probe: FAIL t_walk_open hello\n");
        t_close(attach_fd);
        t_exits(1);
    }

    unsigned char buf[64];
    long n = t_read(hello_fd, buf, sizeof(buf));
    if (n != (long)HELLO_CONTENT_LEN) {
        t_putstr("stub-walk-probe: FAIL t_read length mismatch\n");
        t_close(hello_fd);
        t_close(attach_fd);
        t_exits(1);
    }
    if (!mem_eq(buf, HELLO_CONTENT, HELLO_CONTENT_LEN)) {
        t_putstr("stub-walk-probe: FAIL t_read content mismatch\n");
        t_close(hello_fd);
        t_close(attach_fd);
        t_exits(1);
    }

    long eof = t_read(hello_fd, buf, sizeof(buf));
    if (eof != 0) {
        t_putstr("stub-walk-probe: FAIL t_read EOF expected 0\n");
        t_close(hello_fd);
        t_close(attach_fd);
        t_exits(1);
    }

    t_close(hello_fd);

    // === e2: walk-via-root after t_chroot ===
    if (t_chroot(attach_fd) != 0) {
        t_putstr("stub-walk-probe: FAIL t_chroot\n");
        t_close(attach_fd);
        t_exits(1);
    }

    long root_hello_fd = t_walk_open(T_WALK_OPEN_FROM_ROOT, "hello", 5, T_OREAD);
    if (root_hello_fd < 0) {
        t_putstr("stub-walk-probe: FAIL t_walk_open(FROM_ROOT)\n");
        t_close(attach_fd);
        t_exits(1);
    }

    long rn = t_read(root_hello_fd, buf, sizeof(buf));
    if (rn != (long)HELLO_CONTENT_LEN) {
        t_putstr("stub-walk-probe: FAIL t_read(FROM_ROOT) length mismatch\n");
        t_close(root_hello_fd);
        t_close(attach_fd);
        t_exits(1);
    }
    if (!mem_eq(buf, HELLO_CONTENT, HELLO_CONTENT_LEN)) {
        t_putstr("stub-walk-probe: FAIL t_read(FROM_ROOT) content mismatch\n");
        t_close(root_hello_fd);
        t_close(attach_fd);
        t_exits(1);
    }

    long reof = t_read(root_hello_fd, buf, sizeof(buf));
    if (reof != 0) {
        t_putstr("stub-walk-probe: FAIL t_read(FROM_ROOT) EOF expected 0\n");
        t_close(root_hello_fd);
        t_close(attach_fd);
        t_exits(1);
    }

    t_close(root_hello_fd);
    t_close(attach_fd);
    t_putstr("stub-walk-probe: PASS\n");
    t_exits(0);
}
