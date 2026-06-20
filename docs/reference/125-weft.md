# 125 — Weft: the per-flow capability network dataplane (descriptor ring)

**Status:** Weft-3 substrate landed (the descriptor-ring ABI + the kernel
snapshot-and-bounds-validate consumer + the hybrid threshold). Kernel-internal;
no EL0 ABI. The live guest↔netd wiring + the EL0 share delivery is Weft-6.

Source: `kernel/include/thylacine/weft.h`, `kernel/weft.c`. Spec:
`specs/weft.tla` (ARCH §28 I-37). Design: `docs/NET-THROUGHPUT.md` §5–6.

---

## Purpose

Weft is the per-flow, capability-scoped, zero-copy network dataplane: granting a
Proc its `/net/<proto>/N/data` fid also establishes a per-flow shared-page
Burrow ring between that Proc (the *guest*) and netd; the flow's large payloads
then travel through the shared page with no per-operation mediation by netd
(**isolation is the capability grant; speed is the absence of per-op
mediation**). Weft sits above the audited 9P client (the control messages stay
on the existing path) and the cross-Proc Burrow share (Weft-2's
`burrow_share_into`); it is the data path woven *through* Loom.

This file documents the **Weft-3 substrate**: the on-shared-page descriptor-ring
ABI and the kernel's snapshot-and-bounds-validate *consumer* of that ring (the
spec's `Consume` action). The guest is the *producer* — it writes payload bytes
into the shared payload region and posts `{addr,len}` descriptors into the
split-ring; the kernel drains them, copying each to kernel memory and validating
its bounds against the registered payload region, and acts only on that snapshot
— never re-reading the shared slot (the Loom I-30 ring-TOCTOU lifted to the Weft
descriptor).

What Weft-3 is **not**: the live wiring. Nothing in production calls
`weft_ring_drain` yet — the descriptor ring is exercised by the kernel test
suite over a real `burrow_share_into` cross-Proc page. The flow-keyed EL0
delivery (`SYS_WEFT_SHARE`/`SYS_WEFT_MAP`, the data-fid auto-map) and the
dev9p-Weft-Twrite path that calls the drain land at Weft-6.

---

## Public API

```c
// Does a payload of `len` bytes ride the shared ring (true) or the byte-copy
// fallback (false)? The hybrid threshold (NET-THROUGHPUT.md §4.8): < 1024 B
// stays on the existing byte-copy ring -- the orthodox Plan 9 / virtio-9p rule.
bool weft_should_ring(u32 len);

// Compute the standard [hdr][desc[ring_entries]][payload] layout for a ring
// Burrow of `size` bytes into `*rv` (base + validated geometry + cons_head=0).
// `ring_entries` must be a non-zero power of two <= WEFT_MAX_ENTRIES, and the
// header + descriptor array + a non-empty payload region must fit within `size`.
// Returns 0 on success, -1 if the geometry does not fit. Writes `*rv` ONLY on
// success; does not touch the page.
int weft_ring_layout(u8 *base, size_t size, u32 ring_entries,
                     struct weft_ring_view *rv);

// Write the immutable geometry mirror (magic + geometry, indices/dropped = 0)
// into the shared header so the guest can read the ring geometry. Called once at
// flow grant, after weft_ring_layout. The kernel keeps `*rv` as its trusted copy.
void weft_ring_init_hdr(const struct weft_ring_view *rv);

// Drain up to `max` guest-produced descriptors from the split-ring. Each
// in-flight slot is COPIED to kernel memory (the snapshot -- the spec's
// Consume), bounds-validated against the registered payload region, and the
// VALIDATED snapshot written to out[0..n). An out-of-bounds / malformed /
// reserved-flagged descriptor is rejected (counted in hdr->dropped, not
// emitted). The kernel acts only on out[] -- never on the shared slot. Advances
// + publishes cons_head. Single consumer per ring; cons_head is consumer-private.
// Returns the count of validated descriptors in out[].
int weft_ring_drain(struct weft_ring_view *rv, struct weft_desc *out, int max);
```

---

## Data structures

### `struct weft_desc` — one payload descriptor (16 B, ABI)

```c
struct weft_desc {
    u32 addr;       // payload-region-relative byte offset (NOT the Burrow base)
    u32 len;        // payload byte count (0 is rejected as malformed)
    u32 flags;      // reserved; must be 0 at v1.0 (a non-zero flag is rejected)
    u32 _resv;
};
_Static_assert(sizeof(struct weft_desc) == 16, ...);
```

`addr` is relative to the **payload region**, not the Burrow base, so a
descriptor can never point at the ring header or descriptor array — only at the
guest's own payload buffers. The 16-byte size is `_Static_assert`-pinned; a
layout change is an ABI break (the Weft-6 native client mirrors it, the
`loom.h` + `libthyla_rs::loom` precedent).

### `struct weft_ring_hdr` — the shared control words (64 B, ABI)

