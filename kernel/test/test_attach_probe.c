// /attach-probe regression test (P5-attach-probe).
//
// End-to-end userspace integration of the Phase 5 mount surface:
// SYS_ATTACH_9P + SYS_MOUNT + SYS_UNMOUNT driven through a real
// userspace Proc against a kernel-thread 9P responder.
//
// Pipeline:
//   tools/build.sh userspace  → build/usr/attach-probe/attach-probe
//   build_ramfs                → cpio includes /attach-probe
//   boot                       → devramfs reads cpio
//   this test                  →
//     1. pipe_create × 2 — 4 Spoors (c2s_wr, c2s_rd, s2c_rd, s2c_wr).
//        c2s = client → server; s2c = server → client.
//     2. thread_create_with_arg(kproc, responder_fn, &ctx) — kernel-
//        thread responder. ctx.rx = c2s_rd; ctx.tx = s2c_wr. The
//        responder loop reads framed Tmsgs from rx, builds Rmsgs,
//        writes them to tx. Loop exits on rx EOF (when the probe
//        closes its tx_fd).
//     3. rfork(RFPROC, probe_exec_thunk, &args) — userspace probe
//        Proc. The thunk INSTALLS c2s_wr as KOBJ_SPOOR fd 0 and
//        s2c_rd as fd 1 in the probe's handle table BEFORE
//        exec_setup → the probe's main() finds the transport fds
//        already populated. ABI: rfork's initial handle table is
//        empty; handle_alloc walks low-to-high → first alloc = 0.
//     4. wait_pid(probe) — expect status 0; the probe's PASS message
//        appeared on UART before exit.
//     5. yield via sched() to let the responder drain EOF + exit.
//     6. spoor_clunk on boot's own 4 Spoor refs (the probe's handle
//        table and the responder thread each held independent refs
//        that were already released by their respective close paths).
//
// Why a kernel-thread responder + single userspace Proc rather than
// two cooperating userspace Procs (as the original ROADMAP §7.2
// sketch suggested)? At v1.0 Thylacine's rfork supports RFPROC only;
// RFFDG (shared fd table) is deferred to a future P2-F follow-up.
// Cross-Proc Spoor transfer is gated on RIGHT_TRANSFER through a 9P
// session (ARCH I-4) — but we'd need a session for that, which is
// what we're constructing. The resolution: kernel-thread responder
// lets us exercise the wire side without resolving the chicken-and-
// egg. When RFFDG lands later, this test can graduate to a two-proc
// design.
//
// What this test verifies that test_sys_mount.c doesn't:
//   - SYS_ATTACH_9P SVC dispatch under real EL0 entry.
//   - The full Tversion/Rversion + Tattach/Rattach handshake against
//     an actual concurrent responder (not a synchronous test
//     fixture).
//   - The mount-after-attach + close-of-attach-fd + Tclunk-on-root_fid
//     teardown sequence end-to-end at userspace.
//   - libt's t_attach_9p / t_mount / t_unmount stubs compile + dispatch
//     correctly.

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/devramfs.h>
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/pipe.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/spoor.h>
#include <thylacine/territory.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../../arch/arm64/uart.h"

#include <thylacine/9p_wire.h>

void test_attach_probe_round_trip(void);

// attach-probe ≈ 15 KiB at the current libt + inline-stub footprint.
// 32 KiB cap matches the /pipe-probe headroom.
#define ATTACH_PROBE_BLOB_MAX 32768
static _Alignas(16) u8 g_attach_probe_blob[ATTACH_PROBE_BLOB_MAX];

// =============================================================================
// Kernel-thread 9P responder.
// =============================================================================
//
// Reads framed 9P request messages from `ctx.rx`, builds canonical
// responses (Rversion / Rattach / Rclunk), writes them to `ctx.tx`.
// On rx EOF: spoor_clunks both ends and exits.
//
// Buffer sizes: requests + responses each fit in 64 bytes for the
// op subset we handle (Tversion with version "9P2000.L" = 19 bytes;
// Tattach = 23 bytes; Tclunk = 11 bytes; responses similar).

struct attach_probe_responder_ctx {
    struct Spoor *rx;
    struct Spoor *tx;
};

