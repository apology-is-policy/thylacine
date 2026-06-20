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

    u64 desc_off    = sizeof(struct weft_ring_hdr);
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
    for (int i = 0; i < 8; i++) h->_pad[i] = 0u;
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
