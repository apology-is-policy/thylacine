// FDT (Flattened Device Tree) parser — minimal hand-rolled.
//
// Per ARCHITECTURE.md §22.2: hardware discovery is via DTB. This parser
// reads the DTB QEMU loaded into RAM and lets the kernel ask:
//
//   - dtb_init(base)             validate magic + cache header
//   - dtb_get_memory(&b, &s)     extract /memory@... reg property
//   - dtb_get_compat_reg(...)    find first node matching compatible
//                                string; return its first reg range
//   - dtb_get_chosen_kaslr_seed  extract /chosen/kaslr-seed (P1-C)
//
// All FDT fields on the wire are big-endian; we read on a little-endian
// ARM64 host, so every multi-byte field gets byte-swapped.
//
// Specification: devicetree-specification.readthedocs.io v0.4
// (mirrors Linux's Documentation/devicetree/booting-without-of.txt).

#ifndef THYLACINE_DTB_H
#define THYLACINE_DTB_H

#include <thylacine/types.h>

// FDT magic in HOST byte order. The on-disk value is big-endian
// 0xd00dfeed; our reader byte-swaps before comparing.
#define FDT_MAGIC      0xd00dfeedu

// Structure-block tokens (big-endian on disk; we compare after byte-swap).
#define FDT_BEGIN_NODE 0x00000001u
#define FDT_END_NODE   0x00000002u
#define FDT_PROP       0x00000003u
#define FDT_NOP        0x00000004u
#define FDT_END        0x00000009u

// Initialize the parser. `base` is the physical address of the DTB
// (received in x0 by start.S; stored in _saved_dtb_ptr).
//
// Returns true on success (magic valid, header readable). On failure
// (NULL base, magic mismatch, version unsupported), returns false and
// the parser is not usable.
//
// Re-entrant against itself only in the trivial sense: re-calling with
// the same base re-validates and is idempotent. Calling with a different
// base re-points the parser; subsequent calls operate on the new DTB.
bool dtb_init(paddr_t base);

// Whether dtb_init() previously succeeded.
bool dtb_is_ready(void);

// Get the first /memory@... node's first (base, size) pair.
//
// Assumes #address-cells = 2 and #size-cells = 2 at the root, which is
// the QEMU virt and ARM64 Linux convention. If a DTB uses different
// cell sizes, this returns false. (P1-C may extend to read cell sizes
// dynamically from the parent node.)
//
// Returns true on success; *base and *size are populated.
bool dtb_get_memory(u64 *base, u64 *size);

// Find the first node whose "compatible" property contains `compat`,
// then return its first `reg` (base, size) pair.
//
// Used by P1-B to discover the PL011 base address (compat = "arm,pl011")
// and by P1-F to find the GIC base, ARM generic timer, etc.
//
// Returns true on success.
bool dtb_get_compat_reg(const char *compat, u64 *base, u64 *size);

// Read /chosen/kaslr-seed as 64 bits (the property is two u32 cells in
// the DTB; we concatenate them in the FDT-cell order). Returns 0 if the
// node or property is absent.
//
// UEFI on bare metal (Pi 5) populates /chosen/kaslr-seed. QEMU's direct
// -kernel boot DOES NOT — it only publishes /chosen/rng-seed. Use
// dtb_get_chosen_rng_seed() as the second-tier fallback.
//
// Used by arch/arm64/kaslr.c.
u64 dtb_get_chosen_kaslr_seed(void);

// Read /chosen/rng-seed as 64 bits, XOR-folding all 4-byte cells. The
// property is typically 32 bytes / 8 cells (256 bits) on QEMU virt and
// most UEFI environments; we collapse to 64 bits while preserving
// entropy across all cells via alternating high/low XOR.
//
// Returns 0 if /chosen or the rng-seed property is absent.
//
// Used by arch/arm64/kaslr.c as the second-tier seed source after
// /chosen/kaslr-seed.
u64 dtb_get_chosen_rng_seed(void);

#endif // THYLACINE_DTB_H
