// /irq-probe regression test (P4-Ic5-IRQ-probe).
//
// Companion to test_mmio_probe.c (P4-Ic5a). Closes the IRQ side of
// deferred R10 F159: SVC-path test coverage for SYS_IRQ_CREATE +
// SYS_IRQ_WAIT exercised end-to-end from real EL0 userspace, not from
// kernel-context test code that calls kobj_irq_create / kobj_irq_wait
// directly (which the irqfwd tests already cover at the kernel layer).
//
// Setup:
//   1. devramfs_lookup("irq-probe", ...) — pre-built userspace ELF
//      from the cpio. Graceful skip if not built (fresh checkout).
//   2. gic_set_pending_spi(intid) — manually pend selected SPI
//      at GICD_ISPENDR<n>.bit. The IRQ is now pending but not yet
//      enabled (no driver has gic_enable_irq'd it), so the GIC keeps
//      it in pending state without delivering. Per ARM IHI 0069
//      §12.9.6 the pending bit is orthogonal to the enable bit: pending
//      stays set across enable/disable cycles, and an enable transition
//      with pending=1 delivers the IRQ immediately on the routed CPU
//      (Aff0=0 = CPU 0 per dist_init's IROUTER zeros).
//   3. rfork_with_caps(RFPROC, ..., CAP_HW_CREATE) — child starts with
//      child->caps = kproc.caps & CAP_HW_CREATE = CAP_HW_CREATE.
//
// Child execution:
//   1. exec_setup + userland_enter → _start (libthyla-rs) → rs_main.
//   2. rs_main calls t_irq_create(intid, T_RIGHT_SIGNAL):
//      - SVC enters EL1 with IRQs masked.
//      - sys_irq_create_handler validates cap + rights + intid.
//      - kobj_irq_create:
//        - intid_try_claim(intid): ✓ (intid is not in g_intid_claimed; only
//          SGI 0 and PPI 30 are pre-reserved at irqfwd_init).
//        - kmalloc + magic + ref=1.
//        - gic_attach(intid, kobj_irq_dispatch, k): handler slot set.
//        - gic_enable_irq(intid): GICD_ISENABLER<n>.bit set. Now both
//          enabled AND pending (from our pre-pend), so the GIC delivers
//          the IRQ to CPU 0 as soon as CPU 0 has IRQs unmasked.
//      - returns handle.
//      - ERET to EL0; SPSR restores PSTATE.I=0 → IRQs unmasked.
//   3. CPU 0 takes the pending IRQ (or has already taken it during the
//      enable's MMIO write if CPU 0 was at EL1 with IRQs unmasked when
//      we enabled — the per-CPU dispatch order doesn't matter, the
//      pending_count increment is under r->lock):
//      - gic_acknowledge → intid.
//      - gic_dispatch(intid) → kobj_irq_dispatch(intid, k).
//      - kobj_irq_dispatch under r->lock: pending_count = 1, drop lock,
//        wakeup(&k->rendez) (no waiter yet → no-op).
//      - gic_eoi(intid).
//   4. Child's rs_main calls t_irq_wait(handle):
//      - sys_irq_wait_handler validates handle + KOBJ_IRQ kind +
//        RIGHT_SIGNAL.
//      - kobj_irq_ref(k) (F143 borrow).
//      - kobj_irq_wait → sleep(rendez, cond=pending>0). cond returns
//        true (count=1 from step 3) → sleep does NOT block. Re-take
//        r->lock, count=1, zero pending_count, return 1.
//      - kobj_irq_unref(k).
//      - returns 1 to userspace.
//   5. rs_main verifies count == 1, exits 0.
//
// Test reaps via wait_pid + asserts exit_status == 0.
//
// What this test specifically guards against:
//   - HwHandleImpliesCap regression at the IRQ create side: if a
//     future refactor decouples handle alloc from cap check, the child
//     would create a KObj_IRQ without holding CAP_HW_CREATE. The
//     `child->caps & CAP_HW_CREATE` check in sys_irq_create_handler is
//     the canonical guard; this test exercises it positively (child
//     has the cap → succeeds). The negative regression
//     `caps.rfork_child_has_none` covers the "no cap → reject" path.
//   - F143 UAF regression: the borrow pattern (kobj_irq_ref before
//     sleep + kobj_irq_unref after) protects against the handle being
//     closed on another thread mid-sleep. This test doesn't directly
//     race a close, but the wait-then-return cycle exercises the
//     borrow machinery.
//   - F145 SPI-only regression: if a future refactor lowers the intid
//     bound (currently `intid < 32 → -1`), an attempt to claim
//     SGI/PPI from userspace would succeed. The negative regression
//     `handle_hw.irq_kernel_reserved_rejected` (R9 F142 close) covers
//     the kernel-reserved-INTID path; this test exercises the SPI
//     positive path.
//   - Wait/wake atomicity: the IRQ that fires before t_irq_wait is
//     not lost. cond is re-evaluated under r->lock; pending_count is
//     mutated under r->lock; sleep returns when cond is true. Pins
//     scheduler.tla I-9 NoMissedWakeup at runtime.
//
// If /irq-probe wasn't built (fresh checkout where `tools/build.sh
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

