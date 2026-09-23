// Per-Proc Virtual Memory Area — P3-Da.
//
// A VMA describes a contiguous range of user virtual addresses
// `[vaddr_start, vaddr_end)` with associated permissions and a backing
// BURROW. The sorted-list of VMAs anchored at `struct Proc.vmas` is
// per-Proc state; it forms the "address space description" against
// which page faults are dispatched (P3-Dc adds the dispatcher
// integration).
//
// At v1.0 P3-Da the data structure is a simple sorted doubly-linked
// list: O(N) insert / lookup. RB-tree (or interval-tree) layered on
// top is a Phase 5+ optimization once N grows past ~32 entries per
// Proc — which won't happen at v1.0 (early userspace has a handful of
// segments + a stack).
//
// Per ARCHITECTURE.md §16 (process address space).

#ifndef THYLACINE_VMA_H
#define THYLACINE_VMA_H

#include <thylacine/types.h>

struct Proc;
struct Burrow;
struct AddrSpace;   // LINEAGE L-1/L-2: the *_in forms address by address space

// VMA permission bits. Map to PTE_KERN_TEXT/RO/RW + user-bit at PTE
// installation time (P3-Db). At v1.0 P3-Da these are policy markers
// stored in the VMA; the page-fault handler consults them when
// installing PTEs.
#define VMA_PROT_READ   (1u << 0)
#define VMA_PROT_WRITE  (1u << 1)
#define VMA_PROT_EXEC   (1u << 2)

#define VMA_PROT_RW   (VMA_PROT_READ | VMA_PROT_WRITE)
#define VMA_PROT_RX   (VMA_PROT_READ | VMA_PROT_EXEC)

// VMA_FLAG_SHARED_IN (G-2; TAPESTRY.md §18.12 R2-F3): this VMA was installed by
// burrow_share_into — the backing Burrow is ANOTHER Proc's memory (a netd flow
// ring or a tapestryd weave) mapped cross-Proc into this one. Set by
// burrow_share_into after vma_insert (under the same vma_lock hold); read at
// the two flagged-VMA teardown sites (burrow_unmap + vma_drain) to uncharge the
// per-client shared-mapping budget (Proc.shared_map_pages) exactly once per
// charge. The budget invariant: shared_map_pages == Σ pages of SHARED_IN VMAs.
#define VMA_FLAG_SHARED_IN  (1u << 0)

// VMA_FLAG_COW (LINEAGE L-4b; docs/LINEAGE.md section 5.4): this VMA's
// BURROW_TYPE_ANON_LAZY backing may hold pages that a SECOND address space's
// clone Burrow also points at, so a write must go through the copy-on-write
// break rather than straight to the page. Set by addrspace_clone on BOTH the
// parent's VMA and the child's at fork, never cleared.
//
// Never cleared, deliberately. The flag says "this mapping participates in
// COW", and the per-PAGE cow_share count is what actually decides each break --
// so a VMA whose pages have all been taken in place costs one extra fault per
// page and nothing else, while clearing it would need a scan proving no page in
// the range is still shared. The page count is the truth; this is the routing.
//
// It is the PTE, not the VMA, that is made read-only: vma->prot keeps
// VMA_PROT_WRITE so a write fault passes demand_page_locked's step-2 permission
// check and reaches the break. A VMA that dropped WRITE would turn every COW
// write into a segfault.
#define VMA_FLAG_COW        (1u << 1)

