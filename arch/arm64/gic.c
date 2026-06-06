// ARM Generic Interrupt Controller — v2 + v3 driver.
//
// Per ARCHITECTURE.md §12.3 and invariant I-15 (DTB-driven hardware
// discovery).
//
// GICv3 reference: ARM IHI 0069 (current rev H.b). Init steps (single-CPU
// at P1-G; SMP secondary bring-up extends per-CPU steps at Phase 2):
//
//   Distributor (one-time, CPU 0):
//     1. Disable: GICD_CTLR = 0
//     2. For SPIs (32..N): set group 1 NS in IGROUPRn; set priority
//        0xa0 in IPRIORITYRn; set level/edge config 0 in ICFGRn
//        (level by default; UART / etc. are level-triggered).
//     3. Enable distributor with affinity routing: GICD_CTLR =
//        ARE_NS | EnableGrp1NS.
//
//   Redistributor (per-CPU, runs on the local CPU):
//     1. Wake: clear GICR_WAKER.ProcessorSleep, wait for ChildrenAsleep
//        to clear.
//     2. Configure SGI/PPI bank: IGROUPR0 = all-ones (group 1 NS);
//        IPRIORITYR<n> = 0xa0 for each PPI we care about; default
//        all SGIs/PPIs disabled in ICENABLER0.
//
//   CPU interface (per-CPU, system registers):
//     1. ICC_SRE_EL1 |= 1 (system register interface); ISB.
//     2. ICC_PMR_EL1 = 0xff (lowest priority mask; admit all priorities).
//     3. ICC_BPR1_EL1 = 0 (binary point register; full priority
//        comparison).
//     4. ICC_CTLR_EL1 = 0 (EOImode = 0 — EOIR drops priority + deactivates).
//     5. ICC_IGRPEN1_EL1 = 1 (enable group 1 interrupts).
//
// After init, gic_enable_irq(intid) flips the bit in ISENABLER<n> at the
// source. Once IRQs are unmasked at PSTATE (msr daifclr, #2), the IRQ
// vector path picks them up.
//
// GICv2 (Lazarus W2): the v2 CPU interface is a banked MMIO region (GICC)
// rather than the v3 ICC_* system registers, SGI/PPI config lives in the
// distributor's per-CPU-banked low region (no redistributor), and IPIs go
// through GICD_SGIR rather than ICC_SGI1R_EL1. The MMIO CPU interface is
// precisely what sidesteps the HVF-on-Apple GICv3-distributor MMIO `isv`
// assertion (PORTABILITY.md §2) -- so v2 is the convergent enabler for the
// fast HVF dev loop and for the Pi 4 / GIC-400 bare-metal target. v3 stays
// QEMU virt's gic-version=3 default + the CI baseline; gic_init autodetects
// from the DTB compatible (I-15).
//
// GICv2 reference: ARM IHI 0048B. The two SGI-EOI subtleties the driver
// must honor: (1) GICC_IAR returns the source CPUID in bits [12:10] for an
// SGI and GICC_EOIR must be written back with that same CPUID field, so the
// raw IAR is preserved per-CPU between acknowledge and EOI; (2) SGI/PPI
// enable + priority + the timer PPI config live in the distributor's banked
// low region, which must be programmed from each CPU for its own bank.

#include "gic.h"

#include "exception.h"
#include "mmu.h"
#include "uart.h"

#include <stdint.h>
#include <thylacine/dtb.h>
#include <thylacine/extinction.h>
#include <thylacine/smp.h>     // smp_cpu_count / smp_cpu_idx_self for per-CPU bring-up + IPI range check
#include <thylacine/types.h>

// ---------------------------------------------------------------------------
// GICv3 distributor register offsets (relative to dist base).
// ---------------------------------------------------------------------------

#define GICD_CTLR             0x0000
#define GICD_TYPER            0x0004
#define GICD_IIDR             0x0008
#define GICD_IGROUPR(n)       (0x0080 + ((n)*4))
#define GICD_ISENABLER(n)     (0x0100 + ((n)*4))
#define GICD_ICENABLER(n)     (0x0180 + ((n)*4))
#define GICD_ISPENDR(n)       (0x0200 + ((n)*4))
#define GICD_ICPENDR(n)       (0x0280 + ((n)*4))
#define GICD_ICACTIVER(n)     (0x0380 + ((n)*4))
#define GICD_IPRIORITYR(n)    (0x0400 + ((n)*4))
#define GICD_ITARGETSR(n)     (0x0800 + ((n)*4))    // GICv2 only
#define GICD_ICFGR(n)         (0x0C00 + ((n)*4))
#define GICD_SGIR             0x0F00                 // GICv2 IPI (write-only)
#define GICD_IROUTER(n)       (0x6000 + ((n)*8))    // GICv3 only (per-INTID 64-bit)

#define GICD_CTLR_ENABLE_GRP1NS    (1u << 1)
#define GICD_CTLR_ARE_NS           (1u << 4)
// GICv2 single-security-state distributor enable: bit 0 (the only group on a
// GIC with no security extensions, as QEMU virt's gic-version=2 presents).
#define GICD_CTLR_V2_ENABLE        (1u << 0)

// GICv2 GICD_SGIR fields (ARM IHI 0048B §4.3.15). TargetListFilter 0b00 ==
// "use the CPUTargetList byte"; the byte is a per-cluster CPU-interface
// bitmask indexed by Aff0 (== cpu_idx on QEMU virt / GIC-400, max 8 CPUs).
#define GICD_SGIR_TGT_LIST_SHIFT   16
#define GICD_SGIR_MAX_TARGET       7

// ---------------------------------------------------------------------------
// GICv2 CPU-interface (GICC) register offsets (relative to the GICC base,
// DTB reg[1] for a v2 GIC). ARM IHI 0048B §4.4. The interface is banked
// per accessing CPU, so every CPU uses the same GICC VA for its own bank.
// ---------------------------------------------------------------------------

#define GICC_CTLR             0x0000
#define GICC_PMR              0x0004
#define GICC_BPR              0x0008
#define GICC_IAR              0x000C
#define GICC_EOIR             0x0010
#define GICC_IIDR             0x00FC
#define GICC_DIR              0x1000      // split-EOI deactivate (EOImode=1; unused at v1.0)

