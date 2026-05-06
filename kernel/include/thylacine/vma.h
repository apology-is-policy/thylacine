// Per-Proc Virtual Memory Area — P3-Da.
//
// A VMA describes a contiguous range of user virtual addresses
// `[vaddr_start, vaddr_end)` with associated permissions and a backing
// VMO. The sorted-list of VMAs anchored at `struct Proc.vmas` is
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
struct Vmo;

// VMA permission bits. Map to PTE_KERN_TEXT/RO/RW + user-bit at PTE
// installation time (P3-Db). At v1.0 P3-Da these are policy markers
// stored in the VMA; the page-fault handler consults them when
// installing PTEs.
#define VMA_PROT_READ   (1u << 0)
#define VMA_PROT_WRITE  (1u << 1)
#define VMA_PROT_EXEC   (1u << 2)

#define VMA_PROT_RW   (VMA_PROT_READ | VMA_PROT_WRITE)
#define VMA_PROT_RX   (VMA_PROT_READ | VMA_PROT_EXEC)

// VMA_MAGIC at offset 0 — SLUB freelist clobber defense (mirrors
// struct Proc / struct Thread / struct Vmo / struct Handle pattern).
#define VMA_MAGIC 0x564D413043ADEFADULL    // 'VMA0' || 0xCADE'FADE

struct Vma {
    u64 magic;            // VMA_MAGIC
    u64 vaddr_start;      // inclusive, page-aligned
    u64 vaddr_end;        // exclusive, page-aligned
    u32 prot;             // VMA_PROT_* bitmask
    u32 _pad;             // 8-byte alignment
    struct Vmo *vmo;      // backing object (refcounted)
    u64 vmo_offset;       // byte offset into VMO

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

// Bring up the VMA subsystem (allocate the SLUB cache). Must run after
// slub_init; before any vma_alloc.
void vma_init(void);

// Allocate a fresh Vma descriptor. Initializes magic + the passed
// fields; next/prev set to NULL (caller's responsibility to insert
// into the per-Proc list via vma_insert). Refcounts the VMO via
// vmo_ref. Returns NULL on OOM.
//
// Constraints:
//   - vaddr_start < vaddr_end.
//   - both page-aligned (4 KiB).
//   - vmo non-NULL.
//   - prot ∈ {0, R, RW, RX} (at v1.0 we reject W+X to mirror the W^X
//     invariant; runtime enforcement happens at PTE installation).
//
// Returns NULL on any constraint violation (without partial allocation).
struct Vma *vma_alloc(u64 vaddr_start, u64 vaddr_end, u32 prot,
                     struct Vmo *vmo, u64 vmo_offset);

// Free a Vma descriptor. Releases the VMO ref. Caller MUST have
// removed it from any per-Proc list (extincts otherwise — magic check
// + next/prev != NULL detection).
void vma_free(struct Vma *v);

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

// Walk every VMA in Proc `p`'s list and free it. Used at proc_free
// to release all VMAs (and decrement their VMOs' mapping counts).
// Caller (proc_free) calls this BEFORE handle_table_free — handle
// closure of VMO handles independently decrements vmo->handle_count.
void vma_drain(struct Proc *p);

// Diagnostic accessors.
u64      vma_total_allocated(void);
u64      vma_total_freed(void);

#endif // THYLACINE_VMA_H
