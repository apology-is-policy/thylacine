// ARM Generic Interrupt Controller — v2 / v3 autodetect (P1-G).
//
// Per ARCHITECTURE.md §12.3. The kernel implements:
//   - DTB-driven version detection ("arm,gic-v3" / "arm,cortex-a15-gic" /
//     "arm,gic-400") per invariant I-15.
//   - GICv3 distributor + redistributor + system-register CPU interface.
//   - GICv2 distributor + GICC MMIO CPU interface + GICD_SGIR IPI (Lazarus
//     W2). The v2 MMIO CPU interface is the HVF-on-Apple enabler (sidesteps
//     the GICv3-distributor `isv` assertion) and the Pi 4 / GIC-400 target.
//     QEMU virt's gic-version=3 stays the default + CI baseline; gic_init
//     autodetects from the DTB compatible.
//   - IRQ enable / disable / acknowledge / EOI.
//   - Per-INTID handler dispatch table (16 KiB BSS at INTID_MAX = 1020).
//
// GICv3 INTID encoding (ARM IHI 0069 §2.2.1):
//   0..15        SGI (software-generated, per-CPU; cross-CPU IPI)
//   16..31       PPI (private peripheral, per-CPU; virtual timer at INTID 27)
//   32..1019     SPI (shared peripheral; UART, virtio devices, ...)
//   1020..1023   special (1023 = spurious; do not EOI)
//
// At P1-G: only the timer PPI (INTID 27, virtual timer) and the UART SPI (PL011 IRQ; in
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
    GIC_VERSION_V2   = 2,    // arm,cortex-a15-gic / arm,gic-400 (live, Lazarus W2)
    GIC_VERSION_V3   = 3,    // arm,gic-v3 (live since P1-G)
} gic_version_t;

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

// One-time GIC bring-up. Performs DTB-driven version autodetect, maps
// the GIC MMIO regions (Device-nGnRnE) into the kernel vmalloc range
// via mmu_map_mmio (P3-Bca), initializes the distributor + the calling
// CPU's redistributor + CPU interface, and leaves all IRQs disabled at
// the source. Caller follows up with gic_attach + gic_enable_irq for
// each desired IRQ, then unmasks IRQs at the PSTATE level.
//
// Must run AFTER dtb_init and AFTER exception_init (so an unexpected
// IRQ during bring-up routes through the vector table cleanly).
//
// On v2 or v3 detection: configures and returns true (extincts only on a
// malformed DTB GIC node or vmalloc exhaustion mapping the MMIO).
// On no GIC compat in DTB: extinctions ("no GIC compat in DTB").
//
// Idempotent only in the trivial "do not call twice" sense.
bool gic_init(void);

// Reports the detected version. GIC_VERSION_NONE before gic_init runs.
gic_version_t gic_version(void);

// Reports the live distributor / redistributor MMIO bases. P3-Bca:
// gic_dist_base / gic_redist_base return KERNEL VAs in the vmalloc
// range (0xFFFF_8000_*) — the addresses the MMIO accessors index off.
// gic_dist_pa / gic_redist_pa return the PHYSICAL addresses discovered
// from DTB (informational; banner / debug). All four return zero before
// gic_init runs.
u64 gic_dist_base(void);
u64 gic_redist_base(void);
u64 gic_dist_pa(void);
u64 gic_redist_pa(void);

// GICv2 CPU-interface (GICC) KVA + PA. Both zero on v3 (the CPU interface is
// the ICC_* system registers, not MMIO) and before gic_init runs.
u64 gic_cpu_iface_base(void);
u64 gic_cpu_iface_pa(void);

// R12-gic-edge audit close (F205): runtime-discovered maximum
// dispatchable INTID, from GICD_TYPER.ITLinesNumber at gic_init.
// Returns 0 before gic_init runs. Callers (intid_try_claim) bound
// INTID claims against this rather than GIC_NUM_INTIDS so out-of-range
// intids are rejected before reaching ICFGR / ISENABLER writes.
u32 gic_max_intid(void);

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

// Acknowledge the highest-priority pending INTID. On v3 reads ICC_IAR1_EL1
// (returns the INTID portion, lower 24 bits); on v2 reads GICC_IAR (returns
// the lower 10-bit INTID and stashes the raw IAR per-CPU so gic_eoi can echo
// the SGI source CPUID). The caller dispatches and then issues
// gic_eoi(intid). If no IRQ is pending or the IRQ is spurious, returns an
// INTID >= GIC_NUM_INTIDS — caller MUST NOT EOI in that case.
//
// MUST be paired with exactly one gic_eoi (or no EOI on spurious) on the same
// CPU before the next gic_acknowledge: the v2 per-CPU EOI-token slot holds a
// single in-flight IAR (the kernel never nests IRQ handlers — they run with
// PSTATE.I masked).
u32 gic_acknowledge(void);

