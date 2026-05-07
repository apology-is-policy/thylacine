// ARM64 MMU — page table setup + enable.
//
// At P1-C: identity-map the low 4 GB so the kernel's existing physical
// addresses remain valid after MMU enable, with per-section permissions
// (W^X invariant I-12 enforced at PTE bit level) and Device-nGnRnE
// attributes for MMIO regions (PL011 et al). KASLR (kernel-image
// relocation into TTBR1's high half) and TTBR1 mappings beyond a stub
// are deferred to P1-C-extras (the next sub-chunk).
//
// Per ARCHITECTURE.md §6 (memory management) and §24 (W^X invariant).
//
// The implementation uses 2 MiB block descriptors at L2 for bulk RAM and
// 4 KiB pages at L3 for the kernel image (so .text / .rodata / .data /
// .bss can each carry their own attributes).

#ifndef THYLACINE_ARCH_ARM64_MMU_H
#define THYLACINE_ARCH_ARM64_MMU_H

#include <stddef.h>           // size_t (for mmu_map_mmio)
#include <thylacine/page.h>   // paddr_t (for mmu_map_mmio)
#include <thylacine/types.h>

// ---------------------------------------------------------------------------
// MAIR_EL1 attribute encodings.
//
// MAIR_EL1 holds 8 attribute bytes; PTEs reference one by an AttrIndx
// field (3 bits) in the descriptor. We use four:
//
//   index 0  Device-nGnRnE  (MMIO; no gather, no reorder, no early ack)
//   index 1  Normal Non-Cacheable
//   index 2  Normal Inner+Outer Write-Back, Read+Write Allocate
//   index 3  Normal Inner+Outer Write-Through (unused at P1-C; reserved)
// ---------------------------------------------------------------------------

#define MAIR_IDX_DEVICE       0
#define MAIR_IDX_NORMAL_NC    1
#define MAIR_IDX_NORMAL_WB    2
#define MAIR_IDX_NORMAL_WT    3

#define MAIR_ATTR_DEVICE_nGnRnE  0x00ull
#define MAIR_ATTR_NORMAL_NC      0x44ull
#define MAIR_ATTR_NORMAL_WB      0xFFull
#define MAIR_ATTR_NORMAL_WT      0xBBull

#define MAIR_VALUE \
    ((MAIR_ATTR_DEVICE_nGnRnE << (MAIR_IDX_DEVICE    * 8)) | \
     (MAIR_ATTR_NORMAL_NC     << (MAIR_IDX_NORMAL_NC * 8)) | \
     (MAIR_ATTR_NORMAL_WB     << (MAIR_IDX_NORMAL_WB * 8)) | \
     (MAIR_ATTR_NORMAL_WT     << (MAIR_IDX_NORMAL_WT * 8)))

// ---------------------------------------------------------------------------
// Page table entry bits (Stage 1, EL1, AArch64).
//
// Layout:
//   [0]    Valid (must be 1 for a present descriptor).
//   [1]    Type (table descriptor for L0/L1/L2; pages for L3; 0 = block).
//   [4:2]  AttrIndx (index into MAIR_EL1).
//   [6]    Non-secure (Stage 1 EL1 ignored; we leave 0).
//   [7:6]  AP[2:1] (access permissions; see PTE_AP_*).
//   [9:8]  SH (shareability; 0b11 = inner shareable for normal memory).
//   [10]   AF (access flag; must be 1 to avoid hardware fault on first use).
//   [11]   nG (non-global; 0 for kernel mappings).
//   [47:12] Output address (frame number; bits below 12 are zero).
//   [53]   PXN (privileged execute never; 0 = exec at EL1, 1 = no-exec at EL1).
//   [54]   UXN (unprivileged execute never; we set 1 for all kernel mappings).
// ---------------------------------------------------------------------------

#define PTE_VALID         (1ull << 0)
#define PTE_TYPE_TABLE    (1ull << 1)         // table descriptor
#define PTE_TYPE_PAGE     (1ull << 1)         // L3 page descriptor
#define PTE_TYPE_BLOCK    (0ull << 1)         // L2 block descriptor

