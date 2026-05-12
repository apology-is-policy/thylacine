// /mmio-probe regression test (P4-Ic5a).
//
// First end-to-end userspace + hw-handle test:
//   1. kproc (CAP_ALL) rforks /mmio-probe via rfork_with_caps with
//      caps_mask=CAP_HW_CREATE. Child Proc starts with caps =
//      (kproc.caps & CAP_HW_CREATE) = CAP_HW_CREATE.
//   2. exec_setup loads the Rust no_std mmio-probe ELF from the
//      devramfs (cpio bound at boot via -initrd).
//   3. userland_enter delivers control to EL0; /mmio-probe's _start
//      (from libthyla-rs) calls rs_main.
//   4. rs_main calls t_mmio_create + t_mmio_map for QEMU virt's
//      virtio-mmio slot 31 page (PA 0x0a003000).
//   5. SVC dispatch routes to sys_mmio_create_handler +
//      sys_mmio_map_handler. Each handler verifies CAP_HW_CREATE in
//      child->caps + range / handle / prot / rights validation.
//   6. First MMIO read at user-VA 0x500e00 page-faults →
//      userland_demand_page → case BURROW_TYPE_MMIO → device-memory
//      PTE installed at MAIR_IDX_DEVICE (nGnRnE).
//   7. Subsequent reads return live virtio-mmio register values
//      (MAGIC = 0x74726976 "virt"; DEVICE_ID = 4 = RNG).
//   8. /mmio-probe exits 0 via SYS_EXITS.
//   9. wait_pid reaps; test verifies exit_status == 0.
//
// Closes the bulk of deferred R10 F159 (SVC-path test coverage for
// SYS_MMIO_CREATE + SYS_MMIO_MAP). The remaining SVC paths
// (SYS_IRQ_CREATE + SYS_IRQ_WAIT) are exercised by the future
// P4-Ic5b/c chunks (virtqueue + IRQ-driven block reads).
//
// What this test specifically guards against:
//   - HwHandleImpliesCap regression: if a future refactor decoupled
//     handle creation from cap check, the child would have CAP_HW_CREATE
//     but the syscall would still fail because the validation is at
//     the syscall surface; OR if the cap check was removed, the child
//     would succeed with caps = CAP_NONE in a future test variant
//     (forward-looking guard).
//   - CapsCeiling regression at the rfork-grant layer: if rfork_with_caps
//     somehow granted MORE than the AND-with-parent permits (the
//     BuggyRforkElevate class from specs/handles.tla), the child would
//     hold bits it shouldn't and downstream tests would observe.
//   - Demand-page MMIO dispatch regression: if userland_demand_page lost
//     the BURROW_TYPE_MMIO case (or installed Normal-WB PTE attrs
//     instead of Device-nGnRnE), the MMIO_MAGIC read would return 0 or
//     stale RAM contents instead of the live "virt" magic, and the
//     probe would exit 1.
//
// If /mmio-probe wasn't built (fresh checkout where `tools/build.sh
// userspace` hasn't run yet), the test prints a skip notice and
// returns PASS. Production / CI always builds the full ramfs.

#include "test.h"

#include <thylacine/caps.h>
#include <thylacine/devramfs.h>
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../../arch/arm64/uart.h"

void test_mmio_probe_rfork_with_caps(void);

// 8-aligned static buffer for the loaded ELF blob (R5-G F61 alignment
// requirement on the Ehdr cast in exec_setup). P4-Ic7 shrunk 256 → 128 KiB
// so the cumulative kernel-image .bss + firmware reserve fits the 2 MiB
// L3 mapping (arch/arm64/mmu.c::mmu_map_kernel). mmio-probe compiles
// to ~67 KiB; 96 KiB (P4-Jc shrink) still leaves ~29 KiB headroom.
#define MMIO_PROBE_BLOB_MAX 98304
static _Alignas(16) u8 g_mmio_probe_blob[MMIO_PROBE_BLOB_MAX];

struct mmio_probe_exec_args {
    const void *blob;
    size_t      size;
};

__attribute__((noreturn))
static void mmio_probe_exec_thunk(void *arg) {
    struct mmio_probe_exec_args *ea = (struct mmio_probe_exec_args *)arg;
    struct Thread *t = current_thread();
    if (!t) extinction("mmio_probe_exec_thunk: no current_thread");
    struct Proc *p = t->proc;
    if (!p) extinction("mmio_probe_exec_thunk: no proc");

    // Diagnostic: confirm the child's caps reflect the rfork_with_caps
    // grant. CAP_HW_CREATE must be set; the child's caps must be a
    // proper subset of kproc's CAP_ALL (CapsCeiling — proven at the
    // spec level by RforkWithCaps's `granted ⊆ proc_caps[parent]`).
    if ((p->caps & CAP_HW_CREATE) == 0) {
        uart_puts("    mmio_probe_exec_thunk: child lacks CAP_HW_CREATE (rfork_with_caps grant lost?)\n");
        exits("fail-no-cap");
    }

    u64 entry = 0, sp = 0;
    int rc = exec_setup(p, ea->blob, ea->size, &entry, &sp);
    if (rc != 0) {
        uart_puts("    exec_setup rc=");
        uart_putdec((u64)rc);
        uart_puts(" → exits(fail-exec)\n");
        exits("fail-exec");
    }

    uart_puts("    exec_setup ok entry=");
    uart_puthex64(entry);
    uart_puts(" sp=");
    uart_puthex64(sp);
    uart_puts(" caps=0x");
    uart_puthex64(p->caps);
    uart_puts(" → userland_enter\n");

    userland_enter(entry, sp);
}

void test_mmio_probe_rfork_with_caps(void) {
    const void *cpio_blob = NULL;
    size_t size = 0;

    int rc = devramfs_lookup("mmio-probe", &cpio_blob, &size);
    if (rc != 0) {
        uart_puts("    [skip] /mmio-probe not in ramfs (build with: tools/build.sh all)\n");
        return;
    }

    TEST_ASSERT(size <= MMIO_PROBE_BLOB_MAX,
                "mmio-probe binary too large for static buffer");

    const u8 *src = (const u8 *)cpio_blob;
    for (size_t i = 0; i < size; i++) g_mmio_probe_blob[i] = src[i];

    uart_puts("    /mmio-probe size=");
    uart_putdec((u64)size);
    uart_puts(" bytes → rfork_with_caps(CAP_HW_CREATE)\n");

    struct mmio_probe_exec_args args = {
        .blob = g_mmio_probe_blob, .size = size
    };

    // Grant the child exactly CAP_HW_CREATE (kproc has CAP_ALL = at
    // v1.0 just CAP_HW_CREATE per caps.h's _Static_assert pin, so the
    // AND in rfork_internal yields CAP_HW_CREATE on the child). The
    // child then has the cap it needs for SYS_MMIO_CREATE +
    // SYS_MMIO_MAP.
    int pid = rfork_with_caps(RFPROC, mmio_probe_exec_thunk, &args,
                              CAP_HW_CREATE);
    TEST_ASSERT(pid > 0, "rfork_with_caps failed for /mmio-probe");

    int status = -42;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, 0, "/mmio-probe exit status (0 = MAGIC ok)");

    uart_puts("    /mmio-probe reaped pid=");
    uart_putdec((u64)pid);
    uart_puts(" status=");
    uart_putdec((u64)status);
    uart_puts(" — SVC path verified end-to-end\n");
}