// Signal end-of-interrupt for the given INTID. On v3 writes ICC_EOIR1_EL1;
// on v2 writes GICC_EOIR with the saved raw IAR (echoing the SGI source
// CPUID, as ARM IHI 0048B §4.4.5 requires). With EOImode = 0 (the v1.0
// default on both versions) this drops the running priority + deactivates.
void gic_eoi(u32 intid);

// Internal: dispatch the IRQ to the registered handler. Called from
// exception_irq_curr_el. Provided as a public symbol so the exception
// path doesn't need to crack the GIC header's internal guts; the
// alternative would be to move dispatch into the IRQ handler itself.
void gic_dispatch(u32 intid);

// P2-Cdc: per-CPU GIC bring-up for secondaries. On v3: wakes this CPU's
// redistributor (clear ProcessorSleep, wait ChildrenAsleep), configures
// the SGI/PPI bank (group 1 NS, default priority, all disabled), and
// brings up the system-register CPU interface (ICC_SRE/PMR/BPR/CTLR/
// IGRPEN1). On v2: configures this CPU's banked SGI/PPI region of the
// distributor + brings up its GICC MMIO interface (no redistributor).
// Either way it MUST be called from the secondary CPU itself — the v3
// CPU interface is system-register-only-from-this-EL, and the v2 SGI/PPI
// + GICC state is banked to the accessing CPU.
//
// Preconditions: gic_init() has run on the boot CPU (distributor +
// CPU 0's redistributor + CPU 0's interface are live). cpu_idx is in
// [1, DTB_MAX_CPUS).
//
// Returns true on success; extincts on redistributor wake timeout.
bool gic_init_secondary(unsigned cpu_idx);

// P4-Ic5-IRQ-probe: manually pend an SPI at the GIC distributor by
// writing GICD_ISPENDR<n>.bit. Used by test code to simulate an IRQ
// fire without needing actual hardware to assert. The pending bit is
// orthogonal to the enable bit per ARM IHI 0069 §12.9.6: pending stays
// set across enable/disable cycles, and an enable transition with the
// pending bit set delivers the IRQ immediately (modulo PSTATE.I).
//
// SPI-only (intid >= 32). SGI/PPI use the redistributor's GICR_ISPENDR0
// (banked per-CPU) and aren't needed at v1.0 — the SGI 1 test path in
// irqfwd_test fires via gic_send_ipi (the architectural SGI mechanism)
// rather than via pre-pending.
//
// Returns false if intid is out of SPI range or GIC isn't initialized.
bool gic_set_pending_spi(u32 intid);

// P4-Ic5b2: configure the specified SPI to be edge-triggered (rising
// edge). gic_init defaults all SPIs to level-triggered (ICFGR = 0); a
// device that genuinely uses edge — QEMU virt's virtio-mmio bus is the
// canonical case — calls this from its IRQ-claim path before
// gic_enable_irq so the GIC's edge-detector latches the first
// assertion. Without this transition, level-triggered probing of a
// device-edge IRQ can either fire infinitely or fail to fire at all
// depending on the exact GIC implementation.
//
// SPI-only (intid >= 32). SGIs are RAZ/WI in the ICFGR per IHI 0069
// §12.9.7 (always edge anyway); PPIs live in the redistributor's
// GICR_ICFGR1 and are out of scope at v1.0.
//
// R12-gic-edge audit close (F200 + F201): function returns void and
// extincts on its two failure preconditions (intid outside
// [GIC_SPI_MIN, g_max_intid], OR GIC not initialized). Both indicate
// a kernel-internal-bug if reached — callers MUST honor them via
// kobj_irq_create's `intid >= 32` guard + intid_try_claim's
// g_max_intid bound + boot ordering. The body also issues a `dsb sy`
// after the ICFGR write so the distributor's internal latching is
// observable to a subsequent GICD_ISENABLER<n> write.
void gic_set_spi_edge_triggered(u32 intid);

// P2-Cdc: send a Software Generated Interrupt (SGI) to a target CPU.
// SGIs are GIC INTIDs 0..15 — used as cross-CPU IPI vectors. Target
// receives an IRQ at the given INTID, dispatches it through the same
// gic_dispatch path as PPIs/SPIs (i.e., to whatever handler is
// gic_attach'd).
//
// On QEMU virt (and similar flat-Aff0 boards) all CPUs share Aff{1,2,3}
// = 0 and have Aff0 = 0..N-1. On v3 this is ICC_SGI1R_EL1 with TargetList
// = 1 << target_cpu_idx (bitmap within the cluster). On v2 it is a write
// to GICD_SGIR with TargetListFilter=0 + CPUTargetList = 1 << target
// (GICv2 caps at 8 CPUs, so the target must be 0..7).
//
// sgi_intid must be in [0, GIC_SGI_MAX]. target_cpu_idx must be a valid
// CPU (caller's responsibility — not validated against
// smp_cpu_online_count). Returns false on invalid args.
bool gic_send_ipi(unsigned target_cpu_idx, u32 sgi_intid);

#endif // THYLACINE_ARCH_ARM64_GIC_H
