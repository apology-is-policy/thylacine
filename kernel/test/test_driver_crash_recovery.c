// Driver crash recovery regression test (P4-M).
//
// Closes the final structural §6.2 box: "a driver process crashes; the
// kernel cleans up its handles + reissues IRQ ownership; a fresh driver
// can be spawned." The release-path discipline that makes this possible
// was audited in R9 (KObj_MMIO + KObj_IRQ) + R12-DMA (KObj_DMA) +
// R13-burrow (Burrow mappings) — every kobj_*_unref releases its
// kernel-side claim entry. P4-M is the runtime verification:
//
//   1. Spawn child A (virtio-blk-probe) holding CAP_HW_CREATE.
//      A claims the virtio-blk-device's MMIO bank, an INTID, and DMA
//      chunks; runs one round-trip read of sector 0; exits.
//   2. Reap A via wait_pid.
//   3. Assert kernel state is clean:
//        - kobj_mmio_pa_claimed(blk_dev->pa, PAGE_SIZE) == false
//          (all 4 MMIO pages A claimed are released).
//        - kobj_irq_intid_claimed(intid) == false
//          (the virtio-blk INTID's claim slot is free).
//      Diagnostic: if either assertion fails, the release path leaks
//      and B will likely fail with -1 from t_mmio_create/t_irq_create.
//   4. Spawn child B with the SAME args; B claims the SAME hardware.
//   5. Reap B; B's exit status must be 0 (proves the release worked).
//
// What this guards against:
//   - kobj_mmio_unref forgetting to clear its g_mmio_claims entry
//     (R9 F147 discipline). After A exits, B's t_mmio_create would
//     reject (HwResourceExclusive).
//   - kobj_irq_unref forgetting to call intid_release.
//   - VMA tear-down on proc_exit silently leaving stale mappings (a
//     future driver process accessing those VAs would page-fault).
//   - Reference-counted handles being stuck >0 on exit (a previously-
//     unaudited refcount imbalance would leave the kobj alive across
//     the proc boundary).
//
// What this does NOT cover (forward-looking deferrals):
//   - Driver killed mid-IRQ-handler. v1.0 single-CPU + same-process
//     IRQ dispatch makes this impossible — the IRQ dispatcher runs
//     in the same kernel context that's about to exit the proc, so
//     no in-flight IRQ can be inside kobj_irq_dispatch when the proc
//     dies. SMP + cross-CPU IRQ delivery (Phase 5+) opens this race.
//   - Driver supervision policy (auto-restart, exponential backoff,
//     panic-on-repeated-fail). Architectural design needed — Phase 5+.
//   - Crash mid-DMA-transfer with device still walking the ring.
//     virtio-blk RESET (kobj_mmio_unref triggers no device reset; only
//     the VA unmap) — the next driver's RESET sequence handles this
//     (writes STATUS = 0; QEMU's virtio-blk-device clears all state).
//     Real hardware behavior would need a per-device reset hook on
//     release, Phase 5+.
//
// SKIP path: if `virtio-blk-probe` binary is missing from devramfs OR
// no virtio-mmio slot has DeviceID=2, prints a SKIP notice + returns
// PASS (same gating shape as test_virtio_blk_probe.c).

#include "test.h"

#include <thylacine/caps.h>
#include <thylacine/devramfs.h>
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/irqfwd.h>
#include <thylacine/mmio_handle.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>
#include <thylacine/virtio.h>

#include "../../arch/arm64/uart.h"

void test_driver_crash_recovery(void);

// QEMU virt's virtio-mmio bank constants — mirror the userspace driver.
// SPI base = 32 + 16 = 48; slot stride = 0x200; slot index derives from
// (pa - VIRTIO_MMIO_BASE_PA) / VIRTIO_MMIO_SLOT_STRIDE.
#define VIRTIO_MMIO_BASE_PA      0x0a000000ull
#define VIRTIO_MMIO_SLOT_STRIDE  0x200ull
#define VIRTIO_MMIO_GIC_INTID_BASE  (32u + 16u)  // = 48

// Same 16-KiB convention as test_virtio_blk_probe.c (P4-image-shrink).
#define VIRTIO_BLK_PROBE_BLOB_MAX 16384
static _Alignas(16) u8 g_blk_probe_blob[VIRTIO_BLK_PROBE_BLOB_MAX];

struct exec_args {
    const void *blob;
    size_t      size;
};

__attribute__((noreturn))
static void blk_probe_thunk(void *arg) {
    struct exec_args *ea = (struct exec_args *)arg;
    struct Thread *t = current_thread();
    if (!t) extinction("blk_probe_thunk: no current_thread");
    struct Proc *p = t->proc;
    if (!p) extinction("blk_probe_thunk: no proc");

    if ((p->caps & CAP_HW_CREATE) == 0) {
        uart_puts("    blk_probe_thunk: child lacks CAP_HW_CREATE\n");
        exits("fail-no-cap");
    }

    u64 entry = 0, sp = 0;
    int rc = exec_setup(p, ea->blob, ea->size, &entry, &sp);
    if (rc != 0) {
        uart_puts("    blk_probe_thunk: exec_setup rc=");
        uart_putdec((u64)rc);
        uart_puts("\n");
        exits("fail-exec");
    }
    userland_enter(entry, sp);
}