// The permission CEILING (B-1a; ARCH 6.5 "The permission ceiling"; I-12): bits
// 8..10 of `flags` hold the VMA_PROT_* set this mapping may never be raised past.
// Fixed by the mint and lowered only by PROTECT_SEAL; nothing raises it.
//
// vma_alloc sets it to the MINT prot, which is the safe default and not merely
// the lazy one: the vDSO clock page is a kernel-owned eager anon Burrow mapped
// read-only into every address space (exec_map_vdso), and a default of RW would
// let any Proc raise that mapping and write the kernel's clock. The reserve-style
// mints -- SYS_BURROW_RESERVE, SYS_BURROW_ATTACH_LAZY, the phenotype anon arms --
// raise it to RW explicitly, because their mapping may start at none or R and
// commit later. A guard's ceiling is none. A split piece carries its parent's; a
// fork's child VMA carries the parent's (clone_one_vma), which is also what keeps
// the eager-anon SHARE arm sound: an eager mapping is shared across a fork only
// when its CEILING excludes WRITE, since the current prot can be raised back.
//
// Lives in `flags` so struct Vma stays 64 bytes. The two bits below it are the
// mapping's STATE (shared-in, COW); the D-3b split's "no flags" refusal reads
// the state bits only (VMA_FLAG_STATE_MASK).
#define VMA_FLAG_MAX_SHIFT  8
#define VMA_FLAG_MAX_MASK   (7u << VMA_FLAG_MAX_SHIFT)
#define VMA_FLAG_STATE_MASK (VMA_FLAG_SHARED_IN | VMA_FLAG_COW)

// VMA_MAGIC at offset 0 — SLUB freelist clobber defense (mirrors
// struct Proc / struct Thread / struct Burrow / struct Handle pattern).
#define VMA_MAGIC 0x564D413043ADEFADULL    // 'VMA0' || 0xCADE'FADE

struct Vma {
    u64 magic;            // VMA_MAGIC
    u64 vaddr_start;      // inclusive, page-aligned
    u64 vaddr_end;        // exclusive, page-aligned
    u32 prot;             // VMA_PROT_* bitmask
    u32 flags;            // VMA_FLAG_* bitmask (was the alignment pad; 0 for
                          //   every pre-G-2 VMA — vma_alloc KP_ZEROs it)
    struct Burrow *burrow;      // backing object (refcounted)
    u64 burrow_offset;       // byte offset into BURROW

    // Sorted doubly-linked list. Sorted by vaddr_start ascending.
    // Anchored at struct Proc.vmas.
    struct Vma *next;
    struct Vma *prev;
};

_Static_assert(sizeof(struct Vma) == 64,
               "struct Vma size pinned at 64 bytes (8 u64 = 64). Adding a "
               "field grows the SLUB cache; update this assert deliberately.");
_Static_assert(__builtin_offsetof(struct Vma, magic) == 0,
               "magic must be at offset 0 for SLUB freelist clobber defense");

static inline u32 vma_prot_max(const struct Vma *v) {
    return (v->flags & VMA_FLAG_MAX_MASK) >> VMA_FLAG_MAX_SHIFT;
}
static inline void vma_set_prot_max(struct Vma *v, u32 prot) {
    v->flags = (v->flags & ~VMA_FLAG_MAX_MASK) |
               ((prot & 7u) << VMA_FLAG_MAX_SHIFT);
}

// Bring up the VMA subsystem (allocate the SLUB cache). Must run after
// slub_init; before any vma_alloc.
void vma_init(void);

// Allocate a fresh Vma descriptor. Initializes magic + the passed
// fields; next/prev set to NULL (caller's responsibility to insert
// into the per-Proc list via vma_insert). Refcounts the BURROW via
// burrow_acquire_mapping (mapping_count++). Returns NULL on OOM or any
// constraint violation.
//
// Constraints:
//   - vaddr_start < vaddr_end.
//   - both page-aligned (4 KiB).
//   - burrow non-NULL.
//   - prot ∈ {0, R, RW, RX} (at v1.0 we reject W+X to mirror the W^X
//     invariant; runtime enforcement happens at PTE installation).
//
// Returns NULL on any constraint violation (without partial allocation).
struct Vma *vma_alloc(u64 vaddr_start, u64 vaddr_end, u32 prot,
                     struct Burrow *burrow, u64 burrow_offset);

// Allocate a guard VMA — a reserved address range with NO backing
// BURROW and prot == 0. It exists purely to occupy address space:
//   - vma_insert's overlap rejection keeps any future mapping out of
//     the range, so a stack/heap guard region stays reliably unmapped.
//   - userland_demand_page rejects every fault into it: the prot == 0
//     permission check fails for read, write, AND instruction faults
//     alike, returning before the (NULL) BURROW is ever dereferenced.
// Used for the unmapped guard page directly below the user stack
// (exec.c) so an overflow faults instead of corrupting a lower VMA.
//
// A guard VMA owns no BURROW, so it does not participate in the BURROW
// dual-refcount lifecycle (no burrow_acquire_mapping); vma_free is
// NULL-burrow-safe and mirrors this.
//
// Constraints: vaddr_start < vaddr_end, both page-aligned (4 KiB).
// Returns NULL on OOM or constraint violation. The caller inserts via
// vma_insert and, on overlap rejection, frees the VMA via vma_free.
struct Vma *vma_alloc_guard(u64 vaddr_start, u64 vaddr_end);

