// loom.bridge_probe -- the private Loom syscall bridge reached from EL0
// (docs/ASYNC-SERVICE-ABI.md, "The EL0 probe").
//
// /bin/loom-bridge-probe exits 70 after checking every private route's refusal,
// or 71 after driving every route in the qualification image. The expected code
// comes from THIS kernel's lever, never from what the probe saw, so a kernel that
// routes when it should refuse (or the reverse) fails here. The probe must make
// exactly the expected number of private rings, and every one must retire.
#include "test.h"
#include <thylacine/devramfs.h>
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/loom.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>
#include "../../arch/arm64/timer.h"
#include "../../arch/arm64/uart.h"

void test_loom_bridge_probe(void);

#ifdef LOOM_PRIVATE_QUALIFICATION
#define BRIDGE_PROBE_STATUS 71
#define BRIDGE_PROBE_RINGS  4ull // the first, two rolled back, the last
#else
#define BRIDGE_PROBE_STATUS 70
#define BRIDGE_PROBE_RINGS  0ull
#endif

#define BRIDGE_PROBE_BLOB_MAX 98304
static _Alignas(16) u8 g_bridge_probe_blob[BRIDGE_PROBE_BLOB_MAX];

struct bridge_probe_exec_args {
    const void *blob;
    size_t      size;
};

__attribute__((noreturn))
static void bridge_probe_exec_thunk(void *arg) {
    struct bridge_probe_exec_args *ea = (struct bridge_probe_exec_args *)arg;
    struct Thread *t = current_thread();
    if (!t || !t->proc) extinction("bridge_probe_exec_thunk: no current proc");
    u64 entry = 0, sp = 0;
    int rc = exec_setup(t->proc, ea->blob, ea->size, &entry, &sp);
    if (rc != 0) {
        uart_puts("    loom-bridge-probe exec_setup rc=");
        uart_putdec((u64)rc);
        uart_puts("\n");
        exits("fail-exec");
    }
    userland_enter(entry, sp);
}

void test_loom_bridge_probe(void) {
    const void *cpio_blob = NULL;
    size_t size = 0;
    if (devramfs_lookup("bin/loom-bridge-probe", &cpio_blob, &size) != 0) {
        // Only a kernel booted without userspace may skip; a ramfs that has init
        // but not the probe is stale, and skipping would pass it.
        const void *init_blob = NULL;
        size_t init_size = 0;
        TEST_ASSERT(devramfs_lookup("bin/joey", &init_blob, &init_size) != 0,
                    "an image that carries userspace carries /bin/loom-bridge-probe");
        uart_puts("    [skip] /bin/loom-bridge-probe not in ramfs (build with: tools/build.sh all)\n");
        return;
    }
    TEST_ASSERT(size <= BRIDGE_PROBE_BLOB_MAX, "loom-bridge-probe too large for its buffer");
    // cpio is 4-aligned; the ELF header cast needs 8.
    const u8 *src = (const u8 *)cpio_blob;
    for (size_t i = 0; i < size; i++) g_bridge_probe_blob[i] = src[i];

    u64 made0 = loom_private_created(), gone0 = loom_private_retired();
    struct bridge_probe_exec_args args = { .blob = g_bridge_probe_blob, .size = size };
    int pid = rfork(RFPROC, bridge_probe_exec_thunk, &args);
    TEST_ASSERT(pid > 0, "rfork failed for /loom-bridge-probe");
    int status = -42;
    int reaped = wait_pid(&status);

    // The retirer is its own thread. Both counts are monotonic and every created
    // ring is eventually retired, so wait until the probe's rings have all gone.
    u64 deadline = timer_now_ns() + 5000000000ull;
    while (loom_private_retired() - gone0 < loom_private_created() - made0 &&
           timer_now_ns() < deadline)
        sched();
    u64 made = loom_private_created() - made0;
    u64 gone = loom_private_retired() - gone0;

    uart_puts("    loom-bridge-probe status=");
    uart_putdec((u64)status);
    uart_puts(" expected=");
    uart_putdec((u64)BRIDGE_PROBE_STATUS);
    uart_puts("\n    loom-bridge-probe rings made=");
    uart_putdec(made);
    uart_puts(" retired=");
    uart_putdec(gone);
    uart_puts(" expected=");
    uart_putdec(BRIDGE_PROBE_RINGS);
    uart_puts("\n");
    TEST_EXPECT_EQ(reaped, pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, BRIDGE_PROBE_STATUS,
        "loom-bridge-probe's verdict matches this kernel's qualification lever");
    TEST_EXPECT_EQ(made, BRIDGE_PROBE_RINGS, "the probe made exactly its private rings");
    TEST_EXPECT_EQ(gone, made, "every private ring the probe made retired");
}
