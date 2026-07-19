// Weft -- the per-flow capability network dataplane descriptor ring (Weft-3).
//
// The Weft-3 substrate: the kernel snapshot-and-bounds-validate CONSUMER of the
// guest-produced split-ring (the spec's Consume action; specs/weft.tla). The
// guest posts {addr,len} descriptors into the shared ring; the kernel drains
// them, copying each to kernel memory and validating its bounds against the
// registered payload region, and acts only on that snapshot -- never re-reading
// the shared slot (the Loom I-30 ring TOCTOU lifted to the Weft descriptor;
// weft.tla DescPinnedToSnapshot + ActedDescValidated).
//
// The geometry (ring_entries / *_off / payload_size) lives in the kernel-private
// struct weft_ring_view, NOT read from the guest-mutable shared header on the
// hot path (the loom.c l->sq_entries precedent -- a guest scribble of the header
// mirror cannot move the kernel's geometry). The split-ring is single-writer per
// region: the guest owns prod_tail + the descriptor slots, the kernel owns
// cons_head, so no shared word has two concurrent writers (weft.tla leg (5)).
//
// See kernel/include/thylacine/weft.h + NET-THROUGHPUT.md section 5. The live
// guest<->netd wiring (under a 9P Tread/Twrite) + the EL0 share delivery is
// Weft-6 (SYS_WEFT_SHARE/MAP, keyed on the /net data fid).

#include <thylacine/weft.h>
#include <thylacine/burrow.h>
#include <thylacine/dma_handle.h>   // G-2: the kernel-minted KObj_DMA weave bit
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/spinlock.h>
#include <thylacine/vma.h>          // G-2: vma_lookup for the clunk-unmap F1 guard

#include "../mm/slub.h"

// A descriptor is valid iff: its reserved flags are clear (fail-closed
// forward-compat), its length is non-zero, and [addr, addr+len) lies within the
// payload region. The addr+len sum is computed in u64 so a hostile u32 pair
// cannot wrap back in-bounds (weft.tla ActedDescValidated).
static bool weft_desc_valid(const struct weft_desc *d, u32 payload_size) {
    if (d->flags != 0u) return false;
    if (d->len == 0u) return false;
    if ((u64)d->addr + (u64)d->len > (u64)payload_size) return false;
    return true;
}

bool weft_should_ring(u32 len) {
    return len >= WEFT_HYBRID_THRESHOLD;
}

int weft_ring_layout(u8 *base, size_t size, u32 ring_entries,
                     struct weft_ring_view *rv) {
    if (!base || !rv) return -1;
    if (ring_entries == 0u || (ring_entries & (ring_entries - 1u)) != 0u)
        return -1;                                      // must be a power of two
    if (ring_entries > WEFT_MAX_ENTRIES) return -1;

    // [ring_hdr][ready_hdr][desc[ring_entries]][payload]. The readiness header
    // (two cache lines, single-writer-per-word) sits in the fixed control region
    // between the ring header and the descriptor array.
    u64 ready_off   = sizeof(struct weft_ring_hdr);
    u64 desc_off    = ready_off + sizeof(struct weft_ready_hdr);
    u64 desc_bytes  = (u64)ring_entries * sizeof(struct weft_desc);
    u64 payload_off = desc_off + desc_bytes;
    payload_off = (payload_off + 15u) & ~(u64)15u;      // 16-align the payload region

    if (payload_off >= (u64)size) return -1;            // no room for a payload region
    u64 payload_size = (u64)size - payload_off;
    if (payload_size > 0xFFFFFFFFull) return -1;        // u32 descriptor addr/len domain

    rv->base         = base;
    rv->size         = size;
    rv->ring_entries = ring_entries;
    rv->desc_off     = (u32)desc_off;
    rv->payload_off  = (u32)payload_off;
    rv->payload_size = (u32)payload_size;
    rv->ready_off    = (u32)ready_off;
    rv->cons_head    = 0u;
    return 0;
}

void weft_ring_init_hdr(const struct weft_ring_view *rv) {
    struct weft_ring_hdr *h = (struct weft_ring_hdr *)rv->base;
    h->magic        = WEFT_MAGIC;
    h->ring_entries = rv->ring_entries;
    h->desc_off     = rv->desc_off;
    h->payload_off  = rv->payload_off;
    h->payload_size = rv->payload_size;
    h->prod_tail    = 0u;
    h->cons_head    = 0u;
    h->dropped      = 0u;
    h->ready_off    = rv->ready_off;
    for (int i = 0; i < 7; i++) h->_pad[i] = 0u;
}