// Single shared ctx — there's only one responder at a time and the
// boot thread waits for it before the test function returns.
static struct attach_probe_responder_ctx g_responder_ctx;
static volatile bool                     g_responder_finished;
static volatile u32                      g_responder_msgs_handled;
// #108: release flag for the responder's EXITING-handshake reap (the loom_sqpoll
// join idiom). The responder sets it RELEASE after marking itself EXITING; the
// boot joiner observes it ACQUIRE then thread_free()s the responder. Replaces a
// leaked `for(;;) sched()` busy-spin that pinned every CPU at idle. NOTE the
// two flags differ on purpose: g_responder_finished (above, volatile) is a
// liveness HINT (the responder drained EOF); g_responder_exited is the reap
// SYNCHRONIZATION edge (its ACQUIRE/RELEASE is what orders the thread_free).
static bool                              g_responder_exited;

// Helper: write `n` bytes to a Spoor, looping over short writes.
// Returns 0 on success, -1 on any error.
static int spoor_write_all(struct Spoor *c, const u8 *buf, long n) {
    long off = 0;
    while (off < n) {
        long w = c->dev->write(c, buf + off, n - off, 0);
        if (w <= 0) return -1;
        off += w;
    }
    return 0;
}

// Read EXACTLY `n` bytes from a Spoor. Returns:
//    n  — full read
//    0  — EOF before any byte read
//   -1  — partial read followed by EOF (treated as error) OR read error.
static long spoor_read_exact(struct Spoor *c, u8 *buf, long n) {
    long off = 0;
    while (off < n) {
        long r = c->dev->read(c, buf + off, n - off, 0);
        if (r < 0) return -1;
        if (r == 0) return (off == 0) ? 0 : -1;
        off += r;
    }
    return n;
}

// Build the response for one 9P message. Returns response size in
// bytes, or -1 on unhandled / malformed op.
//
// Handles Tversion / Tattach / Tclunk + every other op as Rlerror so
// the probe's t_attach_9p surfaces a clean failure on protocol drift
// (rather than the responder silently hanging or crashing).
static int build_response(const u8 *req, size_t req_len, u8 *resp, size_t resp_cap) {
    if (req_len < P9_HDR_LEN) return -1;
    u32 size; u8 type; u16 tag;
    if (p9_peek_header(req, req_len, &size, &type, &tag) < 0) return -1;

    if (type == P9_TVERSION) {
        // Rversion: 4-byte msize + 2-byte version-len + "9P2000.L".
        const char *v = "9P2000.L";
        size_t vlen = 8;
        size_t total = P9_HDR_LEN + 4 + 2 + vlen;
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff);
        resp[1] = (u8)((total >> 8) & 0xff);
        resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RVERSION;
        // Tversion uses NOFID tag (0xffff) by convention; mirror it.
        resp[5] = 0xff; resp[6] = 0xff;
        // msize = 4096 (matches SYS_ATTACH_DEFAULT_MSIZE).
        resp[7] = 0; resp[8] = 0x10; resp[9] = 0; resp[10] = 0;
        resp[11] = (u8)(vlen & 0xff); resp[12] = (u8)((vlen >> 8) & 0xff);
        for (size_t i = 0; i < vlen; i++) resp[13 + i] = (u8)v[i];
        return (int)total;
    }

    if (type == P9_TATTACH) {
        // Rattach: 13-byte qid (type + version + path).
        size_t total = P9_HDR_LEN + P9_QID_LEN;
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff); resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RATTACH;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        // Root qid: type=QTDIR, version=0, path=1.
        resp[7] = P9_QTDIR;
        for (int i = 0; i < 4; i++) resp[8 + i] = 0;
        resp[12] = 1; for (int i = 1; i < 8; i++) resp[12 + i] = 0;
        return (int)total;
    }

    if (type == P9_TCLUNK) {
        // Rclunk: empty body.
        size_t total = P9_HDR_LEN;
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff); resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RCLUNK;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        return (int)total;
    }

    // Anything else → Rlerror with errno=EIO (5).
    size_t total = P9_HDR_LEN + 4;
    if (resp_cap < total) return -1;
    resp[0] = (u8)(total & 0xff); resp[1] = 0; resp[2] = 0; resp[3] = 0;
    resp[4] = P9_RLERROR;
    resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
    resp[7] = 5; resp[8] = 0; resp[9] = 0; resp[10] = 0;
    return (int)total;
}

