// Virtual Memory Object (BURROW) — kernel object representing a memory
// region, independent of any address space (P2-Fd / P3-Db).
//
// Per ARCHITECTURE.md §19 + specs/burrow.tla. A BURROW holds:
//   - size (page-aligned, rounded up at create)
//   - backing type (BURROW_TYPE_ANON at v1.0; PHYS at Phase 3; FILE post-v1.0)
//   - handle_count + mapping_count (the dual-refcount lifecycle)
//   - the backing pages (alloc_pages chunk; freed when both counts reach 0)
//
// State invariant pinned by specs/burrow.tla::NoUseAfterFree (TLC-checked):
//
//   pages alive iff (handle_count > 0 OR mapping_count > 0)
//
// API surface (P3-Db):
//   - burrow_create_anon / burrow_ref / burrow_unref — handle-side lifecycle.
//   - burrow_map(Proc, Burrow, vaddr, length, prot) — install a VMA in a Proc's
//     address space. Calls vma_alloc + vma_insert; mapping_count++ on
//     success. Returns 0 on success, negative errno on failure.
//   - burrow_unmap(Proc, vaddr, length) — remove the matching VMA from a
//     Proc; mapping_count--. At v1.0 the (vaddr, length) must match the
//     VMA exactly (no partial unmap; that's post-v1.0).
//   - burrow_acquire_mapping / burrow_release_mapping — the bare refcount-only
//     ops (renamed from old burrow_map/burrow_unmap). Internal to the vma
//     layer; not for general use. Public so test_burrow.c can exercise the
//     refcount lifecycle in isolation per specs/burrow.tla.
//
// At v1.0 P3-Db:
//   - BURROW_TYPE_ANON only. Eager allocation via alloc_pages(order, KP_ZERO)
//     where order = ceil_log2(page_count). Size rounded up to page_count
//     * PAGE_SIZE. Wasted memory possible for non-power-of-two page_count
//     (acceptable for tests; production-grade per-page allocation deferred).
//   - burrow_map creates a VMA via vma_alloc + vma_insert. PTE installation
//     is deferred to demand-paging via the user-mode fault path (P3-Dc);
//     burrow_map only installs the VMA.
//   - SMP-safe lifecycle (#847): handle_count + mapping_count + the
//     dual-counter free decision are serialized by a per-Burrow spin_lock.
//     A multi-threaded Proc (stratumd) can have one thread close a Burrow
//     handle while another unmaps a mapping of the same Burrow; without the
//     lock the non-atomic ++/-- torn-updated the count and the two
//     `handle_count==0 && mapping_count==0` free sites raced (double-free or
//     leak). Pulled forward as the precursor to the #844 handle-lifetime
//     pass, whose handle_put drops the Burrow ref outside the table lock.
//
// Phase 3+ refinement (P3-Dc):
//   - arch_fault_handle's user-mode dispatch path → vma_lookup → page
//     allocate → PTE install in the per-Proc TTBR0 tree.
//
// Phase 3+ extensions:
//   - BURROW_TYPE_PHYS for DMA buffers (CMA-allocated; pinned).
//   - burrow_create_physical(paddr, size) for driver pre-existing-memory.
//
// Post-v1.0:
//   - BURROW_TYPE_FILE for Stratum page cache integration.
//   - Partial unmap (burrow_unmap with sub-VMA range; splits the VMA).

#ifndef THYLACINE_VMO_H
#define THYLACINE_VMO_H

#include <thylacine/page.h>         // #194: PAGE_SIZE (burrow_file_limit_known)
#include <thylacine/spinlock.h>     // #847: per-Burrow lock (spin_lock_t)
#include <thylacine/types.h>

struct Proc;

// VMO_MAGIC — sentinel set at burrow_create_anon; checked at burrow_ref /
// burrow_unref / burrow_acquire_mapping / burrow_release_mapping. Sits at offset
// 0 so SLUB's freelist write on kmem_cache_free clobbers it; subsequent
// operation on a freed BURROW sees magic != VMO_MAGIC and extincts with a
// clear UAF diagnostic.
#define VMO_MAGIC 0x564D4F00BADC0DE5ULL    // 'BURROW\0' || 0xBADC0DE5