At offset 0 of the per-flow ring Burrow. One cache line.

```c
struct weft_ring_hdr {
    u32 magic;          // WEFT_MAGIC (mirror)
    u32 ring_entries;   // descriptor slots, power of two (mirror)
    u32 desc_off;       // byte offset of weft_desc[ring_entries] (mirror)
    u32 payload_off;    // byte offset of the payload region (mirror)
    u32 payload_size;   // payload-region byte length (mirror)
    u32 prod_tail;      // guest-owned: produced
    u32 cons_head;      // kernel-owned: drained (mirror of the view's authority)
    u32 dropped;        // descriptors rejected -- diagnostics
    u32 _pad[8];
};
```

**Ownership (single-producer guest / single-consumer kernel-elected-reader):**
`prod_tail` is guest-written / kernel-read; `cons_head` is kernel-written /
guest-read. A side only ever advances its own producer/consumer word, so no
torn cross-write (the `loom_ring_hdr` split-ring discipline; weft.tla leg (5)).

The geometry words (`magic` / `ring_entries` / `*_off` / `payload_size`) are
**kernel-written once at grant and a user-readable MIRROR only**. The kernel
NEVER trusts them on the drain path — it keeps its own copy in
`struct weft_ring_view` (the `loom.c` `l->sq_entries` precedent: a guest
scribble of the mirror cannot move the kernel's geometry).

### `struct weft_ring_view` — the kernel's private trusted view (not ABI)

```c
struct weft_ring_view {
    u8    *base;          // the Burrow's contiguous KVA base (BURROW_TYPE_ANON)
    size_t size;          // burrow_get_size(v)
    u32    ring_entries, desc_off, payload_off, payload_size;  // computed at grant
    u32    cons_head;     // kernel-private consumer authority (advances on drain)
};
```

Computed once at flow grant (`weft_ring_layout`) from the Burrow's contiguous
direct-map base + size. The drain reads the geometry from HERE, never from the
guest-mutable shared header. `cons_head` is the kernel-private consumer
authority; the mirror in the shared header is published for the guest.

---

## Implementation

### The split-ring layout

A per-flow ring Burrow (a `BURROW_TYPE_ANON`, so `alloc_pages(order, KP_ZERO)`
makes its pages physically contiguous) holds, in order:

```
[ weft_ring_hdr (64 B) ][ weft_desc[ring_entries] ][ payload region (16-aligned) ]
```

`weft_ring_layout` validates `ring_entries` (a non-zero power of two ≤
`WEFT_MAX_ENTRIES` = 1024), 16-aligns the payload region after the descriptor
array, and requires a non-empty payload region whose length is representable in
the u32 descriptor domain. It writes `*rv` only on success.

### The drain — the snapshot discipline (`weft.c::weft_ring_drain`)

The heart of Weft-3, mirroring `loom.c::loom_drain_sq`'s `ksqe = sqes[sqe_idx]`:

```c
u32 tail = __atomic_load_n(&h->prod_tail, __ATOMIC_ACQUIRE);   // pairs w/ guest release
while (rv->cons_head != tail && n < max) {
    u32 pos = rv->cons_head & (rv->ring_entries - 1u);
    struct weft_desc kd = darr[pos];     // COPY to kernel memory -- the snapshot (TOCTOU)
    rv->cons_head++;
    if (weft_desc_valid(&kd, rv->payload_size)) out[n++] = kd;   // act on the snapshot
    else                                        dropped++;        // reject OOB / malformed
}
__atomic_store_n(&h->cons_head, rv->cons_head, __ATOMIC_RELEASE);  // publish freed slots
```

Two memory-ordering legs, each a separate concern:

- **Publish visibility** — the acquire-load of `prod_tail` pairs with the
  guest's release-store of `prod_tail` (made after it finishes writing the
  descriptor slot), so any slot `< tail` is fully published: the kernel never
  reads a half-written descriptor.
- **The TOCTOU** — a guest mutation of the shared slot *after* the post is the
  ring-TOCTOU. The kernel holds a value-copy `kd`; it validates and acts on
  `kd`, never re-reading `darr[pos]`. Even a torn copy is sound — whatever bytes
  landed in `kd` are the bytes validated *and* acted on (the validated value IS
  the acted value). This is `DescPinnedToSnapshot`.

`weft_desc_valid` is the bounds gate (`ActedDescValidated`): reserved flags must
be clear (fail-closed forward-compat), `len` non-zero, and `(u64)addr + len <=
payload_size` (computed in u64 so a hostile u32 pair cannot wrap back
in-bounds). A rejected descriptor is counted in `hdr->dropped` and not emitted.

The per-call work is bounded by `max` (out[] is kernel memory sized `max`); a
hostile `prod_tail` over-advance only wastes drained slots the bounds gate then
rejects — never an overrun. The drain takes no lock: it is single-consumer per
ring (the elected reader), and `cons_head` is consumer-private.