// Free a Vma descriptor. Releases the BURROW ref. Caller MUST have
// removed it from any per-Proc list (extincts otherwise — magic check
// + next/prev != NULL detection).
void vma_free(struct Vma *v);

// vma_free, additionally reporting whether releasing this mapping was the drop
// that freed the Burrow's pages (#130 -- see burrow_unref_freed). False for a
// VMA with no Burrow.
bool vma_free_freed(struct Vma *v);

// D-3c F1: vma_free that DEFERS the Burrow free. Frees the Vma struct and drops
// the mapping ref, but returns the Burrow still owing a free (or NULL) instead
// of freeing it inline -- so a caller holding as->lock can free it AFTER the
// unlock (the FILE arm's spoor_clunk may sleep). *out_freed reports the same
// event vma_free_freed's bool does. See burrow_free_deferred + deferred_free_next.
struct Burrow *vma_free_deferred(struct Vma *v, bool *out_freed);

// Insert `v` into Proc `p`'s sorted VMA list. Rejects overlap with
// any existing VMA in the list. Returns 0 on success, -1 on overlap
// (caller must vma_free the rejected VMA themselves; this function
// doesn't free).
//
// At v1.0 P3-Da this is the single-thread-Proc serialization point.
// Phase 5+ multi-thread Procs need a per-Proc lock around the list;
// documented as a trip-hazard when added.
int vma_insert(struct Proc *p, struct Vma *v);

// Remove `v` from Proc `p`'s VMA list. Caller still owns the Vma
// after the remove (typically calls vma_free next).
void vma_remove(struct Proc *p, struct Vma *v);

// Look up the VMA covering `vaddr`. Returns the matching Vma * or
// NULL. O(N) at v1.0 (sorted list walk; Phase 5+ RB-tree is O(log N)).
//
// `vaddr` need not be page-aligned; the lookup uses
// `vaddr_start <= vaddr < vaddr_end` as the membership predicate.
struct Vma *vma_lookup(struct Proc *p, u64 vaddr);

// P6-pouch-mem: first-fit free-range finder for SYS_BURROW_ATTACH.
// Scans Proc `p`'s sorted VMA list for the lowest free gap of at least
// `length` bytes within [window_start, window_end). On success writes
// the gap's base VA to *out_vaddr and returns 0; returns -1 if no gap
// of that size fits (or on a constraint violation).
//
// `length` must be > 0 and page-aligned; `window_start <= window_end`,
// both page-aligned. The returned range [*out_vaddr, *out_vaddr+length)
// overlaps no existing VMA — vma_insert's overlap rejection is the
// backstop, but a correct find never trips it.
//
// Caller must hold the serializing lock (Proc.vma_lock) across the
// find AND the subsequent vma_insert — the same single-mutator
// discipline vma_insert / vma_remove already assume.
int vma_find_gap(struct Proc *p, u64 length,
                 u64 window_start, u64 window_end, u64 *out_vaddr);

// B-1a: the ALIGNED first-fit (SYS_BURROW_RESERVE's align_log2; Fuchsia's
// ZX_VM_ALIGN_*). `align` is a power of two >= PAGE_SIZE, or 0 for page
// alignment; the candidate base is rounded up to it after every VMA it jumps.
// Same contract and lock discipline as vma_find_gap, which is this with 0.
int vma_find_gap_aligned(struct Proc *p, u64 length, u64 align,
                         u64 window_start, u64 window_end, u64 *out_vaddr);

// Walk every VMA in Proc `p`'s list and free it. Used at proc_free
// to release all VMAs (and decrement their VMOs' mapping counts).
// Caller (proc_free) calls this BEFORE handle_table_free — handle
// closure of BURROW handles independently decrements burrow->handle_count.
void vma_drain(struct Proc *p);