#define PTE_ATTR_IDX(i)   ((u64)(i) << 2)
#define PTE_AP_RW_EL1     (0ull << 6)         // RW at EL1, no access EL0
#define PTE_AP_RW_ANY     (1ull << 6)         // RW at EL1, RW at EL0
#define PTE_AP_RO_EL1     (2ull << 6)         // RO at EL1, no access EL0
#define PTE_AP_RO_ANY     (3ull << 6)         // RO at EL1, RO at EL0
#define PTE_SH_INNER      (3ull << 8)
#define PTE_SH_OUTER      (2ull << 8)
#define PTE_SH_NONE       (0ull << 8)
#define PTE_AF            (1ull << 10)
#define PTE_NG            (1ull << 11)
// PTE_GP — Guarded Page (P1-H, FEAT_BTI ARMv8.5+). When set, PSTATE.BTYPE
// is checked against BTI markers (`bti j/c/jc`) at indirect-branch landing
// pads in this page. Pages without GP are not BTI-checked. We set this
// for kernel text only so the compiler's -mbranch-protection=bti markers
// take effect on ARMv8.5+ hardware. RES0 on ARMv8.0..8.4 — harmless.
#define PTE_GP            (1ull << 50)
#define PTE_PXN           (1ull << 53)
#define PTE_UXN           (1ull << 54)

// ---------------------------------------------------------------------------
// W^X invariant (I-12) — composite attribute helpers.
//
// A "writable" PTE has AP[2:1] in {0b00, 0b01} (i.e., not 0b10/0b11).
// An "executable-at-EL1" PTE has PXN = 0.
// W^X forbids: writable AND executable-at-EL1.
//
// We supply four helpers — KERN_TEXT, KERN_RO, KERN_RW, DEVICE — that
// guarantee the invariant by construction. Static asserts at the bottom
// catch any future addition that violates the rule.
// ---------------------------------------------------------------------------

#define PTE_KERN_TEXT \
    (PTE_VALID | PTE_TYPE_PAGE | PTE_AF | PTE_SH_INNER | \
     PTE_ATTR_IDX(MAIR_IDX_NORMAL_WB) | PTE_AP_RO_EL1 | PTE_UXN | PTE_GP)
//   (RX at EL1; RO -> writable=false; PXN=0 -> executable-at-EL1=true.
//    Composite: not-writable AND executable. W^X OK.
//    GP=1 enables BTI checks on FEAT_BTI hardware (P1-H).)

#define PTE_KERN_RO \
    (PTE_VALID | PTE_TYPE_PAGE | PTE_AF | PTE_SH_INNER | \
     PTE_ATTR_IDX(MAIR_IDX_NORMAL_WB) | PTE_AP_RO_EL1 | PTE_UXN | PTE_PXN)
//   (R at EL1; RO + PXN -> not-writable AND not-executable. W^X OK.)

#define PTE_KERN_RW \
    (PTE_VALID | PTE_TYPE_PAGE | PTE_AF | PTE_SH_INNER | \
     PTE_ATTR_IDX(MAIR_IDX_NORMAL_WB) | PTE_AP_RW_EL1 | PTE_UXN | PTE_PXN)
//   (RW at EL1; RW + PXN -> writable AND not-executable. W^X OK.)

#define PTE_KERN_RW_BLOCK \
    (PTE_VALID | PTE_TYPE_BLOCK | PTE_AF | PTE_SH_INNER | \
     PTE_ATTR_IDX(MAIR_IDX_NORMAL_WB) | PTE_AP_RW_EL1 | PTE_UXN | PTE_PXN)
//   2 MiB block form of PTE_KERN_RW (used for bulk RAM identity mapping).

#define PTE_KERN_RO_BLOCK \
    (PTE_VALID | PTE_TYPE_BLOCK | PTE_AF | PTE_SH_INNER | \
     PTE_ATTR_IDX(MAIR_IDX_NORMAL_WB) | PTE_AP_RO_EL1 | PTE_UXN | PTE_PXN)
//   (P3-Bb-hardening: 2 MiB block, R + XN. Used in direct map for the
//   kernel image's 2 MiB block alongside per-page perms in
//   l3_directmap_kernel — defends against direct-map-write-to-.text
//   speculative attacks.)

#define PTE_DEVICE_RW_BLOCK \
    (PTE_VALID | PTE_TYPE_BLOCK | PTE_AF | PTE_SH_NONE | \
     PTE_ATTR_IDX(MAIR_IDX_DEVICE) | PTE_AP_RW_EL1 | PTE_UXN | PTE_PXN)
//   (Device-nGnRnE; SH_NONE per ARM ARM B2.7.2; W^X OK by PXN.)

#define PTE_DEVICE_RW \
    (PTE_VALID | PTE_TYPE_PAGE | PTE_AF | PTE_SH_NONE | \
     PTE_ATTR_IDX(MAIR_IDX_DEVICE) | PTE_AP_RW_EL1 | PTE_UXN | PTE_PXN)
