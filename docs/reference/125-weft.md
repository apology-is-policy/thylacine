# 125 — Weft: the per-flow capability network dataplane (descriptor + readiness rings + F_NOTIF)

**Status:** Weft-3 (the descriptor-ring ABI + the kernel snapshot-and-bounds-
validate consumer + the hybrid threshold), Weft-4 (the readiness ring — the
single-cache-line poke + the store-buffer register-then-observe), **and** Weft-5
(the F_NOTIF two-CQE zero-copy-send completion contract — the multi-holder
buffer-pin release) substrates landed. Kernel-internal; no EL0 ABI. The live
guest↔netd wiring + the EL0 share delivery is Weft-6 — begun at **Weft-6a-1**:
the `Tweft(F) → Rweft(share_id, geom)` 9P op + `p9_client_weft` (the lazy
fid-keyed ring-setup request; see `docs/reference/44-9p-wire.md` /
`45-9p-session.md` / `47-9p-client.md`). The `SYS_WEFT_SHARE`/`SYS_WEFT_MAP`
syscalls + the kernel-scoped `share_id` registry + the `dev9p_priv` binding are
Weft-6a-2.

Source: `kernel/include/thylacine/weft.h`, `kernel/weft.c`. Specs:
`specs/weft.tla` (ARCH §28 I-37, the data plane + the F_NOTIF holder lifecycle),
`specs/weft_readiness.tla` (ARCH §28 I-9, the readiness poke). Design:
`docs/NET-THROUGHPUT.md` §4.6, §5–6.

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
[ weft_ring_hdr (64 B) ][ weft_ready_hdr (128 B) ][ weft_desc[ring_entries] ][ payload region (16-aligned) ]
```

`weft_ring_layout` validates `ring_entries` (a non-zero power of two ≤
`WEFT_MAX_ENTRIES` = 1024), places the readiness header (two cache lines) in the
fixed control region after the ring header, 16-aligns the payload region after
the descriptor array, and requires a non-empty payload region whose length is
representable in the u32 descriptor domain. It writes `*rv` only on success. The
`ready_off` is mirrored into the shared `weft_ring_hdr` (offset 32) so the guest
can find the readiness block.

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

## Readiness ring (Weft-4)

The descriptor ring moves the payload; the **readiness ring** removes the
remaining round-trip — the *wake*. A consumer parked waiting for the other side
(a guest parked on its `/net` read; netd waiting on a peer) must be woken when
readiness arrives. Today that wake is a poll cadence (the ~50 ms RX-wake floor,
NET-PERF N1). The readiness ring replaces it with a single shared cache line:
the **producer** (netd) bumps a readiness edge counter; the **consumer** (guest)
observes it at memory speed — the Shenango single-cache-line poke. It is the
**push** counterpart of the dev9p.poll elicited **pull** (`net_poll.tla`): no
probe to keep outstanding, the edge is written straight into shared memory.

### Public API

```c
#define WEFT_READY_RX 0x001u   // RX data available (POLLIN-aligned)
#define WEFT_READY_TX 0x004u   // TX space / send complete (POLLOUT-aligned)

// Zero the readiness header. Called once at flow grant, with weft_ring_init_hdr.
void weft_ready_init_hdr(const struct weft_ring_view *rv);

// PRODUCER (netd): post a readiness edge -- bump ready_seq + publish `mask`, then
// read the consumer's park-intent. Returns true iff a parked consumer must be
// woken (the caller issues the Rendez wakeup); false iff it is running (busy-
// polling). Seq-cst bump-then-load: never misses a consumer that armed
// concurrently. NEVER writes the consumer-owned wait_* words.
bool weft_ready_signal(struct weft_ring_view *rv, u32 mask);

// CONSUMER (guest) fast path: acquire-load the edge counter (+ mask). The caller
// compares the returned seq to its private last-seen cursor; a difference is a
// new edge. No park, no syscall -- the lock-free cache-line read.
u32 weft_ready_observe(struct weft_ring_view *rv, u32 *mask_out);

// CONSUMER (guest) park decision: register-then-observe. Publishes the park-
// intent THEN re-reads ready_seq (seq-cst). Returns true iff safe to park (no
// edge raced the window); false iff an edge arrived (don't park -- re-process;
// wait_active cleared). The Rendez sleep on a true return is Weft-6.
bool weft_ready_arm_park(struct weft_ring_view *rv, u32 last_seen);