static void responder_thread_entry(void) {
    u8 req[256];
    u8 resp[256];
    struct Spoor *rx = g_responder_ctx.rx;
    struct Spoor *tx = g_responder_ctx.tx;

    for (;;) {
        // Read 4-byte size prefix. EOF here = clean shutdown.
        long got = spoor_read_exact(rx, req, 4);
        if (got == 0) break;
        if (got < 0) { uart_puts("    responder: hdr-size read failed\n"); break; }

        u32 size = (u32)req[0] | ((u32)req[1] << 8) |
                   ((u32)req[2] << 16) | ((u32)req[3] << 24);
        if (size < P9_HDR_LEN || size > sizeof(req)) {
            uart_puts("    responder: bad frame size\n");
            break;
        }

        // Read remaining bytes of the frame (type + tag + body).
        long body_len = (long)size - 4;
        if (spoor_read_exact(rx, req + 4, body_len) != body_len) {
            uart_puts("    responder: body read short\n");
            break;
        }

        int rlen = build_response(req, (size_t)size, resp, sizeof(resp));
        if (rlen <= 0) {
            uart_puts("    responder: build_response failed\n");
            break;
        }

        if (spoor_write_all(tx, resp, (long)rlen) != 0) {
            uart_puts("    responder: write_all failed\n");
            break;
        }
        g_responder_msgs_handled++;
    }

    // Release the responder's holds on the transport Spoors. Under
    // Plan-9 cclose, if the userspace has already closed its side,
    // this drops to 1 (boot still holds a ref) — NOT last drop, no
    // devpipe_close. Boot's own clunks (after wait_pid) hit the last
    // drop.
    spoor_clunk(rx);
    spoor_clunk(tx);
    g_responder_finished = true;

    // Terminal EXITING handshake (the loom_sqpoll_main idiom, kernel/loom.c):
    // mask preempt across the state=EXITING write + the g_responder_exited
    // RELEASE so no preempt fires between them; the boot joiner, on observing
    // g_responder_exited (ACQUIRE), is guaranteed to see state==EXITING and so
    // thread_free's not-RUNNING gate holds. sched() then switches away
    // PERMANENTLY (EXITING is never re-enqueued); the joiner's thread_free spins
    // on on_cpu before reclaiming. This is the wait_pid reap terminal minus the
    // Proc-zombie bookkeeping a kproc kthread cannot run (exits()/
    // thread_exit_self extinct from kproc). #108: the prior `for(;;) sched()`
    // park left this thread RUNNABLE forever -- the scheduler ran it on every
    // CPU (work-steal bounced it), pinning all cores at idle.
    (void)spin_lock_irqsave(NULL);
    current_thread()->state = THREAD_EXITING;
    __atomic_store_n(&g_responder_exited, true, __ATOMIC_RELEASE);
    sched();
    extinction("responder_thread_entry: returned from terminal sched");
}

// =============================================================================
// Userspace probe rfork thunk.
// =============================================================================

struct attach_probe_exec_args {
    const void   *blob;
    size_t        size;
    struct Spoor *tx_for_probe;  // = c2s_wr
    struct Spoor *rx_for_probe;  // = s2c_rd
};