#define GICC_CTLR_V2_ENABLE   (1u << 0)   // bit 0 = enable; EOImode (bit 9) = 0 (one-step EOI)
#define GICC_PMR_ADMIT_ALL    0xF0u       // mask admits every priority below 0xF0 (default 0xa0)

// GICC_IAR / GICC_EOIR field widths (ARM IHI 0048B §4.4.4). INTID is 10 bits
// (0..1023; 1023 = spurious); for an SGI, bits [12:10] carry the source
// CPUID that GICC_EOIR must echo back.
#define GICC_IAR_INTID_MASK   0x3FFu

// GICD_TYPER.ITLinesNumber field: bits [4:0]; max INTID = 32 * (N + 1) - 1.
#define GICD_TYPER_ITLINES(t)      (((t) & 0x1f) + 1)

// ---------------------------------------------------------------------------
// GICv3 redistributor register offsets (relative to redist base for THIS CPU).
//
// Each redistributor is 0x20000 bytes = RD_base (0..0xFFFF) + SGI_base
// (0x10000..0x1FFFF). For CPU 0 with `#redistributor-regions = 1`, the
// redist base is the region base. For SMP we'll iterate via
// GICR_TYPER.Last to find each CPU's frame; at P1-G we use only CPU 0.
// ---------------------------------------------------------------------------

#define GICR_CTLR              0x0000
#define GICR_TYPER             0x0008
#define GICR_WAKER             0x0014

#define GICR_WAKER_PROC_SLEEP  (1u << 1)
#define GICR_WAKER_CHILD_ASLP  (1u << 2)

// SGI/PPI frame, offset 0x10000 from redist base.
#define GICR_SGI_OFF           0x10000
#define GICR_IGROUPR0          (GICR_SGI_OFF + 0x0080)
#define GICR_ISENABLER0        (GICR_SGI_OFF + 0x0100)
#define GICR_ICENABLER0        (GICR_SGI_OFF + 0x0180)
#define GICR_IPRIORITYR(n)     (GICR_SGI_OFF + 0x0400 + ((n)*4))
#define GICR_ICFGR0            (GICR_SGI_OFF + 0x0C00)
#define GICR_ICFGR1            (GICR_SGI_OFF + 0x0C04)

// ---------------------------------------------------------------------------
// CPU interface system registers (GICv3 SR mode).
//
// Encoded per ARM ARM C5.2.x. Names map directly to MSR/MRS mnemonics
// supported by clang ≥ 14 and ld.lld.
// ---------------------------------------------------------------------------

#define DEFAULT_PRIORITY  0xa0u

// ---------------------------------------------------------------------------
// State.
// ---------------------------------------------------------------------------

static gic_version_t g_version;
// P3-Bca: g_dist_base / g_redist_base are kernel VAs in the vmalloc
// range (0xFFFF_8000_*) returned by mmu_map_mmio. MMIO accessors index
// off them as `base + register_offset`. Pre-P3-Bca these were PA-as-VA
// via TTBR0 identity; P3-Bd retires TTBR0 identity entirely so the
// vmalloc remap is the durable form.
static u64 g_dist_base;
static u64 g_redist_base;
static u64 g_dist_pa;          // PA discovered from DTB (banner / debug)
static u64 g_redist_pa;
static u32 g_max_intid;        // from GICD_TYPER

// GICv2 CPU interface (GICC). g_cpu_base is the kernel VA from mmu_map_mmio
// of DTB reg[1]; banked per accessing CPU. Both zero on v3 (sysreg CPU
// interface). g_v2_eoi_token holds, per CPU, the raw GICC_IAR last
// acknowledged -- so gic_eoi can echo the SGI source CPUID back to GICC_EOIR
// (ARM IHI 0048B §4.4.5). Indexed by smp_cpu_idx_self(); written + read with
// IRQs masked between a CPU's acknowledge and its EOI (no nesting), and each
// CPU touches only its own slot -- no cross-CPU race.
static u64 g_cpu_base;
static u64 g_cpu_pa;
static u32 g_v2_eoi_token[DTB_MAX_CPUS];

// GICv3 redistributor region stride. Each CPU's redistributor frame is
// 0x20000 bytes (RD_base 0x0..0xFFFF + SGI_base 0x10000..0x1FFFF). Per
// IHI 0069 §12.3.1 the redistributor region is contiguous; CPU N's
// frame base is at offset N * 0x20000 from the region base. On QEMU
// virt with -smp 4 the region is 4 × 0x20000 = 0x80000 bytes.
#define GICR_FRAME_STRIDE  0x20000

static inline u64 cpu_redist_base(unsigned cpu_idx) {
    return g_redist_base + (u64)cpu_idx * GICR_FRAME_STRIDE;
}

// Handler dispatch table. 1020 INTIDs × 16 bytes = 16320 bytes BSS.
struct gic_irq_slot {
    gic_irq_handler_t handler;
    void *arg;
};
static struct gic_irq_slot g_handlers[GIC_NUM_INTIDS];

// ---------------------------------------------------------------------------
// MMIO helpers.
// ---------------------------------------------------------------------------

static inline u32 mmio_r32(u64 base, u32 off) {
    return *(volatile u32 *)(uintptr_t)(base + off);
}

static inline void mmio_w32(u64 base, u32 off, u32 val) {
    *(volatile u32 *)(uintptr_t)(base + off) = val;
}

static inline void mmio_w8(u64 base, u32 off, u8 val) {
    *(volatile u8 *)(uintptr_t)(base + off) = val;
}

static inline u64 mmio_r64(u64 base, u32 off) {
    return *(volatile u64 *)(uintptr_t)(base + off);
}

static inline void mmio_w64(u64 base, u32 off, u64 val) {
    *(volatile u64 *)(uintptr_t)(base + off) = val;
}