enum burrow_type {
    BURROW_TYPE_INVALID = 0,
    BURROW_TYPE_ANON    = 1,
    // P4-Ic1: BURROW_TYPE_MMIO — backing is a fixed physical-memory range
    // owned by a KObj_MMIO (the spec-pinned PA-exclusivity claim ticket
    // from P4-Ib). The Burrow holds a reference to the underlying
    // KObj_MMIO; `pages` is NULL (no alloc_pages backing); `pa` carries
    // the device PA. burrow_unref of a MMIO Burrow skips free_pages and
    // calls kobj_mmio_unref(kobj_mmio) to release the held reference.
    BURROW_TYPE_MMIO    = 2,
    // P4-Ic5b1b: BURROW_TYPE_DMA — backing is a kernel-allocated contiguous
    // page chunk owned by a KObj_DMA (PA-stable claim ticket from
    // P4-Ic5b1b). The Burrow holds a reference to the underlying KObj_DMA;
    // `pages` is NULL on the Burrow (the page chunk lives on the KObj_DMA
    // itself, not the Burrow); `pa` carries the buddy-chosen PA. Distinct
    // from BURROW_TYPE_ANON because:
    //   - The backing is owned by a separately-refcounted KObj_DMA (so
    //     handle_close on the user's KObj_DMA handle and burrow_unmap on
    //     the user's VMA can race; either path drops a ref on KObj_DMA
    //     and the last one frees the pages).
    //   - The PTE attrs at userland_demand_page time are Normal cacheable
    //     (matches BURROW_TYPE_ANON; distinct from BURROW_TYPE_MMIO which
    //     uses Device-nGnRnE).
    // burrow_unref of a DMA Burrow skips free_pages and calls
    // kobj_dma_unref(kobj_dma) to release the held reference.
    BURROW_TYPE_DMA     = 3,
    // REVENANT / I-36: BURROW_TYPE_FILE — backing is a `length`-byte range of
    // a FILE, demand-paged one page at a time from a kernel-pinned Spoor (the
    // executable's Chan). This is the Plan 9 Image model (docs/REVENANT.md
    // §4.1): the kernel loads a binary by mapping each PT_LOAD text segment as
    // a file-backed VMA and faulting its pages in lazily (dev->read), instead
    // of slurping the whole ELF eagerly. `pages` is NULL (no contiguous
    // alloc_pages chunk); the per-page physical pages live in the sparse
    // `filepages` array, read in on demand by the R-2 fault arm and freed at
    // burrow_free_internal. The Burrow ADOPTS one reference on the backing
    // `spoor` (pinned at exec, never re-resolved at fault — the I-30 discipline)
    // and spoor_clunks it on the last unref. Text is R+X (W^X-clean by
    // construction — never writable); writable .data is eager-copied into a
    // BURROW_TYPE_ANON segment at exec (D4), so a FILE Burrow is read-only and
    // shareable across Procs (the Image's cross-Proc text share, R-3).
    BURROW_TYPE_FILE    = 4,
    // Overcommit / I-32 (ARCH §6.5 "The overcommit model"; SYS_BURROW_ATTACH_LAZY):
    // BURROW_TYPE_ANON_LAZY — backing is a `length`-byte ANONYMOUS region whose
    // pages are demand-ZEROED one at a time on first touch. The structural twin of
    // BURROW_TYPE_FILE but SIMPLER: there is no backing file, so the fault arm
    // allocates + zero-fills + installs RW/XN ENTIRELY under vma_lock (no blocking
    // read -> no slow path / no pin / no death-interruptible read). `pages` is NULL
    // (no contiguous alloc_pages chunk); the per-page physical pages live in the
    // sparse `filepages` array (the SAME field FILE uses), each slot NULL until
    // faulted in (or after SYS_BURROW_DECOMMIT releases it). The I-32 page_count
    // charge moves to FAULT time, per page — the whole point is a free reservation,
    // so page_count tracks true committed RSS. No `spoor` (anon), no file/cache
    // fields, no kobj. burrow_free_internal frees every resident page (order 0) +
    // kfrees the array (mirrors the FILE arm, minus the spoor_clunk).
    BURROW_TYPE_ANON_LAZY = 5,
    // I-42 (CL-7k; docs/JIT-ON-WX-DESIGN.md, LLVM-DESIGN.md §8):
    // BURROW_TYPE_CODE — an anonymous, eagerly-allocated region that is the ONLY
    // backing object from which userspace may hold an EXECUTABLE mapping. Backing
    // is identical to BURROW_TYPE_ANON (one contiguous alloc_pages chunk in
    // `pages`/`order`); the type is not a different allocator, it is a different
    // ADMISSIBILITY. It exists so "this region may carry an RX alias" is a property
    // the KERNEL mints at creation under CAP_JIT — never one a caller asserts at map
    // time. That is the G-2 WEAVE discipline (a shareable DMA region is minted by
    // SYS_DMA_CREATE_WEAVE, not flagged by its creator) applied to the W^X boundary:
    // with the property carried by the object, "may I map this executable?" is
    // answered by a field the caller cannot forge, and every future mapping path
    // gets the gate for free rather than having to remember it.
    //
    // The I-42 mechanism is TWO aliases of this one physical region in ONE Proc:
    // RW at VA_w (the JIT writes) and RX at VA_x (it executes). No PTE is ever
    // W AND X — each alias is a separate VMA with a separate, fixed prot, so
    // I-12 holds at page granularity exactly as it does for any other mapping.
    // This is the Lazarus W1.5 LSE alternatives-patcher's own discipline (it
    // writes .text through a transient RW-not-X scratch alias while the canonical
    // mapping stays RO+X) turned outward for userspace.
    //
    // Emitted bytes become fetchable only after an explicit SYS_ICACHE_SYNC over
    // the range — the D-cache-clean / I-cache-invalidate sequence the architecture
    // requires between a data write and an instruction fetch of the same address.
    BURROW_TYPE_CODE    = 6,
};

struct page;
struct AddrSpace;      // <thylacine/addrspace.h> — LINEAGE L-1; burrow_lazy_populate /
                       //   burrow_map_in take one. Forward-declared so a translation
                       //   unit that includes only this header does not get the
                       //   -Wvisibility "will not be visible outside of this function"
                       //   warning (and, worse, a struct type incompatible with the
                       //   real one). Pre-existing since L-4a; noticed at L-4b.
struct KObj_MMIO;
struct KObj_DMA;
struct Spoor;          // <thylacine/spoor.h> — REVENANT: the pinned backing Chan

struct Burrow {
    u64            magic;          // VMO_MAGIC; clobbered to 0 in burrow_free_internal before kmem_cache_free (R9 F148 discipline; R13 F213)
    enum burrow_type  type;
    size_t         size;           // rounded-up to page_count * PAGE_SIZE
    size_t         page_count;
    // #847: serializes the two counts below + the dual-counter free decision
    // against concurrent ref/unref/{acquire,release}_mapping from sibling
    // threads of a multi-threaded Proc. Plain spin_lock (process-context only,
    // never from IRQ -- matches p->vma_lock). KP_ZERO at create inits it
    // unlocked (SPIN_LOCK_INIT == {0}). A leaf lock: burrow_free_internal runs
    // OUTSIDE it. Lock order (with #844): handle-table lock -> v->lock (a
    // handle_get/dup acquire is under the table lock); never the reverse.
    spin_lock_t    lock;
    int            handle_count;   // open handles to this BURROW (under lock)
    int            mapping_count;  // open mappings / vma's (under lock)
    struct page   *pages;          // alloc_pages chunk; NULL after free; NULL for MMIO
    unsigned       order;          // for free_pages; unused for MMIO
    // P4-Ic1 / P4-Ic5b1b: hw-backed-Burrow fields. For BURROW_TYPE_ANON
    // these are zero. For BURROW_TYPE_MMIO: kobj_mmio is the underlying
    // KObj_MMIO whose PA claim this Burrow holds; pa is the device PA
    // (page-aligned, matches kobj_mmio->pa). For BURROW_TYPE_DMA:
    // kobj_dma is the underlying KObj_DMA whose pinned page chunk this
    // Burrow wraps; pa is the buddy-chosen PA (matches kobj_dma->pa).
    // Exactly one of kobj_mmio / kobj_dma is non-NULL for hw types; both
    // are NULL for BURROW_TYPE_ANON. The non-NULL hw ref is released at
    // burrow_free_internal via the type-dispatched switch.
    struct KObj_MMIO *kobj_mmio;   // NULL except for BURROW_TYPE_MMIO
    struct KObj_DMA  *kobj_dma;    // NULL except for BURROW_TYPE_DMA
    u64               pa;           // 0 except for hw-backed types