//   (P3-Bb: page-grain device mapping for vmalloc-region MMIO.
//   Device-nGnRnE; SH_NONE per ARM ARM B2.7.2; W^X OK by PXN.)

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

// =============================================================================
// P3-Bb: Kernel direct map + vmalloc API.
//
// Per ARCH §6.2 + §6.10. The kernel direct map provides linear PA→KVA
// translation at base KERNEL_DIRECT_MAP_BASE (0xFFFF_0000_0000_0000); see
// `<thylacine/page.h>` for the inline pa_to_kva / kva_to_pa helpers.
// The vmalloc range (0xFFFF_8000_0000_0000) holds page-grain MMIO
// mappings discovered at runtime from the DTB.
//
// At v1.0 P3-Bb the direct map covers physical RAM up to 8 GiB (PA
// 1 GiB → 9 GiB; PA < 1 GiB is conventionally MMIO on QEMU virt and is
// excluded). This is sufficient for QEMU virt's 2 GiB default and
// Pi 5's 8 GiB. Larger configurations need the bump extended.
// =============================================================================

#define VMALLOC_BASE  0xFFFF800000000000ull

// Map a device-region MMIO range into the vmalloc area. Returns a kernel
// VA that the caller can pass to MMIO accessors (mmio_w32 etc.). The
// mapping is Device-nGnRnE attribute (per ARM ARM B2.7.2 — strongly-
// ordered, no gathering, no reordering, no early write acknowledgement).
//
// `pa` and `size` need not be page-aligned; the implementation rounds
// down `pa` and rounds up `size` to PAGE_SIZE. Returns NULL on out-of-
// vmalloc-space; extincts on misuse (pa+size overflow, etc.).
//
// Idempotent: calling twice with the same (pa, size) returns two
// DIFFERENT kvas, both mapping the same PA. Caller is responsible for
// caching the returned kva (typically in a `g_xxx_base` global at the
// driver layer).
//
// Cost: a few PTE writes + dsb_ishst + isb. No TLB flush needed because
// the new entries were previously invalid (no stale entries to flush).
void *mmu_map_mmio(paddr_t pa, size_t size);

// Build the page tables in BSS and program MAIR / TCR / TTBR / SCTLR.
//
// At P1-C-extras Part B, this builds two parallel mappings:
//   1. TTBR0: identity for the low 4 GiB. Keeps DTB and MMIO accessible
//      via their physical addresses (kernel needs these by absolute PA).
//      Also covers the kernel's load PA so the boot stub continues to
//      execute through the MMU enable transition.
//   2. TTBR1: kernel image at KASLR_LINK_VA + slide. The boot stub
//      long-branches to this high VA after mmu_enable() returns; from
//      then on, code runs through TTBR1.
//
// Per-section permissions (W^X invariant I-12) apply to BOTH mappings —
// the L3 table covering the kernel's 2 MiB block is shared between
// TTBR0 and TTBR1. The boot-stack guard page is non-present in this
// shared L3, so a stack overflow faults via either translation root.
//
// `slide` is the KASLR offset (page-block aligned, < 1 GiB), as chosen
// by kaslr_init(). Pass 0 to disable KASLR (used for debug builds; not
// recommended for normal operation).
//
// Called from arch/arm64/start.S after BSS clear, DTB save, and
// kaslr_init(). Idempotent only in the trivial sense (don't call twice).
void mmu_enable(u64 slide);

// Program MMU registers on the calling CPU using the page tables built
// by a prior mmu_enable() call (P2-Ca). Used by secondary CPU bring-up:
// the primary calls mmu_enable() at boot which builds + programs;
// secondaries call mmu_program_this_cpu() to program their MMU
// registers using the SAME tables (the kernel image is shared).
//
// Preconditions:
//   - mmu_enable() has run on some CPU (built l0_ttbr0 + l0_ttbr1).
//   - Caller has dsb-ish'd or otherwise observes the table writes
//     (build_page_tables uses dc cvau + dsb ish, so any later CPU's
//     MMU walk will see the published tables).
//   - Caller is at EL1 with MMU off; SCTLR.M is set on return.
//
// Idempotent within a CPU; the SCTLR.M OR-in is harmless when set.
void mmu_program_this_cpu(void);

// Inspect a PTE for W^X compliance. Returns true iff the PTE is writable
// AND executable-at-EL1, which is the forbidden combination. Used by the
// audit-trigger surface verifier (and by tests at P1-I+).
bool pte_violates_wxe(u64 pte);