// DISTRO D-3b: place a mapping at a CHOSEN address, splitting whatever is
// already there around it. The MAP_FIXED primitive: musl's map_library reserves
// a whole-span mapping and then overlays each PT_LOAD onto it (dynlink.c:842 /
// :851), which is precisely "swap the backing of a sub-range and keep the rest".
//
// THE DOMAIN IS TWO SHAPES, and the second one is not optional:
//   (a) [vaddr, vaddr+length) lies WHOLLY INSIDE one existing VMA -> split it.
//   (b) the range is entirely FREE -> plain insert, nothing to split.
// Anything else -- spanning two VMAs, or partially overlapping one -- is
// refused. Linux serves that third shape by unmapping the overlapped part;
// partial unmap is post-v1.0, so the divergence is stated rather than faked.
//
// Shape (b) exists because Linux MAP_FIXED does NOT require the target to be
// mapped already. Omitting it made an unmapped-address request answer ENOMEM,
// which is a worse reply than the ENOSYS it replaced -- ENOMEM cannot be told
// apart from real memory pressure, so an allocator reads it as OOM. (#196.)
//
// Neither existing primitive can express it: burrow_map_in has no burrow_offset
// parameter (it hardcodes 0), and burrow_unmap demands an EXACT (vaddr, length)
// match because partial unmap is post-v1.0. So this is new surgery, and it is
// the audit-bearing half of D-3b.
//
// NO HOLE CAN EXIST, on any path. The old Vma struct is REUSED as the surviving
// remainder -- shrunk in place, never removed -- so an insert failure restores a
// bound and frees an un-inserted piece rather than having to put back a mapping
// it already tore out. Only the exact-cover case (no remainder at all) removes
// the old VMA, and there the restoring re-insert is provably infallible: it runs
// under the same as->lock hold, into the range it just vacated (so no overlap),
// with the VMA count strictly below its value at entry (so no cap refusal).
//
// The survivor keeps its (burrow, offset) relationship EXACTLY: for any VA it
// still covers, `burrow_offset + (va - vaddr_start)` is unchanged by the split.
// That is what lets the caller uninstall only the REPLACED window's PTEs and
// leave the remainder's resident pages installed -- and it is also what makes
// the file-fault arm's post-sleep geometry check (arch/arm64/fault.c, the #190
// verify-and-bail) come out right against a concurrent split: the check passes
// exactly when the bytes read before the sleep still belong at that slot.
//
// Constraints (all rejected with -1, nothing mutated):
//   - vaddr/length page-aligned, length > 0, vaddr+length does not wrap.
//   - either no VMA covers vaddr and the range is free (shape b), or one VMA
//     covers vaddr and the range lies wholly within it (shape a).
//   - that VMA has flags == 0 and a non-NULL Burrow. A SHARED_IN VMA is another
//     Proc's memory carrying a per-span budget charge, and a COW VMA's per-page
//     share counts would have to be reasoned about across the cut; both are
//     refused rather than handled, since neither is reachable from the ldso
//     overlay this exists to serve.
//   - `nb` non-NULL; `prot` whatever vma_alloc accepts (W+X and W-without-R are
//     rejected there, so I-12 needs no separate gate here).
//
// CALLER MUST HOLD as->lock across the call, and MUST have already uninstalled
// the leaf PTEs for [vaddr, vaddr+length) -- see burrow_map_fixed_in, which is
// the wrapper that does both and is what callers outside vma.c should use.
//
// D-3c re-audit F5 [P1]: the exact-cover arm frees the REPLACED old VMA's Burrow,
// which can be a 9P-backed FILE Burrow whose free reaches a possibly-sleeping
// spoor_clunk -- and this runs under as->lock, so an inline free is the
// lock-across-sleep extinction (the identical hazard F1 deferred at the three
// teardown sites; this was the fourth). `*out_free` (non-NULL) receives the dead
// Burrow (or NULL) instead of freeing it inline; the caller frees it with
// burrow_free_deferred AFTER dropping as->lock. Written on every return path.
int vma_replace_range_in(struct AddrSpace *as, bool exempt,
                         u64 vaddr, u64 length,
                         struct Burrow *nb, u32 prot, u64 nb_offset,
                         struct Burrow **out_free);

