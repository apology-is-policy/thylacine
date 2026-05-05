// ARM Generic Interrupt Controller — v2 / v3 autodetect (P1-G).
//
// Per ARCHITECTURE.md §12.3. The kernel implements:
//   - DTB-driven version detection ("arm,gic-v3" / "arm,cortex-a15-gic" /
//     "arm,gic-400") per invariant I-15.
//   - GICv3 distributor + redistributor + system-register CPU interface.
//   - GICv2 detection that extinctions cleanly with a deferred-to-future-
//     chunk diagnostic. The v2 path is real on Pi 4 and other older
//     boards but neither QEMU virt (with run-vm.sh's gic-version=3) nor
//     Pi 5 exercise it; we hold off on implementing it until there's a
//     test target. v1.0's autodetect commitment is preserved by failing
//     loudly rather than silently.
//   - IRQ enable / disable / acknowledge / EOI.
//   - Per-INTID handler dispatch table (16 KiB BSS at INTID_MAX = 1020).
//
// GICv3 INTID encoding (ARM IHI 0069 §2.2.1):
//   0..15        SGI (software-generated, per-CPU; cross-CPU IPI)
//   16..31       PPI (private peripheral, per-CPU; timer at INTID 30)
//   32..1019     SPI (shared peripheral; UART, virtio devices, ...)
//   1020..1023   special (1023 = spurious; do not EOI)
//
// At P1-G: only the timer PPI (INTID 30) and the UART SPI (PL011 IRQ; in
// the future) are wired. Phase 2 adds SGIs for SMP IPI; Phase 3 adds
// SPIs for userspace virtio drivers.

#ifndef THYLACINE_ARCH_ARM64_GIC_H
#define THYLACINE_ARCH_ARM64_GIC_H

#include <thylacine/types.h>

// ---------------------------------------------------------------------------
// INTID conventions (architectural, not platform-specific).
// ---------------------------------------------------------------------------

#define GIC_NUM_INTIDS         1020   // 0..1019 are dispatchable
#define GIC_INTID_SPURIOUS     1023

#define GIC_SGI_MIN            0
#define GIC_SGI_MAX            15
#define GIC_PPI_MIN            16
#define GIC_PPI_MAX            31
#define GIC_SPI_MIN            32

// Convert PPI number (architectural, 0..15) to INTID (16..31).
#define GIC_PPI_TO_INTID(ppi)  ((ppi) + 16)

// Pin the macro contract: PPI 0 maps to INTID 16 (start of the PPI
// range) per ARM IHI 0069 §2.2.1. A regression that off-by-twos the
// offset would silently misroute every PPI; this assert catches it
// at build time.
_Static_assert(GIC_PPI_TO_INTID(0) == 16,
               "GIC_PPI_TO_INTID must offset by 16 (PPI bank starts at INTID 16)");
_Static_assert(GIC_PPI_TO_INTID(15) == 31,
               "GIC_PPI_TO_INTID(15) must equal 31 (PPI bank ends at INTID 31)");

// ---------------------------------------------------------------------------
// GIC versions.
// ---------------------------------------------------------------------------

typedef enum {
    GIC_VERSION_NONE = 0,    // pre-init / detection failed
    GIC_VERSION_V2   = 2,    // arm,cortex-a15-gic / arm,gic-400 (deferred)
    GIC_VERSION_V3   = 3,    // arm,gic-v3 (live at P1-G)
} gic_version_t;

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

// One-time GIC bring-up. Performs DTB-driven version autodetect, maps
// the GIC MMIO regions (Device-nGnRnE) via mmu_map_device, initializes
// the distributor + the calling CPU's redistributor + CPU interface,
// and leaves all IRQs disabled at the source. Caller follows up with
// gic_attach + gic_enable_irq for each desired IRQ, then unmasks
// IRQs at the PSTATE level.
//
// Must run AFTER dtb_init and AFTER exception_init (so an unexpected
// IRQ during bring-up routes through the vector table cleanly).
//
// On v3 detection: configures and returns true.
// On v2 detection: extinctions (deferred path).
// On no GIC compat in DTB: extinctions ("no GIC compat in DTB").
//
// Idempotent only in the trivial "do not call twice" sense.
bool gic_init(void);

// Reports the detected version. GIC_VERSION_NONE before gic_init runs.
gic_version_t gic_version(void);

