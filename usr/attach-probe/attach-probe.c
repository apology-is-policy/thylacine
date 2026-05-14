// /attach-probe — userspace integration test of the full
// SYS_ATTACH_9P + SYS_MOUNT + SYS_UNMOUNT cycle (P5-attach-probe).
//
// Calling convention: the kernel test harness pre-installs two
// KOBJ_SPOOR handles in this Proc's handle table BEFORE exec_setup:
//
//   fd 0 = tx — write end of a pipe whose read end is held by
//                the kernel responder thread. Probe writes 9P
//                Tmsg frames here; kernel responder reads them.
//   fd 1 = rx — read end of a pipe whose write end is held by
//                the kernel responder thread. Probe reads 9P
//                Rmsg frames here; kernel responder writes them.
//
// Why pre-installed: SYS_ATTACH_9P expects two byte-pipe Spoor
// fds it can wrap into a 9P session. Userspace SYS_PIPE produces
// intra-Proc pipes; the OTHER ends of those pipes can't be
// transferred to a separate Proc at v1.0 (cross-Proc handle
// transfer is gated on RIGHT_TRANSFER + 9P-only per ARCH I-4,
// which we'd need a session for — circular dependency). The
// resolution: kernel allocates the pipes + installs one side
// in the probe + holds the other side as kthread responder
// state. When userspace rfork(RFFDG=share-fds) lands later,
// the two-userspace-Procs design becomes feasible and this
// probe can be ported.
//
// Sequence:
//   1. t_attach_9p(0, 1, "/", 1, 0) → drives Tversion + Tattach
//      handshake against the kernel responder; returns
//      attach_fd (KOBJ_SPOOR pointing at the 9P tree's root,
//      backed by dev9p with attached_owner set).
//   2. t_mount(attach_fd, 99, 0) → grafts at target_path_id 99
//      in the Proc's Territory mount table. Mount-table entry
//      holds its own spoor_ref on the dev9p Spoor.
//   3. t_unmount(99) → drops the mount-table entry. With Plan-9
//      cclose semantics, the dev9p Spoor still has the user's
//      handle ref (= 1); the Dev's close hook doesn't run yet.
//   4. t_close(attach_fd) → drops the last ref. dev9p_close
//      fires: p9_attached_destroy sends Tclunk on root_fid via
//      the transport; spoor_clunk on transport Spoors.
//   5. t_putstr("attach-probe: PASS\n") + t_exits(0).
//
// On any error: diagnostic + exit 1.
//
// The responder's loop drives the wire side: reads Tversion +
// writes Rversion; reads Tattach + writes Rattach; reads Tclunk +
// writes Rclunk. When the probe exits, the user's handles on the
// tx pipe close, EOF propagates to the responder's read, and the
// responder thread exits cleanly.

#include <thyla/syscall.h>

int main(void) {
    // The kernel pre-installs the two transport fds at indices 0+1.
    // No SYS_PIPE call here — those handles are already present.
    const long tx_fd = 0;
    const long rx_fd = 1;

    static const char aname[] = "/";

    long attach_fd = t_attach_9p(tx_fd, rx_fd, aname, 1, 0);
    if (attach_fd < 0) {
        t_putstr("attach-probe: t_attach_9p FAIL\n");
        return 1;
    }

    // Target path 99 is arbitrary at v1.0 — the abstract u32 token
    // matches the PgrpMount table's keying. String-path resolution
    // arrives with the walk subsystem.
    if (t_mount(attach_fd, 99, 0) < 0) {
        t_putstr("attach-probe: t_mount FAIL\n");
        return 1;
    }

    // Unmount the just-installed entry; mount table goes 1→0.
    if (t_unmount(99) < 0) {
        t_putstr("attach-probe: t_unmount FAIL\n");
        return 1;
    }

    // Unmount of a non-existent path → -1 (regression coverage of
    // the SYS_UNMOUNT error path while we're here).
    if (t_unmount(99) >= 0) {
        t_putstr("attach-probe: t_unmount of already-unmounted should fail\n");
        return 1;
    }

    // Closing the attach fd drops the last ref on the dev9p Spoor.
    // The dev9p_close hook fires: tears down the entire attach
    // session (Tclunk root_fid sent via tx; transport Spoors clunked).
    if (t_close(attach_fd) != 0) {
        t_putstr("attach-probe: t_close(attach_fd) FAIL\n");
        return 1;
    }

    // Close the pre-installed transport fds so the responder reads
    // EOF and exits cleanly.
    if (t_close(tx_fd) != 0) {
        t_putstr("attach-probe: t_close(tx_fd) FAIL\n");
        return 1;
    }
    if (t_close(rx_fd) != 0) {
        t_putstr("attach-probe: t_close(rx_fd) FAIL\n");
        return 1;
    }

    t_putstr("attach-probe: PASS\n");
    return 0;
}