// GICv2-only MMIO accessors that drain with `dsb sy` after every access.
//
// PORTABILITY.md §5 (HVF-MMIO mitigation): under QEMU/HVF on Apple Silicon,
// rapid back-to-back GIC distributor MMIO can trap into Hypervisor.framework
// with ESR.ISV=0 (no instruction syndrome), which QEMU cannot decode and
// asserts on. The empirical mitigation from the assessment is a barrier
// between consecutive GIC MMIO accesses; a `dsb sy` after each access drains
// it to completion before the next, pacing the trap rate. Device-nGnRnE
// memory is already strongly ordered architecturally, so the barrier is for
// the emulator's benefit, not for correctness on real silicon. Confined to
// the v2 path (the only path taken under HVF + GIC-400); v3 keeps the plain
// accessors. The cost is a drain per GIC access on the IRQ/IPI path -- a
// handful per interrupt, negligible at interrupt rates.
static inline void v2_w32(u64 base, u32 off, u32 val) {
    *(volatile u32 *)(uintptr_t)(base + off) = val;
    __asm__ __volatile__("dsb sy" ::: "memory");
}
static inline u32 v2_r32(u64 base, u32 off) {
    u32 v = *(volatile u32 *)(uintptr_t)(base + off);
    __asm__ __volatile__("dsb sy" ::: "memory");
    return v;
}
static inline void v2_w8(u64 base, u32 off, u8 val) {
    *(volatile u8 *)(uintptr_t)(base + off) = val;
    __asm__ __volatile__("dsb sy" ::: "memory");
}

