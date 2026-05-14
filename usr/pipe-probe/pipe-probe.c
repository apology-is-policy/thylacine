// /pipe-probe — userspace test of the full pipe + read/write/close
// + dup cycle (P5-fd-syscalls). First userspace binary that uses
// the byte I/O syscalls end-to-end.
//
// Sequence:
//   1. t_pipe(&rd, &wr) — creates a connected Spoor pair, installs
//      both as KOBJ_SPOOR handles. Returns 0 with rd / wr fds.
//   2. t_write(wr, payload, N) — bytes flow through the kernel
//      bounce buffer + dev->write into the pipe's ring buffer.
//      Returns N.
//   3. t_read(rd, got, N) — bytes flow from the ring buffer through
//      dev->read + kernel bounce buffer back to user-VA. Returns N.
//   4. Verify FIFO order.
//   5. t_dup(rd, RIGHT_READ) — second handle pointing at the same
//      Spoor. Demonstrates the KOBJ_SPOOR acquire path
//      (handle_acquire_obj → spoor_ref) wired at P5-fd-pipe.
//   6. t_close(dup_fd) — releases the second handle. The Spoor
//      stays alive (rd still holds a ref).
//   7. t_close(rd); t_close(wr) — releases both endpoints. The
//      Spoors + ring buffer are freed.
//   8. t_putstr("pipe-probe: PASS\n") — boot-log success marker
//      that tools/test.sh can grep for.
//   9. t_exits(0).
//
// On any failure: t_putstr a diagnostic + t_exits(1).

#include <thyla/syscall.h>

// Same convention as the other usr/ binaries: simple, no stdlib.

static int strneq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

int main(void) {
    long rd = -1, wr = -1;
    if (t_pipe(&rd, &wr) < 0) {
        t_putstr("pipe-probe: t_pipe FAIL\n");
        return 1;
    }
    if (rd < 0 || wr < 0 || rd == wr) {
        t_putstr("pipe-probe: bad fds from t_pipe\n");
        return 1;
    }

    static const char payload[] = "hello from /pipe-probe";
    const long payload_len = (long)(sizeof(payload) - 1);

    long n_wrote = t_write(wr, payload, (size_t)payload_len);
    if (n_wrote != payload_len) {
        t_putstr("pipe-probe: t_write short or error\n");
        return 1;
    }

    char got[64] = { 0 };
    long n_read = t_read(rd, got, sizeof(got));
    if (n_read != payload_len) {
        t_putstr("pipe-probe: t_read returned wrong length\n");
        return 1;
    }
    if (!strneq(got, payload, (size_t)payload_len)) {
        t_putstr("pipe-probe: payload mismatch\n");
        return 1;
    }

    // dup the read end with RIGHT_READ only (subset of the original
    // READ|WRITE|TRANSFER granted by SYS_PIPE). The dup'd handle is
    // a second reference to the same Spoor via the KOBJ_SPOOR
    // acquire path's spoor_ref.
    long rd_dup = t_dup(rd, T_RIGHT_READ);
    if (rd_dup < 0 || rd_dup == rd) {
        t_putstr("pipe-probe: t_dup failed or returned same fd\n");
        return 1;
    }

    // Rights elevation must be rejected. Try to dup with WRITE which
    // the dup target rd_dup doesn't have — should return -1.
    long bad_dup = t_dup(rd_dup, T_RIGHT_READ | T_RIGHT_WRITE);
    if (bad_dup >= 0) {
        t_putstr("pipe-probe: rights elevation NOT rejected (bug)\n");
        return 1;
    }

    if (t_close(rd_dup) != 0) {
        t_putstr("pipe-probe: t_close(rd_dup) failed\n");
        return 1;
    }
    if (t_close(rd) != 0) {
        t_putstr("pipe-probe: t_close(rd) failed\n");
        return 1;
    }
    if (t_close(wr) != 0) {
        t_putstr("pipe-probe: t_close(wr) failed\n");
        return 1;
    }

    // Closed fds must reject subsequent operations.
    char dummy = 0;
    if (t_read(rd, &dummy, 1) >= 0) {
        t_putstr("pipe-probe: t_read on closed fd should have failed\n");
        return 1;
    }
    if (t_close(rd) >= 0) {
        t_putstr("pipe-probe: double-close should have failed\n");
        return 1;
    }

    t_putstr("pipe-probe: PASS\n");
    return 0;
}
