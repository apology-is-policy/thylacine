// /irq-bench — IRQ-to-userspace latency benchmark (P4-Ic-latency).
//
// Closes the Phase-4 §6.2 ROADMAP exit criterion: "IRQ-to-userspace
// handler latency p99 < 5 µs (VISION §4.5 budget). Measured via
// dedicated benchmark."
//
// Methodology:
//   1. Kernel test allocates an 8 KiB KObj_DMA, wraps it in a Burrow,
//      maps it into this child's address space at SHARED_USER_VA
//      BEFORE userland_enter. The kernel side accesses the same page
//      via pa_to_kva (kernel direct map). PIPT data caches keep the
//      two aliases coherent (same PA → same cache line).
//   2. Kernel test pre-fills num_iter into the shared region, then
//      pre-pends SPI 96 (race-free pattern from irq-probe), then
//      spawns this child.
//   3. This child calls t_irq_create(96, T_RIGHT_SIGNAL) — the
//      gic_enable_irq inside causes the pending IRQ to deliver
//      immediately on CPU 0.
//   4. Loop: t_irq_wait → read CNTPCT_EL0 → store user_ts[i] →
//      bump completed. Kernel side polls `completed` to drive the
//      next iteration's gic_set_pending_spi.
//   5. After all iterations, exit cleanly.
//   6. Kernel test reads user_ts[0..N] via the kernel direct map
//      AFTER wait_pid (so child's stores have drained), computes
//      deltas against kernel_t_arm[i] (captured around each
//      gic_set_pending_spi call), sorts, reports p50/p99/max.
//
// CNTPCT_EL0 access from EL0 is gated by CNTKCTL_EL1.EL0PCTEN; the
// kernel sets this bit on every CPU at bringup (boot_main +
// per_cpu_main → timer_enable_el0_counter_access). Without the bit,
// this binary would take a Sync EC=0x18 (Trapped MSR/MRS).
//
// Both CPUs read the SAME architectural counter — CNTPCT_EL0 is a
// system-global register on AArch64 (ARM ARM D11.1), so kernel-side
// timer_get_counter and userspace-side cntpct_el0 reads are directly
// comparable regardless of CPU affinity.
//
// The kernel side does NOT read user_ts[i] iteration-by-iteration —
// only `completed` is polled. The user_ts array is read in bulk after
// wait_pid, which provides cross-CPU memory ordering (exit semantics
// + scheduler's DSB SY drain ensure all child stores are observable).
// This avoids the need for explicit DMB-release on userspace side.

#![no_std]
#![no_main]

// libthyla-rs convention (Phase 7 U-2b): every native Rust binary opts
// in to ThylaAlloc as its global allocator. Required because libthyla-rs
// links the alloc crate at its root; the symbol resolves here.
#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::handle::Rights;
use libthyla_rs::hardware::Irq;
use libthyla_rs::{t_exits, t_putstr};

// SPI 96 — same as /irq-probe. Pinned in lockstep with kernel test's
// IRQ_BENCH_TEST_INTID. Safe unused SPI on QEMU virt's GIC (not in
// PL011/PCI/GPIO/virtio-mmio ranges).
const IRQ_BENCH_INTID: u32 = 96;

// Shared-region user-VA. Mapped by kernel test before userland_enter.
// Pinned in lockstep with kernel test's SHARED_USER_VA.
const SHARED_USER_VA: u64 = 0x0080_0000;

// Shared-block field offsets (all u64, naturally aligned).
const OFF_NUM_ITER:  u64 = 0;
const OFF_READY:     u64 = 8;
const OFF_COMPLETED: u64 = 16;
const OFF_USER_TS:   u64 = 32;

// Maximum iterations the shared block can hold. (8192 - 32) / 8 = 1020.
const N_MAX: u64 = 1020;

#[inline(always)]
fn read_cntpct() -> u64 {
    let v: u64;
    unsafe {
        core::arch::asm!(
            "isb",
            "mrs {0}, cntpct_el0",
            out(reg) v,
            options(nomem, nostack, preserves_flags),
        );
    }
    v
}

#[inline(always)]
unsafe fn read_u64(va: u64) -> u64 {
    core::ptr::read_volatile(va as *const u64)
}

#[inline(always)]
unsafe fn write_u64(va: u64, val: u64) {
    core::ptr::write_volatile(va as *mut u64, val);
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("irq-bench: starting (P4-Ic-latency)\n");

    let num_iter = unsafe { read_u64(SHARED_USER_VA + OFF_NUM_ITER) };
    if num_iter == 0 || num_iter > N_MAX {
        t_putstr("irq-bench: FAIL — invalid num_iter (kernel didn't populate shared block?)\n");
        unsafe { t_exits(1) };
    }

    // Claim SPI 96. The kernel pre-pended one IRQ before rfork, so
    // gic_enable_irq inside Irq::new delivers immediately. U-2h-hardware:
    // typed Irq wraps SYS_IRQ_CREATE.
    let irq = match Irq::new(IRQ_BENCH_INTID, Rights::SIGNAL) {
        Ok(i) => i,
        Err(_) => {
            t_putstr("irq-bench: FAIL — Irq::new failed\n");
            unsafe { t_exits(1) };
        }
    };

    // Signal ready (informational; kernel test polls `completed`).
    unsafe { write_u64(SHARED_USER_VA + OFF_READY, 1); }

    // Drain `num_iter` IRQs. Iteration 0 consumes the pre-pend;
    // iterations 1..num_iter consume per-iteration kernel triggers.
    for i in 0..num_iter {
        match irq.wait() {
            Ok(c) if c > 0 => {}
            _ => {
                t_putstr("irq-bench: FAIL — irq.wait() returned error or zero count\n");
                unsafe { t_exits(1) };
            }
        }

        let ts = read_cntpct();
        let ts_va = SHARED_USER_VA + OFF_USER_TS + i * 8;
        unsafe { write_u64(ts_va, ts); }

        // Bump completed AFTER storing ts. The kernel side does NOT
        // read user_ts[i] until after wait_pid (post-exit), so no
        // userspace-side release fence is needed for visibility —
        // the exit path's DSB SY drains all stores.
        unsafe { write_u64(SHARED_USER_VA + OFF_COMPLETED, i + 1); }
    }

    t_putstr("irq-bench: PASS\n");
    0
}