// CONSUMER (guest): clear the park-intent on resume (after a wakeup, or after
// deciding not to park). The consumer owns wait_active; the producer only reads.
void weft_ready_unpark(struct weft_ring_view *rv);
```

### Data structure — `struct weft_ready_hdr` (128 B, two cache lines, ABI)

At `ready_off` (= `sizeof(weft_ring_hdr)` = 64) of the per-flow Burrow.

```c
struct weft_ready_hdr {
    u32 ready_seq;      // producer-owned: bumped per readiness edge (the poke)
    u32 ready_mask;     // producer-owned: WEFT_READY_* of the latest edge
    u32 _ppad[14];      // pad the producer words to a full cache line
    u32 wait_seq;       // consumer-owned: the seq registered before parking
    u32 wait_active;    // consumer-owned: 1 while parked, 0 while running
    u32 _cpad[14];      // pad the consumer words to a full cache line
};
_Static_assert(sizeof(struct weft_ready_hdr) == 128, ...);
_Static_assert(__builtin_offsetof(struct weft_ready_hdr, wait_seq) == 64, ...);
```

**Single-writer-per-word, on two cache lines** (no false sharing — the producer
never touches the consumer's line and vice versa): `ready_seq` / `ready_mask`
are producer-owned (netd writes, guest reads); `wait_seq` / `wait_active` are
consumer-owned (guest writes, netd reads *to decide a wakeup*, never writes —
the producer's wake is a Rendez wakeup, wired at Weft-6, not a write of
`wait_active`). The Shenango cache-line: the consumer reads one line to learn
readiness without a round-trip.

### The no-lost-wake — the store-buffer register-then-observe (`I-9`)

The hazard is the classic store-buffer (SB) litmus: two parties write disjoint
words and read the other's; the bad outcome is both reading the stale value — the
consumer parks having observed "no edge", while the producer's edge, posted in
the window, found the consumer "not yet parked" and issued no wake. The edge is
lost; the consumer sleeps forever on a ready channel.

The discipline that forecloses it is **register-then-observe**:

```c
// CONSUMER arm_park: REGISTER (publish park-intent) THEN OBSERVE (re-read seq)
__atomic_store_n(&rh->wait_active, 1u, __ATOMIC_SEQ_CST);     // register
u32 seq = __atomic_load_n(&rh->ready_seq, __ATOMIC_SEQ_CST);  // observe (re-check)
if (seq != last_seen) { un-arm; return false; /* don't park */ }
return true; /* safe to park */

