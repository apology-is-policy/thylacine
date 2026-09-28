// B-1a' (ARCH 6.5 "Capacity"): the pagemap -- a Burrow's sparse per-page slot
// table, charged and on-touch.
//
// A FILE or ANON_LAZY Burrow reserves address space and fills it one page at a
// time; the per-page pointers that say which slots hold a page used to be a
// flat `struct page *[page_count]` allocated whole at create -- 8 bytes per
// reserved page of UNCHARGED kernel memory, which is what pinned every
// reservation to a cap (BURROW_RESERVE_MAX) and left the array's size as a
// per-Proc kernel-memory DoS bounded only by graceful OOM. The pagemap replaces
// it with the Linux page-table-radix shape: a 512-ary tree of 4 KiB nodes,
// allocated only when a slot beneath them is first installed, freed again when
// the last slot beneath them is taken, and CHARGED to the address space that
// caused them when the caller names one. An untouched reservation costs one
// small struct, however large it is, so the reservation caps lift to the
// address-space window and the budget.
//
// Two representations, chosen by size at init and never changed:
//   - count <= PAGEMAP_INLINE_MAX: an INLINE leaf, `count` pointers from the
//     slab (<= 256 bytes), allocated at init, uncharged. Bounded per address
//     space by PROC_VMA_MAX x 256 B (16 MiB), which is the same DoS floor the
//     Vma slab itself stands on. This keeps mallocng's many small groups as
//     cheap as they were.
//   - otherwise: a radix of `depth` levels (1..4; 512^4 slots covers 256 TiB,
//     more than the whole burrow window). Every node is one page, `count` at
//     depth 1 rounding up to a full node. The ROOT is a node like any other:
//     absent until the first install.
//
// Node occupancy lives in the node page's own descriptor, `page->refcount`
// (ESTABLISHED at 0 when the node is allocated -- never inherited from the
// buddy, exactly the cow_share rule in page.h), so a take knows in O(depth)
// whether it emptied a node, without scanning 512 entries.
//
// LOCKING. The pagemap is embedded in the Burrow and guarded by the Burrow's
// v->lock, which the caller passes to every slot operation. The rule the rest of
// burrow.c keeps -- the buddy is NEVER entered under v->lock -- holds here too:
// an install allocates the missing nodes outside the lock (a retry loop, in case
// a racer installed some of them first) and a take hands the nodes it emptied
// back to the caller, who frees them after unlocking. The only allocation under
// no lock at all is pagemap_init's inline leaf.
//
// CHARGING. Metadata is charged where the data is: an install with a non-NULL
// `as` charges the nodes it allocates to that address space (refusing, and
// allocating nothing, when the cap is hit), and a take with a non-NULL `as`
// uncharges the nodes it freed. The DATA page is the caller's charge in both
// directions -- the pagemap never charges or uncharges a slot's page. FILE
// Burrows pass NULL (their pages are the Image cache's, shared by every space
// that maps the file, bounded by EXEC_FILE_MAX); ANON_LAZY Burrows pass the
// mapping address space. So page_count reads as data pages plus the nodes
// that index them (plus the page tables, since audit F1) -- dense, one node
// per 512 pages plus the root; sparse, up to `depth` nodes per touched page
// (a 4 GiB map touched every 512 MiB holds 8 pages under 13 nodes). Every
// node page is a USER page whatever `as` says: allocated by alloc_user_pages
// under `exempt`'s policy, so the machine-wide pool counts it and refuses a
// non-exempt caller at exhaustion, and free_pages returns it.

#ifndef THYLACINE_PAGEMAP_H
#define THYLACINE_PAGEMAP_H

#include <thylacine/spinlock.h>
#include <thylacine/types.h>

struct page;
struct AddrSpace;

#define PAGEMAP_INLINE_MAX    32u        // slots served by an inline slab leaf
#define PAGEMAP_NODE_SHIFT    9
#define PAGEMAP_NODE_ENTRIES  (1u << PAGEMAP_NODE_SHIFT)   // 512 pointers per 4 KiB node
#define PAGEMAP_MAX_DEPTH     4u         // 512^4 slots: 256 TiB of pages

struct pagemap {
    size_t   count;      // slots; 0 == destroyed or never initialized (not live)
    u32      depth;      // 0 = inline leaf; 1..PAGEMAP_MAX_DEPTH = radix levels
    u32      nodes;      // node pages currently allocated (radix only)
    size_t   resident;   // non-NULL slots
    void    *root;       // inline: struct page *[count]; radix: the root node or NULL
};

// Set up an empty map for `count` slots. The inline representation allocates
// its leaf here (KP_ZERO); the radix allocates nothing. Returns -1 on count 0,
// a count beyond PAGEMAP_MAX_DEPTH levels, or slab OOM.
int  pagemap_init(struct pagemap *pm, size_t count);

// True between a successful init and destroy. The Burrow's liveness test for
// the two sparse types (the old `filepages != NULL`).
static inline bool pagemap_live(const struct pagemap *pm) { return pm->count != 0; }

// The page in slot `idx`, or NULL when absent (never installed, taken, or out
// of range). Caller holds the lock the slot operations use. No allocation.
struct page *pagemap_get(const struct pagemap *pm, size_t idx);

// The number of non-NULL slots, O(1). Same lock discipline as pagemap_get.
static inline size_t pagemap_resident(const struct pagemap *pm) { return pm->resident; }

