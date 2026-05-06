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

// Pin the FDT format constants at compile time (P1-I audit F35). A
// merge-conflict typo on FDT_MAGIC would silently reject every DTB on
// boot; static_asserts catch the drift before the kernel ships.
_Static_assert(FDT_MAGIC == 0xd00dfeedu,
               "FDT_MAGIC must be 0xd00dfeed per Devicetree Specification v0.4");
_Static_assert(FDT_BEGIN_NODE == 1 && FDT_END_NODE == 2 &&
               FDT_PROP == 3 && FDT_NOP == 4 && FDT_END == 9,
               "FDT_* token values pinned to spec");

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

// Total size in bytes of the DTB blob (= the FDT header's totalsize
// field). Used by phys_init to reserve the DTB region from the
// physical allocator. Returns 0 if dtb_init has not run.
u32 dtb_get_total_size(void);

// P3-Bda: relocate the DTB blob from the original PA (where QEMU placed
// it) to a kernel-allocated buffer. After this call, all DTB walks
// (dtb_struct_base / dtb_strings_base internally) read through the
// buffer's kernel direct-map KVA instead of via TTBR0 identity at the
// original PA. Once relocated, retiring TTBR0 identity is safe (no
// DTB code path touches the original PA anymore).
//
// Must be called AFTER phys_init (the buffer comes from buddy via
// kpage_alloc). Idempotent: re-calling is a no-op once g_dtb.relocated
// is true.
//
// Returns true on success. Returns false on:
//   - dtb_init not run yet (g_dtb.ready == false).
//   - kpage_alloc OOM (insufficient buddy memory at boot — typical
//     v1.0 DTB is ~1 KiB, which always fits).
bool dtb_relocate_to_buffer(void);

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
// and by P1-G to find the GIC distributor, ARM generic timer, etc.
//
// Returns true on success.
bool dtb_get_compat_reg(const char *compat, u64 *base, u64 *size);

// Like dtb_get_compat_reg, but returns the `reg` pair at `idx` (0-based)
// within the matched node's reg property. GICv3 uses a two-region reg
// (distributor at idx 0, redistributor at idx 1); GICv2 uses two regions
// (distributor at idx 0, CPU interface at idx 1) too. A single-region
// reg (e.g. PL011) is reachable with idx = 0 — equivalent to
// dtb_get_compat_reg.
//
// Returns false if no node matches `compat`, or if the matched node's
// reg property holds fewer than (idx + 1) pairs.
bool dtb_get_compat_reg_n(const char *compat, u32 idx,
                          u64 *base, u64 *size);

// True iff some DTB node's "compatible" property contains `compat`.
// Used by gic.c for v2-vs-v3 autodetect (probe "arm,gic-v3" first, then
// "arm,cortex-a15-gic" / "arm,gic-400" for v2). Cheaper than the full
// dtb_get_compat_reg walk if you only care whether a string exists.
bool dtb_has_compat(const char *compat);

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

// CPU enumeration (P2-Ca). Counts and inspects /cpus/cpu@* nodes —
// identified by device_type = "cpu". QEMU virt convention: each
// cpu@N has reg = single u32 cell holding the MPIDR aff bits (linear
// 0..N-1), and enable-method = "psci".
//
// Bound at v1.0: DTB_MAX_CPUS = 8 (matches ARCH §20.7 v1.0 SMP cap).
// Cores past this are reported in count but not enumerable via
// dtb_cpu_mpidr(). Phase 7 hardening would raise the bound.
#define DTB_MAX_CPUS 8u

// Number of /cpus/cpu@* nodes whose device_type = "cpu". Returns 0 if
// dtb_init has not run or no cpu nodes are present (impossible on a
// well-formed ARM64 DTB).
u32 dtb_cpu_count(void);

// MPIDR aff value for the `idx`-th cpu node (0-based, in DTB
// declaration order). Returns false if idx >= dtb_cpu_count() or
// idx >= DTB_MAX_CPUS. The return is the raw `reg` cell value; on
// QEMU virt this is the linear core index (0, 1, 2, ...). Real
// hardware may pack aff0/aff1/aff2/aff3 into the value; the caller
// passes the raw value to PSCI_CPU_ON.
bool dtb_cpu_mpidr(u32 idx, u64 *out_mpidr);

// PSCI calling-convention method. Encoded as:
//   DTB_PSCI_NONE = 0  — no /psci node, or method missing/unknown.
//   DTB_PSCI_HVC  = 1  — /psci/method = "hvc" (QEMU virt default).
//   DTB_PSCI_SMC  = 2  — /psci/method = "smc" (most ARM bare metal).
//
// PSCI version not exposed — assume PSCI 0.2+ standard function IDs
// (CPU_ON_64 = 0xc4000003, CPU_OFF = 0x84000002). Older PSCI variants
// with custom IDs fall back to NONE; if a future board needs them the
// /psci/cpu_on overrides can be exposed via a richer API.
typedef enum dtb_psci_method {
    DTB_PSCI_NONE = 0,
    DTB_PSCI_HVC  = 1,
    DTB_PSCI_SMC  = 2,
} dtb_psci_method_t;

dtb_psci_method_t dtb_psci_method(void);

#endif // THYLACINE_DTB_H