// PRODUCER signal: bump seq (release+barrier) THEN read park-intent
u32 seq = __atomic_add_fetch(&rh->ready_seq, 1u, __ATOMIC_SEQ_CST);  // poke
bool armed = __atomic_load_n(&rh->wait_active, __ATOMIC_SEQ_CST);    // wake?
return armed && wseq != seq;
```

Each side does a **seq-cst store then a seq-cst load on opposite words** — the
StoreLoad barrier the SB litmus needs (the Linux `set_current_state()` +
`smp_mb()` before a cond re-check). In the global seq-cst order, either the
producer's store-`ready_seq` precedes the consumer's load-`ready_seq` (the
consumer sees the new edge → doesn't park) **or** the consumer's
store-`wait_active` precedes the producer's load-`wait_active` (the producer
sees armed → wakes). At least one holds; no edge in the window is lost. This is
`specs/weft_readiness.tla`'s `NoLostReadyWake` (I-9). The live Weft-6 wiring
*additionally* serializes the park-decision and the wake under the consumer's
Rendez lock (the #811 `wait_lock` register-then-observe) — both are sound
realizations of the one atomic discipline the spec models.

The producer never writes `wait_active` (single-writer-per-word): on a wake it
issues the Rendez wakeup (re-scheduling the parked consumer), and the consumer
clears its own `wait_active` on resume (`weft_ready_unpark`). A stale
`wait_active` (set while the consumer is actually running) at most costs a
redundant — harmless — wakeup, never a lost one. The `wait_seq` lets the producer
skip even that redundant wakeup (it wakes only for a consumer parked at a seq the
new edge passed).

What Weft-4 is **not**: the live park/wake. `weft_ready_arm_park` returns the
*decision* to park; the actual `sleep(&rendez)` on a true return, and the
`wakeup(&rendez)` on a true `weft_ready_signal`, are the Weft-6 wiring (per
direction: netd→guest RX-ready, guest→netd TX-queued).

---

## F_NOTIF zero-copy-send completion (Weft-5)

A zero-copy send completing means only **queued**: the registered payload page is
still *in flight* — the NIC may DMA from it, and for TCP it stays pinned until the
peer ACKs (the stack may retransmit). So a Weft ZC send posts **two** CQEs (the
io_uring `IORING_SEND_ZC` contract; NET-THROUGHPUT.md §4.6): a **result CQE**
(`LOOM_CQE_MORE` = "queued", a notification follows) and, later, a **notification
CQE** (`LOOM_CQE_F_NOTIF` = "buffer reusable"). **The I-30 buffer pin releases at
the *notification*-terminal — the last of {netd stack done, NIC DMA done, peer
ACK} — never at op-terminal.** Releasing at op-terminal (netd's stack done = the
result CQE) reuses a page a holder may still be reading: the io_uring `ubuf_info`
UAF (`weft.tla` `PinHeldWhileInFlight` + `NoInFlightReuse`).

`struct weft_notif` is the **kernel-private** per-send in-flight tracker (the
spec's `holders[b]`): not a shared-page structure, not ABI — the guest sees only
the two CQEs. The holder bitmask alone is the complete state (in flight iff
`holders != 0`); no `armed`/`terminated` flag is needed. The release gate
(`weft_notif_clear` → `WEFT_NOTIF_RELEASE`) fires **exactly once**, on the
last-holder transition, so a premature drop is *structurally unreachable* through
the API: there is no function that returns `RELEASE` or drops the pin while a
holder is pending, and a clear of an already-clear holder (a stray/duplicate/late
event) is a no-op returning `HELD`. A caller that releases the pin **only** on
`RELEASE` therefore cannot reuse an in-flight page — the spec's `ReleasePremature`
action has no impl path. The live tracker rides the `loom_async_op` (the audited
Loom completion state, under the engine lock) at Weft-6, where the two CQEs are
posted onto the flow's Loom ring.

The **fallback-copied indicator** (`WEFT_NOTIF_COPIED`, the
`IORING_SEND_ZC_REPORT_USAGE` analog): if the send falls back to a copy (the
payload is copied into a netd-side buffer), the page is free *immediately* — no
in-flight window — and the result CQE carries `WEFT_NOTIF_COPIED` so netd is never
silently-wrong about whether the buffer is in flight.

### Public API

```c
// The F_NOTIF in-flight holder set (weft.tla Holders). A zero-copied page is held
// until ALL THREE clear; each is a distinct completion event.
#define WEFT_HOLDER_NETD 0x1u   // netd's stack still references the page
#define WEFT_HOLDER_NIC  0x2u   // the NIC may still DMA from the page
#define WEFT_HOLDER_ACK  0x4u   // the peer has not yet ACKed (TCP may retransmit)
#define WEFT_HOLDERS_ALL (WEFT_HOLDER_NETD | WEFT_HOLDER_NIC | WEFT_HOLDER_ACK)
#define WEFT_NOTIF_COPIED 0x1u  // result-CQE indicator: the send fell back to a copy

enum weft_notif_phase { WEFT_NOTIF_HELD = 0, WEFT_NOTIF_RELEASE };

struct weft_notif { u32 holders; u32 result_flags; };

// Arm for a zero-copied send: in flight under `holders` (masked to WEFT_HOLDERS_ALL
// -- an over-wide arg cannot manufacture a phantom holder that clear() can never
// reach). weft.tla NetdAct.
void weft_notif_arm(struct weft_notif *n, u32 holders);

// Arm for a COPIED send: free immediately (no in-flight window), result_flags =
// WEFT_NOTIF_COPIED; weft_notif_inflight is false at once.
void weft_notif_arm_copied(struct weft_notif *n);

// Clear one holder (a netd-stack / NIC-DMA / peer-ACK event; one WEFT_HOLDER_*
// bit). Returns WEFT_NOTIF_RELEASE iff this emptied the set -- release the pin +
// post the notification CQE (EXACTLY ONCE; weft.tla HolderRelease -> ReleaseClean).
enum weft_notif_phase weft_notif_clear(struct weft_notif *n, u32 holder);

// PinHeldWhileInFlight query: true iff a holder is still pending -- the pin must
// stay held + the page must NOT be reused (weft.tla NoInFlightReuse).
bool weft_notif_inflight(const struct weft_notif *n);