__attribute__((noreturn))
static void attach_probe_exec_thunk(void *arg) {
    struct attach_probe_exec_args *ea = (struct attach_probe_exec_args *)arg;
    struct Thread *t = current_thread();
    if (!t) extinction("attach_probe_exec_thunk: no current_thread");
    struct Proc *p = t->proc;
    if (!p) extinction("attach_probe_exec_thunk: no proc");

    // Install the two transport Spoors as KOBJ_SPOOR handles at the
    // lowest free slots (= fd 0 and fd 1 in a fresh handle table).
    // handle_alloc takes ownership of the spoor_ref we hold here;
    // when the probe exits → handle_table_free runs spoor_clunk on
    // each handle's KOBJ_SPOOR via handle_release_obj.
    //
    // Pre-condition: this Proc's handle table is empty (post-rfork,
    // pre-exec). handle_alloc walks low-to-high.
    hidx_t fd_tx = handle_alloc(p, KOBJ_SPOOR,
                                RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER,
                                ea->tx_for_probe);
    if (fd_tx != 0) {
        uart_puts("    attach-probe: tx_for_probe got unexpected fd ");
        uart_putdec((u64)fd_tx);
        uart_puts(" (want 0)\n");
        exits("fail-fd-tx");
    }
    hidx_t fd_rx = handle_alloc(p, KOBJ_SPOOR,
                                RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER,
                                ea->rx_for_probe);
    if (fd_rx != 1) {
        uart_puts("    attach-probe: rx_for_probe got unexpected fd ");
        uart_putdec((u64)fd_rx);
        uart_puts(" (want 1)\n");
        exits("fail-fd-rx");
    }

    // stalk-2: give the probe a devramfs root so its path-keyed
    // t_mount("/srv", ...) resolves. rfork cloned the (rootless kproc)
    // Territory; chroot it to a fresh devramfs root here. devramfs ships a
    // synthetic /srv mount-point dir (Plan 9 M1).
    struct Spoor *ramfs_root = devramfs.attach(NULL);
    if (!ramfs_root) extinction("attach_probe_exec_thunk: devramfs.attach failed");
    if (territory_chroot(p->territory, ramfs_root) != 0)
        extinction("attach_probe_exec_thunk: territory_chroot failed");
    spoor_clunk(ramfs_root);   // territory_chroot took its own ref

    u64 entry = 0, sp = 0;
    int rc = exec_setup(p, ea->blob, ea->size, &entry, &sp);
    if (rc != 0) {
        uart_puts("    attach-probe exec_setup rc=");
        uart_putdec((u64)rc);
        uart_puts(" → exits(fail-exec)\n");
        exits("fail-exec");
    }

    userland_enter(entry, sp);
}

// =============================================================================
// Test entry.
// =============================================================================