#include "../../arch/arm64/gic.h"
#include "../../arch/arm64/uart.h"

void test_irq_probe_rfork_with_caps(void);

// The kernel selects a synthetic SPI and passes it as argv[1]. The userspace
// probe has no platform/vector constant that can drift from MSI reservations.

// 8-aligned static buffer for the loaded ELF blob (R5-G F61 alignment
// requirement on the Ehdr cast in exec_setup). Sized to match the P4-K
// convention (16 KiB) after every userspace binary dropped under 16 KiB
// with -z max-page-size=4096 applied to both Rust and C toolchains.
#define IRQ_PROBE_BLOB_MAX 16384
static _Alignas(16) u8 g_irq_probe_blob[IRQ_PROBE_BLOB_MAX];

struct irq_probe_exec_args {
    const void *blob;
    size_t      size;
    u32         intid;
};

__attribute__((noreturn))
static void irq_probe_exec_thunk(void *arg) {
    struct irq_probe_exec_args *ea = (struct irq_probe_exec_args *)arg;
    struct Thread *t = current_thread();
    if (!t) extinction("irq_probe_exec_thunk: no current_thread");
    struct Proc *p = t->proc;
    if (!p) extinction("irq_probe_exec_thunk: no proc");

    // Diagnostic: confirm CAP_HW_CREATE survived the rfork_with_caps
    // grant. CapsCeiling pins this at the spec layer
    // (specs/handles.tla::RforkWithCaps: granted ⊆ proc_caps[parent]);
    // this assert catches a regression at the runtime layer.
    if ((p->caps & CAP_HW_CREATE) == 0) {
        uart_puts("    irq_probe_exec_thunk: child lacks CAP_HW_CREATE (rfork_with_caps grant lost?)\n");
        exits("fail-no-cap");
    }

    u64 entry = 0, sp = 0;
    char argv[32] = "irq-probe";
    u32 n = 10, v = ea->intid; char digits[10]; u32 nd = 0;
    do { digits[nd++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (nd) argv[n++] = digits[--nd];
    argv[n++] = 0;
    int rc = exec_setup_with_argv(p, ea->blob, ea->size, argv, n, 2, &entry, &sp);
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

void test_irq_probe_rfork_with_caps(void) {
    u32 intid = test_irq_choose_spi();
    TEST_ASSERT(intid != 0xffffffffu, "topology needs a synthetic test SPI");
    const void *cpio_blob = NULL;
    size_t size = 0;

    int rc = devramfs_lookup("irq-probe", &cpio_blob, &size);
    if (rc != 0) {
        uart_puts("    [skip] /irq-probe not in ramfs (build with: tools/build.sh all)\n");
        return;
    }

    TEST_ASSERT(size <= IRQ_PROBE_BLOB_MAX,
                "irq-probe binary too large for static buffer");

    const u8 *src = (const u8 *)cpio_blob;
    for (size_t i = 0; i < size; i++) g_irq_probe_blob[i] = src[i];

    uart_puts("    /irq-probe size=");
    uart_putdec((u64)size);
    uart_puts(" bytes → pre-pend SPI ");
    uart_putdec((u64)intid);
    uart_puts(" + rfork_with_caps(CAP_HW_CREATE)\n");

    // Pre-pend the SPI BEFORE spawning the child. The IRQ is pending
    // but not yet enabled, so it doesn't deliver yet. When the child's
    // t_irq_create calls gic_enable_irq(intid), the GIC sees pending+
    // enabled and delivers immediately (modulo CPU IRQ masking). This
    // makes the test race-free: the child's t_irq_wait will always
    // observe pending_count >= 1 by the time cond is evaluated.
    bool pended = gic_set_pending_spi(intid);
    TEST_ASSERT(pended, "gic_set_pending_spi failed for selected SPI");

    struct irq_probe_exec_args args = {
        .blob = g_irq_probe_blob, .size = size, .intid = intid
    };

    // Grant the child CAP_HW_CREATE (kproc has CAP_ALL = CAP_HW_CREATE
    // at v1.0; AND-with-parent yields CAP_HW_CREATE on the child).
    int pid = rfork_with_caps(RFPROC, irq_probe_exec_thunk, &args,
                              CAP_HW_CREATE);
    TEST_ASSERT(pid > 0, "rfork_with_caps failed for /irq-probe");

    int status = -42;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, 0, "/irq-probe exit status (0 = IRQ wait returned count>=1)");

    uart_puts("    /irq-probe reaped pid=");
    uart_putdec((u64)pid);
    uart_puts(" status=");
    uart_putdec((u64)status);
    uart_puts(" — IRQ SVC path verified end-to-end\n");
}