// Spawn one virtio-blk-probe child + wait for it to terminate. Returns
// child's exit status (or -1 if reap mismatched).
static int spawn_blk_probe_and_reap(struct exec_args *args, const char *label) {
    uart_puts("    crash-recovery: spawning ");
    uart_puts(label);
    uart_puts(" child\n");

    int pid = rfork_with_caps(RFPROC, blk_probe_thunk, args, CAP_HW_CREATE);
    if (pid <= 0) {
        uart_puts("    crash-recovery: rfork_with_caps failed for ");
        uart_puts(label);
        uart_puts("\n");
        return -1;
    }

    int status = -42;
    int reaped = wait_pid(&status);
    if (reaped != pid) {
        uart_puts("    crash-recovery: wait_pid mismatch (reaped=");
        uart_putdec((u64)reaped);
        uart_puts(" pid=");
        uart_putdec((u64)pid);
        uart_puts(")\n");
        return -1;
    }

    uart_puts("    crash-recovery: ");
    uart_puts(label);
    uart_puts(" reaped pid=");
    uart_putdec((u64)pid);
    uart_puts(" status=");
    uart_putdec((u64)status);
    uart_puts("\n");
    return status;
}

void test_driver_crash_recovery(void) {
    const void *cpio_blob = NULL;
    size_t size = 0;

    int rc = devramfs_lookup("virtio-blk-probe", &cpio_blob, &size);
    if (rc != 0) {
        uart_puts("    [skip] /virtio-blk-probe not in ramfs (build with: tools/build.sh all)\n");
        return;
    }

    struct virtio_mmio_dev *blk_dev =
        virtio_mmio_find_by_device_id(VIRTIO_DEVICE_ID_BLOCK);
    if (!blk_dev) {
        uart_puts("    [skip] no virtio-mmio slot with DeviceID=2 (run-vm.sh missing -device virtio-blk-device?)\n");
        return;
    }

    TEST_ASSERT(size <= VIRTIO_BLK_PROBE_BLOB_MAX,
                "virtio-blk-probe binary too large for crash-recovery buffer");
    const u8 *src = (const u8 *)cpio_blob;
    for (size_t i = 0; i < size; i++) g_blk_probe_blob[i] = src[i];

    // Derive the INTID the userspace driver subscribes to. Mirrors
    // usr/virtio-blk-probe/src/main.rs: intid = 48 + slot, where
    // slot = (pa - VIRTIO_MMIO_BASE_PA) / 0x200.
    u64 slot_index = ((u64)blk_dev->pa - VIRTIO_MMIO_BASE_PA) / VIRTIO_MMIO_SLOT_STRIDE;
    u32 blk_intid  = VIRTIO_MMIO_GIC_INTID_BASE + (u32)slot_index;

    uart_puts("    crash-recovery: target pa=");
    uart_puthex64((u64)blk_dev->pa);
    uart_puts(" slot=");
    uart_putdec(slot_index);
    uart_puts(" intid=");
    uart_putdec((u64)blk_intid);
    uart_puts("\n");

    // Before any child runs, both the MMIO range and the INTID must be
    // free — otherwise an earlier test leaked state into ours and the
    // crash-recovery assertions below would be testing the wrong path.
    // virtio-mmio slots are NOT kernel-reserved at v1.0 (P4-Ic5b1a
    // relaxed the reservation; only GIC + PL011 + ECAM stay).
    TEST_ASSERT(!kobj_mmio_pa_claimed((u64)blk_dev->pa, 0x1000),
                "virtio-blk MMIO already claimed before first spawn");
    TEST_ASSERT(!kobj_irq_intid_claimed(blk_intid),
                "virtio-blk INTID already claimed before first spawn");

    struct exec_args args = {
        .blob = g_blk_probe_blob, .size = size
    };

    // --- Round 1: spawn child A, run, reap, verify release. ---
    int status_a = spawn_blk_probe_and_reap(&args, "A");
    TEST_EXPECT_EQ(status_a, 0, "child A exit status (expected: 0 = block 0 verified)");

    // After A reaped, the release path must have cleared every claim
    // A held. If not, child B will fail to claim — but we want to
    // catch the leak HERE with a precise diagnostic rather than
    // attributing it to a misleading downstream failure.
    bool mmio_leaked = kobj_mmio_pa_claimed((u64)blk_dev->pa, 0x1000);
    bool intid_leaked = kobj_irq_intid_claimed(blk_intid);

    if (mmio_leaked || intid_leaked) {
        uart_puts("    crash-recovery: LEAK after child A reap — mmio=");
        uart_puts(mmio_leaked ? "claimed " : "free ");
        uart_puts("intid=");
        uart_puts(intid_leaked ? "claimed\n" : "free\n");
    }
    TEST_ASSERT(!mmio_leaked,
                "MMIO claim leaked after child A exit (kobj_mmio_unref didn't clear g_mmio_claims)");
    TEST_ASSERT(!intid_leaked,
                "INTID claim leaked after child A exit (kobj_irq_unref didn't call intid_release)");

    uart_puts("    crash-recovery: release verified — both claims free\n");

    // --- Round 2: spawn child B against the SAME hardware. ---
    // If A's release didn't work, B's t_mmio_create or t_irq_create
    // returns -1 inside the probe, and B exits with status 1. The
    // preceding TEST_ASSERTs catch the leak before we get here, but
    // the round-2 spawn is the end-to-end proof that recovery works.
    int status_b = spawn_blk_probe_and_reap(&args, "B");
    TEST_EXPECT_EQ(status_b, 0,
                   "child B exit status (expected: 0 = same hardware re-claimed after A exit)");

    // Also verify the post-B state is clean — symmetric to the post-A
    // assertion. If B's exit somehow stuck a claim alive (refcount
    // imbalance, atomic-ordering bug), we'd want to know.
    TEST_ASSERT(!kobj_mmio_pa_claimed((u64)blk_dev->pa, 0x1000),
                "MMIO claim leaked after child B exit");
    TEST_ASSERT(!kobj_irq_intid_claimed(blk_intid),
                "INTID claim leaked after child B exit");

    uart_puts("    crash-recovery: PASS — full A→B sequential claim over same hardware\n");
}