void test_attach_probe_round_trip(void) {
    const void *cpio_blob = NULL;
    size_t size = 0;

    int rc = devramfs_lookup("attach-probe", &cpio_blob, &size);
    if (rc != 0) {
        uart_puts("    [skip] /attach-probe not in ramfs (build with: tools/build.sh all)\n");
        return;
    }

    TEST_ASSERT(size <= ATTACH_PROBE_BLOB_MAX,
                "attach-probe binary too large for static buffer");

    const u8 *src = (const u8 *)cpio_blob;
    for (size_t i = 0; i < size; i++) g_attach_probe_blob[i] = src[i];

    uart_puts("    /attach-probe size=");
    uart_putdec((u64)size);
    uart_puts(" bytes → setting up responder + rfork + exec\n");

    // 1. Create two pipes: client → server (c2s) + server → client (s2c).
    //    Each pipe_create yields two Spoors with ref=1 + a shared ring
    //    with ref=2 (one per endpoint).
    //
    //    Ref discipline at v1.0: each Spoor's ref=1 from pipe_create
    //    is the holder's only reference. We TRANSFER each Spoor to its
    //    eventual owner without bumping:
    //      - c2s_rd → responder ctx (responder thread owns this ref).
    //      - s2c_wr → responder ctx (responder thread owns this ref).
    //      - c2s_wr → probe's handle table fd 0 (handle_alloc
    //        consumes the ref).
    //      - s2c_rd → probe's handle table fd 1 (handle_alloc
    //        consumes the ref).
    //    Boot retains no refs. After setup, boot's local pointers are
    //    handed off — the test framework's responsibility ends here.
    //    The probe's t_close + responder's terminal spoor_clunks drive
    //    teardown.
    //
    //    KEY INVARIANT: boot must NOT hold a ref on any of these Spoors
    //    after the responder thread + probe Proc are spawned. If boot
    //    held an extra c2s_wr ref, the probe's t_close(tx_fd) would
    //    drop to 1 (boot's residual), NOT trigger devpipe_close + EOF,
    //    and the responder's read would block forever — a hang the
    //    earlier draft of this test surfaced.
    struct Spoor *c2s_rd = NULL, *c2s_wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&c2s_rd, &c2s_wr), 0,
        "pipe_create c2s");
    struct Spoor *s2c_rd = NULL, *s2c_wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&s2c_rd, &s2c_wr), 0,
        "pipe_create s2c");

    // 2. Hand off the responder's two Spoors via the static ctx.
    //    Single-shared ctx is fine — there's only one responder at a
    //    time and boot waits for it to finish.
    g_responder_ctx.rx       = c2s_rd;
    g_responder_ctx.tx       = s2c_wr;
    g_responder_finished     = false;
    g_responder_msgs_handled = 0;
    __atomic_store_n(&g_responder_exited, false, __ATOMIC_RELAXED);

    struct Thread *responder = thread_create(kproc(), responder_thread_entry);
    TEST_ASSERT(responder != NULL, "thread_create responder");
    ready(responder);

    // 3. rfork the probe Proc + exec. Handle_alloc inside the thunk
    //    consumes the c2s_wr / s2c_rd refs.
    struct attach_probe_exec_args args = {
        .blob         = g_attach_probe_blob,
        .size         = size,
        .tx_for_probe = c2s_wr,
        .rx_for_probe = s2c_rd,
    };

    int pid = rfork(RFPROC, attach_probe_exec_thunk, &args);
    TEST_ASSERT(pid > 0, "rfork failed for /attach-probe");

    int status = -42;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, 0,
        "/attach-probe exit status (0 = PASS: attach_9p + mount + unmount round-trip)");

    // 4. The probe is dead; its handle table is gone; its closes have
    //    propagated EOF to the c2s ring (write_eof set on the c2s
    //    ring at probe's t_close(tx_fd) since that was the LAST ref).
    //    The responder's next dev_read returns 0 → sets
    //    g_responder_finished then runs its terminal EXITING handshake.
    //    Sched() until we observe the flag.
    for (int i = 0; i < 256 && !g_responder_finished; i++) sched();
    if (!g_responder_finished) {
        uart_puts("    boot: responder not finished after 256 scheds; "
                  "msgs handled = ");
        uart_putdec((u64)g_responder_msgs_handled);
        uart_puts("\n");
    }
    TEST_ASSERT(g_responder_finished,
        "responder thread finished after probe exit");

    // #108: reap the responder kthread (the loom_free join idiom). It has set
    // g_responder_finished then run its EXITING handshake; wait for the RELEASE
    // flag (pairs with the responder's store -> state==EXITING visible so
    // thread_free's not-RUNNING gate passes; thread_free then spins on on_cpu --
    // cleared by the next thread's finish-task-switch -- so the kstack reclaim
    // waits until the responder is physically off its CPU, #788), then free it.
    // Without this the responder leaked as a runnable thread the scheduler spun
    // on every CPU forever -- the #108 idle-CPU burn.
    while (!__atomic_load_n(&g_responder_exited, __ATOMIC_ACQUIRE)) sched();
    thread_free(responder);

    // No boot-side spoor_clunks needed — all 4 Spoor refs were
    // transferred (responder owns 2; probe's handle table owned 2;
    // probe's t_close + responder's terminal spoor_clunks released
    // them). Local pointers c2s_rd / c2s_wr / s2c_rd / s2c_wr are
    // dangling at this point and must not be dereferenced.

    uart_puts("    /attach-probe reaped pid=");
    uart_putdec((u64)pid);
    uart_puts(" status=");
    uart_putdec((u64)status);
    uart_puts(" responder_msgs=");
    uart_putdec((u64)g_responder_msgs_handled);
    uart_puts(" — SYS_ATTACH_9P + SYS_MOUNT + SYS_UNMOUNT verified end-to-end\n");

    // Sanity: responder handled at least Tversion + Tattach = 2.
    //
    // Why not 3 (Tclunk)? p9_session_send_clunk rejects clunking
    // root_fid by design — root fids are session-scoped and get
    // cleaned up implicitly at session close (p9_session_close is
    // "no wire op"). p9_attached_destroy ignores the clunk-rejected
    // return for that reason. Confirmed at the spec layer
    // (specs/9p_client.tla::SendClunk precondition).
    //
    // The Tversion + Tattach pair is sufficient evidence of the
    // SVC dispatch → wire-codec → real-pipe-transport → responder
    // chain working end-to-end.
    TEST_ASSERT(g_responder_msgs_handled >= 2,
        "responder handled at least Tversion + Tattach");
}