int weft_ring_drain(struct weft_ring_view *rv, struct weft_desc *out, int max) {
    if (!rv || !out || max <= 0) return 0;

    struct weft_ring_hdr *h = (struct weft_ring_hdr *)rv->base;
    struct weft_desc *darr  = (struct weft_desc *)(rv->base + rv->desc_off);

    // Acquire-load the guest's producer tail: it pairs with the guest's
    // release-store of prod_tail (made after the guest finishes writing the
    // descriptor slot), so any slot < tail is fully published -- the kernel
    // never reads a half-written descriptor. (A guest mutation AFTER the post
    // is the TOCTOU the snapshot below defends, not this pairing.)
    u32 tail = __atomic_load_n(&h->prod_tail, __ATOMIC_ACQUIRE);

    int n = 0;
    u32 dropped = 0u;
    // `n < max` bounds the per-call work (out[] is kernel memory sized `max`). A
    // sane producer never advances prod_tail more than ring_entries past
    // cons_head (the ring-full back-pressure); a hostile over-advance only
    // wastes drained slots the bounds gate then rejects -- never an overrun.
    while (rv->cons_head != tail && n < max) {
        u32 pos = rv->cons_head & (rv->ring_entries - 1u);
        struct weft_desc kd = darr[pos];     // COPY to kernel memory -- the snapshot (TOCTOU)
        rv->cons_head++;
        if (weft_desc_valid(&kd, rv->payload_size))
            out[n++] = kd;                   // act only on the validated snapshot
        else
            dropped++;                       // reject out-of-bounds / malformed
    }

    if (dropped)
        __atomic_store_n(&h->dropped, h->dropped + dropped, __ATOMIC_RELAXED);
    // Publish the consumed head: release-store so the guest sees the freed slots
    // only after the kernel has finished reading them.
    __atomic_store_n(&h->cons_head, rv->cons_head, __ATOMIC_RELEASE);
    return n;
}

// =============================================================================
// Readiness ring (Weft-4) -- the netd<->guest single-cache-line poke. The
// no-lost-wake is the store-buffer register-then-observe (specs/weft_readiness
// .tla; I-9): the consumer publishes its park-intent (wait_active) then re-reads
// ready_seq; the producer bumps ready_seq then reads wait_active. Each side does
// a seq-cst store THEN a seq-cst load on opposite words -- the StoreLoad barrier
// the SB litmus needs (the Linux set_current_state()+smp_mb() before a cond
// re-check) -- so in the global seq-cst order at least one side sees the other's
// write, and a readiness edge in the park window is never lost. Single-writer-
// per-word: the producer owns ready_seq/ready_mask, the consumer owns wait_seq/
// wait_active; neither writes the other's words (the producer's wake is a Rendez
// wakeup, wired at Weft-6, not a write of wait_active).
// =============================================================================

static struct weft_ready_hdr *weft_ready_of(struct weft_ring_view *rv) {
    return (struct weft_ready_hdr *)(rv->base + rv->ready_off);
}

void weft_ready_init_hdr(const struct weft_ring_view *rv) {
    struct weft_ready_hdr *rh = (struct weft_ready_hdr *)(rv->base + rv->ready_off);
    rh->ready_seq   = 0u;
    rh->ready_mask  = 0u;
    rh->wait_seq    = 0u;
    rh->wait_active = 0u;
    for (int i = 0; i < 14; i++) { rh->_ppad[i] = 0u; rh->_cpad[i] = 0u; }
}

bool weft_ready_signal(struct weft_ring_view *rv, u32 mask) {
    struct weft_ready_hdr *rh = weft_ready_of(rv);
    // Publish the readiness mask (relaxed -- ordered by the seq bump below), then
    // bump the edge counter with a seq-cst RMW. The seq-cst bump release-publishes
    // the mask to a concurrent acquire-observer AND is the StoreLoad barrier
    // before the wait_active load: it cannot float after the load, so a consumer
    // that armed concurrently is never missed (weft_readiness.tla ProducerEdge).
    __atomic_store_n(&rh->ready_mask, mask, __ATOMIC_RELAXED);
    u32 seq = __atomic_add_fetch(&rh->ready_seq, 1u, __ATOMIC_SEQ_CST);

    // Decide whether a parked consumer must be woken. Read the consumer-owned
    // park-intent (seq-cst, ordered AFTER the bump). A consumer armed at a seq
    // this edge passed (wseq != the new seq) is parked and must be woken; a
    // running consumer (wait_active == 0) needs no wakeup. We NEVER write the
    // wait_* words -- they are the consumer's (single-writer-per-word).
    bool armed = __atomic_load_n(&rh->wait_active, __ATOMIC_SEQ_CST) != 0u;
    u32  wseq  = __atomic_load_n(&rh->wait_seq, __ATOMIC_RELAXED);
    return armed && wseq != seq;
}

