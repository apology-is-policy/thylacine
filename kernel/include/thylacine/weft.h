// Weft -- the per-flow capability network dataplane (NET-THROUGHPUT.md section 5;
// ARCH section 28 I-37). Granting a Proc its /net/<proto>/N/data fid also
// establishes a per-flow shared-page Burrow ring guest<->netd; the flow's
// payload bytes then travel through the shared page with no per-op mediation.
// Isolation is the capability grant; speed is the absence of per-op mediation.
//
// Spec: specs/weft.tla (TLC-green; model-first, gates the impl). This header is
// the Weft-3 SUBSTRATE: the on-shared-page descriptor-ring ABI + the kernel
// snapshot-and-bounds-validate CONSUMER (the spec's Consume action). The guest
// is the producer (it posts {addr,len} descriptors into the split-ring); the
// kernel drains them, COPYING each to kernel memory + validating its bounds
// against the registered payload region, and acts only on that snapshot --
// never re-reading the shared slot (the I-30 ring TOCTOU lifted to the Weft
// descriptor; weft.tla DescPinnedToSnapshot + ActedDescValidated).
//
// Sub-chunk status (NET-THROUGHPUT.md section 6):
//   Weft-2: the cross-Proc Burrow-share substrate (burrow_share_into).
//   Weft-3 (this): the descriptor-ring ABI + the snapshot-validate drain +
//     the hybrid threshold. Kernel-internal; NO EL0 ABI.
//   Weft-6: the per-flow EL0 delivery (SYS_WEFT_SHARE/MAP, keyed on the data
//     fid) wires this ring under a live guest<->netd 9P Tread/Twrite.
//
// The ABI portion (weft_desc + weft_ring_hdr) is byte-pinned by _Static_assert;
// a layout change is an ABI break. The Weft-6 native client mirrors it (the
// loom.h + libthyla_rs::loom precedent).

#ifndef THYLACINE_WEFT_H
#define THYLACINE_WEFT_H

#include <thylacine/types.h>

struct weft_ring_view;

#define WEFT_MAGIC 0x57454654u      // "WEFT"

// Max descriptor slots per ring (a power-of-two sanity cap; the real ceiling is
// the shared Burrow size, validated by weft_ring_layout). A flow needs only a
// handful of in-flight payload descriptors (bounded by the TCP window / msize).
#define WEFT_MAX_ENTRIES 1024u

// Payloads below this stay on the byte-copy ring (the orthodox Plan 9 baseline;
// virtio-9p's <1024 B copy threshold -- NET-THROUGHPUT.md section 4.8). The
// shared page is for large Tread/Twrite payloads only.
#define WEFT_HYBRID_THRESHOLD 1024u

// =============================================================================
// weft_desc -- one payload descriptor the guest posts into the shared split-ring.
// `addr` is a byte offset WITHIN the per-flow payload region (NOT the Burrow
// base), so a descriptor can never point at the ring header or descriptor array.
// 16 bytes. flags are reserved (zero) at v1.0 -- a non-zero flag is rejected at
// consume (fail-closed forward-compat).
// =============================================================================
struct weft_desc {
    u32 addr;       // payload-region-relative byte offset
    u32 len;        // payload byte count (0 is rejected as malformed)
    u32 flags;      // WEFT_DESC_* -- reserved; must be 0 at v1.0
    u32 _resv;
};

_Static_assert(sizeof(struct weft_desc) == 16, "weft_desc ABI pinned at 16 bytes");
_Static_assert(__builtin_offsetof(struct weft_desc, addr) == 0, "addr at 0");
_Static_assert(__builtin_offsetof(struct weft_desc, len) == 4, "len at 4");
_Static_assert(__builtin_offsetof(struct weft_desc, flags) == 8, "flags at 8");