// The result-CQE flags (WEFT_NOTIF_COPIED or 0) -- the never-silently-wrong
// fallback indicator, OR'd into the result CQE.
u32 weft_notif_result_flags(const struct weft_notif *n);
```

These functions are **not internally synchronized**: the caller serializes them
(the live tracker is per-op state mutated under the Loom engine lock at Weft-6,
exactly as `weft.tla` models each action atomic under the per-client lock). No
allocation, no atomics, no shared memory — pure bitmask state transitions.

What Weft-5 is **not**: the live two-CQE posting. Nothing in production arms a
`weft_notif` yet — the holder lifecycle is exercised by the kernel test suite. The
flow's ZC send (the `weft_notif` on its `loom_async_op`, the result + notification
CQEs onto its Loom ring, the holders driven by netd-stack / NIC-DMA-done /
peer-ACK events) lands at Weft-6.

---

## Spec cross-reference

`specs/weft.tla` (model-first at Weft-1; ARCH §28 I-37). Weft-3 (the descriptor
ring) **and** Weft-5 (the F_NOTIF holder lifecycle) realize:

| weft.tla action / invariant | impl |
|---|---|
| `GuestPostDesc` / `GuestMutateDesc` | the guest writes/mutates `darr[pos]` + release-bumps `prod_tail` (Weft-6 guest; the test producer) |
| `Consume` (copy + bounds-validate) | `weft_ring_drain` (the snapshot + `weft_desc_valid`) |
| `DescPinnedToSnapshot` | the value-copy `kd`; act on `out[]`, never the shared slot |
| `ActedDescValidated` | `weft_desc_valid` (u64 bound; flags clear; len > 0) |
| split-ring leg (5) | `prod_tail` guest-only / `cons_head` kernel-only |
| `NetdAct` (arm the in-flight holder set) | `weft_notif_arm` (`holders := WEFT_HOLDERS_ALL`) |
| `HolderRelease(b, h)` | `weft_notif_clear` (clear one holder bit) |
| `ReleaseClean` (pin drop at notification-terminal) | `weft_notif_clear` → `WEFT_NOTIF_RELEASE` (exactly once, on `holders → 0`) |
| `PinHeldWhileInFlight` | `weft_notif_inflight` (the reuse guard; `holders != 0`) |
| `ReleasePremature` (the F_NOTIF UAF) | **no impl path** — RELEASE fires only on the last-holder transition |

The buggy cfgs `weft_buggy_ring_toctou` → `DescPinnedToSnapshot` (the descriptor
ring) and `weft_buggy_premature_release` → `PinHeldWhileInFlight` (the F_NOTIF
op-terminal-drop UAF) are the durable regressions for these surfaces (the Weft-7
audit). The spec gate re-ran green at Weft-5 (clean 1412 distinct; each buggy cfg
violates its named invariant; liveness `EventuallyReleased` green); Weft-3 and
Weft-5 realize the modeled mechanism without changing the spec.

`specs/weft_readiness.tla` (model-first at Weft-4; ARCH §28 I-9). Weft-4
realizes (the readiness ring is a *new* I-9 surface — a new focused module,
leaving the audited `weft.tla` untouched, the `cons_poll.tla` / `net_poll.tla`
precedent):

| weft_readiness.tla action / invariant | impl |
|---|---|
| `ProducerEdge` (bump + wake-decision) | `weft_ready_signal` (seq-cst bump-then-load) |
| `ConsumerProcess` (fast-path drain) | `weft_ready_observe` (acquire-load) + the caller |
| `ConsumerPark` (register-then-observe) | `weft_ready_arm_park` (seq-cst store-then-load) |
| `NoLostReadyWake` (I-9) | the SB barrier on both halves; no parked consumer with an unprocessed edge |
| `EventuallyDrained` (liveness) | the poke wakes the parked consumer (Weft-6 Rendez) |

The buggy cfg `weft_readiness_buggy_lost_wake` → `NoLostReadyWake` (the
observe-before-arm inversion) is the durable regression for the readiness ring
(the Weft-7 audit). Spec gate at Weft-4: clean + liveness green, the buggy cfg
violates `NoLostReadyWake`.

---

## Tests

`kernel/test/test_weft_ring.c` — 12 tests (`weft.*`); the ring tests run over a
`burrow_share_into` cross-Proc shared page, the F_NOTIF tests over the
kernel-private tracker. The descriptor ring (Weft-3):

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

The readiness ring (Weft-4) — the primitives over the shared page; the SB
hardware reordering itself is the spec's + the seq-cst code's proof (a single-
threaded test cannot reorder), so these assert the primitives' contracts:

- `weft.ready_signal_observe` — the producer bumps the edge counter + publishes
  the mask; the consumer observes the new seq + mask over the shared page (the
  cache-line read); a not-armed consumer is no wake.
- `weft.ready_park_handshake` — the consumer (caught up) arms a park; a producer
  edge then finds it armed and reports a wake is needed (the no-lost-wake: the
  poke wakes the parked consumer); the consumer un-parks + observes the edge.
- `weft.ready_arm_park_sees_race` — the register-then-observe re-check: an edge
  posted *before* the arm is caught by the post-register re-read, so the consumer
  does not park (it re-processes) and the park-intent is left clear.

The F_NOTIF completion contract (Weft-5) — pure struct-level tests of the holder
lifecycle (the tracker is kernel-private completion state, not a shared page):

- `weft.notif_terminal_release` — arm `{netd,nic,ack}`; clear each holder in a
  non-trivial order; the page stays in flight (the pin held) through each non-last
  clear, and the last clear yields `WEFT_NOTIF_RELEASE` **exactly once** (the
  notification-terminal); a stray late event after the terminal is a `HELD` no-op
  (no double-release).
- `weft.notif_premature_blocked` — the `ubuf_info` UAF, structurally prevented:
  after netd's stack is done (op-terminal) the page is **still** in flight, so a
  reuse gating on `weft_notif_inflight()` is blocked; only the *true* last holder
  (the peer ACK) yields `RELEASE`; a duplicate netd-done cannot fake the terminal
  (`PinHeldWhileInFlight` + `NoInFlightReuse`).
- `weft.notif_copied_immediate` — the fallback-copied path: `arm_copied` →
  not-in-flight at once + `WEFT_NOTIF_COPIED`; plus the arm-domain masking (an
  over-wide `holders` masks to the three real holders; an empty arm is
  not-in-flight).

949/949 PASS, boot OK, 0 EXTINCTION at landing.

---

## Error paths

- `weft_ring_layout` → `-1` on: NULL base / view, `ring_entries` zero or not a
  power of two or over `WEFT_MAX_ENTRIES`, no room for a non-empty payload
  region, a payload region larger than the u32 descriptor domain. Writes nothing
  on failure.
- `weft_ring_drain` → `0` on NULL `rv`/`out` or `max <= 0` (no side effects). A
  rejected descriptor is dropped (counted), not an error return.
- The readiness primitives take a valid `rv` (constructed by `weft_ring_layout`
  at grant) and do not validate it — they are internal substrate the Weft-6 grant
  path drives, not an EL0 entry point. `weft_ready_signal` returns a bool
  (wake-needed), `weft_ready_arm_park` a bool (safe-to-park); neither has an
  error return.

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
  last of {netd stack, NIC DMA, peer ACK}) is the `weft_notif` tracker (Weft-5).
  The contract is structurally enforced (`RELEASE` only on the last-holder
  transition), but nothing in production arms a tracker yet: the **live two-CQE
  posting** — the `weft_notif` on the flow's `loom_async_op`, the result +
  notification CQEs, and the holders driven by real netd-stack / NIC-DMA-done /
  peer-ACK events — is Weft-6. Until then a caller must still honour the contract:
  release the pin **only** on `WEFT_NOTIF_RELEASE`, never on the result CQE.
- **`weft_notif` is not internally synchronized.** The caller serializes the
  arm/clear/query (the live tracker is per-op state under the Loom engine lock at
  Weft-6). A `clear` takes one `WEFT_HOLDER_*` bit; the impl is robust against
  out-of-domain bits (they no-op), but a multi-bit `holder` clears multiple — pass
  one event per call.
- **The readiness park/wake is not live.** `weft_ready_arm_park` returns the
  *decision*; the `sleep(&rendez)`/`wakeup(&rendez)` integration (per direction)
  is Weft-6. Until then nothing in production calls the readiness primitives —
  they are exercised by the kernel test over a `burrow_share_into` page.
- **`ready_seq` / `wait_seq` are free-running u32 counters.** Wraparound is
  astronomically far (one increment per readiness edge); the `!=` comparisons
  (not `<`) are wrap-immune by construction (a parked-at seq differs from the
  current after any edge).
- **The descriptor-ring `weft_ring_hdr` co-locates `prod_tail` (guest) and
  `cons_head` (kernel) on one cache line** (a Weft-3 ABI, unchanged). They are
  written by different parties, so that line false-shares under a live
  producer↔consumer — a v1.x layout refinement (split onto separate lines, the
  way the Weft-4 `weft_ready_hdr` already separates its two writers). Correctness
  is unaffected (single-writer-per-word holds); only the ping-pong cost. The
  readiness ring, the latency-critical path, is already split.