    // REVENANT / I-36: BURROW_TYPE_FILE fields. Zero/NULL for every other type.
    // The Burrow ADOPTS one ref on `spoor` at burrow_create_file (the I-30 pin:
    // the backing Chan is pinned at exec, never re-resolved at fault) and
    // spoor_clunks it in burrow_free_internal on the last unref. `file_offset`
    // is the segment's base byte offset in the backing file; the segment length
    // is v->size. The cache-key scalars are the executable's file identity
    // sampled at create — the R-3 Image-cache key AND the coherence token (a
    // binary atomically replaced bumps qid.vers -> a fresh key; the running
    // Proc stays pinned to the version it exec'd). `filepages` is the sparse
    // per-page array (page_count entries, kzalloc'd at create, each slot NULL
    // until the R-2 fault arm dev->reads that page in); the slots are protected
    // by v->lock (read/install), with the blocking dev->read done OUTSIDE the
    // lock. burrow_free_internal frees every resident page (order 0) + kfrees
    // the array + clunks the spoor (free runs at {h:0,m:0} — no mapping holds
    // it, so no concurrent faulter touches filepages).
    struct Spoor     *spoor;        // NULL except FILE: the adopted+pinned backing Chan
    u64               file_offset;  // FILE: segment base byte offset in the backing file
    // #194 (I-32): bytes in the backing file, sampled ONCE when the Burrow is
    // created (image_lookup_or_create stamps it from the caller's stat; the
    // close-to-open posture makes creation-time the honest sample point). The
    // fault arm refuses to demand-page a page WHOLLY past round_up(file_limit)
    // -- Linux's SIGBUS-past-EOF -- so no anonymous-in-effect page is ever
    // minted against the uncharged FILE posture. BURROW_FILE_LIMIT_UNKNOWN
    // disables the bound (a backing Dev with no stat_native: the baked,
    // immutable ramfs -- argued at the image.c stamp site).
    u64               file_limit;   // FILE: backing file size in bytes at create
    int               file_dc;      // FILE: cache key — backing dc       (sampled at create)
    u32               file_devno;   // FILE: cache key — backing devno    (sampled at create)
    u64               file_qid_path;// FILE: cache key — backing qid.path (sampled at create)
    u32               file_qid_vers;// FILE: cache key — backing qid.vers (coherence token)
    struct page     **filepages;    // FILE / ANON_LAZY: sparse [page_count]; slot NULL until faulted in

    // #131/#132: WHO PAID the I-32 page_count for this region, and how much.
    // A Burrow's `type` tells you the region's SHAPE; it has never told you who
    // paid, and every refund site before this recorded field had to INFER the
    // payer from the shape it happened to be looking at. That inference is what
    // #122 got wrong at the detach path (refunding a SHARED_IN mapping nobody
    // paid for) and what #132 got wrong again at the two Loom refunds -- where
    // "p owns the Loom" was silently read as "p paid for the buffer", though a
    // registered buffer may be a weft ring NETD paid for (the shipped Weft-6c-1
    // path). Recording the payer makes a refund ATTRIBUTED instead of inferred,
    // so a new refund site is safe by construction rather than by remembering.
    //
    // charge_pid is the payer's pid (never a pointer: the payer can die while
    // the region lives on in a consumer, and a raw pointer would dangle);
    // 0 means unpaid or already released. Both are under `lock`, and the
    // release is a CLAIM (read-and-clear) so exactly one caller ever refunds.
    int              charge_pid;    // paying Proc's pid; 0 == unpaid / released
    u32              charge_pages;  // what it paid (buddy-rounded, the alloc's own count)

    // #131: set once by burrow_share_into -- this region has been mapped into
    // a SECOND Proc. Monotonic (a region is never un-shared in a way that
    // returns the charge to the sharer). It is the discriminator the sharer's
    // own detach needs: when the sharer unmaps and the region survives, this
    // says whether the survivor is somebody ELSE (release the charge -- the
    // sharer can no longer reach the pages) or the sharer's OWN other claim,
    // e.g. a Loom registered-buffer pin (keep it -- that claim's own drop
    // refunds). Read under `lock`.
    bool             shared_out;

    // D-3c F1: a single-linked stack for DEFERRED free. A teardown that runs
    // under as->lock cannot free a Burrow inline -- the FILE arm's spoor_clunk
    // may sleep (a 9P Tclunk), and sleeping under a plain spinlock is the
    // lock-across-sleep extinction. So the locked teardown decrements the
    // mapping ref (burrow_release_mapping_deferred), and if that was the last
    // ref it pushes the dead Burrow onto a caller-local stack via this link and
    // frees the whole chain (burrow_free_deferred) AFTER dropping as->lock.
    // Meaningful ONLY while a Burrow sits on such a stack -- {handle:0,map:0},
    // unreachable by any other path, so the write needs no lock; NULL always
    // otherwise. This is the teardown twin of #193 (which hoisted the mmap
    // success-path construction-handle unref outside the lock for the same
    // reason). Freeing inline was the pre-D-3 norm because every detachable
    // Burrow's free was non-sleeping (ANON free_pages); D-3 put a 9P-backed
    // FILE Burrow at a guest-detachable address, which is what made it live.
    struct Burrow   *deferred_free_next;
};

_Static_assert(__builtin_offsetof(struct Burrow, magic) == 0,
               "magic must be at offset 0 — SLUB freelist write on free "
               "clobbers it (use-after-free defense)");

// Bring up the BURROW subsystem. Allocates the SLUB cache. Must be called
// after slub_init; idempotent guard panics on re-call.
void burrow_init(void);

