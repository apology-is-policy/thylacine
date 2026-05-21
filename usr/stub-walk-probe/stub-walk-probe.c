// /stub-walk-probe — t_attach_9p + t_walk_open + t_read round-trip against
// the stratumd-stub synthetic FS (P5-stratumd-stub-bringup-e1).
//
// Where /stub-fs-probe drove the stub's wire handlers directly via raw
// 9P over pipes, this probe goes through the kernel's 9P client + the
// dev9p Dev vtable + the new SYS_WALK_OPEN syscall — testing the same
// end-to-end content path but exercising production kernel surfaces.
//
// Pre-installed transport fds (kernel test framework / joey thunk sets
// them up BEFORE exec_setup):
//   fd 0 = c2s_wr — write end of client→server pipe
//   fd 1 = s2c_rd — read end of server→client pipe
//
// Sequence:
//   1. t_attach_9p(0, 1, "/", 1, 0) → attach_fd (KOBJ_SPOOR for root)
//   2. t_walk_open(attach_fd, "hello", 5, T_OREAD) → hello_fd
//      (kernel runs Twalk("hello") + Tlopen on the new fid; nc gets
//      KOBJ_SPOOR with R|W|TRANSFER rights)
//   3. t_read(hello_fd, buf, 64) → must return 25
//      memcmp(buf, "hello from stratumd-stub\n", 25) must be 0
//   4. t_read(hello_fd, buf, 64) again → EOF (count == 0)
//   5. t_close(hello_fd); t_close(attach_fd); exit 0
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
    t_putstr("stub-walk-probe: t_attach_9p + t_walk_open + t_read on fds 0/1\n");

    long attach_fd = t_attach_9p(TX_FD, RX_FD, "/", 1, 0);
    if (attach_fd < 0) {
        t_putstr("stub-walk-probe: FAIL t_attach_9p\n");
        t_exits(1);
    }

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
    t_close(attach_fd);
    t_putstr("stub-walk-probe: PASS\n");
    t_exits(0);
}