u32 weft_ready_observe(struct weft_ring_view *rv, u32 *mask_out) {
    struct weft_ready_hdr *rh = weft_ready_of(rv);
    // Acquire-load the edge counter: pairs with the producer's seq-cst bump
    // (which release-published the mask), so a new seq implies its mask is
    // visible. The caller compares the returned seq to its private last-seen.
    u32 seq = __atomic_load_n(&rh->ready_seq, __ATOMIC_ACQUIRE);
    if (mask_out) *mask_out = __atomic_load_n(&rh->ready_mask, __ATOMIC_RELAXED);
    return seq;
}

bool weft_ready_arm_park(struct weft_ring_view *rv, u32 last_seen) {
    struct weft_ready_hdr *rh = weft_ready_of(rv);
    // REGISTER: publish the park-intent. wait_seq first (relaxed -- ordered by the
    // seq-cst wait_active store), then wait_active seq-cst. We own these words.
    __atomic_store_n(&rh->wait_seq, last_seen, __ATOMIC_RELAXED);
    __atomic_store_n(&rh->wait_active, 1u, __ATOMIC_SEQ_CST);

    // OBSERVE: re-read the edge counter (seq-cst, the StoreLoad barrier after the
    // register). If it advanced, a producer edge raced our park -- un-arm and tell
    // the caller NOT to park (re-process). Else it is safe to park: a future edge
    // finds us armed and wakes us. Paired with weft_ready_signal's seq-cst
    // store+load, no edge in the window is lost (weft_readiness.tla ConsumerPark).
    u32 seq = __atomic_load_n(&rh->ready_seq, __ATOMIC_SEQ_CST);
    if (seq != last_seen) {
        __atomic_store_n(&rh->wait_active, 0u, __ATOMIC_SEQ_CST);   // un-arm
        return false;                                              // don't park, re-process
    }
    return true;                                                   // safe to park
}

void weft_ready_unpark(struct weft_ring_view *rv) {
    struct weft_ready_hdr *rh = weft_ready_of(rv);
    __atomic_store_n(&rh->wait_active, 0u, __ATOMIC_SEQ_CST);
}

// =============================================================================
// F_NOTIF zero-copy-send completion contract (Weft-5). The kernel-private
// multi-holder tracker for one in-flight ZC send. The whole point is the
// notification-terminal pin release: the I-30 buffer pin is held while ANY of
// {netd stack, NIC DMA, peer ACK} still references the page, and released ONLY
// once the LAST clears -- the io_uring F_NOTIF two-CQE contract. Releasing at
// op-terminal (netd done) reuses a page the NIC may still DMA / TCP may still
// retransmit = the ubuf_info UAF (weft.tla PinHeldWhileInFlight + NoInFlightReuse;
// the ReleasePremature buggy cfg is the counterexample).
//
// The holder bitmask is the COMPLETE state -- in flight iff holders != 0 -- so no
// `armed`/`terminated` flag is needed: a clear of an already-clear holder (a stray
// or duplicate completion event, or any event after the terminal) finds the bit
// 0, no-ops, and returns HELD, so WEFT_NOTIF_RELEASE is emitted EXACTLY ONCE (on
// the nonzero->zero transition). A caller that releases the pin only on RELEASE
// therefore cannot drop it early -- ReleasePremature is unreachable through the
// API. These run on the kernel's per-op completion state under the engine's lock
// (the loom_async_op the live tracker rides at Weft-6); no shared memory.
// =============================================================================

void weft_notif_arm(struct weft_notif *n, u32 holders) {
    // Mask to the valid holder domain (a degenerate over-wide/empty argument
    // cannot manufacture a phantom holder; an empty result is a no-op send,
    // not-in-flight). weft.tla NetdAct sets holders = Holders.
    n->holders      = holders & WEFT_HOLDERS_ALL;
    n->result_flags = 0u;        // zero-copied: in flight until the notification
}

