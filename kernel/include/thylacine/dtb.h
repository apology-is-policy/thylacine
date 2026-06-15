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

// P4-F: enumerate every node whose `compatible` contains `compat`.
// Calls `cb(match_idx, reg_base, reg_size, arg)` per matching node
// using the node's first reg pair. The match_idx is 0-based in DTB
// declaration order. Iteration stops if the callback returns non-zero
// or the DTB is exhausted. Returns the number of matches dispatched.
//
// Used by virtio.c to enumerate the 32 virtio-mmio slots QEMU virt
// publishes; future drivers needing multi-match enumeration follow
// the same pattern.
typedef int (*dtb_compat_cb)(u32 match_idx, u64 reg_base, u64 reg_size, void *arg);
u32 dtb_for_each_compat_reg(const char *compat, dtb_compat_cb cb, void *arg);

// Read a named property's raw bytes from the FIRST node whose "compatible"
// contains `compat`. Returns the property data pointer + length via
// *out_data / *out_len (pointing into the DTB blob, valid for the kernel's
// lifetime). Returns false if no matching node has the property. Like
// dtb_get_compat_reg_n it tolerates "compatible" appearing after the target
// property within the node (depth-stack walk). (pci-1a.)
bool dtb_get_compat_prop(const char *compat, const char *prop,
                         const u8 **out_data, u32 *out_len);

// PCI INTx -> GIC INTID routing (pci-1a, the virtio-PCI transport).
//
// Parse the PCIe host bridge's `interrupt-map` (compatible =
// "pci-host-ecam-generic") to resolve the GIC INTID a device at PCI
// device-number `pci_dev` (bus 0, function 0) raises on legacy INTx pin
// `pin` (1 = INTA .. 4 = INTD). Honors I-15: the routing DATA comes from
// the DTB interrupt-map, not a hardcoded base. The child cell-count is
// taken from `interrupt-map-mask`; the per-row stride is DERIVED from the
// repeated parent phandle (falling back to the documented QEMU-virt/ARM
// layout: GIC #address-cells=2 + #interrupt-cells=3); only the universal
// GIC #interrupt-cells=3 specifier (`<type intid flags>`, type 0 = SPI) is
// assumed -- the same "assume the universal cell layout" house style as
// dtb_get_memory.
//
// Returns true + *out_gic_intid (an ABSOLUTE GIC SPI INTID, >= 32) on a
// clean SPI match; false if the node/property is absent or malformed, the
// matched entry is not a GIC SPI, or no row matches (pin out of range).
bool dtb_pci_intx_route(u8 pci_dev, u8 pin, u32 *out_gic_intid);

// PCIe 32-bit non-prefetchable MMIO window (pci-1a) -- the CPU-PA range
// from the host bridge's `ranges` from which the kernel assigns BARs (we
// boot bare, so no UEFI/firmware assigns them). Returns true + *out_base /
// *out_size (on QEMU virt: 0x10000000 / ~768 MiB); false if the node or
// `ranges` is absent or has no 32-bit-MMIO entry.
bool dtb_pci_mem_window(u64 *out_base, u64 *out_size);

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

// P4-E: read /chosen/linux,initrd-start + linux,initrd-end as u64
// big-endian addresses (QEMU virt #address-cells=2 publishes 8-byte
// cells; 4-byte cells also accepted for portability). Returns true
// on a clean read (both properties present + end > start > 0); false
// on absence or malformation. The values are physical addresses; the
// kernel's direct map provides the KVA via pa_to_kva.
bool dtb_get_chosen_initrd(u64 *start, u64 *end);

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

