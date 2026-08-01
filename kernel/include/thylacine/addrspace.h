// kernel/include/thylacine/addrspace.h -- struct AddrSpace: the refcounted
// address space (LINEAGE L-1; docs/LINEAGE.md section 5.1; ARCH section 28 I-44).
//
// WHY THIS OBJECT EXISTS
//
// Until L-1 the address space was six inline fields of struct Proc, which made
// "two Procs, one address space" unrepresentable -- and that is precisely what
// rfork(RFPROC|RFMEM) (L-3) and copy-on-write fork (L-4/L-5) both need. The two
// features block on the SAME extraction, which is why it is the arc's stage 0
// rather than a COW implementation detail.
//
// At L-1 nothing shares: `ref` is 1 for every AddrSpace in the tree, allocated
// by proc_alloc and dropped by proc_free. The refcount is written through
// __atomic_* from the start anyway -- an int that is "always 1 today" and
// becomes contended at L-3 is exactly the latent-P1 shape CLAUDE.md's
// multi-thread-shared-state rule warns about, and the cost of getting it right
// now is one line.
//
// WHICH FIELDS LIVE HERE, AND WHY EACH ONE
//
// The membership test is "does this describe the TRANSLATION, or the PROCESS?"
//
//   pgtable_root      the L0 table itself -- the definition of the address space
//   context_id        the rolling ASID (I-31) names a TRANSLATION TABLE, which is
//                     what the allocator always semantically meant. Two Procs
//                     sharing an AS share one ASID: correct, and cheaper than
//                     two. The inverse (two tables, one ASID) is the I-31
//                     corruption the ASID arc exists to prevent, and holding the
//                     field here makes that structurally unrepresentable.
//   vmas              the mapping list -- shared by definition
//   lock              was Proc.vma_lock; serializes the shared VMA list, so it
//                     must be the AS's lock, not any one sharer's
//   page_count        the I-32 RSS axis. Sharing an AS means sharing the pages,
//                     so one charge is the honest count; the per-Proc cap
//                     becomes a per-AS cap. A fork bomb is still bounded -- N
//                     children means N address spaces, each capped.
//   vma_count         the I-32 VMA-slab axis, same argument
//   shared_map_pages  the I-32 cross-Proc shared-in axis (G-2). vma.h states the
//                     invariant as `shared_map_pages == sum of pages of
//                     SHARED_IN VMAs`, and the uncharge runs off the VMA list --
//                     which lives here. Two Procs sharing an AS would otherwise
//                     keep divergent counts for one VMA set, and the uncharge
//                     would not know whose counter to decrement.
//
// Deliberately NOT here: bounce_bytes (CF-3 syscall staging, a per-caller
// budget), and everything else in struct Proc -- identity, handles, territory,
// notes, the process tree. Those describe the PROCESS.
//
// `as == NULL` MEANS KERNEL-ONLY
//
// kproc is built by proc_init via a direct KP_ZERO kmem_cache_alloc and never
// calls proc_alloc, so its `as` stays NULL for free -- exactly the equivalence
// the old `pgtable_root == 0` test encoded, now held in one pointer. A NULL
// deref on a kernel-only path is a LOUD failure, which is the intent: it is the
// opposite of the silent-miss class, where a half-converted predicate would
// quietly treat a live user address space as a kernel Proc.
//
// Note the mmu API is unaffected and deliberately so: mmu_install_user_pte and
// friends keep taking a bare `paddr_t pgtable_root`, because the MMU layer has
// no business knowing what a Proc is. Their own `pgtable_root == 0` rejects are
// parameter validation, not the kernel-Proc test, and do not convert.

#ifndef THYLACINE_ADDRSPACE_H
#define THYLACINE_ADDRSPACE_H

#include <thylacine/page.h>      // paddr_t
#include <thylacine/spinlock.h>
#include <thylacine/types.h>

struct Vma;

struct AddrSpace {
    // Procs sharing this address space. 1 at L-1 (nothing shares yet); becomes
    // genuinely contended at L-3 (RFMEM) and L-5 (fork). Accessed via
    // __atomic_* with ACQ_REL on the drop, matching the Spoor/SrvConn
    // discipline -- the last release must happen-after every other sharer's
    // final access.
    int            ref;

    // Was Proc.vma_lock (#713). Serializes every `vmas` mutation and the
    // demand-page reader, and guards the three I-32 counters below so they stay
    // EXACT. Lock order is unchanged by the extraction: as->lock -> burrow
    // v->lock -> buddy zone->lock.
    spin_lock_t    lock;

    // PA of the L0 translation table for TTBR0 (user half). Created by
    // addrspace_alloc, destroyed by the last addrspace_unref.
    paddr_t        pgtable_root;

    // The rolling-ASID context (generation | hardware ASID) -- RW-1 B-F1, ARCH
    // section 6.2.1. 0 == "never assigned" (always misses); stamped at the first
    // context switch by asid_resolve, never freed at teardown (rollover reclaims
    // the whole space at once). Accessed via __atomic_* -- the fast path is
    // lockless.
    u64            context_id;

    // Sorted-by-vaddr_start VMA list: the address-space description against
    // which page faults are dispatched.
    struct Vma    *vmas;

    // I-32 resource axes. All three are charged/uncharged under `lock` so they
    // are exact, and stored with __atomic_store_n so lockless /proc readers get
    // a coherent snapshot.
    u32            page_count;         // live anonymous pages (true committed RSS)
    u32            vma_count;          // VMA-slab bound
    u32            shared_map_pages;   // pages shared IN from other Procs (G-2)

    u32            _pad;               // explicit tail padding; struct is 48 B
};

_Static_assert(sizeof(struct AddrSpace) == 48,
               "AddrSpace is 48 bytes: ref+lock (8) + pgtable_root (8) + "
               "context_id (8) + vmas (8) + the three I-32 u32 axes + pad (16). "
               "Growth is fine -- this assert is a drift alarm, not an ABI.");

// Allocate an address space with a fresh, empty L0 table. Returns NULL on OOM
// (either the struct or the page table), having freed whatever it did get --
// callers roll back exactly as they did for the old proc_pgtable_create.
// ref starts at 1, held by the allocating Proc.
struct AddrSpace *addrspace_alloc(void);

// Take a reference. L-3 (RFMEM) is the first caller with a real second sharer;
// at L-1 this exists so the lifecycle is complete and testable.
void addrspace_ref(struct AddrSpace *as);

// Drop a reference; the last one destroys the page table and frees the struct.
// NULL-safe (a kernel-only Proc, or a rollback before addrspace_alloc ran).
//
// PRECONDITION on the last drop: the VMA list is already drained and no CPU
// holds this table in TTBR0. proc_free establishes both -- vma_drain runs
// earlier in the teardown, and every thread was reaped and on_cpu-spun before
// proc_free is reached. No TLB flush is needed here (the Linux model): the leaf
// mappings were invalidated by vma_drain's all-ASID `tlbi vaae1is`, and any
// eventual reuse of the ASID value is gated by the rollover's per-CPU
// flush_pending local flush.
void addrspace_unref(struct AddrSpace *as);

#endif // THYLACINE_ADDRSPACE_H