### Why the kernel is the validator

The spec models `Consume` as a kernel action ("the kernel copies +
bounds-validates"). The kernel pinned the ring Burrow at grant (the I-30 pin),
so it is the natural validator — it bounds-checks each descriptor against the
payload region it owns, in the TCB, exactly like RDMA's HCA validates against
the registered region. This is **not** per-op *mediation* (no capability
re-resolution — `NoPerOpMediation` is about the flow cap, not the snapshot); it
is the I-30 ring-TOCTOU discipline, validate-once-at-submit.

---

## Spec cross-reference

`specs/weft.tla` (model-first at Weft-1; ARCH §28 I-37). Weft-3 realizes:

| weft.tla action / invariant | impl |
|---|---|
| `GuestPostDesc` / `GuestMutateDesc` | the guest writes/mutates `darr[pos]` + release-bumps `prod_tail` (Weft-6 guest; the test producer) |
| `Consume` (copy + bounds-validate) | `weft_ring_drain` (the snapshot + `weft_desc_valid`) |
| `DescPinnedToSnapshot` | the value-copy `kd`; act on `out[]`, never the shared slot |
| `ActedDescValidated` | `weft_desc_valid` (u64 bound; flags clear; len > 0) |
| split-ring leg (5) | `prod_tail` guest-only / `cons_head` kernel-only |

The buggy cfg `weft_buggy_ring_toctou` → `DescPinnedToSnapshot` is the durable
regression for this surface (the Weft-7 audit). The spec gate re-ran green at
Weft-3 (clean 1412 distinct; each buggy cfg violates its named invariant);
Weft-3 extends the modeled mechanism without changing the spec.

---

## Tests

`kernel/test/test_weft_ring.c` — 6 tests over a `burrow_share_into` cross-Proc
shared page (`weft.*`):

- `weft.ring_basic` — a ≥-threshold payload rides the ring; the kernel snapshots
  the descriptor and reads the payload **in place** at the validated offset (no
  copy); the geometry mirror is correct, `cons_head` advances + publishes.
- `weft.ring_toctou_snapshot` — after the drain, the shared slot is mutated to an
  OOB value; the snapshot in `out[0]` is unchanged and the in-place read follows
  the snapshot, not the poisoned slot (`DescPinnedToSnapshot`).
- `weft.ring_bounds_reject` — one valid + four bad descriptors (OOB, zero-length,
  reserved-flag, u32-wrap); only the valid one is emitted, `dropped == 4`,
  `cons_head` advances past all five (`ActedDescValidated`).
- `weft.ring_multi_split` — three descriptors drained in order; the consumer is
  read-only on the producer's regions (`prod_tail` + the descriptor slots are
  byte-unchanged after the drain — the single-writer split-ring discipline).
- `weft.should_ring_threshold` — the hybrid `< 1024 B` byte-copy fallback.
- `weft.ring_layout_constraints` — power-of-two entries, the regions must fit,
  NULL + degenerate inputs fail closed.

943/943 PASS, boot OK, 0 EXTINCTION at landing.

---

## Error paths

- `weft_ring_layout` → `-1` on: NULL base / view, `ring_entries` zero or not a
  power of two or over `WEFT_MAX_ENTRIES`, no room for a non-empty payload
  region, a payload region larger than the u32 descriptor domain. Writes nothing
  on failure.
- `weft_ring_drain` → `0` on NULL `rv`/`out` or `max <= 0` (no side effects). A
  rejected descriptor is dropped (counted), not an error return.

---

## Known caveats / footguns (Weft-6 obligations)

- **Single consumer per ring.** `weft_ring_drain` is lock-free under the
  precondition that one party (the dev9p elected reader) drains a given ring;
  `cons_head` is consumer-private. Weft-6 must uphold this (it does — the elected
  reader drives the dev9p Weft path).
- **The geometry is kernel-owned.** The drain trusts `rv` (the kernel's copy),
  never the shared header's geometry mirror. Weft-6 must construct `rv` via
  `weft_ring_layout` at grant and never re-derive geometry from the shared page.
- **The Burrow must outlive the drain.** The substrate is a pure function over a
  live mapped view; the #847 dual-refcount (via `burrow_share_into`) keeps the
  ring alive while the flow is active. The flow-bounded teardown
  (`ShareBoundedByFlow`) is Weft-2 + Weft-7's domain.
- **Live concurrent producer↔consumer** (a guest posting while the kernel
  drains) is exercised by the SMP gate at Weft-6 with the live ring; Weft-3
  proves the *discipline* (acquire/release + the value-copy snapshot) over a
  single-threaded test.
- **The F_NOTIF multi-holder buffer lifetime** (the page not reused until the
  last of {netd stack, NIC DMA, peer ACK}) is Weft-5, not here. Weft-3 is the
  descriptor ring; the buffer-lifetime pin is a separate leg.
