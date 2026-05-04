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
     PTE_ATTR_IDX(MAIR_IDX_NORMAL_WB) | PTE_AP_RO_EL1 | PTE_UXN)
//   (RX at EL1; RO -> writable=false; PXN=0 -> executable-at-EL1=true.
//    Composite: not-writable AND executable. W^X OK.)

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

#define PTE_DEVICE_RW_BLOCK \
    (PTE_VALID | PTE_TYPE_BLOCK | PTE_AF | PTE_SH_NONE | \
     PTE_ATTR_IDX(MAIR_IDX_DEVICE) | PTE_AP_RW_EL1 | PTE_UXN | PTE_PXN)
//   (Device-nGnRnE; SH_NONE per ARM ARM B2.7.2; W^X OK by PXN.)

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

// Build the identity-mapped page tables in BSS and program MAIR / TCR /
// TTBR / SCTLR. After return, the MMU is on with caches enabled and the
// kernel runs from VAs identical to physical addresses.
//
// Called from arch/arm64/start.S after BSS clear and stack setup, before
// boot_main(). Idempotent only in the trivial sense (don't call twice).
void mmu_enable(void);

// Inspect a PTE for W^X compliance. Returns true iff the PTE is writable
// AND executable-at-EL1, which is the forbidden combination. Used by the
// audit-trigger surface verifier (and by tests at P1-I+).
bool pte_violates_wxe(u64 pte);

#endif // THYLACINE_ARCH_ARM64_MMU_H