void weft_notif_arm_copied(struct weft_notif *n) {
    n->holders      = 0u;        // copied -> no in-flight window; reusable at once
    n->result_flags = WEFT_NOTIF_COPIED;
}

enum weft_notif_phase weft_notif_clear(struct weft_notif *n, u32 holder) {
    // A clear of a holder that is not currently pending (a stray/duplicate/late
    // completion, an event for a never-armed holder, or anything after the
    // terminal) is a no-op: the page is not freed twice, and RELEASE is never
    // emitted a second time. weft.tla HolderRelease requires h \in holders[b].
    if ((n->holders & holder) == 0u)
        return WEFT_NOTIF_HELD;

    n->holders &= ~holder;
    // RELEASE iff this clear emptied the set -- the notification-terminal. The
    // pin drops here (weft.tla ReleaseClean), never with a holder still pending.
    return (n->holders == 0u) ? WEFT_NOTIF_RELEASE : WEFT_NOTIF_HELD;
}

bool weft_notif_inflight(const struct weft_notif *n) {
    return n->holders != 0u;
}

u32 weft_notif_result_flags(const struct weft_notif *n) {
    return n->result_flags;
}

// =============================================================================
// share_id registry (Weft-6a-2) -- the netd<->kernel join key for the per-flow
// ring (NET-THROUGHPUT.md section 6; weft.h). A small fixed table: an entry
// normally lives only for the Tweft round-trip window (SYS_WEFT_SHARE mints it,
// the kernel's SYS_WEFT_MAP claims it in the SAME call), so occupancy is ~0.
//
// The lifetime authority is the I-30 REGISTRATION PIN (a burrow_ref). It is
// taken at register, TRANSFERRED to the claimer (or to owner-death GC), and
// dropped exactly once -- so a ring Burrow that a netd shared but the kernel
// never claimed cannot leak (weft.tla ShareBoundedByFlow). g_weft_lock is a
// pure LEAF: every burrow_ref / burrow_unref runs OUTSIDE it (those take the
// per-Burrow v->lock + may free, which takes the buddy lock), so the registry
// lock never nests another lock and cannot deadlock.
// =============================================================================

#define WEFT_MAX_SHARES 64u

struct weft_share {
    u64            share_id;   // 0 == free slot
    struct Burrow *burrow;     // registration pin held (burrow_ref)
    struct Proc   *owner;      // the registering netd Proc (owner-death GC key)
};

static struct weft_share g_weft_shares[WEFT_MAX_SHARES];
static u64               g_weft_next_id = 1;   // monotonic; never mints 0
static spin_lock_t       g_weft_lock = SPIN_LOCK_INIT;   // leaf; process context only

u64 weft_share_register(struct Proc *owner, struct Burrow *v) {
    if (!owner || !v) return 0u;

    // Take the registration pin BEFORE publishing the entry, so the slot never
    // names a Burrow it does not hold alive (and so burrow_ref stays OUTSIDE
    // g_weft_lock -- the leaf discipline). On a full table the pin is dropped.
    burrow_ref(v);

    spin_lock(&g_weft_lock);
    int slot = -1;
    for (u32 i = 0; i < WEFT_MAX_SHARES; i++) {
        if (g_weft_shares[i].share_id == 0u) { slot = (int)i; break; }
    }
    if (slot < 0) {
        spin_unlock(&g_weft_lock);
        burrow_unref(v);                 // table full: drop the pin we took
        return 0u;
    }
    u64 id = g_weft_next_id++;
    if (g_weft_next_id == 0u) g_weft_next_id = 1u;   // never reuse 0 across the u64 wrap
    g_weft_shares[slot].share_id = id;
    g_weft_shares[slot].burrow   = v;
    g_weft_shares[slot].owner    = owner;
    spin_unlock(&g_weft_lock);
    return id;
}

struct Burrow *weft_share_claim(u64 share_id) {
    if (share_id == 0u) return NULL;     // 0 is never a valid id

    spin_lock(&g_weft_lock);
    for (u32 i = 0; i < WEFT_MAX_SHARES; i++) {
        if (g_weft_shares[i].share_id == share_id) {
            struct Burrow *v = g_weft_shares[i].burrow;
            // Consume-exactly-once: free the slot. The registration pin is now
            // OWNED by the caller (transferred out of the registry) -- no extra
            // ref/unref. A replay of this id finds nothing -> NULL.
            g_weft_shares[i].share_id = 0u;
            g_weft_shares[i].burrow   = NULL;
            g_weft_shares[i].owner    = NULL;
            spin_unlock(&g_weft_lock);
            return v;
        }
    }
    spin_unlock(&g_weft_lock);
    return NULL;
}