// Raw `capacity-dmips-mhz` for the `idx`-th cpu node (0-based, DTB
// declaration order) — the per-core relative-performance hint the HMP
// scheduler normalizes into a capacity class (ARCH §8.4.4; composes with
// I-15, "the hardware view derives entirely from the DTB"). The property
// is a single u32 cell; a higher value means a more capable core (Linux's
// `capacity-dmips-mhz` convention). Returns true and writes *out only when
// the node declares the property with a positive value; returns false (out
// untouched) when absent — the QEMU-virt / homogeneous case, where every
// core is treated at the default full capacity. idx >= dtb_cpu_count() or
// idx >= DTB_MAX_CPUS also returns false.
bool dtb_cpu_capacity(u32 idx, u32 *out_dmips_mhz);

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

// =============================================================================
// Tree-walk API (Menagerie devhw — the DTB published as a walkable tree).
// =============================================================================
//
// The point-lookup API above (dtb_get_compat_reg, ...) answers "where is the
// node matching compatible X?" — sufficient for the kernel's own driver
// discovery, but it cannot ENUMERATE the tree the way the warden + userspace
// drivers must (walk the node hierarchy, read arbitrary properties). This
// layer exposes the FDT structure block as a navigable node tree without
// copying it: a node is identified by the structure-block byte offset of its
// FDT_BEGIN_NODE token (the root node is offset 0 — DTB_NODE_ROOT); a property
// by the offset of its FDT_PROP token. These offsets are stable for the
// kernel's lifetime (the relocated DTB buffer is immutable) and fit the
// devhw qid encoding (docs/reference). Every entry point is a no-op returning
// false if dtb_init has not run, and bounds-checks its offset against the
// structure/strings block before dereferencing — a malformed offset is
// rejected, never followed into arbitrary memory.

#define DTB_NODE_ROOT 0u

// One direct child of a node — either a sub-node (a directory in the devhw
// view) or a property (a file). `off` is the structure-block offset of the
// child's FDT_BEGIN_NODE token (when is_node) or its FDT_PROP token (when a
// property). `name` / `data` point into the DTB blob (kernel-lifetime, never
// freed); the caller must not write through them.
struct dtb_node_entry {
    bool          is_node;     // true: a sub-node (directory); false: a property (file)
    u32           off;         // struct-block offset of the child's BEGIN_NODE / FDT_PROP token
    const char   *name;        // unit-name (sub-node) or property name; NUL-terminated
    u32           namelen;     // strlen(name)
    const u8     *data;        // property value (NULL for a sub-node)
    u32           datalen;     // property length (0 for a sub-node)
};

// Validate that `node_off` names a BEGIN_NODE; return its unit-name. The root
// node's name is "" (empty). Returns false on a malformed / out-of-range
// offset. `out_name` / `out_namelen` are optional (NULL to ignore — use it as
// a pure validity probe).
bool dtb_node_at(u32 node_off, const char **out_name, u32 *out_namelen);

// Iterate a node's DIRECT contents (sub-nodes + properties) in document order.
// Start with *cursor = 0; each call fills *out, advances *cursor past the
// entry, and returns true while entries remain; returns false at end-of-node
// (or on a malformed / out-of-range node_off). The cursor is an OPAQUE resume
// position (a structure-block offset, NEVER 0 after the first entry, strictly
// increasing) — the caller passes it back unchanged; it doubles as the devhw
// readdir cookie. Single-entry-per-call (the readdir/walk drivers loop).
bool dtb_node_iter(u32 node_off, u32 *cursor, struct dtb_node_entry *out);

// Read a property given the structure-block offset of its FDT_PROP token
// (the offset an iter / walk handed out). Writes name / data / len. Returns
// false on a malformed / out-of-range offset. Optional out-params (NULL ok).
bool dtb_prop_at(u32 prop_off, const char **out_name,
                 const u8 **out_data, u32 *out_len);

// Parent node offset (for ".."). The root (DTB_NODE_ROOT) is its own parent.
// Returns false on a malformed / not-found node_off. O(tree) — the FDT stores
// no parent pointer, so this re-walks; ".." is resolver-handled in the common
// path (stalk pops the trail) so this is a robustness / direct-call fallback.
bool dtb_node_parent(u32 node_off, u32 *out_parent_off);

#endif // THYLACINE_DTB_H