// =============================================================================
// weft_ring_hdr -- the shared control words at offset 0 of the per-flow ring
// Burrow. 64 bytes (one cache line).
//
// Ownership (single-producer guest / single-consumer kernel-elected-reader):
//   prod_tail -- GUEST writes (produces descriptors); the kernel reads.
//   cons_head -- the KERNEL writes (drains); the guest reads (free-slot signal).
// A side only ever advances its own producer/consumer word, so no torn
// cross-write (the loom_ring_hdr split-ring discipline; weft.tla leg (5)).
// Free-running u32 indices; the live slot is index & (ring_entries - 1).
//
// The geometry words (magic / ring_entries / *_off / payload_size) are
// kernel-written ONCE at flow grant (weft_ring_init_hdr) and immutable
// thereafter -- a USER-READABLE MIRROR only. The kernel NEVER trusts them on
// the drain path: it keeps its own copy in `struct weft_ring_view` (the
// loom.c l->sq_entries precedent -- a guest scribble of the mirror cannot
// move the kernel's geometry).
// =============================================================================
struct weft_ring_hdr {
    u32 magic;          // WEFT_MAGIC (mirror)
    u32 ring_entries;   // descriptor slots, power of two (mirror)
    u32 desc_off;       // byte offset of weft_desc[ring_entries] in the Burrow (mirror)
    u32 payload_off;    // byte offset of the payload region in the Burrow (mirror)
    u32 payload_size;   // payload-region byte length (mirror)
    u32 prod_tail;      // guest-owned: produced
    u32 cons_head;      // kernel-owned: drained (mirror of the view's authority)
    u32 dropped;        // descriptors rejected (out-of-bounds / malformed) -- diagnostics
    u32 _pad[8];
};

_Static_assert(sizeof(struct weft_ring_hdr) == 64, "weft_ring_hdr pinned at 64 bytes");
_Static_assert(__builtin_offsetof(struct weft_ring_hdr, prod_tail) == 20, "prod_tail at 20");
_Static_assert(__builtin_offsetof(struct weft_ring_hdr, cons_head) == 24, "cons_head at 24");

// =============================================================================
// weft_ring_view -- the kernel's PRIVATE, trusted view of one mapped per-flow
// ring Burrow. NOT ABI (kernel-only). The geometry is computed once at grant
// (weft_ring_layout) from the Burrow's contiguous direct-map base + size, and
// the drain reads it from HERE, never from the guest-mutable shared header.
// `cons_head` is the kernel-private consumer authority (the mirror in the
// shared header is published for the guest).
// =============================================================================
struct weft_ring_view {
    u8    *base;          // the Burrow's contiguous KVA base (BURROW_TYPE_ANON; trusted)
    size_t size;          // burrow_get_size(v) (trusted)
    u32    ring_entries;  // descriptor slots (power of two)
    u32    desc_off;      // byte offset of the descriptor array
    u32    payload_off;   // byte offset of the payload region
    u32    payload_size;  // payload-region byte length (the bounds-validate ceiling)
    u32    cons_head;     // kernel-private consumer position (advances on drain)
};

// Does a payload of `len` bytes ride the shared ring (true) or the byte-copy
// fallback (false)? The hybrid threshold (NET-THROUGHPUT.md section 4.8).
bool weft_should_ring(u32 len);

// Compute the standard [hdr][desc[ring_entries]][payload] layout for a ring
// Burrow of `size` bytes into `*rv` (base + the validated geometry + cons_head=0).
// `ring_entries` must be a non-zero power of two <= WEFT_MAX_ENTRIES, and the
// header + descriptor array + a non-empty payload region must fit within `size`
// (with the payload region's length representable in the u32 descriptor domain).
// Returns 0 on success, -1 if the geometry does not fit. Does not touch the page.
int weft_ring_layout(u8 *base, size_t size, u32 ring_entries, struct weft_ring_view *rv);

// Write the immutable geometry mirror (magic + geometry, prod_tail/cons_head=0,
// dropped=0) into the shared header so the guest can read the ring geometry.
// Called once at flow grant, after weft_ring_layout. The kernel keeps `*rv` as
// its trusted copy.
void weft_ring_init_hdr(const struct weft_ring_view *rv);

// Drain up to `max` guest-produced descriptors from the split-ring. For each
// in-flight slot the kernel COPIES the descriptor to kernel memory (the
// snapshot -- the spec's Consume), bounds-validates it against the registered
// payload region, and writes the VALIDATED snapshot to out[0..n). An
// out-of-bounds / malformed / reserved-flagged descriptor is rejected (counted
// in hdr->dropped, not emitted). The kernel acts only on out[] -- never on the
// shared slot (weft.tla DescPinnedToSnapshot). Advances + publishes cons_head.
// Single consumer per ring (the elected reader); cons_head is consumer-private.
// Returns the count of validated descriptors in out[].
int weft_ring_drain(struct weft_ring_view *rv, struct weft_desc *out, int max);

#endif