int weft_share_unregister(struct Proc *owner, u64 share_id) {
    if (!owner || share_id == 0u) return -1;

    // Find-and-remove under the leaf lock; drop the pin OUTSIDE it (the
    // release_owner discipline -- burrow_unref takes v->lock + may free, which
    // takes the buddy lock, never under the registry leaf). The owner gate:
    // only the registrant may disarm its own share -- a stranger's guess of a
    // live id (kernel-minted monotonic u64, never guest-visible) cannot yank
    // another server's in-flight grant.
    struct Burrow *victim = NULL;
    spin_lock(&g_weft_lock);
    for (u32 i = 0; i < WEFT_MAX_SHARES; i++) {
        if (g_weft_shares[i].share_id == share_id &&
            g_weft_shares[i].owner == owner) {
            victim = g_weft_shares[i].burrow;
            g_weft_shares[i].share_id = 0u;
            g_weft_shares[i].burrow   = NULL;
            g_weft_shares[i].owner    = NULL;
            break;
        }
    }
    spin_unlock(&g_weft_lock);
    if (!victim) return -1;    // already claimed / GC'd / forged / not yours
    burrow_unref(victim);      // the registration pin: removal-before-free (R2-F5)
    return 0;
}

int weft_claimed_kind(const struct Burrow *v, u32 ring_entries) {
    if (!v) return -1;
    // The kernel-minted Burrow type is the single source of truth; the
    // server's declared geometry must AGREE with it, else fail closed. Types +
    // the weave bit are create-immutable -- lock-free reads are coherent.
    if (v->type == BURROW_TYPE_ANON)
        return (ring_entries != 0u) ? WEFT_BIND_RING : -1;
    if (v->type == BURROW_TYPE_DMA && v->kobj_dma != NULL && v->kobj_dma->weave)
        return (ring_entries == 0u) ? WEFT_BIND_WEAVE : -1;
    // Anything else in the registry would be a register-gate breach; the
    // claim-side re-check keeps it unmappable regardless (defense-in-depth).
    return -1;
}

void weft_share_release_owner(struct Proc *owner) {
    if (!owner) return;

    // Snapshot the orphaned Burrows under the lock, then drop the pins OUTSIDE
    // it (burrow_unref takes v->lock + may free via burrow_free_internal, which
    // takes the buddy lock -- never under the registry leaf).
    struct Burrow *orphans[WEFT_MAX_SHARES];
    u32 n = 0;
    spin_lock(&g_weft_lock);
    for (u32 i = 0; i < WEFT_MAX_SHARES; i++) {
        if (g_weft_shares[i].share_id != 0u && g_weft_shares[i].owner == owner) {
            orphans[n++] = g_weft_shares[i].burrow;
            g_weft_shares[i].share_id = 0u;
            g_weft_shares[i].burrow   = NULL;
            g_weft_shares[i].owner    = NULL;
        }
    }
    spin_unlock(&g_weft_lock);
    for (u32 i = 0; i < n; i++) burrow_unref(orphans[i]);
}

struct weft_binding *weft_binding_alloc(struct Burrow *burrow, u64 guest_va,
                                        u32 ring_size, u32 ring_entries) {
    if (!burrow || burrow->pages == NULL) return NULL;
    if (burrow->type != BURROW_TYPE_ANON) return NULL;   // G-2: rings are ANON
    // Compute the kernel-private ring view from the Burrow's contiguous KVA.
    // BURROW_TYPE_ANON pages are physically contiguous (alloc_pages), so the
    // ring is one direct-map span; weft_ring_layout validates ring_entries +
    // the [hdr][ready][desc][payload] geometry against ring_size.
    u8 *base = (u8 *)pa_to_kva(page_to_pa(burrow->pages));
    struct weft_ring_view rv;
    if (weft_ring_layout(base, (size_t)ring_size, ring_entries, &rv) != 0)
        return NULL;

    struct weft_binding *b = kmalloc(sizeof(*b), KP_ZERO);
    if (!b) return NULL;
    b->burrow    = burrow;
    b->guest_va  = guest_va;
    b->ring_size = ring_size;
    b->kind      = WEFT_BIND_RING;
    b->view      = rv;
    return b;
}

