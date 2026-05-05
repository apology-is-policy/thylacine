// ARM Generic Interrupt Controller — v3 driver + v2 detect-only stub.
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
// Why GICv3 only at P1-G: QEMU virt with run-vm.sh's gic-version=3 +
// Pi 5 are both v3. v2 (Pi 4, older boards) needs separate distributor
// MMIO + CPU interface MMIO + different acknowledge/EOI register layout.
// Implementing v2 untested would risk silent bugs; the autodetect
// commitment from ARCH §12 is preserved by extincting cleanly when v2
// is detected, with a diagnostic that points at the deferred work. v2
// support lands when there's a Pi 4 testbed.

#include "gic.h"

#include "exception.h"
#include "mmu.h"
#include "uart.h"

#include <stdint.h>
#include <thylacine/dtb.h>
#include <thylacine/extinction.h>
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
#define GICD_IPRIORITYR(n)    (0x0400 + ((n)*4))
#define GICD_ITARGETSR(n)     (0x0800 + ((n)*4))    // GICv2 only
#define GICD_ICFGR(n)         (0x0C00 + ((n)*4))
#define GICD_IROUTER(n)       (0x6000 + ((n)*8))    // GICv3 only (per-INTID 64-bit)

#define GICD_CTLR_ENABLE_GRP1NS    (1u << 1)
#define GICD_CTLR_ARE_NS           (1u << 4)

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
static u64 g_dist_base;        // virtual / identity-mapped physical
static u64 g_redist_base;      // base of the redistributor REGION;
                               // per-CPU frames are at +N * 0x20000.
static u32 g_max_intid;        // from GICD_TYPER

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
// gic_init.
// ---------------------------------------------------------------------------

bool gic_init(void) {
    // Step 1: detect version from DTB.
    if (dtb_has_compat("arm,gic-v3")) {
        g_version = GIC_VERSION_V3;
    } else if (dtb_has_compat("arm,cortex-a15-gic") ||
               dtb_has_compat("arm,gic-400")) {
        g_version = GIC_VERSION_V2;
    } else {
        g_version = GIC_VERSION_NONE;
        extinction("gic_init: no GIC compat in DTB (need arm,gic-v3 or arm,cortex-a15-gic)");
    }

    if (g_version == GIC_VERSION_V2) {
        // v2 path is real but unverified; ARCH §12 commits to autodetect
        // at v1.0 and we honor that by failing loudly rather than running
        // untested code on misconfigured hardware. v2 implementation lands
        // when there's a Pi 4 (or similar) testbed.
        extinction("gic_init: GICv2 detected; v2 path deferred (no test target). Run QEMU virt with gic-version=3.");
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
    g_dist_base   = dbase;
    g_redist_base = rbase;

    // Step 3: map both regions Device-nGnRnE in TTBR0. Both are below
    // 4 GiB on QEMU virt; mmu_map_device validates the bound.
    if (!mmu_map_device((paddr_t)dbase, dsize)) {
        extinction("gic_init: mmu_map_device(dist) failed (>4 GiB?)");
    }
    if (!mmu_map_device((paddr_t)rbase, rsize)) {
        extinction("gic_init: mmu_map_device(redist) failed (>4 GiB?)");
    }

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
    if (g_version != GIC_VERSION_V3) return false;
    if (cpu_idx == 0 || cpu_idx >= 64) return false;

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
    if (target_cpu_idx >= 16) return false;        // TargetList is 16 bits

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

#include <thylacine/smp.h>     // smp_cpu_idx_self for per-CPU redist base

bool gic_enable_irq(u32 intid) {
    if (intid >= GIC_NUM_INTIDS) return false;
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

bool gic_disable_irq(u32 intid) {
    if (intid >= GIC_NUM_INTIDS) return false;
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
    write_icc_eoir1_el1((u64)intid);
}

// ---------------------------------------------------------------------------
// Diagnostics.
// ---------------------------------------------------------------------------

gic_version_t gic_version(void) { return g_version; }
u64 gic_dist_base(void)         { return g_dist_base; }
u64 gic_redist_base(void)       { return g_redist_base; }
