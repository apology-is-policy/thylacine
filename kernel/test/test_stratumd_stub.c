// /stratumd-stub + /attach-probe end-to-end regression test
// (P5-stratumd-stub-bringup-a).
//
// This is the first Phase 5 test that runs TWO userspace Procs
// cooperating over a pair of pipes. The kernel test framework
// wires them up; the Procs themselves are independent userspace
// binaries.
//
// Pipeline:
//   build → /stratumd-stub + /attach-probe live in the cpio.
//   this test:
//     1. pipe_create × 2 — 4 Spoors. Pipes are independent half-
//        duplex rings:
//        - c2s (client → server): c2s_wr (client side) + c2s_rd (server).
//        - s2c (server → client): s2c_wr (server) + s2c_rd (client).
//     2. rfork stratumd-stub Proc. Thunk installs c2s_rd at fd 0
//        (server reads Tmsgs) + s2c_wr at fd 1 (server writes
//        Rmsgs) BEFORE exec_setup. stub binary's main() finds the
//        transport fds already populated.
//     3. rfork /attach-probe Proc. Thunk installs c2s_wr at fd 0
//        (client writes Tmsgs) + s2c_rd at fd 1 (client reads
//        Rmsgs) BEFORE exec_setup. (Same convention as the existing
//        test_attach_probe.c: probe's fd 0 = tx, fd 1 = rx.)
//     4. wait_pid × 2. Both children exit 0 in close succession:
//        - /attach-probe finishes first (PASS message + exit 0).
//        - /attach-probe's t_close(tx_fd) drops c2s_wr to ref 0;
//          devpipe_close fires; write_eof on c2s ring.
//        - /stratumd-stub's read on c2s_rd returns 0 (EOF); stub
//          breaks loop, prints "EOF on rx; exit 0", exits.
//     5. Both reaped via wait_pid; status assertions check both 0.
//
// What this verifies that test_attach_probe.c doesn't:
//   - A real userspace process serves the 9P responder role (not
//     a kernel kthread). This is the production shape for real
//     stratumd: a userspace Proc holding two transport Spoors,
//     reading Tmsgs in a loop, writing Rmsgs back.
//   - Two userspace Procs can be wired together by the kernel
//     before either runs (kernel-supervised setup). The pipe
//     ends are transferred from boot's local pointers into each
//     child's handle table via the rfork thunk, with no need for
//     RFFDG (shared fd table) or pivot mechanisms — those land
//     in subsequent chunks.
//
// Refcount discipline (load-bearing — same as test_attach_probe):
//   Each pipe_create Spoor's ref=1 is TRANSFERRED (not bumped) to
//   its owner's handle table via handle_alloc. Boot holds ZERO
//   refs after the rfork thunks run. The 4 Spoor refs:
//     - c2s_rd → stub's handle table fd 0
//     - s2c_wr → stub's handle table fd 1
//     - c2s_wr → client's handle table fd 0
//     - s2c_rd → client's handle table fd 1
//   When each Proc exits, handle_table_free runs spoor_clunk on
//   each fd. The last drop fires devpipe_close, propagating EOF.
//   If boot held any residual ref, the EOF wouldn't propagate and
//   the stub would block forever on the next read.

#include "test.h"

#include <thylacine/devramfs.h>
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/pipe.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/spoor.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../../arch/arm64/uart.h"

void test_stratumd_stub_round_trip(void);

// Each userspace binary at this chunk is well under 32 KiB.
// (attach-probe ≈ 15 KiB; stratumd-stub similar.)
#define STUB_BLOB_MAX 32768

// Two independent buffers — one per Proc — since the rfork'd children
// run concurrently and exec_setup reads from the blob asynchronously.
static _Alignas(16) u8 g_stub_blob[STUB_BLOB_MAX];
static _Alignas(16) u8 g_client_blob[STUB_BLOB_MAX];

struct stub_exec_args {
    const void   *blob;
    size_t        size;
    struct Spoor *fd0_spoor;
    struct Spoor *fd1_spoor;
    const char   *diag_label;   // for failure diagnostics
};

__attribute__((noreturn))
static void stub_exec_thunk(void *arg) {
    struct stub_exec_args *ea = (struct stub_exec_args *)arg;
    struct Thread *t = current_thread();
    if (!t) extinction("stub_exec_thunk: no current_thread");
    struct Proc *p = t->proc;
    if (!p) extinction("stub_exec_thunk: no proc");

    hidx_t fd0 = handle_alloc(p, KOBJ_SPOOR,
                              RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER,
                              ea->fd0_spoor);
    if (fd0 != 0) {
        uart_puts("    ");
        uart_puts(ea->diag_label);
        uart_puts(": fd0 install got unexpected fd ");
        uart_putdec((u64)fd0);
        uart_puts("\n");
        exits("fail-fd0");
    }
    hidx_t fd1 = handle_alloc(p, KOBJ_SPOOR,
                              RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER,
                              ea->fd1_spoor);
    if (fd1 != 1) {
        uart_puts("    ");
        uart_puts(ea->diag_label);
        uart_puts(": fd1 install got unexpected fd ");
        uart_putdec((u64)fd1);
        uart_puts("\n");
        exits("fail-fd1");
    }

    u64 entry = 0, sp = 0;
    int rc = exec_setup(p, ea->blob, ea->size, &entry, &sp);
    if (rc != 0) {
        uart_puts("    ");
        uart_puts(ea->diag_label);
        uart_puts(": exec_setup rc=");
        uart_putdec((u64)rc);
        uart_puts("\n");
        exits("fail-exec");
    }

    userland_enter(entry, sp);
}

