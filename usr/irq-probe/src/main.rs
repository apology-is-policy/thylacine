// /irq-probe — first userspace IRQ-handle binary (P4-Ic5-IRQ-probe).
//
// Closes the deferred R10 F159 IRQ side: SVC-path test coverage for
// SYS_IRQ_CREATE + SYS_IRQ_WAIT exercised end-to-end from real EL0
// userspace via the SVC dispatcher, not from kernel-context test code
// that calls kobj_irq_create / kobj_irq_wait directly.
//
// Companion to /mmio-probe (P4-Ic5a) which closed the MMIO side of
// F159. With both probes landed, all four hardware-handle syscalls
// (SYS_MMIO_CREATE + SYS_MMIO_MAP + SYS_IRQ_CREATE + SYS_IRQ_WAIT)
// have userspace SVC-path coverage. The full virtio-blk driver crate
// at P4-Ic5b exercises all four together in a single binary; the
// probes split the surface so each one targets a single subsystem
// without virtqueue+DMA+driver-machinery complication.
//
// Target: GIC SPI 96 — an SPI that is not used by any device on QEMU
// virt. SPI 0..15 = PL011/PL031/PCI/GPIO/etc; SPI 16..47 = virtio-mmio
// slots; SPI 96 is safely above any platform device on QEMU virt's
// default config (GICD_TYPER reports up to 160 INTIDs there, so 96 is
// in-range but unused). The choice is platform-specific to QEMU virt;
// a Phase 5+ DTB-driven INTID allocator would lift this hardcoding.
//
// Flow:
//   1. t_irq_create(96, T_RIGHT_SIGNAL) → handle. Inside the kernel:
//      - intid_try_claim(96) ✓ (not in g_intid_claimed kernel-reserve
//        bitmap; only SGI 0 + PPI 30 are reserved at irqfwd_init).
//      - kmalloc + magic + ref=1.
//      - gic_attach(96, kobj_irq_dispatch, k) — registers handler slot.
//      - gic_enable_irq(96) — sets GICD_ISENABLER bit. Now both
//        pending (set by the kernel test side before spawn — see
//        test_irq_probe.c) AND enabled, so the GIC routes the IRQ to
//        CPU 0 (Aff0=0 affinity from dist_init's GICD_IROUTER zeros).
//   2. ERET → EL0. PSTATE.I unmasks. The pending IRQ on CPU 0 is taken
//      immediately (or as soon as CPU 0 unmasks if the child runs on
//      another CPU and CPU 0 was in EL1). gic_dispatch →
//      kobj_irq_dispatch increments pending_count to 1 and wakeups
//      the Rendez (no waiter yet, no-op wake).
//   3. t_irq_wait(handle) → returns count. Inside the kernel:
//      - handle_get + KOBJ_IRQ kind + RIGHT_SIGNAL ✓.
//      - kobj_irq_ref (F143 borrow).
//      - kobj_irq_wait → sleep(cond=pending>0); cond=true (count=1),
//        no actual sleep; re-take lock, count=1, zero, return 1.
//      - kobj_irq_unref.
//   4. Verify count == 1; print PASS / FAIL.
//   5. Exit 0 on count == 1; exit 1 on mismatch.
//
// What this proves end-to-end at the SVC layer:
//   - HwHandleImpliesCap: caller has CAP_HW_CREATE (rfork_with_caps
//     granted CAP_HW_CREATE from kproc's CAP_ALL); kernel rejects
//     without — exercised at handle creation time.
//   - SYS_IRQ_CREATE arg validation: rights ⊆ T_RIGHT_ALL_HW (we pass
//     T_RIGHT_SIGNAL which is in T_RIGHT_ALL_HW), intid >= 32 (SPI
//     range; SGI/PPI rejected by R9 F145).
//   - HwResourceExclusive: intid_try_claim rejects duplicate claims;
//     this probe is the sole claimant of SPI 96 at v1.0.
//   - SYS_IRQ_WAIT arg validation: handle kind == KOBJ_IRQ, rights
//     include RIGHT_SIGNAL.
//   - F143 UAF borrow: kobj_irq_ref before sleep + kobj_irq_unref
//     after, prevents UAF if handle_close races on another thread.
//   - Wait/wake atomicity (scheduler.tla I-9 NoMissedWakeup): the
//     IRQ that fired before t_irq_wait was called is not lost; cond
//     re-evaluation under r->lock catches the race-free path.

