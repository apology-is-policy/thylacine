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