static int load_binary(const char *name, u8 *dst, size_t dst_cap, size_t *out_size) {
    const void *cpio_blob = NULL;
    size_t      size = 0;

    int rc = devramfs_lookup(name, &cpio_blob, &size);
    if (rc != 0)              return -1;
    if (size > dst_cap)       return -2;

    const u8 *src = (const u8 *)cpio_blob;
    for (size_t i = 0; i < size; i++) dst[i] = src[i];
    *out_size = size;
    return 0;
}

// Reap one zombie child by pid. Returns 0 on success, -1 on mismatch.
// wait_pid may return either child first; the caller handles ordering.
static int reap_one(int *out_pid, int *out_status) {
    *out_pid    = wait_pid(out_status);
    if (*out_pid < 0) return -1;
    return 0;
}

void test_stratumd_stub_round_trip(void) {
    size_t stub_size = 0, client_size = 0;
    int rc;

    rc = load_binary("stratumd-stub", g_stub_blob, STUB_BLOB_MAX, &stub_size);
    if (rc != 0) {
        uart_puts("    [skip] /stratumd-stub not in ramfs "
                  "(build with: tools/build.sh all)\n");
        return;
    }
    rc = load_binary("attach-probe", g_client_blob, STUB_BLOB_MAX, &client_size);
    if (rc != 0) {
        uart_puts("    [skip] /attach-probe not in ramfs "
                  "(build with: tools/build.sh all)\n");
        return;
    }

    uart_puts("    /stratumd-stub size=");
    uart_putdec((u64)stub_size);
    uart_puts(" + /attach-probe size=");
    uart_putdec((u64)client_size);
    uart_puts(" — wiring 2 userspace Procs via 2 pipes\n");

    struct Spoor *c2s_rd = NULL, *c2s_wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&c2s_rd, &c2s_wr), 0,
        "pipe_create c2s (client → server)");
    struct Spoor *s2c_rd = NULL, *s2c_wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&s2c_rd, &s2c_wr), 0,
        "pipe_create s2c (server → client)");

    // Spawn the stub first so it's blocked on read by the time the
    // client rforks + drives its first Tversion. Order is not strictly
    // required (the c2s ring buffers up to PIPE_BUF_SIZE) but feels
    // closer to production sequencing where the server is ready before
    // the client connects.
    struct stub_exec_args stub_args = {
        .blob       = g_stub_blob,
        .size       = stub_size,
        .fd0_spoor  = c2s_rd,
        .fd1_spoor  = s2c_wr,
        .diag_label = "stratumd-stub",
    };
    int stub_pid = rfork(RFPROC, stub_exec_thunk, &stub_args);
    TEST_ASSERT(stub_pid > 0, "rfork for /stratumd-stub failed");

    struct stub_exec_args client_args = {
        .blob       = g_client_blob,
        .size       = client_size,
        .fd0_spoor  = c2s_wr,
        .fd1_spoor  = s2c_rd,
        .diag_label = "attach-probe",
    };
    int client_pid = rfork(RFPROC, stub_exec_thunk, &client_args);
    TEST_ASSERT(client_pid > 0, "rfork for /attach-probe failed");

    // Reap both children. wait_pid returns whichever zombie is ready
    // first — the order matters only for diagnostic clarity, not for
    // correctness. We collect both PIDs and statuses, then assert.
    int first_pid = -1,  first_status  = -42;
    int second_pid = -1, second_status = -42;
    TEST_EXPECT_EQ(reap_one(&first_pid,  &first_status),  0, "reap first child");
    TEST_EXPECT_EQ(reap_one(&second_pid, &second_status), 0, "reap second child");

    // Both children should be the ones we spawned, in some order.
    bool got_stub   = (first_pid == stub_pid)   || (second_pid == stub_pid);
    bool got_client = (first_pid == client_pid) || (second_pid == client_pid);
    TEST_ASSERT(got_stub,   "stratumd-stub child reaped");
    TEST_ASSERT(got_client, "attach-probe child reaped");

    int stub_status   = (first_pid == stub_pid)   ? first_status  : second_status;
    int client_status = (first_pid == client_pid) ? first_status  : second_status;

    TEST_EXPECT_EQ(client_status, 0,
        "/attach-probe exited 0 (round-trip via userspace stratumd-stub)");
    TEST_EXPECT_EQ(stub_status, 0,
        "/stratumd-stub exited 0 (clean EOF on rx)");

    uart_puts("    stratumd-stub pid=");
    uart_putdec((u64)stub_pid);
    uart_puts(" status=");
    uart_putdec((u64)stub_status);
    uart_puts(" + attach-probe pid=");
    uart_putdec((u64)client_pid);
    uart_puts(" status=");
    uart_putdec((u64)client_status);
    uart_puts(" — userspace 9P server end-to-end verified\n");
}