// Allocate an anonymous BURROW of `size` bytes. Size is rounded up to a
// multiple of PAGE_SIZE. Backing pages are allocated eagerly via
// alloc_pages(order, KP_ZERO) where order = ceil_log2(page_count) —
// this rounds the allocation up to a power of two of pages, possibly
// wasting some.
//
// Returns a struct Burrow * with handle_count=1, mapping_count=0,
// representing the caller's exclusive initial reference. Returns NULL
// on:
//   - size == 0
//   - SLUB OOM
//   - alloc_pages OOM
//
// The caller's handle_count=1 is "consumed" — handle_alloc on this
// BURROW does NOT increment; burrow_create_anon's count of 1 IS the count
// that the eventual handle_close will decrement.
struct Burrow *burrow_create_anon(size_t size);

// P4-Ic1: burrow_create_mmio — wrap a KObj_MMIO in a Burrow so the
// VMA + page-fault dispatch path can install device-memory PTEs
// uniformly with the anon-Burrow flow.
//
// The Burrow takes a reference on the KObj_MMIO via kobj_mmio_ref so
// the underlying PA claim survives the caller's eventual kobj_mmio_unref
// (typically the user's handle_close on their KOBJ_MMIO handle). The
// Burrow's reference is released when burrow_free_internal fires
// (handle_count + mapping_count both reach 0).
//
// Returns the Burrow with handle_count=1 (caller's construction
// reference, consumed by either burrow_map → burrow_unref transfer or
// explicit burrow_unref).
//
// Returns NULL on:
//   - NULL kobj_mmio, corrupted magic.
//   - kobj_mmio->size == 0 (defensive; kobj_mmio_create rejects this).
//   - SLUB OOM.
//
// At v1.0 P4-Ic1 the Burrow does NOT install any PTEs — that's the
// VMA layer's job at burrow_map. Demand-page integration in
// arch/arm64/fault.c (handling the MMIO PA + device-memory PTE attrs)
// lands at P4-Ic2.
struct Burrow *burrow_create_mmio(struct KObj_MMIO *kobj_mmio);

// P4-Ic5b1b: burrow_create_dma — wrap a KObj_DMA in a Burrow so the
// VMA + page-fault dispatch path can install user-VA mappings backed
// by the kernel-allocated pinned page chunk.
//
// The Burrow takes a reference on the KObj_DMA via kobj_dma_ref so the
// underlying page chunk survives the caller's eventual handle_close on
// their KOBJ_DMA handle. The Burrow's reference is released when
// burrow_free_internal fires (handle_count + mapping_count both reach 0).
//
// Returns the Burrow with handle_count=1 (caller's construction
// reference, consumed by either burrow_map → burrow_unref transfer or
// explicit burrow_unref).
//
// Returns NULL on:
//   - NULL kobj_dma, corrupted magic.
//   - kobj_dma->size == 0 (defensive; kobj_dma_create rejects).
//   - SLUB OOM.
//
// Distinct from burrow_create_mmio at the demand-page layer: DMA Burrows
// install Normal cacheable PTEs (CPU + device coherent on QEMU virt's
// VirtIO transports), MMIO Burrows install Device-nGnRnE PTEs. The
// dispatch happens in arch/arm64/fault.c::userland_demand_page.
struct Burrow *burrow_create_dma(struct KObj_DMA *kobj_dma);

// REVENANT / I-36: burrow_create_file — the file-backed demand-paged text
// Burrow (the Plan 9 Image realized as BURROW_TYPE_FILE; docs/REVENANT.md §4).
// Backs a `length`-byte segment of the file behind `spoor`, starting at byte
// `file_offset`. The Burrow ADOPTS one reference on `spoor` (transfers
// ownership, like loom_register_handles "adopt the caller's ref"):
//   - on SUCCESS the Burrow owns the ref and spoor_clunks it at
//     burrow_free_internal (the last unref);
//   - on FAILURE (NULL return) the caller RETAINS its ref (must spoor_clunk it)
//     — burrow_create_file takes NO ref on any error path.
// `spoor` should be opened OEXEC (the R-2 fault arm dev->reads it) and is
// pinned for the Burrow's life (the I-30 "pin at exec, never re-resolve at
// fault" discipline). The cache-key scalars (dc/devno/qid.{path,vers}) are
// sampled from `spoor` here — the R-3 Image-cache key + coherence token.
//
// The backing pages are NOT allocated here — they are demand-paged one at a
// time by the R-2 fault arm into the sparse `filepages` array (allocated here,
// page_count slots, all NULL). `length` is rounded up to a page multiple
// (size = page_count * PAGE_SIZE); v->pages stays NULL + v->order 0 (FILE has
// no contiguous alloc_pages chunk). handle_count starts at 1 (the construction
// reference, consumed by burrow_map -> burrow_unref transfer or explicit unref).
//
// Returns NULL (taking NO spoor ref) on:
//   - NULL spoor / corrupted spoor magic, burrow_init not run, length == 0,
//     length overflow, SLUB OOM, or filepages-array OOM.
struct Burrow *burrow_create_file(struct Spoor *spoor, u64 file_offset, size_t length);

// #194: `file_limit` sentinel + validity predicate. A limit in the top page's
// worth of u64 values would wrap the fault arm's round-up, so the predicate
// excludes the whole band -- a hostile server reporting a near-2^64 size gets
// the UNBOUNDED (pre-#194) behavior, never a wrapped false-BUS window.
#define BURROW_FILE_LIMIT_UNKNOWN ((u64)-1)
static inline bool burrow_file_limit_known(u64 lim) {
    return lim < (u64)-1 - (u64)(PAGE_SIZE - 1);
}

// Overcommit / I-32: burrow_create_anon_lazy — the demand-ZERO anonymous Burrow
// (ARCH §6.5 "The overcommit model"; SYS_BURROW_ATTACH_LAZY). Reserves a `size`-byte
// anonymous region (rounded up to whole pages) but allocates NO backing pages: each
// page faults in zero-filled on first touch (the BURROW_TYPE_ANON_LAZY arm of
// userland_demand_page), into the sparse `filepages` array allocated here
// (page_count slots, all NULL). The structural twin of burrow_create_file minus the
// backing Spoor — the simpler half (zero-fill, no read). handle_count starts at 1
// (the construction reference, consumed by burrow_map -> burrow_unref transfer or
// explicit unref); pages == NULL, order 0 (no contiguous chunk).
//
// Returns NULL on: burrow_init not run (extincts), size == 0, size overflow, SLUB
// OOM, or filepages-array OOM.
struct Burrow *burrow_create_anon_lazy(size_t size);