// P3-Bda: retire the TTBR0 identity map. Zeros every L2 entry in
// l2_ttbr0[0..3] (the 4-GiB identity map) so any subsequent TTBR0 walk
// for a low VA hits an invalid block and faults. The L0 + L1 tables
// stay valid (table descriptors pointing at the now-empty L2s) so that
// TTBR0_EL1 itself can be left pointing at l0_ttbr0 — the MMU walks one
// level deeper before hitting the invalid entry, which is fine for fault
// triggering and avoids leaving TTBR0 at PA 0 (which would point at
// arbitrary RAM contents).
//
// Called from boot_main AFTER:
//   - uart_remap_to_vmalloc (UART no longer needs PL011 PA fallback).
//   - phys_init (buddy ready for DTB relocation).
//   - dtb_relocate_to_buffer (DTB queries no longer need original PA).
//
// At v1.0 P3-Bda: no further low-VA access exists in the kernel (kstack
// → direct map at P3-Bca; DTB → direct-map buffer at P3-Bda; UART →
// vmalloc at P3-Bca; GIC → vmalloc at P3-Bca). After this call, any
// stray low-VA dereference (a future bug with PA-as-VA-via-TTBR0) faults
// loudly rather than silently succeeding.
//
// Issues a broadcast TLB invalidate after PTE writes (per ARM ARM
// B2.7.1 break-before-make: zero the entry, dsb_ishst, tlbi_vmalle1is,
// dsb_ish, isb).
void mmu_retire_ttbr0_identity(void);

// P3-Bca: per-thread kstack guard pages — direct map flavor.
//
// `mmu_set_no_access(pa)` makes the 4 KiB page at PA unreadable +
// unwritable + unexecutable IN THE KERNEL DIRECT MAP (TTBR1, base
// 0xFFFF_0000_0000_0000). A kstack overflow (which accesses
// `pa_to_kva(pa)` via the SP register) triggers a permission/translation
// fault that the exception handler reports as a stack-overflow
// extinction.
//
// Implementation requires demoting the relevant `l1_directmap[gib]`
// entry from a 1 GiB block to a 512×2MiB-block L2, then demoting the
// relevant L2 entry from a 2 MiB block to a 4-KiB-page L3. Both demotes
// allocate fresh tables from buddy. Demotes are idempotent: subsequent
// calls hitting the same GiB / 2 MiB block reuse the existing L2 / L3.
//
// `mmu_restore_normal(pa)` restores the page to RW + XN in the direct
// map (PTE_KERN_RW). Called on thread_free before the underlying pages
// are returned to buddy.
//
// At v1.0 P3-Bca these are called single-threaded from the boot CPU
// (thread_create / thread_free are not yet SMP-safe). Phase 5+ adds a
// global mmu_lock when multi-thread Procs make concurrent thread_create
// possible.
//
// Returns false on OOM (L2 / L3 table allocation failed) or if the PA
// is outside the direct-map range (PA < 1 GiB or PA >= 9 GiB at v1.0).
bool mmu_set_no_access(paddr_t pa);
bool mmu_restore_normal(paddr_t pa);

// Batched variants for callers protecting / unprotecting a contiguous
// run of pages within a single 2 MiB L2 block (the common case for
// per-thread guard regions: 4 pages ≪ 1 block). Operates on
// `[pa, pa + n_pages * PAGE_SIZE)`. One TLB flush at the end instead
// of per-page.
//
// Constraints:
//   - pa must be 4 KiB aligned.
//   - n_pages > 0 and the range must lie within a single 2 MiB block
//     (i.e., (pa + n_pages * PAGE_SIZE - 1) >> 21 == pa >> 21).
//   - PA range is in the direct map (1 GiB ≤ PA < 9 GiB at v1.0).
//
// Returns false on alignment / range / OOM violations.
bool mmu_set_no_access_range(paddr_t pa, unsigned n_pages);
bool mmu_restore_normal_range(paddr_t pa, unsigned n_pages);

// P3-Bdb: PA of the kernel-only TTBR0 page table (l0_ttbr0). Used as the
// pgtable_root for kproc threads (which have proc->pgtable_root = 0).
// Captured in build_page_tables at boot time (when PC = load PA, so
// `(uintptr_t)l0_ttbr0` resolves to load PA via PIC adrp+add).
//
// Loading TTBR0_EL1 with this PA gives a "kernel-only" TTBR0: the L0/L1
// tables are valid, but every L2 entry is invalid (post-mmu_retire_ttbr0_
// identity). Any low-VA dereference faults at L2. For kernel threads
// running purely in EL1 with no need for user-half mappings, this is
// the safe choice — never accidentally translate a low VA.
paddr_t mmu_kernel_ttbr0_pa(void);