// Node pages currently allocated. Read under the same lock as the slots (or
// with the map otherwise quiesced -- the clone's case).
static inline u32 pagemap_node_count(const struct pagemap *pm) { return pm->nodes; }

// INSTALL-ONCE `pg` into slot `idx`:
//   0   installed; `pg` is the slot's page now (the map owns it)
//   1   a page was already there: *out_winner names it, `pg` is untouched and
//       still the caller's to free
//  -1   out of range, or the path's nodes could not be allocated (buddy OOM,
//       or the charge refused when `as` is non-NULL): nothing changed
// Takes `lock` for the slot walk; allocates missing nodes OUTSIDE it, charging
// them to `as` (if non-NULL, under `exempt`'s policy) BEFORE allocating, so a
// cap hit allocates nothing. Loops if a racer filled some of the path first;
// nodes it allocated but did not need are freed and uncharged again.
int  pagemap_install(struct pagemap *pm, spin_lock_t *lock, size_t idx,
                     struct page *pg, struct AddrSpace *as, bool exempt,
                     struct page **out_winner);

// Take slot `idx`'s page out (*out_pg, or NULL if it was absent) and NULL the
// slot. A node emptied by the take is unlinked and handed back in `freed[]`
// (capacity PAGEMAP_MAX_DEPTH; *nfreed says how many), for the caller to
// free_pages(.., 0) AFTER dropping the lock; the nodes are uncharged from `as`
// here (if non-NULL). Takes `lock`. Never fails; out of range reads as absent.
void pagemap_take(struct pagemap *pm, spin_lock_t *lock, size_t idx,
                  struct AddrSpace *as, struct page **out_pg,
                  struct page **freed, u32 *nfreed);

// Take the FIRST resident slot in [from, hi): returns its index, with *out_pg
// its page, or `hi` (nothing taken) when no slot in the range is resident.
// Walks only PRESENT nodes -- a present node always holds a resident slot
// beneath it (the take that empties a node unlinks it), so every step descends
// toward a page: a call costs O(depth x 512) entry visits, a range with nothing
// left O(depth), never O(range). That bound is what keeps releasing a
// reservation proportional to what was TOUCHED, and it matters because a
// reservation is free and may be the whole window (64 TiB = 2^34 slots): a
// per-slot release of one would spin under as->lock for hours. Emptied nodes
// are handed back and uncharged exactly as pagemap_take does. Takes `lock`;
// `hi` is clamped to the map's count.
size_t pagemap_take_next(struct pagemap *pm, spin_lock_t *lock, size_t from,
                         size_t hi, struct AddrSpace *as, struct page **out_pg,
                         struct page **freed, u32 *nfreed);

// Diagnostics: node entries visited by pagemap_take_next since boot -- the
// witness of the bound above (the vma_scan_steps shape).
u64  pagemap_walk_steps(void);

// Replace the page in slot `idx` iff it is still `expect` (the COW break's
// commit). Occupancy is unchanged (one page for one page). Takes `lock`.
// Returns true on the swap, false if the slot changed or is absent.
bool pagemap_swap(struct pagemap *pm, spin_lock_t *lock, size_t idx,
                  struct page *expect, struct page *replacement);

// The clone (Plan 9 dupseg, burrow_clone_cow). `dst` must be init'd with the
// SAME count as `src` and be empty. Copies every slot pointer and every node,
// calling `on_page` for each page copied (the COW share is taken there), using
// node pages from `pool` -- exactly pagemap_node_count(src) of them, allocated
// by the caller with pagemap_pool_alloc OUTSIDE every lock, because this runs
// under `src`'s lock (the caller takes it) and may not enter the buddy.
// Returns 0, or -1 with `dst` partially built (the pool ran short: the source
// grew under a lock the caller did not hold -- a caller bug) -- the caller
// destroys `dst`, whose put hook undoes each `on_page`. Pool pages consumed are
// NULLed in `pool`; the caller frees the rest with pagemap_pool_free.
int  pagemap_mirror(struct pagemap *dst, const struct pagemap *src,
                    struct page **pool, u32 pool_n,
                    void (*on_page)(struct page *pg, void *ctx), void *ctx);

// Allocate / free `n` node pages for pagemap_mirror. Each is KP_ZERO with its
// occupancy established at 0, a user page under `exempt`'s pool policy.
// Returns the number allocated (< n on OOM or a pool refusal; the caller
// frees what it got). Charged to no address space: the caller charges the
// clone's footprint (resident + nodes) as one decision after the mirror.
u32  pagemap_pool_alloc(struct page **pool, u32 n, bool exempt);
void pagemap_pool_free(struct page **pool, u32 n);

// Tear the map down: `put` is called for every resident page (the caller's
// free policy: a COW put, a plain free), every node page is freed, the inline
// leaf is kfreed, and the map reads as not live. Runs with NO lock -- the
// Burrow is at {0,0}, nothing maps it, nothing can race. Nothing is uncharged
// here: this is Proc-agnostic (burrow_free_internal), which is exactly why
// every unmapping path releases its slots BEFORE the Burrow can reach this
// (specs/capacity.tla, NoOrphan).
void pagemap_destroy(struct pagemap *pm, void (*put)(struct page *pg, void *ctx),
                     void *ctx);

#endif // THYLACINE_PAGEMAP_H