// I-42 / CL-7k: burrow_create_code — the dual-mappable CODE Burrow, the only
// backing object from which userspace may hold an executable mapping
// (docs/JIT-ON-WX-DESIGN.md; LLVM-DESIGN.md §8). Allocation is byte-identical to
// burrow_create_anon (one eager contiguous KP_ZERO chunk); the ONLY difference
// is `type`, which is the kernel-minted, create-immutable admissibility token
// the RX-mapping gate reads.
//
// KP_ZERO is load-bearing here, not incidental hygiene: a code page handed back
// with stale contents would be a region the Proc can EXECUTE without having
// written it. Zero-filled AArch64 decodes as UDF #0 (an always-undefined
// encoding), so an un-emitted page faults rather than running whatever the
// previous owner left behind.
//
// Callers MUST hold CAP_JIT (enforced at the syscall boundary, not here — this
// is the mechanism; kernel tests drive it directly). handle_count starts at 1
// (the construction reference), mapping_count 0, exactly as burrow_create_anon.
//
// Returns NULL on: burrow_init not run (extincts), size == 0, size overflow, or
// allocator OOM.
struct Burrow *burrow_create_code(size_t size);
// LINEAGE L-4b: clone an ANON_LAZY Burrow for a forking address space -- Plan 9's
// dupseg. The result is a SEPARATE Burrow of the same size whose filepages[] holds
// the SAME page pointers, one extra COW share taken per resident page.
//
// WHY A CLONE AND NOT A SHARED REFERENCE. A COW break has to put the private page
// somewhere, and a shared Burrow's slot is the one place it cannot go -- the other
// sharer still needs the pristine page there. Cloning gives each address space a
// slot it may overwrite; cow.h has the rest of the argument, including why that in
// turn forces the share count onto the PAGE rather than onto a slot.
//
// The clone preserves the property every other ANON_LAZY path already relies on:
// ONE Burrow, ONE mapping, ONE address space (burrow_share_into admits only ANON
// and the DMA weave, and SYS_BURROW_ATTACH_LAZY drops the construction handle, so
// no ANON_LAZY Burrow is reachable from two address spaces). That is what lets the
// break's slot swap be serialized by the faulting address space's OWN lock.
//
// NON-resident slots stay NULL in the clone, and that is correct rather than lazy:
// a slot with no page has never been written, so it reads as zero -- and each side
// demand-zeroing its own page after the fork is exactly the divergence a fork is
// supposed to produce.
//
// Takes src->lock (to snapshot the slots) and, beneath it, the global COW lock per
// resident page -- the established as->lock -> v->lock -> g_cow_lock order. The
// caller is expected to hold the source address space's lock, which is what keeps a
// peer thread's fault from filling a slot midway through the snapshot.
//
// Returns NULL on: a non-ANON_LAZY Burrow, a malformed one, or OOM (the struct or
// the filepages array), having taken no shares and left `src` untouched.
struct Burrow *burrow_clone_cow(struct Burrow *src);

// LINEAGE L-4b: swap a private page into one ANON_LAZY slot, replacing `expect`.
// The COW break's commit step -- separated out so burrow.c keeps its monopoly on
// filepages[] and on the v->lock discipline.
//
// Returns true when the slot still held `expect` and now holds `replacement`.
// Returns false if the slot has changed (unreachable at v1.0 -- see the ONE
// Burrow, ONE address space property above -- so the caller may treat it as a
// bail rather than a retry).
//
// Does NOT touch either page's share count: the caller establishes the new page's
// count BEFORE the swap (it must be set before the page is reachable) and releases
// the old page's AFTER it (the retained share is the pin that keeps a concurrent
// exit from freeing the page mid-copy -- cow.tla::BUGGY_TEARDOWN_NO_PIN).
bool burrow_lazy_swap_slot(struct Burrow *v, size_t slot,
                           struct page *expect, struct page *replacement);

// LINEAGE L-4a: make slots [first, first+n) of an ANON_LAZY Burrow resident up
// front, instead of leaving them to the demand-zero fault arm. exec is the only
// caller: it has bytes to put in these pages (a segment's `filesz`, or the argv/
// auxv frame), so faulting them in and then writing them would allocate the same
// pages one syscall later for no gain.
//
// Charges the I-32 page axis against `as` -- the SAME per-page charge the fault
// arm makes, taken here because these pages are resident for the same reason and
// `page_count == true RSS` (ARCH §6.5) must hold either way. Charged ONCE for the
// whole run so the cap decision sees the entire request; a run that would straddle
// PROC_PAGE_MAX is refused whole rather than half-populated. Takes as->lock
// (addrspace_charge_pages' stated precondition) and v->lock, in that order; the
// buddy allocator is entered under NEITHER (the leaf-lock discipline burrow.c
// already keeps for free_pages).
//
// ALL-OR-NOTHING: on any failure every page this call installed is freed and the
// whole charge returned, leaving the Burrow exactly as it was found -- so a caller
// that gives up can simply burrow_unref. Returns 0, or -1 on: a non-ANON_LAZY
// Burrow, a range outside page_count, an alloc_pages shortfall, an over-cap charge,
// or a slot in the run that was ALREADY resident (a caller bug -- exec populates
// each run exactly once; failing closed beats silently miscounting the charge).
int burrow_lazy_populate(struct AddrSpace *as, bool exempt, struct Burrow *v,
                         size_t first, size_t n);

// LINEAGE L-4a: the kernel direct-map address of one resident ANON_LAZY slot, or
// NULL if the slot is not resident (or the Burrow is the wrong type / the slot is
// out of range).
//
// PRECONDITION -- the Burrow must be PRIVATE to the caller: not yet mapped into any
// address space, and not yet reachable from a second thread. That is exec's
// situation and nothing else's (it creates the Burrow, populates it, fills it, and
// only then burrow_map_ins it), which is what makes the returned raw pointer safe
// without a lifetime guard: no concurrent decommit or teardown can free the page
// under the caller. Do NOT reach for this from a path where the Burrow is live.
void *burrow_lazy_slot_kva(struct Burrow *v, size_t slot);

