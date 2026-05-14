// /stub-driver regression test (P5-stratumd-stub-bringup-b).
//
// Loads /stub-driver from devramfs and rfork-execs it. /stub-driver
// itself does the full production-shape orchestration:
//   1. t_pipe × 2 → 4 fds.
//   2. t_spawn_with_fds("stratumd-stub", [c2s_rd, s2c_wr]) → spawns
//      stub with two fds pre-installed.
//   3. t_close on the driver-side copies of the stub's fds.
//   4. t_attach_9p(c2s_wr, s2c_rd, "/", ...) → 9P round-trip.
//   5. t_mount + t_unmount + t_close all.
//   6. t_wait_pid for stub.
//   7. t_exits(0) on success.
//
// This test framework's only job is to load + spawn /stub-driver +
// wait for it + check status=0. All the architectural pieces being
// proven (SYS_SPAWN_WITH_FDS + production-shape orchestration) run
// entirely at EL0 inside /stub-driver.
//
// What this verifies that test_stratumd_stub.c (kernel-supervised)
// doesn't:
//   - SYS_SPAWN_WITH_FDS exercised end-to-end at EL0 (not just via
//     the for_proc inner).
//   - The fd-inheritance refcount discipline (parent bumps; child
//     consumes via handle_alloc; parent's own holds unaffected).
//   - The driver-closes-its-side-then-stub-sees-EOF cascade through
//     real SYS_CLOSE rather than kernel-side spoor_clunk.

#include "test.h"

#include <thylacine/devramfs.h>
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../../arch/arm64/uart.h"

void test_stub_driver_round_trip(void);

#define STUB_DRIVER_BLOB_MAX 32768
static _Alignas(16) u8 g_stub_driver_blob[STUB_DRIVER_BLOB_MAX];

struct stub_driver_args {
    const void *blob;
    size_t      size;
};

__attribute__((noreturn))
static void stub_driver_thunk(void *arg) {
    struct stub_driver_args *ea = (struct stub_driver_args *)arg;
    struct Thread *t = current_thread();
    if (!t) extinction("stub_driver_thunk: no current_thread");
    struct Proc *p = t->proc;
    if (!p) extinction("stub_driver_thunk: no proc");

    u64 entry = 0, sp = 0;
    int rc = exec_setup(p, ea->blob, ea->size, &entry, &sp);
    if (rc != 0) {
        uart_puts("    stub-driver exec_setup rc=");
        uart_putdec((u64)rc);
        uart_puts("\n");
        exits("fail-exec");
    }

    userland_enter(entry, sp);
}

void test_stub_driver_round_trip(void) {
    const void *cpio_blob = NULL;
    size_t size = 0;

    int rc = devramfs_lookup("stub-driver", &cpio_blob, &size);
    if (rc != 0) {
        uart_puts("    [skip] /stub-driver not in ramfs "
                  "(build with: tools/build.sh all)\n");
        return;
    }
    TEST_ASSERT(size <= STUB_DRIVER_BLOB_MAX,
                "stub-driver binary too large for static buffer");

    const u8 *src = (const u8 *)cpio_blob;
    for (size_t i = 0; i < size; i++) g_stub_driver_blob[i] = src[i];

    uart_puts("    /stub-driver size=");
    uart_putdec((u64)size);
    uart_puts(" → exec; driver orchestrates stratumd-stub itself\n");

    struct stub_driver_args args = { .blob = g_stub_driver_blob, .size = size };
    int pid = rfork(RFPROC, stub_driver_thunk, &args);
    TEST_ASSERT(pid > 0, "rfork for /stub-driver failed");

    int status = -42;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, 0,
        "/stub-driver exit status (0 = PASS: production-shape orchestration verified)");

    // /stub-driver waits for its own child (stratumd-stub) before
    // exiting, so by the time we observe driver's zombie, stratumd-stub
    // is already reaped. No second wait_pid needed.

    uart_puts("    /stub-driver reaped pid=");
    uart_putdec((u64)pid);
    uart_puts(" status=");
    uart_putdec((u64)status);
    uart_puts(" — SYS_SPAWN_WITH_FDS production-shape orchestration verified end-to-end\n");
}
