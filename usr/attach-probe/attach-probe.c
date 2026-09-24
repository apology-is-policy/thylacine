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
//   0. the flags word (x5): t_attach_9p with LOOSE (a /srv-only bit)
//      and with an unknown bit must both fail -- before anything
//      reaches the wire (the kernel test counts one Tattach).
//   1. t_attach_9p(0, 1, "/", 1, 0, T_ATTACH_9P_CAPE) → drives Tversion +
//      Tattach handshake against the kernel responder; returns
//      attach_fd (KOBJ_SPOOR pointing at the 9P tree's root,
//      backed by dev9p with attached_owner set). The session is
//      CAPED (IDENTITY-DESIGN 3.2): t_fstat on the root reports THIS
//      Proc as owner and group, whatever ids the server reports (the
//      kernel responder says 501:20, a Mac's; the stratumd-stub run
//      reuses this probe against its own). The mode-kept half is the
//      kernel tests' (dev9p.cape, 9p_client.loom_cape).
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

    if (t_attach_9p(tx_fd, rx_fd, aname, 1, 0, 0x1ul) >= 0) {
        t_putstr("attach-probe: a pipe attach admitted LOOSE\n");
        return 2;
    }
    if (t_attach_9p(tx_fd, rx_fd, aname, 1, 0, 0x4ul) >= 0) {
        t_putstr("attach-probe: an unknown flags bit was admitted\n");
        return 3;
    }

    long attach_fd = t_attach_9p(tx_fd, rx_fd, aname, 1, 0, T_ATTACH_9P_CAPE);
    if (attach_fd < 0) {
        t_putstr("attach-probe: t_attach_9p FAIL\n");
        return 1;
    }

    // This Proc is an rfork child of the kernel test's kproc, so it carries
    // the TCB identity; the cape reports it as the owner of the whole tree.
    struct t_stat st;
    if (t_fstat(attach_fd, &st) != 0) {
        t_putstr("attach-probe: t_fstat on the caped root FAIL\n");
        return 4;
    }
    if (st.uid != T_PRINCIPAL_SYSTEM) {
        t_putstr("attach-probe: the caped root is not owned by the attacher\n");
        return 5;
    }
    if (st.gid != T_GID_SYSTEM) {
        t_putstr("attach-probe: the caped root's group is not the attacher's\n");
        return 6;
    }

    // stalk-2: mount is path-keyed. The kernel test thunk chrooted us to a
    // devramfs root, which ships a synthetic /srv mount-point dir. Graft the
    // attached 9P root onto /srv; the mount table keys on /srv's identity.
    static const char MP_SRV[] = "/srv";
    if (t_mount(MP_SRV, sizeof(MP_SRV) - 1, attach_fd, 0) < 0) {
        t_putstr("attach-probe: t_mount FAIL\n");
        return 1;
    }

    // Unmount the just-installed entry; mount table goes 1→0.
    if (t_unmount(MP_SRV, sizeof(MP_SRV) - 1) < 0) {
        t_putstr("attach-probe: t_unmount FAIL\n");
        return 1;
    }

    // Unmount of an already-unmounted point → -1 (regression coverage of
    // the SYS_UNMOUNT error path while we're here).
    if (t_unmount(MP_SRV, sizeof(MP_SRV) - 1) >= 0) {
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