// Increment handle_count. Maps to spec's HandleOpen action. Called by
// handle_dup (and Phase 4's handle_transfer_via_9p) for KOBJ_BURROW
// handles.
//
// Extincts on NULL or corrupted magic (UAF defense).
void burrow_ref(struct Burrow *v);

// Decrement handle_count. If both counts reach 0, free pages and the
// struct. Maps to spec's HandleClose action. Called by handle_close
// for KOBJ_BURROW handles.
//
// Extincts on corrupted magic or zero-ref unref. NULL is a safe no-op.
//
// IMPORTANT: after the last unref that triggers free, the v pointer
// is INVALID — the SLUB freelist clobbers magic and the memory may be
// reused. Callers must not dereference v after the unref that brings
// both counts to 0.
void burrow_unref(struct Burrow *v);

// Increment mapping_count (refcount-only — does NOT install a VMA or
// PTEs). Maps to spec's MapVmo action. Internal helper used by the VMA
// layer (vma_alloc) and by tests that exercise the refcount lifecycle in
// isolation. Public callers should use burrow_map(Proc, ...) below.
//
// Extincts on NULL, corrupted magic, or BURROW whose pages have been
// freed (this is impossible if the caller holds a handle — the handle
// keeps handle_count > 0, which keeps pages alive).
//
// Was named `burrow_map` pre-P3-Db; renamed when the high-level
// burrow_map(Proc*, ...) entry point arrived.
void burrow_acquire_mapping(struct Burrow *v);

// Decrement mapping_count (refcount-only — does NOT remove any VMA).
// If both counts reach 0, free pages and the struct. Maps to spec's
// UnmapVmo action. Internal helper used by the VMA layer (vma_free).
// Public callers should use burrow_unmap(Proc, ...) below.
//
// Extincts on NULL, corrupted magic, or zero-mapping unmap.
//
// IMPORTANT: same pointer-invalidation caveat as burrow_unref.
//
// Was named `burrow_unmap` pre-P3-Db.
void burrow_release_mapping(struct Burrow *v);

// #130: the ref-drop primitives, reporting whether THIS drop freed the pages.
//
// I-32 charges OCCUPANCY, so the uncharge belongs to the drop that ENDS the
// occupancy. Which drop that is cannot be predicted from the VMA or the Burrow
// type: a Loom ring, a Loom registered buffer, and a Weft share each hold a
// handle_count ref that can outlive the mapping, so "the VMA went away" and
// "the pages went away" are different events. Each charger pairs its uncharge
// to the bool rather than inferring the event from a type or from a
// handle_count sampled BEFORE the drop (which is both racy and, on the normal
// Loom teardown order, simply wrong -- the #130 P1).
//
// The void-returning names above stay as wrappers for the many callers that
// have no charge to settle.
bool burrow_unref_freed(struct Burrow *v);
bool burrow_release_mapping_freed(struct Burrow *v);

// D-3c F1: the DEFERRED teardown pair. `_deferred` drops the mapping ref under
// v->lock and returns the dead Burrow (or NULL) WITHOUT freeing; the caller,
// once it has dropped as->lock, frees it with burrow_free_deferred. Together
// they keep the FILE arm's sleeping spoor_clunk off the locked teardown path
// (the twin of #193). See the deferred_free_next field + the burrow.c bodies.
struct Burrow *burrow_release_mapping_deferred(struct Burrow *v);
void           burrow_free_deferred(struct Burrow *v);

// #131/#132: the I-32 charge record -- WHO paid, so a refund is attributed
// instead of inferred from the region's shape (see struct Burrow above).
//
// burrow_charge_record: stamp the payer at the eager charge. Called by each
// site that succeeds a proc_page_charge for a whole region (SYS_BURROW_ATTACH,
// SYS_JIT_CREATE, SYS_LOOM_SETUP's ring).
//
// burrow_charge_claim: read-and-CLEAR p's charge record, returning the pages it
// paid (0 if p is not the recorded payer, or the charge was already claimed).
// The clear is what makes a refund exactly-once: two paths racing to settle the
// same region cannot both win, so the counter can never be refunded twice --
// the direction that would inflate a Proc's effective budget.
//
// burrow_charge_restore: put back a claim the caller decided NOT to settle.
// Callers claim BEFORE the drop (the record dies with the Burrow, so it cannot
// be read after) and restore when the drop turns out not to end the payer's
// involvement. A concurrent settler that saw the momentarily-cleared record
// simply skips -- so the failure mode of that window is a charge that outlives
// its region until the payer's next release point (benign: an over-charge on
// the payer, never a refund to a Proc that did not pay).
void burrow_charge_record(struct Burrow *v, const struct Proc *p, u32 pages);
u32  burrow_charge_claim(struct Burrow *v, const struct Proc *p);
void burrow_charge_restore(struct Burrow *v, const struct Proc *p, u32 pages);

// burrow_is_shared_out: has this region been mapped into a SECOND Proc?
// The discriminator the sharer's own detach needs -- see the field comment on
// struct Burrow. Monotonic once set, so a read is never stale in the direction
// that matters (false -> true only ever ADDS a reason to release).
bool burrow_is_shared_out(const struct Burrow *v);

// =============================================================================
// P3-Db: high-level map / unmap into a Proc's address space.
// =============================================================================

// burrow_map: install a VMA backed by `v` into Proc `p`'s address space at
// user-VA range [vaddr, vaddr + length). Allocates a Vma via vma_alloc
// (which takes burrow_acquire_mapping; mapping_count++) and inserts it via
// vma_insert (which rejects overlap with existing VMAs).
//
// Does NOT install PTEs — the per-Proc TTBR0 tree starts empty (every L0
// entry invalid) and pages are populated on demand by the user-mode page-
// fault handler (P3-Dc).
//
// Returns:
//   0  on success.
//   -1 on any failure: invalid argument (zero-length, unaligned vaddr/
//      length, NULL inputs, W+X prot), VMA SLUB OOM, or VMA overlap with
//      existing entry. On failure, no VMA is installed and mapping_count
//      is unchanged.
//
// Constraints (validated by vma_alloc):
//   - p, v non-NULL.
//   - length > 0.
//   - vaddr and (vaddr + length) page-aligned.
//   - prot ∈ {0, R, RW, RX} (W+X rejected per ARCH §28 I-12).
//
// At v1.0 P3-Db, burrow_offset is implicitly 0 (the VMA covers the head of
// the BURROW). Phase 5+ extends with an explicit offset for shared VMOs.
//
// PRECONDITION (#713 / RW-1 C-F2): the caller MUST hold `p->vma_lock` -- this
// is a `p->vmas` mutator (via vma_insert), and every vmas mutator + the
// demand-page reader serialize on vma_lock for multi-thread-Proc SMP safety.
// Lock order: vma_lock -> buddy zone->lock. (exec_setup is exempt -- single-
// threaded by construction.)
int burrow_map(struct Proc *p, struct Burrow *v, u64 vaddr, size_t length, u32 prot);