// =============================================================================
// P3-Bcb / P3-Db: per-Proc translation table allocator + recursive teardown.
//
// Each non-kernel Proc owns its own L0 translation table for TTBR0
// (user-half). proc_pgtable_create allocates a fresh L0 (4 KiB,
// page-aligned, KP_ZERO so all 512 entries are invalid). The returned
// PA is the value caller writes into struct Proc.pgtable_root; P3-Bd
// loads it into TTBR0_EL1 at context switch.
//
// proc_pgtable_destroy walks the L0 → L1 → L2 → L3 tree (P3-Db) and
// frees every sub-table reached via a table descriptor, then frees
// the L0 root. Leaf pages (L3 page descriptors / L2 block descriptors)
// are NOT freed — those user-VA-mapped pages are owned by the VMA
// layer (VMO mapping_count). The walker frees only translation-table
// pages.
//
// Pre-P3-Db, this only freed the L0 page. P3-Db extended it to handle
// the post-P3-Dc world where demand-paging populates sub-tables.
// Today's tree continues to have no sub-tables (P3-Db is wired in
// advance of demand paging); the walk is no-op-on-empty.
//
// Returns 0 on OOM (caller treats as ENOMEM and rolls back the Proc
// allocation). Idempotent on input root == 0.
//
// TLB lifecycle: asid_free issues a broadcast `tlbi aside1is` that
// invalidates all TLB entries tagged with the freed Proc's ASID. The
// flush happens BEFORE the ASID is returned to the free-list (see
// arch/arm64/asid.c), which ensures any subsequent reuser starts with a
// clean TLB. Independently, the freed pgtable PAs may be returned to
// buddy by proc_pgtable_destroy and recycled — that's safe because:
//   (a) The Proc is not running on any CPU at proc_free time (its sole
//       thread reached THREAD_EXITING + sched dropped it; wait_pid spun
//       on on_cpu == 0 before reaping).
//   (b) No other Proc uses our ASID until asid_free's flush + free-list
//       push, after which any reuser sees no stale entries.
// proc.c::proc_free's order (proc_pgtable_destroy before asid_free) is
// thus correct; reversing would also be correct.
// =============================================================================

paddr_t proc_pgtable_create(void);
void    proc_pgtable_destroy(paddr_t root);

// =============================================================================
// P3-Dc: install a user-mode leaf PTE in a per-Proc page-table tree.
//
// Walks the L0 → L1 → L2 → L3 tree rooted at `pgtable_root`, allocating
// any missing sub-tables (KP_ZERO via alloc_pages). Installs an L3 leaf
// page descriptor mapping `vaddr` (4 KiB-aligned user VA) to `pa`
// (4 KiB-aligned PA) with permissions derived from `prot`:
//
//   prot & VMA_PROT_WRITE   → AP_RW_ANY (RW EL0+EL1)   else AP_RO_ANY
//   prot & VMA_PROT_EXEC    → UXN clear (user can exec) else UXN set
//
// PXN is set unconditionally — kernel never executes user pages. nG is
// set so the entry is ASID-tagged. AF + SH_INNER + ATTR_IDX_NORMAL_WB.
//
// Idempotent on already-installed mapping with matching PA + prot
// (concurrent fault on the same page from a multi-thread Proc; not
// expected at v1.0 single-thread Procs but defensively handled).
// Returns -1 if an existing PTE points at a different PA or carries
// different prot bits — signals a logic bug in the demand-paging path.
//
// Returns:
//   0   on success.
//   -1  on alignment violation (vaddr / pa not 4-KiB-aligned), out-of-
//       range vaddr (high bit set — not user-half), out-of-IPS pa,
//       sub-table alloc OOM, or already-installed-with-mismatch.
//
// No TLB flush issued: invalid → valid transitions don't require it
// (per ARM ARM B2.7.1). The caller (userland_demand_page) issues
// `dsb_ishst + isb` after this returns to ensure the new PTE is
// observable to the MMU walker before ERET resumes execution.
//
// At v1.0 P3-Dc this runs single-threaded (single-thread Proc + single
// CPU touching that Proc's pgtable at a time). Phase 5+ multi-thread
// concurrent demand-page on the same Proc requires a per-Proc
// pgtable lock; documented as a trip-hazard.
//
// `asid` is currently unused; reserved for future replace-PTE paths
// that need an ASID-targeted TLB invalidate.
// =============================================================================

int mmu_install_user_pte(paddr_t pgtable_root, u16 asid,
                         u64 vaddr, paddr_t pa, u32 prot);

#endif // THYLACINE_ARCH_ARM64_MMU_H