struct weft_binding *weft_binding_alloc_weave(struct Burrow *burrow,
                                              u64 guest_va, u32 size,
                                              u32 map_pid) {
    // Defense-in-depth: only the kernel-minted weave subtype reaches here (the
    // caller derived WEAVE via weft_claimed_kind); re-check anyway so a future
    // caller cannot mint a weave binding over an inadmissible region.
    if (!burrow || burrow->type != BURROW_TYPE_DMA ||
        burrow->kobj_dma == NULL || !burrow->kobj_dma->weave)
        return NULL;

    struct weft_binding *b = kmalloc(sizeof(*b), KP_ZERO);
    if (!b) return NULL;
    b->burrow    = burrow;
    b->guest_va  = guest_va;
    b->ring_size = size;
    b->kind      = WEFT_BIND_WEAVE;
    b->map_pid   = map_pid;
    // b->view stays zeroed -- a weave has no descriptor ring; the kind gate in
    // weft_binding_validate_rw keeps every Tweftio consumer off it.
    return b;
}

int weft_binding_validate_rw(const struct weft_binding *b, u64 ubuf_va,
                             u32 len, u32 *out_off) {
    if (!b || !out_off) return -1;
    // G-2 kind gate: only a RING binding carries a payload-region geometry.
    // This single check closes ALL Tweftio fast paths (sync dev9p_weft_try_
    // {read,write} + the Loom weft routing) for a WEAVE binding -- a weave fid
    // is a map capability, never a data-drive target; its reads/writes stay on
    // the ordinary byte path (the geometry text read).
    if (b->kind != WEFT_BIND_RING) return -1;
    // Direction-agnostic: a Tweftio READ and WRITE both name a region [off,
    // off+len) within the payload, so the descriptor validation is identical --
    // the buffer (the WRITE source, or the READ destination) must sit at or after
    // the payload region's base in the SAME address space the guest mapped the
    // ring into (guest_va is the guest mapping; ubuf_va is the same-Proc syscall
    // buffer that points INTO the ring).
    u64 payload_base = b->guest_va + (u64)b->view.payload_off;
    if (ubuf_va < payload_base) return -1;        // below the payload region
    u64 o = ubuf_va - payload_base;
    if (o > 0xFFFFFFFFull) return -1;             // beyond the u32 descriptor domain
    // Reuse the Weft-3 bounds gate: flags clear + len != 0 + (o + len) <=
    // payload_size, computed in u64 so a hostile pair cannot wrap in-bounds.
    struct weft_desc d = { (u32)o, len, 0u, 0u };
    if (!weft_desc_valid(&d, b->view.payload_size)) return -1;
    *out_off = (u32)o;
    return 0;
}

void weft_binding_release(struct weft_binding *b) {
    if (!b) return;
    // Drop the registration pin (handle_count). The guest's ring MAPPING
    // (mapping_count) is owned by the guest VMA + reclaimed by vma_drain at
    // guest exit (the Loom-ring precedent) -- never here.
    if (b->burrow) burrow_unref(b->burrow);
    b->burrow = NULL;
    kfree(b);
}

int weft_binding_clunk_unmap(struct weft_binding *wb, struct Proc *closer) {
    if (!wb || !closer) return -1;
    if (wb->kind != WEFT_BIND_WEAVE) return -1;       // RING keeps its lifetime
    if (closer->pid != wb->map_pid)  return -1;       // not the mapping Proc
    spin_lock(&closer->vma_lock);
    // The G-2 audit F1 guard: the binding's guest_va is a RECORD, not a live
    // claim -- the client may have SYS_BURROW_DETACHed the weave and a fresh
    // unrelated mapping may have landed at the same VA (vma_find_gap reuses
    // gaps). Unmap ONLY if the VMA at guest_va is still backed by THIS weave;
    // anything else survives untouched (the mapping Proc's own vma_drain, or
    // an earlier explicit detach, already handled -- or never will need to
    // handle -- the weave's mapping).
    struct Vma *v = vma_lookup(closer, wb->guest_va);
    int rc = -1;
    if (v && v->burrow == wb->burrow && v->vaddr_start == wb->guest_va)
        rc = burrow_unmap(closer, wb->guest_va, (size_t)wb->ring_size);
    spin_unlock(&closer->vma_lock);
    return rc;
}