// Counter accessors for bounded-poll timeouts. cntfrq_el0 is set by
// firmware at boot; cntpct_el0 ticks at that frequency. We use them
// in redist bring-up to bound the GICR_WAKER.ChildrenAsleep poll;
// they're available at EL1 once start.S's EL2 drop sets
// CNTHCTL_EL2.{EL1PCEN,EL1PCTEN}=1 (or QEMU virt's direct EL1 entry).
static inline u64 read_cntpct(void) {
    u64 v;
    __asm__ __volatile__("isb\nmrs %0, cntpct_el0\n" : "=r"(v));
    return v;
}
static inline u64 read_cntfrq(void) {
    u64 v;
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

// ---------------------------------------------------------------------------
// CPU interface system-register accessors.
// ---------------------------------------------------------------------------

static inline u64 read_icc_sre_el1(void) {
    u64 v;
    __asm__ __volatile__("mrs %0, ICC_SRE_EL1" : "=r"(v));
    return v;
}
static inline void write_icc_sre_el1(u64 v) {
    __asm__ __volatile__("msr ICC_SRE_EL1, %0\nisb\n" :: "r"(v));
}
static inline void write_icc_pmr_el1(u64 v) {
    __asm__ __volatile__("msr ICC_PMR_EL1, %0" :: "r"(v));
}
static inline void write_icc_bpr1_el1(u64 v) {
    __asm__ __volatile__("msr ICC_BPR1_EL1, %0" :: "r"(v));
}
static inline void write_icc_ctlr_el1(u64 v) {
    __asm__ __volatile__("msr ICC_CTLR_EL1, %0" :: "r"(v));
}
static inline void write_icc_igrpen1_el1(u64 v) {
    __asm__ __volatile__("msr ICC_IGRPEN1_EL1, %0\nisb\n" :: "r"(v));
}
static inline u64 read_icc_iar1_el1(void) {
    u64 v;
    // dsb sy before IAR per ARM IHI 0069 §3.7 to ensure the read is
    // ordered after any preceding gic_eoi from a re-entry chain.
    __asm__ __volatile__("dsb sy\nmrs %0, ICC_IAR1_EL1\n" : "=r"(v));
    return v;
}
static inline void write_icc_eoir1_el1(u64 v) {
    __asm__ __volatile__("msr ICC_EOIR1_EL1, %0\nisb\n" :: "r"(v));
}

// ---------------------------------------------------------------------------
// Distributor + redistributor init (GICv3).
// ---------------------------------------------------------------------------

static void dist_init(void) {
    // 1. Disable distributor.
    mmio_w32(g_dist_base, GICD_CTLR, 0);

    // 2. Discover max INTID.
    u32 typer = mmio_r32(g_dist_base, GICD_TYPER);
    u32 itlines = GICD_TYPER_ITLINES(typer);     // count in groups of 32
    u32 max = itlines * 32u - 1u;
    if (max >= GIC_NUM_INTIDS) max = GIC_NUM_INTIDS - 1;
    g_max_intid = max;

    // 3. Enable affinity routing BEFORE programming GICD_IROUTER<n>.
    //    Per ARM IHI 0069H.b §12.9.1: "If GICD_CTLR.ARE_NS == 0,
    //    GICD_IROUTER<n> is RES0. Writes are ignored." Group enables
    //    stay off until step 6 so no IRQ can route until SPI config
    //    has settled.
    mmio_w32(g_dist_base, GICD_CTLR, GICD_CTLR_ARE_NS);

    // 4. Configure SPIs (intid >= 32). For each 32-INTID group beyond
    //    the SGI/PPI bank: clear pending, disable, group=1NS,
    //    edge/level config = level (0).
    for (u32 n = 1; n * 32u <= max; n++) {
        mmio_w32(g_dist_base, GICD_ICENABLER(n), 0xFFFFFFFFu);
        mmio_w32(g_dist_base, GICD_ICPENDR(n),   0xFFFFFFFFu);
        mmio_w32(g_dist_base, GICD_IGROUPR(n),   0xFFFFFFFFu); // group 1 NS
        // ICFGR: 2 bits per INTID; 0 = level, 0b10 = edge. Default level.
        mmio_w32(g_dist_base, GICD_ICFGR(n * 2),     0);
        mmio_w32(g_dist_base, GICD_ICFGR(n * 2 + 1), 0);
    }
    // IPRIORITYR supports byte stores per IHI 0069 §12.9.16; using the
    // explicit byte-store form makes the writes atomic against any
    // future SMP secondary that touches a different INTID in the same
    // 32-bit word (the RMW form would lose updates under that race).
    for (u32 n = 32; n <= max; n++) {
        mmio_w8(g_dist_base, GICD_IPRIORITYR(0) + n, DEFAULT_PRIORITY);
    }

    // 5. Affinity routing: set GICD_IROUTER<n> for SPIs to {Aff3=0,
    //    Aff2=0, Aff1=0, Aff0=0} = CPU 0. Bit 31 (IRM) = 0 means route
    //    to a single PE; bit 0..7 are Aff0. Zero is correct for a
    //    single-CPU build at P1-G. Now effective because ARE_NS=1 from
    //    step 3.
    for (u32 n = 32; n <= max; n++) {
        mmio_w64(g_dist_base, GICD_IROUTER(n), 0);
    }

    // 6. Final enable: ARE_NS still set + group 1 NS.
    mmio_w32(g_dist_base, GICD_CTLR,
             GICD_CTLR_ENABLE_GRP1NS | GICD_CTLR_ARE_NS);
}

// Pending + active state offsets within the SGI frame (offset 0x10000).
// Used at bring-up to clear any leftover SGI/PPI state from firmware /
// a previous OS (kexec scenario).
#define GICR_ICPENDR0      (GICR_SGI_OFF + 0x0280)
#define GICR_ICACTIVER0    (GICR_SGI_OFF + 0x0380)

static void redist_init_cpu(unsigned cpu_idx) {
    u64 base = cpu_redist_base(cpu_idx);

    // Wake the redistributor: clear ProcessorSleep, wait for
    // ChildrenAsleep to clear (hardware acknowledges the wake).
    //
    // Bound the poll with a deadline: 100 ms at the architectural
    // counter frequency. If hardware never asserts ChildrenAsleep
    // clear, extinct loudly rather than wedging silently.
    u32 waker = mmio_r32(base, GICR_WAKER);
    waker &= ~GICR_WAKER_PROC_SLEEP;
    mmio_w32(base, GICR_WAKER, waker);
    u64 freq = read_cntfrq();
    if (freq == 0) freq = 1000000;          // conservative fallback
    u64 deadline = read_cntpct() + freq / 10;
    while (mmio_r32(base, GICR_WAKER) & GICR_WAKER_CHILD_ASLP) {
        if (read_cntpct() > deadline) {
            extinction("redist_init_cpu: ChildrenAsleep never cleared (broken redistributor frame?)");
        }
    }

    // SGI/PPI bank: disable all by default; group 1 NS; priority 0xa0;
    // PPI config = level (0), SGI config is RAO/WI on most impls.
    // Also clear pending + active state in case firmware / a previous
    // OS (kexec) left INTIDs in a non-clean state — otherwise the
    // first IRQ after PSTATE.I unmask could be a stale SGI for which
    // we have no handler attached.
    mmio_w32(base, GICR_ICENABLER0,  0xFFFFFFFFu);
    mmio_w32(base, GICR_ICPENDR0,    0xFFFFFFFFu);
    mmio_w32(base, GICR_ICACTIVER0,  0xFFFFFFFFu);
    mmio_w32(base, GICR_IGROUPR0,    0xFFFFFFFFu);
    for (u32 n = 0; n < 32; n++) {
        mmio_w8(base, GICR_IPRIORITYR(0) + n, DEFAULT_PRIORITY);
    }
    // PPI config: ICFGR1 covers INTIDs 16..31 (2 bits per INTID).
    // Leave at zero (level-triggered) for the timer + UART.
    mmio_w32(base, GICR_ICFGR1, 0);
}

static void cpu_iface_init(void) {
    // Enable system register interface. ICC_SRE_EL1.SRE = 1.
    u64 sre = read_icc_sre_el1();
    sre |= 1u;
    write_icc_sre_el1(sre);

    // Lowest priority mask: admit all priorities.
    write_icc_pmr_el1(0xff);

    // Binary point register: 0 = full priority comparison.
    write_icc_bpr1_el1(0);

    // EOImode = 0 (one-step EOI: drop priority + deactivate).
    write_icc_ctlr_el1(0);

    // Enable group 1 interrupts at the CPU interface.
    write_icc_igrpen1_el1(1);
}

// ---------------------------------------------------------------------------
// Distributor + CPU-interface init (GICv2).
//
// v2 has no redistributor: SGI/PPI config lives in the distributor's banked
// low region (INTIDs 0..31 -- each CPU sees its own bank at the same offset),
// and the CPU interface is the GICC MMIO block (also banked per accessing
// CPU). dist_init_v2 runs once on the boot CPU for the global SPI config;
// gic_cpu_config_v2 + cpu_iface_init_v2 run on EACH CPU for its own bank.
// ---------------------------------------------------------------------------

static void dist_init_v2(void) {
    // 1. Disable the distributor while we program it.
    v2_w32(g_dist_base, GICD_CTLR, 0);

    // 2. Discover the max INTID (same TYPER.ITLinesNumber field as v3).
    u32 typer   = v2_r32(g_dist_base, GICD_TYPER);
    u32 itlines = GICD_TYPER_ITLINES(typer);
    u32 max     = itlines * 32u - 1u;
    if (max >= GIC_NUM_INTIDS) max = GIC_NUM_INTIDS - 1;
    g_max_intid = max;

    // 3. SPIs (intid >= 32): disable, clear pending + active, level config.
    //    The 32-INTID group n maps to ICFGR(2n) + ICFGR(2n+1) (16 INTIDs,
    //    2 bits each). No IGROUPR programming: QEMU virt's gic-version=2 has
    //    no security extensions, so there is a single group and IGROUPR is
    //    RAZ/WI (Linux's gic-v2 driver likewise leaves it untouched).
    for (u32 n = 1; n * 32u <= max; n++) {
        v2_w32(g_dist_base, GICD_ICENABLER(n), 0xFFFFFFFFu);
        v2_w32(g_dist_base, GICD_ICPENDR(n),   0xFFFFFFFFu);
        v2_w32(g_dist_base, GICD_ICACTIVER(n), 0xFFFFFFFFu);
        v2_w32(g_dist_base, GICD_ICFGR(n * 2),     0);
        v2_w32(g_dist_base, GICD_ICFGR(n * 2 + 1), 0);
    }

    // 4. SPI priority (byte stores; same atomicity rationale as v3) + route
    //    every SPI to CPU interface 0 via ITARGETSR (byte = target bitmask,
    //    0x01 = interface 0). ITARGETSR for INTIDs 0..31 is banked + RO, so
    //    only 32.. is written -- matching v3's IROUTER<n>=0 (route to CPU 0).
    for (u32 n = 32; n <= max; n++)
        v2_w8(g_dist_base, GICD_IPRIORITYR(0) + n, DEFAULT_PRIORITY);
    for (u32 n = 32; n <= max; n++)
        v2_w8(g_dist_base, GICD_ITARGETSR(0) + n, 0x01u);

    // 5. Enable the distributor (single security state -> bit 0).
    v2_w32(g_dist_base, GICD_CTLR, GICD_CTLR_V2_ENABLE);
}

// Per-CPU banked SGI/PPI config. Runs on the local CPU so the banked low
// distributor registers hit ITS bank. Mirrors v3's redist_init_cpu minus the
// redistributor wake (v2 has none).
static void gic_cpu_config_v2(void) {
    // Disable all SGI/PPI + clear stale pending/active. SGI enable bits
    // (0..15) are IMPLEMENTATION DEFINED -- often RAO/WI (SGIs always
    // enabled), in which case these writes are no-ops for the SGI half and
    // the PPI half is cleared; either way the only later-enabled SGI/PPI are
    // those passed to gic_enable_irq (IPI_RESCHED + the timer PPI).
    v2_w32(g_dist_base, GICD_ICENABLER(0), 0xFFFFFFFFu);
    v2_w32(g_dist_base, GICD_ICPENDR(0),   0xFFFFFFFFu);
    v2_w32(g_dist_base, GICD_ICACTIVER(0), 0xFFFFFFFFu);
    for (u32 n = 0; n < 32; n++)
        v2_w8(g_dist_base, GICD_IPRIORITYR(0) + n, DEFAULT_PRIORITY);
    // PPI config (ICFGR(1) covers INTIDs 16..31): level (timer is level).
    // ICFGR(0) (SGIs) is RO/always-edge -- not touched.
    v2_w32(g_dist_base, GICD_ICFGR(1), 0);
}

static void cpu_iface_init_v2(void) {
    // Priority mask first (admit all), then enable the interface last so the
    // CPU starts admitting IRQs only once the mask is set. BPR 0 = full
    // priority comparison (no sub-priority preemption grouping).
    v2_w32(g_cpu_base, GICC_PMR,  GICC_PMR_ADMIT_ALL);
    v2_w32(g_cpu_base, GICC_BPR,  0);
    v2_w32(g_cpu_base, GICC_CTLR, GICC_CTLR_V2_ENABLE);
}

// ---------------------------------------------------------------------------
// gic_init.
// ---------------------------------------------------------------------------

// GICv2 bring-up: discover GICD (reg[0]) + GICC (reg[1]) from DTB, map both
// Device-nGnRnE, and init the distributor + this (boot) CPU's banked SGI/PPI
// + its GICC interface. `compat` is the matched v2 compatible string.
static bool gic_init_v2(const char *compat) {
    u64 dbase = 0, dsize = 0, cbase = 0, csize = 0;
    if (!dtb_get_compat_reg_n(compat, 0, &dbase, &dsize)) {
        extinction("gic_init: GICv2 DTB node has no reg[0] (distributor)");
    }
    if (!dtb_get_compat_reg_n(compat, 1, &cbase, &csize)) {
        extinction("gic_init: GICv2 DTB node has no reg[1] (CPU interface)");
    }
    g_dist_pa = dbase;
    g_cpu_pa  = cbase;

    void *dist_kva = mmu_map_mmio((paddr_t)dbase, dsize);
    if (!dist_kva) {
        extinction("gic_init: mmu_map_mmio(GICv2 dist) returned NULL (vmalloc exhausted?)");
    }
    void *cpu_kva = mmu_map_mmio((paddr_t)cbase, csize);
    if (!cpu_kva) {
        extinction("gic_init: mmu_map_mmio(GICv2 cpuif) returned NULL (vmalloc exhausted?)");
    }
    g_dist_base = (u64)(uintptr_t)dist_kva;
    g_cpu_base  = (u64)(uintptr_t)cpu_kva;

    // Distributor (global SPI config) -> this CPU's banked SGI/PPI -> this
    // CPU's GICC interface (enabled last, so IRQs are admitted only after
    // both distributor + bank are armed).
    dist_init_v2();
    gic_cpu_config_v2();
    cpu_iface_init_v2();
    return true;
}

bool gic_init(void) {
    // Step 1: detect version from DTB (I-15). Remember the matched compatible
    // so the reg discovery below uses the right node.
    const char *gic_compat = NULL;
    if (dtb_has_compat("arm,gic-v3")) {
        g_version  = GIC_VERSION_V3;
        gic_compat = "arm,gic-v3";
    } else if (dtb_has_compat("arm,gic-400")) {
        g_version  = GIC_VERSION_V2;
        gic_compat = "arm,gic-400";
    } else if (dtb_has_compat("arm,cortex-a15-gic")) {
        g_version  = GIC_VERSION_V2;
        gic_compat = "arm,cortex-a15-gic";
    } else {
        g_version = GIC_VERSION_NONE;
        extinction("gic_init: no GIC compat in DTB (need arm,gic-v3 or arm,cortex-a15-gic/arm,gic-400)");
    }

    if (g_version == GIC_VERSION_V2) {
        return gic_init_v2(gic_compat);
    }

    // Step 2: discover MMIO regions from DTB (compat = "arm,gic-v3").
    //   reg[0] = distributor   (size 0x10000)
    //   reg[1] = redistributor region (size = 0x20000 × N CPUs, plus padding)
    u64 dbase = 0, dsize = 0;
    u64 rbase = 0, rsize = 0;
    if (!dtb_get_compat_reg_n("arm,gic-v3", 0, &dbase, &dsize)) {
        extinction("gic_init: DTB arm,gic-v3 has no reg[0] (distributor)");
    }
    if (!dtb_get_compat_reg_n("arm,gic-v3", 1, &rbase, &rsize)) {
        extinction("gic_init: DTB arm,gic-v3 has no reg[1] (redistributor)");
    }
    g_dist_pa   = dbase;
    g_redist_pa = rbase;

    // Step 3: map both regions Device-nGnRnE into the kernel vmalloc
    // range (P3-Bca). mmu_map_mmio returns a kernel VA in
    // 0xFFFF_8000_* that the MMIO accessors below index off. Pre-P3-Bca
    // gic_init used mmu_map_device which converted the TTBR0-identity
    // L2 entry to a Device block; P3-Bd retires TTBR0 identity entirely
    // so the vmalloc-driven path is the durable form.
    //
    // Redist region: cap the mapped size at the actual CPU count.
    // QEMU virt's DTB reports the full reservation for max_cpus (≈ 64 MiB
    // at 256 max-CPUs × 0x40000 per CPU), but only the configured CPUs'
    // frames are populated. Mapping the full reported region would burn
    // through l3_vmalloc unnecessarily. The usable redist region is
    // `dtb_cpu_count * GICR_FRAME_STRIDE`. dtb_cpu_count is set BEFORE
    // gic_init in boot_main; smp_init / smp_cpu_count haven't run yet.
    u32 cpu_count = dtb_cpu_count();
    if (cpu_count == 0) cpu_count = 1;
    u64 redist_used_size = (u64)cpu_count * GICR_FRAME_STRIDE;
    if (redist_used_size > rsize) {
        redist_used_size = rsize;     // defensive: never exceed DTB-reported
    }

    void *dist_kva = mmu_map_mmio((paddr_t)dbase, dsize);
    if (!dist_kva) {
        extinction("gic_init: mmu_map_mmio(dist) returned NULL (vmalloc exhausted?)");
    }
    void *redist_kva = mmu_map_mmio((paddr_t)rbase, redist_used_size);
    if (!redist_kva) {
        extinction("gic_init: mmu_map_mmio(redist) returned NULL (vmalloc exhausted?)");
    }
    g_dist_base   = (u64)(uintptr_t)dist_kva;
    g_redist_base = (u64)(uintptr_t)redist_kva;

    // Step 4: bring up distributor + this CPU's redistributor + CPU
    // interface. Order matters: distributor first (so SPIs are
    // configured before any SPI source can fire), then redistributor
    // (banked SGI/PPI for this CPU), then CPU interface (last so the
    // CPU starts admitting IRQs only after both sides are armed).
    dist_init();
    redist_init_cpu(0);
    cpu_iface_init();

    return true;
}

bool gic_init_secondary(unsigned cpu_idx) {
    if (cpu_idx == 0 || cpu_idx >= 64) return false;

    if (g_version == GIC_VERSION_V2) {
        // v2: no redistributor. Configure THIS CPU's banked SGI/PPI region of
        // the distributor + bring up its GICC interface. Both touch banked
        // state, so this MUST run on the secondary itself (the caller's
        // contract) -- same constraint as the v3 sysreg interface.
        gic_cpu_config_v2();
        cpu_iface_init_v2();
        return true;
    }

    if (g_version != GIC_VERSION_V3) return false;

    // Per-CPU redistributor wake + SGI/PPI bank config.
    redist_init_cpu(cpu_idx);

    // Per-CPU CPU interface bring-up (system registers — only writable
    // from THIS CPU). Same sequence as the boot CPU; the priority mask,
    // BPR, CTLR, and group-1 enable are all banked per-CPU.
    cpu_iface_init();

    return true;
}

bool gic_send_ipi(unsigned target_cpu_idx, u32 sgi_intid) {
    if (sgi_intid > GIC_SGI_MAX) return false;
    // R5-H F83: validate against the actual CPU count, not the SGI
    // TargetList width. The previous bound `target_cpu_idx >= 16` rejected
    // truly-out-of-range indices (the encoding limit) but silently
    // accepted any 0..15 — including 0..15 indices that don't map to a
    // configured CPU on the platform. The caller would think the IPI
    // was sent; hardware would drop the bit (no Aff0=N CPU exists). Now
    // we reject any index >= the count of CPUs that came online via PSCI.
    // Same observable behavior as before for all in-range callers; closes
    // the silent-drop hazard for forward-looking out-of-range loops.
    if (target_cpu_idx >= smp_cpu_count()) return false;

    if (g_version == GIC_VERSION_V2) {
        // GICD_SGIR (ARM IHI 0048B §4.3.15): TargetListFilter 0b00 (bits
        // [25:24] = 0, "use the CPUTargetList byte"), CPUTargetList [23:16] =
        // 1 << target (CPU-interface bitmap, Aff0-indexed), SGIINTID [3:0].
        // GICv2 supports at most 8 CPUs, so the 8-bit list bounds the target.
        if (target_cpu_idx > GICD_SGIR_MAX_TARGET) return false;
        u32 sgir = (1u << (GICD_SGIR_TGT_LIST_SHIFT + target_cpu_idx)) |
                   (sgi_intid & 0xFu);
        v2_w32(g_dist_base, GICD_SGIR, sgir);
        return true;
    }

    // ICC_SGI1R_EL1 encoding (ARM ARM C5.2.18):
    //   [55:48]  Aff3
    //   [47:41]  reserved
    //   [40]     IRM (0 = TargetList, 1 = broadcast to all-but-self)
    //   [39:32]  Aff2
    //   [31:28]  reserved
    //   [27:24]  INTID (0..15)
    //   [23:16]  Aff1
    //   [15:0]   TargetList — bitmap of CPUs within the (Aff1,Aff2,Aff3)
    //                          cluster, indexed by Aff0
    //
    // On QEMU virt all CPUs are Aff3=Aff2=Aff1=0; Aff0 == cpu_idx, so
    // TargetList = 1 << target_cpu_idx selects the single target.
    u64 sgi = ((u64)sgi_intid << 24) | (1ULL << target_cpu_idx);
    __asm__ __volatile__("msr ICC_SGI1R_EL1, %0\nisb\n" :: "r"(sgi) : "memory");
    return true;
}

// ---------------------------------------------------------------------------
// gic_attach / dispatch.
// ---------------------------------------------------------------------------

bool gic_attach(u32 intid, gic_irq_handler_t handler, void *arg) {
    if (intid >= GIC_NUM_INTIDS) return false;
    // Reject NULL handler: the attach API exists to install a real
    // handler; "detach" should go through gic_disable_irq, not via
    // an attach with NULL that quietly arms a future "no handler"
    // extinction. Forces callers to be explicit.
    if (!handler) return false;
    g_handlers[intid].handler = handler;
    g_handlers[intid].arg     = arg;
    return true;
}

void gic_dispatch(u32 intid) {
    if (intid >= GIC_NUM_INTIDS) {
        extinction_with_addr("gic_dispatch: INTID out of range", (uintptr_t)intid);
    }
    gic_irq_handler_t h = g_handlers[intid].handler;
    if (!h) {
        extinction_with_addr("gic_dispatch: no handler for INTID", (uintptr_t)intid);
    }
    h(intid, g_handlers[intid].arg);
}

// ---------------------------------------------------------------------------
// gic_enable_irq / gic_disable_irq.
//
// SGI/PPI (intid < 32): use redistributor's banked GICR_ISENABLER0 of
// the CALLING CPU. SGI/PPI registers are CPU-banked per ARM IHI 0069
// §5.4.1, so a write affects only the CPU whose frame is targeted.
// gic_enable_irq is the caller's promise: "enable this SGI/PPI on me."
//
// SPI (intid >= 32): use distributor's GICD_ISENABLERn (n = intid / 32).
// SPIs are global; affinity routing (GICD_IROUTERn) decides which CPU
// receives the IRQ.
// ---------------------------------------------------------------------------

bool gic_enable_irq(u32 intid) {
    if (intid >= GIC_NUM_INTIDS) return false;
    if (g_version == GIC_VERSION_V2) {
        // v2: SGI/PPI (0..31) live in the distributor's per-CPU-banked low
        // region; SPIs are global. GICD_ISENABLER(intid/32) covers both --
        // the low register (n=0) is banked to the calling CPU.
        u32 n = intid / 32, bit = intid % 32;
        v2_w32(g_dist_base, GICD_ISENABLER(n), 1u << bit);
        return true;
    }
    if (intid < 32) {
        u64 base = cpu_redist_base(smp_cpu_idx_self());
        mmio_w32(base, GICR_ISENABLER0, 1u << intid);
    } else {
        u32 n = intid / 32;
        u32 bit = intid % 32;
        mmio_w32(g_dist_base, GICD_ISENABLER(n), 1u << bit);
    }
    return true;
}

void gic_set_spi_edge_triggered(u32 intid) {
    // P4-Ic5b2: configure a specific SPI to be edge-triggered (rising
    // edge). GIC init defaults all SPIs to level-triggered (ICFGR = 0,
    // per gic_init), which is the safer default for unknown signalling
    // patterns. Devices that genuinely use edge — QEMU virt's
    // virtio-mmio bus is the canonical case (DTB declares
    // `interrupts = <SPI N IRQ_TYPE_EDGE_RISING>` per
    // hw/arm/virt.c::create_virtio_devices) — call this from their
    // IRQ-claim path to flip ICFGR to 0b10 for the specific INTID.
    //
    // Without this transition, a level-triggered virtio-mmio IRQ would
    // either fire infinitely until the driver deasserts via
    // InterruptACK (the kernel doesn't yet, at the v1.0 KObj_IRQ layer)
    // or fail to fire at all on some GIC implementations that strictly
    // require the configured edge type to match the wire reality.
    //
    // The 2-bit-per-INTID encoding: bit-pair at position (intid % 16)*2
    // in GICD_ICFGR(intid / 16). 0b10 = edge, 0b00 = level. We RMW
    // because the same 32-bit register covers 16 INTIDs and a blind
    // write would clobber neighboring SPI configurations.
    //
    // SPI-only because ICFGR for SGIs (intid 0..15) is RAZ/WI per ARM
    // IHI 0069 §12.9.7 (SGIs are always edge); PPIs (16..31) live in
    // the redistributor's GICR_ICFGR1 and are out of scope.
    //
    // R12-gic-edge audit close (F201 P2): converted from bool return
    // to void + extinction. Preconditions: (a) GIC_SPI_MIN <= intid
    // <= g_max_intid, (b) g_dist_base != 0. Both are kernel-internal-
    // bug indicators if violated — callers must enforce them via
    // kobj_irq_create's `if (intid >= 32)` guard + intid_try_claim's
    // g_max_intid bound + boot-ordering invariant (gic_init runs
    // before any kobj_irq_create path). Reaching this function with
    // either condition violated means the INTID-bounds or boot-order
    // discipline broke.
    if (intid < GIC_SPI_MIN || intid > g_max_intid)
        extinction("gic_set_spi_edge_triggered: intid out of SPI range "
                   "(precondition broken — kernel-internal bug)");
    if (g_dist_base == 0)
        extinction("gic_set_spi_edge_triggered: GIC not initialized "
                   "(precondition broken — boot-order discipline)");

    // R12-gic-edge audit close (F204 P3): pin ICFGR encoding constants
    // against drift. Per IHI 0069 §12.9.7, bit 0 of each 2-bit pair is
    // SBZ (Should Be Zero) on writes; bit 1 selects level (0) vs.
    // edge (1). 0b10 = edge. Future refactor flipping the constants
    // (e.g., to 0b11 mistakenly) would write SBZ=1 — UNPREDICTABLE on
    // strict GIC implementations.
    _Static_assert((0x2u & 0x1u) == 0,
                   "ICFGR edge encoding 0b10 must keep SBZ bit clear");
    _Static_assert((0x0u & 0x1u) == 0,
                   "ICFGR level encoding 0b00 must keep SBZ bit clear");

    u32 reg     = GICD_ICFGR(intid / 16);
    u32 bit_off = (intid % 16) * 2;
    u32 mask    = 0x3u << bit_off;
    u32 val     = 0x2u << bit_off;  // 0b10 = edge-triggered
    // The read+write to GICD_ICFGR is a back-to-back distributor access; on
    // v2 it MUST drain (v2_r32/v2_w32) like every other v2 GIC MMIO, or under
    // HVF the pair can trap with ESR.ISV=0. v3 uses the plain accessors.
    if (g_version == GIC_VERSION_V2) {
        u32 cur = v2_r32(g_dist_base, reg);
        v2_w32(g_dist_base, reg, (cur & ~mask) | val);
    } else {
        u32 cur = mmio_r32(g_dist_base, reg);
        mmio_w32(g_dist_base, reg, (cur & ~mask) | val);
    }

    // R12-gic-edge audit close (F200 P2): ensure the ICFGR write has
    // been observed by the GIC distributor's internal state BEFORE any
    // subsequent MMIO write to GICD_ISENABLER<n> in gic_enable_irq.
    // ARM IHI 0069 §3.4.2 + Linux's GICv3 driver pattern wrap
    // distributor RMWs in dsb + GICD_CTLR.RWP polling — without a
    // barrier here, a strict GIC implementation could process the
    // subsequent ENABLE write before the edge configuration has
    // latched, causing the first IRQ to fire as level-triggered
    // (mis-asserting until device deassert — which the v1.0 KObj_IRQ
    // layer never does, so the IRQ storm is unrecoverable).
    //
    // QEMU virt's GIC model latches in zero time; this barrier is
    // forward-looking for real-hardware Phase 5+ targets (Pi 5, etc.).
    // The full Linux pattern adds GICD_CTLR.RWP polling — deferred to
    // Phase 5+ real-hardware bring-up.
    __asm__ __volatile__("dsb sy" ::: "memory");
}

bool gic_set_pending_spi(u32 intid) {
    // P4-Ic5-IRQ-probe: write GICD_ISPENDR<n>.bit to mark this SPI as
    // pending. SPI-only because the distributor frame's GICD_ISPENDR
    // covers INTIDs 32..1019 (with N=0 being SGI/PPI which live in the
    // redistributor's banked GICR_ISPENDR0).
    if (intid < GIC_SPI_MIN || intid >= GIC_NUM_INTIDS) return false;
    if (g_dist_base == 0)                              return false;
    u32 n = intid / 32;
    u32 bit = intid % 32;
    // v2 distributor writes drain (HVF ISV mitigation); v3 uses the plain form.
    if (g_version == GIC_VERSION_V2)
        v2_w32(g_dist_base, GICD_ISPENDR(n), 1u << bit);
    else
        mmio_w32(g_dist_base, GICD_ISPENDR(n), 1u << bit);
    return true;
}

bool gic_disable_irq(u32 intid) {
    if (intid >= GIC_NUM_INTIDS) return false;
    if (g_version == GIC_VERSION_V2) {
        u32 n = intid / 32, bit = intid % 32;
        v2_w32(g_dist_base, GICD_ICENABLER(n), 1u << bit);
        return true;
    }
    if (intid < 32) {
        u64 base = cpu_redist_base(smp_cpu_idx_self());
        mmio_w32(base, GICR_ICENABLER0, 1u << intid);
    } else {
        u32 n = intid / 32;
        u32 bit = intid % 32;
        mmio_w32(g_dist_base, GICD_ICENABLER(n), 1u << bit);
    }
    return true;
}

// ---------------------------------------------------------------------------
// gic_acknowledge / gic_eoi.
// ---------------------------------------------------------------------------

u32 gic_acknowledge(void) {
    if (g_version == GIC_VERSION_V2) {
        // GICC_IAR: [9:0] INTID, [12:10] source CPUID (SGIs). Save the raw
        // value for this CPU so gic_eoi can echo the CPUID back to GICC_EOIR
        // (ARM IHI 0048B §4.4.5). No nesting between this and the matching
        // EOI (IRQs masked in the handler), so the slot is single-writer.
        u32 iar = v2_r32(g_cpu_base, GICC_IAR);
        unsigned cpu = smp_cpu_idx_self();
        if (cpu < DTB_MAX_CPUS) g_v2_eoi_token[cpu] = iar;
        return iar & GICC_IAR_INTID_MASK;     // 1023 == spurious (filtered upstream)
    }
    u64 iar = read_icc_iar1_el1();
    return (u32)(iar & 0xFFFFFFu);
}

void gic_eoi(u32 intid) {
    // INTIDs 1020..1023 (GIC_NUM_INTIDS .. 1023) are reserved per ARM
    // IHI 0069 §2.2.1 — must NOT EOI. The exception path filters these
    // before reaching here, but defend the API call too in case a
    // future caller (manual EOI in a handler) passes a stale IAR.
    if (intid >= GIC_NUM_INTIDS) {
        return;
    }
    if (g_version == GIC_VERSION_V2) {
        // Echo the saved raw IAR (with the SGI source CPUID) when its INTID
        // matches what we're EOI'ing -- the normal ack->eoi pairing, which is
        // the ONLY reachable path (the exception handler always EOIs the INTID
        // it just acknowledged on the same CPU; see gic_acknowledge's contract
        // note). The bare-INTID branch (CPUID 0) is dead for that pairing; it
        // would only fire for an out-of-contract manual EOI of an INTID never
        // acknowledged here, and CPUID 0 is correct for any non-SGI INTID. A
        // manual EOI of a cross-CPU SGI (CPUID != 0) is unsupported.
        unsigned cpu = smp_cpu_idx_self();
        u32 token = intid;
        if (cpu < DTB_MAX_CPUS &&
            (g_v2_eoi_token[cpu] & GICC_IAR_INTID_MASK) == intid) {
            token = g_v2_eoi_token[cpu];
        }
        v2_w32(g_cpu_base, GICC_EOIR, token);
        return;
    }
    write_icc_eoir1_el1((u64)intid);
}

// ---------------------------------------------------------------------------
// Diagnostics.
// ---------------------------------------------------------------------------

gic_version_t gic_version(void) { return g_version; }
u64 gic_dist_base(void)         { return g_dist_base; }
u64 gic_redist_base(void)       { return g_redist_base; }
u64 gic_dist_pa(void)           { return g_dist_pa; }
u64 gic_redist_pa(void)         { return g_redist_pa; }

// GICv2 CPU-interface (GICC) bases; zero on v3 (sysreg interface).
u64 gic_cpu_iface_base(void)    { return g_cpu_base; }
u64 gic_cpu_iface_pa(void)      { return g_cpu_pa; }

// R12-gic-edge audit close (F205 P3): expose the runtime-discovered
// maximum dispatchable INTID, sourced from GICD_TYPER.ITLinesNumber at
// gic_init. Returns 0 before gic_init runs. Used by intid_try_claim to
// bound INTID claims against the actual distributor implementation
// rather than the architectural GIC_NUM_INTIDS upper bound (1020), so
// CAP_HW_CREATE userspace cannot reach the GIC helpers with intids in
// (g_max_intid, GIC_NUM_INTIDS] that would produce UNPREDICTABLE
// register writes per IHI 0069 §12.9.7.
u32 gic_max_intid(void)         { return g_max_intid; }