// Reports the live distributor / redistributor / CPU interface bases
// discovered from DTB (informational; banner / debug). Zero before
// gic_init runs. GICv3: cpu_interface_base = 0 (system registers
// instead of MMIO); GICv2 (deferred): cpu_interface_base = real CPU
// interface MMIO base.
u64 gic_dist_base(void);
u64 gic_redist_base(void);

// Attach a handler for an INTID. The handler runs in IRQ context with
// IRQs masked at PSTATE, on the existing SP_EL1 (boot stack at P1-G;
// per-CPU exception stack at Phase 2). Replacing an existing handler
// is supported (returns the previous handler in *prev if non-NULL).
// `arg` is passed verbatim to the handler.
//
// Returns false if intid is out of range (>= GIC_NUM_INTIDS).
typedef void (*gic_irq_handler_t)(u32 intid, void *arg);
bool gic_attach(u32 intid, gic_irq_handler_t handler, void *arg);

// Enable / disable an INTID at the source. For SGI/PPI (intid < 32),
// this hits the redistributor's GICR_ISENABLER0 / GICR_ICENABLER0
// (the calling CPU's banked register). For SPI (intid >= 32), this
// hits the distributor's GICD_ISENABLER<n> / GICD_ICENABLER<n>.
//
// Setting a priority other than the default (0xa0) is post-v1.0;
// v1.0 leaves all IRQs at the same priority.
bool gic_enable_irq(u32 intid);
bool gic_disable_irq(u32 intid);

// Acknowledge the highest-priority pending INTID. Reads ICC_IAR1_EL1.
// Returns the INTID portion (lower 24 bits); the caller dispatches and
// then issues gic_eoi(intid). If no IRQ is pending or the IRQ is
// spurious, returns GIC_INTID_SPURIOUS — caller MUST NOT EOI in that
// case (per GICv3 SR conventions).
u32 gic_acknowledge(void);

// Signal end-of-interrupt for the given INTID via ICC_EOIR1_EL1. With
// ICC_CTLR_EL1.EOImode = 0 (default at P1-G), this both drops the
// running priority and deactivates the interrupt.
void gic_eoi(u32 intid);

// Internal: dispatch the IRQ to the registered handler. Called from
// exception_irq_curr_el. Provided as a public symbol so the exception
// path doesn't need to crack the GIC header's internal guts; the
// alternative would be to move dispatch into the IRQ handler itself.
void gic_dispatch(u32 intid);

// P2-Cdc: per-CPU GIC bring-up for secondaries. Wakes this CPU's
// redistributor (clear ProcessorSleep, wait ChildrenAsleep), configures
// the SGI/PPI bank (group 1 NS, default priority, all disabled), and
// brings up the system-register CPU interface (ICC_SRE/PMR/BPR/CTLR/
// IGRPEN1). Must be called from the secondary CPU itself — the
// redistributor MMIO is per-CPU-banked and the CPU interface is
// system-register-only-from-this-EL.
//
// Preconditions: gic_init() has run on the boot CPU (distributor +
// CPU 0's redistributor + CPU 0's interface are live). cpu_idx is in
// [1, DTB_MAX_CPUS).
//
// Returns true on success; extincts on redistributor wake timeout.
bool gic_init_secondary(unsigned cpu_idx);

// P2-Cdc: send a Software Generated Interrupt (SGI) to a target CPU.
// SGIs are GIC INTIDs 0..15 — used as cross-CPU IPI vectors. Target
// receives an IRQ at the given INTID, dispatches it through the same
// gic_dispatch path as PPIs/SPIs (i.e., to whatever handler is
// gic_attach'd).
//
// On QEMU virt (and similar flat-Aff0 boards) all CPUs share Aff{1,2,3}
// = 0 and have Aff0 = 0..N-1. ICC_SGI1R_EL1 is encoded with TargetList =
// 1 << target_cpu_idx (bitmap within the cluster).
//
// sgi_intid must be in [0, GIC_SGI_MAX]. target_cpu_idx must be a valid
// CPU (caller's responsibility — not validated against
// smp_cpu_online_count). Returns false on invalid args.
bool gic_send_ipi(unsigned target_cpu_idx, u32 sgi_intid);

#endif // THYLACINE_ARCH_ARM64_GIC_H