// LINEAGE L-2: map into an explicit address space (see vma.h's *_in block for
// why exec needs a detached target). burrow_map is the wrapper; this is the
// body. `exempt` is the I-32 policy verdict for whoever the address space is
// being built for.
struct AddrSpace;
int burrow_map_in(struct AddrSpace *as, bool exempt, struct Burrow *v,
                  u64 vaddr, size_t length, u32 prot);

// DISTRO D-3b: burrow_map_fixed -- the MAP_FIXED analog. Install `v` at the
// CALLER'S address rather than wherever a gap search lands it. Two shapes: a
// window wholly inside one existing VMA (split that VMA around it, the survivor
// keeping its backing, its prot, and -- exactly -- its byte identity for every
// VA it still covers), or an entirely free range (plain insert).
//
// Two things burrow_map cannot express, both needed by musl's map_library
// overlay: a caller-CHOSEN address that replaces existing mapping, and a
// non-zero burrow_offset for the piece left over on the right of the cut.
//
// Refuses (returning -1, having mutated no VMA) when: the window neither fits
// inside one VMA nor lies free (it spans two, or partially overlaps one), the
// covering VMA carries any flag (SHARED_IN is another Proc's memory; COW would
// need its per-page share counts reasoned across the cut), or the I-32
// VMA-count headroom for the split is absent. See vma.h's vma_replace_range_in
// for the full contract and the hole-free argument.
//
// Clears the leaf PTEs for the replaced window before the surgery -- necessarily
// so, since hardware resolves a PTE without taking as->lock. Caller holds
// as->lock, exactly as for burrow_map.
int burrow_map_fixed(struct Proc *p, struct Burrow *v, u64 vaddr, size_t length,
                     u32 prot, u64 burrow_offset);
int burrow_map_fixed_in(struct AddrSpace *as, bool exempt, struct Burrow *v,
                        u64 vaddr, size_t length, u32 prot, u64 burrow_offset);

// burrow_unmap: remove the VMA at user-VA range [vaddr, vaddr + length)
// from Proc `p`. Calls vma_remove + vma_free (which calls
// burrow_release_mapping; mapping_count--).
//
// At v1.0 P3-Db, the (vaddr, length) range must match an existing VMA
// EXACTLY — partial unmap (splitting a VMA into two halves with a hole)
// is post-v1.0.
//
// Returns:
//   0  on success.
//   -1 on no matching VMA (no VMA starts at `vaddr` with the requested
//      length).
//
// DOES tear down PTEs (RW-1 C-F6 doc-fold; the p6 hardening #2 / F1 fix):
// burrow_unmap calls mmu_uninstall_user_range over the VMA's range, clearing
// the leaf PTEs + broadcasting `tlbi vaae1is` BEFORE the backing pages are
// freed -- without it, stale PTEs/TLB entries would persist after detach (the
// suspected AEGIS-256/mallocng corruption class). Idempotent on never-faulted-
// in pages.
//
// PRECONDITION (#713 / RW-1 C-F2): the caller MUST hold `p->vma_lock` (a
// `p->vmas` mutator; same discipline as burrow_map).
int burrow_unmap(struct Proc *p, u64 vaddr, size_t length);

// burrow_unmap, additionally reporting whether removing this mapping was the
// drop that freed the Burrow's pages (see burrow_unref_freed above). *out_freed
// is written on every path, including the -1 ones (false).
// D-3c F1: `out_free`, when non-NULL, DEFERS the Burrow free: the function
// removes the VMA + drops the mapping ref under the caller's as->lock but
// returns the dead Burrow (or NULL) via *out_free instead of freeing it inline;
// the caller frees it with burrow_free_deferred after the unlock (the FILE arm's
// spoor_clunk may sleep). NULL keeps the inline free (non-sleeping ANON/CODE/DMA
// callers via burrow_unmap). *out_free is written whenever it is non-NULL,
// including the -1 paths (NULL).
int burrow_unmap_reporting(struct Proc *p, u64 vaddr, size_t length,
                           bool *out_freed, struct Burrow **out_free);

// =============================================================================
// Overcommit / I-32: lazy-anon decommit + resident-page accounting (ARCH §6.5).
// =============================================================================

// burrow_decommit: release the resident pages backing [vaddr, vaddr+length) of a
// BURROW_TYPE_ANON_LAZY mapping WITHOUT removing the VMA — the
// madvise(MADV_DONTNEED) analog (SYS_BURROW_DECOMMIT). For each page in range: if
// resident, clear the leaf PTE (+ broadcast TLBI BEFORE the page is freed to the
// buddy — the burrow_unmap / §"MMU user-PTE clear + TLBI" discipline), free the
// page, NULL its sparse slot, and uncharge the I-32 page_count. The VMA + the
// reservation stay; a later touch re-faults a fresh zero page. Idempotent on
// never-faulted (already-NULL) slots. The range must fall WITHIN a single
// BURROW_TYPE_ANON_LAZY VMA; any other type (ANON/FILE/MMIO/DMA) or a range
// spanning / outside a VMA is rejected (-1, no-op).
//
// Returns 0 on success (>= 0 pages released), -1 on a bad range / wrong VMA type.
//
// PRECONDITION: the caller MUST hold p->vma_lock (a p->vmas reader + a filepages
// mutator — the #713 discipline, like burrow_unmap). Decommit relies on vma_lock
// excluding concurrent faulters from filepages (the single-mapping invariant), so
// the sparse-slot read/NULL is safe; free_pages runs under vma_lock (the
// established attach/unmap order), never under v->lock. Lock order vma_lock ->
// v->lock for the per-slot read/NULL.
int burrow_decommit(struct Proc *p, u64 vaddr, size_t length);