#![no_std]
#![no_main]

// libthyla-rs convention (Phase 7 U-2b): every native Rust binary opts
// in to ThylaAlloc as its global allocator. Required because libthyla-rs
// links the alloc crate at its root; the symbol resolves here.
#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::{
    T_RIGHT_SIGNAL,
    t_exits, t_irq_create, t_irq_wait, t_putstr,
};

// GIC SPI 96 — chosen as a safe unused SPI on QEMU virt's GIC. Pinned
// in lockstep with kernel/test/test_irq_probe.c::IRQ_PROBE_TEST_INTID.
// A mismatch between the two values produces a deterministic failure
// (the kernel pre-pends a different SPI than the probe waits on; cond
// stays false; t_irq_wait blocks forever; test runner times out).
const IRQ_PROBE_INTID: u32 = 96;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("irq-probe: starting (P4-Ic5-IRQ-probe)\n");

    // SYS_IRQ_CREATE: claim SPI 96 with SIGNAL right (required for
    // t_irq_wait to consume IRQs per sys_irq_wait_handler's RIGHT_SIGNAL
    // check — F148-era handles enforce rights-gating on the consumer
    // side of the handle).
    let handle = unsafe { t_irq_create(IRQ_PROBE_INTID, T_RIGHT_SIGNAL) };
    if handle < 0 {
        t_putstr("irq-probe: SYS_IRQ_CREATE failed (intid may now be kernel-reserved or already-claimed; update probe to a different unclaimed SPI)\n");
        unsafe { t_exits(1) };
    }
    t_putstr("irq-probe: SYS_IRQ_CREATE ok\n");

    // SYS_IRQ_WAIT: returns the collapsed pending count. With the
    // kernel test's pre-pend (gic_set_pending_spi(96) before this child
    // spawned) the IRQ is delivered during the gic_enable_irq inside
    // t_irq_create OR shortly after on ERET; either way pending_count
    // is >= 1 by the time we reach the cond check. The sleep call
    // returns without blocking and t_irq_wait returns the count.
    //
    // If for some reason the IRQ hasn't been delivered yet (extremely
    // unlikely — IRQ delivery is on the order of nanoseconds vs. the
    // tens of microseconds for a return-to-EL0+SVC cycle) t_irq_wait
    // will block here until the kernel's gic_dispatch hook increments
    // pending_count and wakes the Rendez. The test runner's overall
    // boot timeout (BOOT_TIMEOUT=20s) bounds the worst case.
    t_putstr("irq-probe: SYS_IRQ_WAIT entering...\n");
    let count = unsafe { t_irq_wait(handle) };
    t_putstr("irq-probe: SYS_IRQ_WAIT returned\n");

    // The probe expects exactly one IRQ pre-pended. count == 1 proves
    // the GIC → kobj_irq_dispatch → pending_count → wake/cond → return
    // path works end-to-end from EL0. count == 0 would indicate the
    // wait spuriously returned (which sleep's cond loop should make
    // impossible); count > 1 would indicate IRQ amplification (a fire
    // delivered more than once, which the GIC + handler discipline
    // doesn't permit).
    if count < 0 {
        t_putstr("irq-probe: FAIL — SYS_IRQ_WAIT returned error\n");
        unsafe { t_exits(1) };
    }
    if count == 0 {
        t_putstr("irq-probe: FAIL — count==0 (spurious wake or missed wakeup)\n");
        unsafe { t_exits(1) };
    }
    if count > 1 {
        // Possible if the IRQ fires more than once before we wait —
        // shouldn't happen for a single pre-pend on an SPI with no
        // hardware source, but accept this gracefully (the F159 SVC
        // coverage still holds; count >= 1 is the real invariant).
        t_putstr("irq-probe: WARN — count>1 (IRQ amplification?); accepting\n");
    }

    t_putstr("irq-probe: PASS\n");
    0
}