// LINEAGE L-2: the same four operations, addressed by AddrSpace instead of by
// Proc. The Proc-taking forms above are thin wrappers over these -- they resolve
// p->as and, where a cap is involved, ask proc_resource_exempt for the policy
// verdict. Nothing else changes; the ~90 existing call sites keep their
// signatures, and only the exec load path uses these.
//
// The distinction is not cosmetic: exec must build a COMPLETE address space
// before it commits to it, so the target is DETACHED -- no Proc points at it
// yet, so there is no p->as to route through, and charging the caller's current
// (outgoing) address space would inflate a counter that is about to be freed
// while leaving the new one reading zero for the rest of the Proc's life.
//
// `exempt` is the I-32 policy verdict for whoever the address space is being
// built FOR. Passing it in rather than a Proc keeps this layer free of identity.
int         vma_insert_in(struct AddrSpace *as, bool exempt, struct Vma *v);
void        vma_remove_in(struct AddrSpace *as, struct Vma *v);
struct Vma *vma_lookup_in(struct AddrSpace *as, u64 vaddr);

// #199: lowest-addressed VMA overlapping [lo, hi), or NULL. Caller holds
// as->lock. The point probe (vma_lookup) is blind to a VMA lying strictly
// inside a range; this is the range scan the phenotype munmap row needs to
// tell "nothing mapped here" (Linux: success) from a boundary-straddling
// partial overlap (refused: partial unmap is post-v1.0).
struct Vma *vma_next_overlap_in(struct AddrSpace *as, u64 lo, u64 hi);
void        vma_drain_in(struct AddrSpace *as);

// B-1a: the permission change (ARCH 6.5 "The permission ceiling"). Move every
// page of [vaddr, vaddr+length) to `prot` (a subset of R|W -- X is never a
// target), splitting the first and last mapping where the range cuts them (at
// most two new pieces, each carrying its parent's identity: for every VA a piece
// still covers, `burrow_offset + (va - vaddr_start)` is unchanged) and changing
// every interior mapping IN PLACE. `seal` also lowers each affected mapping's
// ceiling to `prot`, irrevocably.
//
// ALL OR NOTHING. vma_reprotect_precheck_in decides every refusal before the
// first mutation, and vma_reprotect_range_in runs it again before it touches
// the list: a refusal leaves every mapping exactly as it was. Refused, as
// -T_E_*:
//   INVAL  unaligned / zero length / wrap; a prot with X, W-without-R, or
//          bits beyond R|W
//   NOMEM  a hole in the range (nothing mapped, or a gap between mappings), a
//          guard (unmapped address space that happens to be reserved), or no
//          I-32 headroom / memory for the split pieces
//   ACCES  a shared-in mapping (another Proc's memory), a CODE alias (the I-42
//          pair), a hardware mapping (MMIO / DMA / HOSTMEM), or `prot` above
//          the mapping's ceiling
// The range may span several mappings: a whole-region protect over a partially
// committed reservation is two VMAs (rw then none), and Linux serves it. After
// the change a MERGE pass coalesces each affected piece with a neighbour of the
// same Burrow, prot, flags and contiguous offset, so the grow/shrink ladders an
// engine runs over a reservation stay at two VMAs instead of one per step
// (PROC_VMA_MAX is 65536, and a 4 GiB Wasm reservation grown in 64 KiB pages
// would otherwise reach it).
//
// CALLER MUST HOLD as->lock, and MUST have already uninstalled the leaf PTEs of
// the range (the D-3b rule: hardware resolves a PTE without the lock, so a
// writable PTE must be gone before the prot that justified it is) -- see
// burrow_protect_in, the wrapper that does both, and cow.tla's
// BUGGY_PROTECT_KEEPS_PTE for what skipping it looks like.
int vma_reprotect_precheck_in(struct AddrSpace *as, u64 vaddr, u64 length,
                              u32 prot);
int vma_reprotect_range_in(struct AddrSpace *as, bool exempt,
                           u64 vaddr, u64 length, u32 prot, bool seal);

// Diagnostic accessors.
u64      vma_total_allocated(void);
u64      vma_total_freed(void);

#endif // THYLACINE_VMA_H