// burrow_lazy_resident_count: the number of resident (faulted-in, not-decommitted)
// pages in a BURROW_TYPE_ANON_LAZY Burrow — the count SYS_BURROW_DETACH uncharges
// from page_count (a lazy region charged page_count per FAULT, so a full detach
// uncharges only the resident pages, not the whole reservation). Counts non-NULL
// filepages slots under v->lock (lock order vma_lock -> v->lock; the caller holds
// vma_lock). Returns 0 for a NULL / corrupted / non-ANON_LAZY Burrow.
u32 burrow_lazy_resident_count(struct Burrow *v);

// #106: burrow_backing_pages — the number of physical pages an EAGER Burrow of
// `size` bytes actually OCCUPIES, i.e. the buddy's power-of-two rounding rather
// than the page-rounded request. THIS is the I-32 charge unit for every eager
// anon/code region; size / PAGE_SIZE understates it by up to 2x, which is
// exactly enough for a Proc to hold twice its page budget.
//
// Pairs both ways: a charge site computes it from the requested length before
// creating the Burrow (so an over-budget Proc never reaches the allocator), and
// the matching uncharge recomputes it from the VMA's length -- the same input,
// hence the same answer. Returns 0 for size 0 and for a size whose page
// round-up would wrap (the inputs on which burrow_create_anon /
// burrow_create_code refuse to produce a Burrow at all). Full rationale,
// including why the underlying waste is deliberate, at the definition.
size_t burrow_backing_pages(size_t size);

// =============================================================================
// Weft-2 / I-37: cross-Proc Burrow share (the per-flow dataplane ring).
// =============================================================================

// burrow_share_into: map the WHOLE of an existing ANON Burrow `v` into a
// SECOND Proc `dst`'s address space at `vaddr`, establishing the cross-Proc
// share the Weft capability dataplane (ARCH §28 I-37; docs/NET-THROUGHPUT.md
// §6) builds the per-flow guest<->netd ring on. The tree's FIRST path that
// makes one Burrow reachable from two Procs: the mapping_count ref taken here
// keeps `v` alive for `dst` independent of the other Proc's refs (the #847
// dual-refcount, now cross-Proc). NO Burrow handle crosses Procs -- `dst`
// holds only a mapping (grant-is-the-share; the capability is holding the
// namespace-gated flow fid, I-1/I-28).
//
// Length is the whole Burrow (v->size) -- a share is always whole-ring, so the
// caller passes no length (unlike burrow_map). ANON only (cross-Proc MMIO/DMA
// mapping is out of scope; I-5 analysis owed). prot is RW (W+X rejected by
// vma_alloc, I-12).
//
// Returns 0 on success; -1 on NULL inputs, a corrupted/non-ANON `v`, W+X prot,
// VMA overlap, or SLUB OOM. On failure no mapping is installed and v's
// refcount is unchanged.
//
// PRECONDITIONS (see kernel/burrow.c for the cross-Proc #847 proof + lock
// order): the caller MUST hold `dst->vma_lock` (a dst->vmas mutator, the same
// #713 / RW-1 C-F2 discipline as burrow_map) AND MUST guarantee `v` stays live
// across the call (a held ref, or a higher-level lock excluding a concurrent
// teardown to {h:0,m:0} -- the Weft-6 caller serializes the data-fid open
// against flow teardown).
int burrow_share_into(struct Proc *dst, struct Burrow *v, u64 vaddr, u32 prot);

// Diagnostic accessors. Safe to call on a non-NULL, non-freed BURROW.
// Behavior on a freed BURROW is UB (the magic check would catch a
// straight post-free deref, but a coincidental magic re-use after
// SLUB recycle is theoretically possible).
//
// #130-R2 F3/F4: the two counts are SNAPSHOTS. They do not take v->lock (a
// caller may hold it), so an ACQUIRE load is all they promise: the value was
// true at some instant. "Did this drop free the pages?" is answered by
// burrow_unref_freed / burrow_release_mapping_freed / burrow_unmap_reporting,
// which decide it under the lock and REPORT it -- never by reading these first.
// Branching a lifecycle decision on a snapshot is legitimate ONLY with an
// external lock that makes it stable AND a ref-discipline argument for why no
// untracked party can bump it; image.c's eviction has both (see burrow.c).
size_t burrow_get_size(const struct Burrow *v);
int    burrow_handle_count(const struct Burrow *v);
int    burrow_mapping_count(const struct Burrow *v);

// Cumulative diagnostic counters. Tests use these to verify lifecycle
// transitions:
//   - burrow_total_created increments on every successful burrow_create_anon.
//   - burrow_total_destroyed increments on every actual page-free.
//   - At any state, (created - destroyed) == live BURROW count.
u64 burrow_total_created(void);
u64 burrow_total_destroyed(void);

#ifdef KERNEL_TESTS
// REVENANT R-1 test hooks — exercise the FILE-Burrow lifecycle (the
// resident-page free) in isolation, before the R-2 fault arm exists to
// populate slots. Returns the FILE Burrow's page_count; extincts on a
// non-FILE Burrow.
size_t burrow_file_page_count_for_test(const struct Burrow *v);
// Install a freshly alloc_pages(0, KP_ZERO) page into filepages[idx], so the
// burrow_free_internal FILE arm has a resident page to free. Extincts on a
// non-FILE Burrow, out-of-range idx, an already-resident slot, or page OOM.
void   burrow_file_install_page_for_test(struct Burrow *v, size_t idx);
// REVENANT R-2: the page resident in filepages[idx] (NULL if not faulted in or
// out of range) -- lets the demand-page tests verify the PTE target + content.
struct page *burrow_file_slot_for_test(const struct Burrow *v, size_t idx);
// Overcommit: the page resident in an ANON_LAZY Burrow's filepages[idx] (NULL if
// not faulted in / decommitted / out of range) -- lets the lazy-fault tests verify
// the demand-zero install + the decommit release. Extincts on a non-ANON_LAZY Burrow.
struct page *burrow_lazy_slot_for_test(const struct Burrow *v, size_t idx);
#endif

#endif // THYLACINE_VMO_H
