// /stub-driver — production-shape orchestrator that spawns
// /stratumd-stub and drives the full SYS_ATTACH_9P + SYS_MOUNT +
// SYS_UNMOUNT cycle against it. P5-stratumd-stub-bringup-b.
//
// What this proves: a userspace process can do the full joey-style
// orchestration entirely from EL0, with no kernel-supervised handle
// pre-installation. The kernel test framework just devramfs_lookup's
// /stub-driver and rfork-execs it; everything else happens in
// userspace.
//
// Sequence:
//   1. t_pipe × 2 — get four fds: c2s_wr (client→server write), c2s_rd
//      (server→server read), s2c_wr (server→client write), s2c_rd
//      (client→server read).
//   2. t_spawn_with_fds("stratumd-stub", 13, [c2s_rd, s2c_wr], 2)
//      → spawns stratumd-stub with c2s_rd as its fd 0 (rx) and s2c_wr
//      as its fd 1 (tx). The parent retains its own holds on those
//      fds; the spawn installs independent refs in the child.
//   3. t_close(c2s_rd) + t_close(s2c_wr) — drop driver-side refs on
//      the fds the child now holds. Without this, the child can't
//      see EOF when the driver finishes (the ring would still have
//      readers/writers via the driver's handle).
//   4. t_attach_9p(c2s_wr, s2c_rd, "/", 1, 0) — drive the 9P
//      handshake (Tversion + Tattach) over the byte-pipe pair, with
//      stratumd-stub serving the responder side.
//   5. t_mount(attach_fd, 99, 0) — graft at path_id 99.
//   6. t_unmount(99).
//   7. t_close(attach_fd) — last drop on the dev9p Spoor; tears down
//      the attach session (Tclunk on root_fid is rejected at session
//      layer; no wire op).
//   8. t_close(c2s_wr) + t_close(s2c_rd) — drop driver's last holds
//      on the transport rings. c2s_wr last drop fires write_eof on
//      c2s ring; stratumd-stub's next read returns 0; stub exits.
//   9. t_wait_pid for stratumd-stub. Expect status=0.
//  10. t_putstr("stub-driver: PASS") + t_exits(0).
//
// On any error: print a diagnostic + exit 1.

#include <thyla/syscall.h>

static const char STUB_NAME[] = "stratumd-stub";

int main(void) {
    long c2s_rd = -1, c2s_wr = -1;
    if (t_pipe(&c2s_rd, &c2s_wr) < 0) {
        t_putstr("stub-driver: t_pipe c2s FAIL\n");
        return 1;
    }
    long s2c_rd = -1, s2c_wr = -1;
    if (t_pipe(&s2c_rd, &s2c_wr) < 0) {
        t_putstr("stub-driver: t_pipe s2c FAIL\n");
        return 1;
    }

    // Pass the server-side ends to the stub. Order is significant:
    // the stub reads from its fd 0 and writes to its fd 1, so the
    // fds[] order must be (read-end-from-client, write-end-to-client).
    unsigned int stub_fds[2] = { (unsigned int)c2s_rd, (unsigned int)s2c_wr };
    long stub_pid = t_spawn_with_fds(STUB_NAME, sizeof(STUB_NAME) - 1,
                                     stub_fds, 2);
    if (stub_pid <= 0) {
        t_putstr("stub-driver: t_spawn_with_fds FAIL\n");
        return 1;
    }

    // Drop driver-side refs on the stub's transport fds. The stub
    // holds its own refs (installed by the spawn); without this drop,
    // c2s ring would still have c2s_rd alive via the driver's handle
    // and the stub couldn't see EOF when the driver closes its tx.
    if (t_close(c2s_rd) != 0) { t_putstr("stub-driver: t_close c2s_rd FAIL\n"); return 1; }
    if (t_close(s2c_wr) != 0) { t_putstr("stub-driver: t_close s2c_wr FAIL\n"); return 1; }

    // Drive the 9P handshake against the stub.
    static const char aname[] = "/";
    long attach_fd = t_attach_9p(c2s_wr, s2c_rd, aname, 1, 0);
    if (attach_fd < 0) {
        t_putstr("stub-driver: t_attach_9p FAIL\n");
        return 1;
    }

    // stalk-2: mount is path-keyed. The kernel test thunk chrooted us to a
    // devramfs root with a synthetic /srv mount-point dir; graft + ungraft
    // the attached 9P root there.
    static const char MP_SRV[] = "/srv";
    if (t_mount(MP_SRV, sizeof(MP_SRV) - 1, attach_fd, 0) < 0) {
        t_putstr("stub-driver: t_mount FAIL\n");
        return 1;
    }
    if (t_unmount(MP_SRV, sizeof(MP_SRV) - 1) < 0) {
        t_putstr("stub-driver: t_unmount FAIL\n");
        return 1;
    }

    if (t_close(attach_fd) != 0) {
        t_putstr("stub-driver: t_close(attach_fd) FAIL\n");
        return 1;
    }

    // Drop the last driver-side refs on the transport rings; this
    // propagates EOF to the stub (write_eof on c2s) and signals
    // graceful shutdown.
    if (t_close(c2s_wr) != 0) { t_putstr("stub-driver: t_close c2s_wr FAIL\n"); return 1; }
    if (t_close(s2c_rd) != 0) { t_putstr("stub-driver: t_close s2c_rd FAIL\n"); return 1; }

    // Reap the stub. Expect clean exit (status=0 on EOF).
    int status = -1;
    long reaped = t_wait_pid(&status);
    if (reaped != stub_pid) {
        t_putstr("stub-driver: t_wait_pid wrong pid\n");
        return 1;
    }
    if (status != 0) {
        t_putstr("stub-driver: stratumd-stub exited non-zero\n");
        return 1;
    }

    t_putstr("stub-driver: PASS\n");
    return 0;
}
