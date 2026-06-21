# 125 — Weft: the per-flow capability network dataplane (descriptor + readiness rings + F_NOTIF)

**Status:** Weft-3 (the descriptor-ring ABI + the kernel snapshot-and-bounds-
validate consumer + the hybrid threshold), Weft-4 (the readiness ring — the
single-cache-line poke + the store-buffer register-then-observe), **and** Weft-5
(the F_NOTIF two-CQE zero-copy-send completion contract — the multi-holder
buffer-pin release) substrates landed. Kernel-internal; no EL0 ABI. The live
guest↔netd wiring + the EL0 share delivery is Weft-6 — begun at **Weft-6a-1**:
the `Tweft(F) → Rweft(share_id, geom)` 9P op + `p9_client_weft` (the lazy
fid-keyed ring-setup request; see `docs/reference/44-9p-wire.md` /
`45-9p-session.md` / `47-9p-client.md`). **Weft-6a-2** landed the kernel ABI: the
`SYS_WEFT_SHARE` (81) / `SYS_WEFT_MAP` (82) syscalls + the kernel-scoped
`share_id` registry + the `dev9p_priv` ring binding (see "The EL0 delivery"
below). **Weft-6b-1** landed the netd side — the `h_weft` handler that allocates +
registers a per-flow ring and answers `Rweft`, making the grant-is-the-share
mapping LIVE end to end (proven in-guest: a real `SYS_WEFT_MAP` round-trip maps a
ring whose geometry mirror is visible; see "The netd ring register" below). The
**Weft-6b-2** lands the live TX data drive — the `Tweftio`/`Rweftio` op
(6b-2a) + the kernel write fast-path + the binding ring view + netd's
`h_weftio` (6b-2b): a large write whose buffer points into a flow's shared ring
moves zero-copy (see "The data drive" below), proven in-guest by `net-echo`'s
`weft_tx_e2e`. **Weft-6b-3a** lands the symmetric RX direction (`Tweftio`
`dir=READ` + the kernel read fast-path + netd's recv-into-ring + the blocking
defer), proven by `weft_rx_e2e`. **Weft-6c (DESIGN, 2026-06-20)** folds the
standalone readiness park/wake + F_NOTIF legs into one coherent native-async
chunk: the native `libthyla_rs::net` push/pop/wait API **rides Loom** (`pop` =
`LOOM_OP_READ` / `push` = `LOOM_OP_WRITE` routed to the weft fast-path; `wait` =
`SYS_LOOM_ENTER` — no new wait/wake syscall), `push` posts the live F_NOTIF
two-CQE on the op's `loom_async_op.notif`, and the readiness ring is the
syscall-free busy-poll edge (its `arm_park`/Rendez leg stays validated-not-wired).
Sub-split 6c-1 (kernel weft-Loom routing + live F_NOTIF) / 6c-2 (native API +
netd readiness edge + in-guest E2E) / 6c-3 = Weft-7 (audit + SMP + bench). The
readiness ring removes the **guest-wake** round-trip; the measured ~50 ms
**netd-notice** floor (NET-PERF N1) is the orthogonal pollable-NIC-IRQ-fd chunk,
not the readiness ring (NET-THROUGHPUT §5.4). See `docs/NET-THROUGHPUT.md` §6.

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

## The EL0 delivery — the `share_id` registry + the per-flow binding (Weft-6a-2)

Weft-6a-2 is the kernel half of **grant-is-the-share**: the two syscalls that
wire a `/net` flow's shared ring into a guest, plus the kernel-scoped `share_id`
registry that joins the two sides. The wire op (`Tweft`/`Rweft` + `p9_client_weft`)
landed at Weft-6a-1; the netd `Tweft` handler + the live ring drive are Weft-6b;
the native `libthyla_rs::net` API + the in-guest E2E are Weft-6c.

### The mechanism

A flow goes zero-copy lazily, on its first large transfer (the §4.8 hybrid
threshold). The guest's native client calls `SYS_WEFT_MAP(data_fd)`; the kernel:

1. resolves `data_fd → (p9_client, fid F)` via `dev9p_client_fid`,
2. issues `Tweft(F)` on the shared `/net` 9P client; netd allocates + initializes
   the per-flow ring in **its own** address space (`SYS_BURROW_ATTACH` +
   `weft_ring_init_hdr` + `weft_ready_init_hdr`), registers it with
   `SYS_WEFT_SHARE` → a `share_id`, and returns `Rweft(share_id, geom)`,
3. **claims** the `share_id` (consume-once), `burrow_share_into`s the ring into
   the guest's burrow-attach window, and records the binding in the data Spoor's
   `dev9p_priv->weft`.

The `share_id` is the join key. It travels **netd → kernel** inside the kernel's
own `Tweft` round-trip and is **never handed to the guest** — the RDMA-`rkey`
shape (§4.6). So a guest cannot forge a mapping: the only `share_id` the kernel
honours is the one its own `Tweft` elicited.

### The `share_id` registry (`weft.c`)

A fixed 64-entry table (`g_weft_shares`) under a leaf spinlock (`g_weft_lock`),
keyed by a monotonic `u64` (never 0). Occupancy is normally ~0: an entry lives
only for the `Tweft` round-trip window, since the kernel's `SYS_WEFT_MAP` claims
it in the same call.

| function | role |
|---|---|
| `weft_share_register(owner, v)` | take the I-30 **registration pin** (`burrow_ref`) + mint a `share_id`. Returns 0 on a full table (the flow stays byte-copy) — the pin is taken **before** the lock and dropped on a full table, so `g_weft_lock` nests no other lock. |
| `weft_share_claim(share_id)` | consume-EXACTLY-once: remove the entry, **transfer** the registration pin to the caller, return the Burrow (NULL on a replayed / forged / already-claimed id). |
| `weft_share_release_owner(owner)` | GC every un-claimed share owned by a dying netd (drops each pin) — the `ShareBoundedByFlow` backstop. Called from `proc.c` exit-notify, alongside `srv`/`cap_proc_exit_notify`. |

### The lifetime — registration pin vs guest mapping

The ring Burrow is held alive by the **#847 dual refcount**, split across two
owners that drop independently (free iff **both** reach 0, in any order):

- **The registration pin** (`handle_count`) is taken at `SYS_WEFT_SHARE`,
  transferred to the `weft_binding` at the claim, and dropped at `dev9p_close`
  (`weft_binding_release → burrow_unref`). It bridges the netd↔kernel correlation
  window — even if netd unmaps its own copy, the pin keeps the ring alive until
  the kernel maps it.
- **The guest's ring mapping** (`mapping_count`) is owned by the guest's VMA and
  reclaimed by `vma_drain` at guest exit — **the Loom-ring precedent**
  (`loom_free` drops only its handle ref; the guest mapping rides `vma_drain`).
  `dev9p_close` deliberately does **not** unmap the guest's ring: doing so would
  need the guest Proc pointer at close time, which is unsafe if the data fd was
  `handle_dup`'d to a now-dead child. So the binding drops only the pin; the
  mapping is bounded by the guest's address-space lifetime.

A flow-close while the guest lives therefore leaves a benign stale mapping (the
guest's own ring, no longer driven by netd) until the guest exits — exactly the
semantics a closed `loom_fd`'s ring has.

### Concurrency (`SYS_WEFT_MAP` on a shared data fd)

A data fd is `handle_dup`-shareable, so two threads of a multi-thread guest can
race `SYS_WEFT_MAP` on it. `priv->weft` is therefore accessed via `__atomic`
(load-acquire on the idempotent fast path; a CAS-acq/rel at install;
`dev9p_close` reads it plainly at the last ref, where no mapper remains). The
install is a compare-exchange: if a concurrent map won (its own ring, a distinct
`share_id`), the loser tears its ring down (`burrow_unmap` + `weft_binding_release`)
and returns the winner's cached VA. If netd returned the **same** `share_id`
idempotently, the second claim returns NULL and the kernel returns the winner's
VA from `priv->weft` (or a transient `-1` if the winner has not yet installed —
a benign retryable spurious failure, the v1.0 seam).

### Tests + what's deferred

`weft.share_register_claim` / `weft.share_full` / `weft.share_owner_gc` /
`weft.syscall_share` / `weft.map_binding_lifetime` exercise the registry
(register/claim/consume-once/owner-GC/full-table), the `SYS_WEFT_SHARE` handler
(ANON + RW + whole-ring validation; the rejects), and the binding lifetime (the
pin held by the binding, the mapping reclaimed by `vma_drain`, the cross-Proc
#847 free-on-last). The **full live `SYS_WEFT_MAP` E2E** (issuing a real `Tweft`
to netd's handler) needs Weft-6b/6c and is owed there — the same impl-against-
existing-spec posture as Weft-2..5. The `Tweft`/`share_id` correlation (consumed-
exactly-once, no cross-flow mis-binding, the netd-pin GC if the guest dies
pre-map) is the **Weft-7** focused-audit prosecution surface.

---

## The netd ring register — the live `Tweft` handler (Weft-6b-1)

Weft-6b-1 lands the netd half that makes grant-is-the-share LIVE: when a guest's
first zero-copy use of a flow drives `SYS_WEFT_MAP`, the kernel issues `Tweft(F)`
on the shared `/net` dev9p client; netd's 9P server answers `Rweft(share_id,
geom)`; the kernel claims the share + maps the ring into the guest. Pure
userspace (`usr/netd`, `usr/lib/libthyla-rs`); the kernel is byte-unchanged
(Weft-6a-2 already landed every kernel-side step).

### The handler (`usr/netd/src/server.rs::Conn::h_weft`)

Dispatched from the 9P serve loop on `p9::P9_TWEFT` (134), alongside the other
kernel-client-issued ops (`Tflush`, the #845 precedent). It:

1. `p9::parse_tweft` → the fid `F`.
2. Resolves `F → connection N` via the server fid table (`fid_find` →
   `conn_n(path)`), and **fail-closes** unless `F` is an *opened* `/net/<proto>/N/
   data` fid of a *live* slot (`f.opened && is_conn && conn_filekind == FK_DATA &&
   slot_live`). A weft ring is a per-flow DATA channel; a `Tweft` on a ctl /
   listen / dir fid, or a dead slot, returns an `Rlerror` so the guest's
   `SYS_WEFT_MAP` returns `-1` and the flow stays on the byte-copy path. (Defence
   in depth: the kernel issues `Tweft` only on a dev9p fd, but a non-`/net` dev9p
   server simply does not handle op 134 and rejects it — the tagged #841 elected
   reader matches the `Rlerror` by tag, no desync.)
3. `Net::weft_ensure(N)` → the flow's `share_id`, embedded in `Rweft` with the
   geometry (`WEFT_RING_SIZE` / `WEFT_RING_ENTRIES`).

### The per-flow ring (`Net::weft_ensure` + `Slot::weft`)

One ring + one `share_id` per connection `N`, recorded in the connection slot
(`Slot.weft: Option<WeftFlow { ring_va, ring_size, share_id }>`), allocated lazily
on the FIRST `Tweft`. `weft_ensure`:

- returns the stored `share_id` if the flow already has a ring (**idempotent** — a
  repeat `Tweft`, e.g. from a concurrent multi-thread first-MAP race, re-returns
  the same id; the kernel's claim consumed it on the first map, so `weft_map_-
  claimed` then falls back to the data fd's cached binding — the designed
  consumed-`share_id` path);
- else `SYS_BURROW_ATTACH`es a 256 KiB / 64-entry ANON ring (RW, demand-zero —
  satisfies the kernel's ANON + RW-only + whole-ring check; page-aligned so a
  later detach matches), writes its geometry mirror + zeroes the readiness header
  (`libthyla_rs::weft::init_ring` — the shared ABI mirror, the `loom.h ↔
  libthyla_rs::loom` precedent), then `SYS_WEFT_SHARE`es it to take the kernel's
  I-30 registration pin + mint the `share_id`. Any failure (OOM, bad layout, full
  registry) returns `None` → the flow stays on the byte-copy path: **a denied ring
  is never fatal** (the hybrid threshold's whole point).

### Teardown (the netd half of `ShareBoundedByFlow`)

`slot_unref` (the connection's only free path) detaches netd's own ring mapping
(`SYS_BURROW_DETACH`, dropping netd's #847 *mapping* ref) when the last fid clunks.
The *registration pin* (the #847 *handle* ref) transferred to the kernel at the
guest's claim and is dropped at the guest's `dev9p_close`; the ring Burrow frees
when both reach 0, in any order. netd is single-threaded, so `Slot.weft` needs no
lock (it is touched only in the serve loop).

### The ABI mirror (`usr/lib/libthyla-rs/src/weft.rs`) + the 9P codec

`libthyla_rs::weft` mirrors the on-page ring ABI (`WeftRingHdr` 64 B / `WeftReady-
Hdr` 128 B / `WeftDesc` 16 B, `repr(C)` byte-pinned to `weft.h` by `const _:
assert!(offset_of!…)`), plus `ring_layout` / `init_ring`. `libthyla_rs::ninep`
gains `P9_TWEFT`/`P9_RWEFT` + `parse_tweft` / `build_rweft` (+ `build_tweft` /
`parse_rweft` for symmetry), mirroring `kernel/9p_wire.c` byte for byte. Both are
the shared substrate the Weft-6c native client reuses.

### Proof + what's deferred

The live round-trip is gated at boot by `net-echo`'s `weft_e2e` (`usr/net-echo`):
over netd's resident loopback it bind/connect/accepts a flow, `SYS_WEFT_MAP`s the
client's data fd, and asserts a real ring VA comes back, the ring header's magic +
entry count are visible in the shared mapping, and a second map is idempotent
(same VA, no second `Tweft`) — printed as `net-echo: weft-6b MAP E2E PASS`. The
soak's leak-baseline (run right after) confirms the ring + connection teardown
leaves no leak. The live DATA DRIVE (the descriptor ring on large Twrite/Tread,
the readiness park/wake, the F_NOTIF two-CQE posting) is Weft-6b-2/6b-3; the
`Tweft`/`share_id` correlation + the unclaimed-pin GC (a guest that dies between
`Tweft` and the kernel claim leaks the registration pin until netd's
owner-death GC — a narrow window, since the claim follows `Tweft` in the same
`SYS_WEFT_MAP`) are the **Weft-7** prosecution surface.

---

## The data drive — `Tweftio` (Weft-6b-2 TX / Weft-6b-3 RX)

Weft-6b-2/6b-3 make the *data* live: a large payload travels through the per-flow
ring instead of the 9P body, in **both directions**. The wire mechanism
(user-voted 2026-06-20, NET-THROUGHPUT §6.2) is a new Thylacine-private op,
**`Tweftio`** with a `dir` field (`WEFT_DIR_WRITE` / `WEFT_DIR_READ`) — the
kernel stays the I-30 validator-once and a 9P op still mediates every transfer
(§4.8). The TX direction landed at 6b-2; the RX direction is its symmetric twin
(6b-3a) — the validation, the I-30 pin, and the fast-path shape are identical, so
the validator is direction-agnostic (`weft_binding_validate_rw`).

### The kick + the kernel validator (`6b-2b`)

The kick is the **existing `SYS_WRITE`** (no new syscall; the virtio-9p
`p9_client_zc_rpc` shape): the native client writes the payload into the ring's
payload region (its own RW mapping) and issues `SYS_WRITE(data_fd, ring_va +
payload_off + O, L)` — the buffer points *into* the bound ring.
`sys_write_handler`, gated on `len >= WEFT_HYBRID_THRESHOLD` so small writes are
unaffected, runs `sys_write_weft_fastpath` → `dev9p_weft_try_write`:

- `weft_binding_validate_rw` derives the descriptor `{O = buf - (guest_va +
  payload_off), L}` from the **trusted, register-passed syscall args** and
  bounds-checks it against the binding's kernel-private `weft_ring_view` (the
  same `weft_desc_valid` gate as the descriptor ring — but sourced from a
  trusted arg, so inherently free of the descriptor-ring TOCTOU);
- on success it issues `p9_client_weftio(off=O, len=L, dir=WRITE)` and returns
  the moved-byte count; a small write, a buffer outside the ring, or a no-binding
  fd falls through to the byte-copy path (the hybrid threshold).

**The RX fast-path (`6b-3a`)** is the symmetric twin: `sys_read_handler` →
`sys_read_weft_fastpath` → `dev9p_weft_try_read` runs the **same**
`weft_binding_validate_rw` (the descriptor names a `[off, off+len)` window either
way) and issues `p9_client_weftio(dir=READ)`. The one structural difference:
because netd writes the recv'd bytes **directly into the guest's shared ring**,
the read handler does **no `uaccess_store`** copy-out — the guest reads the bytes
from its own ring mapping (the true zero-copy property). The `p9_client_weftio`
call **blocks** until netd replies (the #841 elected reader, death-interruptible,
no per-op deadline), so RX inherits the blocking semantics free from the
synchronous client + netd's deferred reply (below).

Because the fast-path bypasses the `SYS_RW_MAX` (4 KiB) scratch bounce, a single
weft write/read moves up to `payload_size` (the ring is the bound) — the
byte-copy cap does not apply to the zero-copy path.

The binding's **`weft_ring_view`** is computed once at `SYS_WEFT_MAP`
(`weft_binding_alloc` → `weft_ring_layout` from the Burrow's contiguous KVA +
the `Rweft`-reported `ring_entries`) — the trusted geometry the validate reads,
never the guest-mutable shared header mirror.

### netd reads/writes in place (`h_weftio`)

`usr/netd/src/server.rs::h_weftio` (dispatched on `P9_TWEFTIO`) resolves
`fid → connection N` (the same fail-closed gate as `h_weft`), validates the dir,
and re-bounds `[off, off+len)` against its own ring geometry (defence in depth —
a memory bound, not a per-op capability re-check; a malformed window is
`E_INVAL`, never silently treated as EOF). Then:

- **TX (`WEFT_DIR_WRITE`):** reads `ring_va + payload_off + off` for `len` **in
  place** (no 9P-body copy), hands it to smoltcp (`data_send`), replies
  `Rweftio(count)`.
- **RX (`WEFT_DIR_READ`, 6b-3a):** recvs from the socket **directly into** the
  ring at `[off, off+len)` (`weft_recv_into_ring` → `data_recv_outcome` over the
  in-place `&mut` slice). Bytes now → reply the count (already in the guest's
  shared mapping). An **empty-but-open** socket **DEFERS** — netd parks a
  `PendingWeftRead` and returns `Disp::Deferred` (no reply), so the kernel
  client's `p9_client_weftio` blocks on the held tag; `poll_weftio` (the
  serve-loop pass, after `net.poll`) recvs into the ring + delivers the held
  `Rweftio` when bytes arrive (or `0` on EOF). This is the weft twin of the
  net-6a-1 byte-copy `h_read`/`poll_data` defer — without it a 0-byte reply would
  look like EOF to the guest's `recv()`. The defer's I-9 no-lost-wake holds the
  same way: netd is single-threaded, so no readiness edge is lost between the
  empty-observe and the park; the `PendingWeftRead` is cancelled on its data
  fid's clunk / a `Tflush` on its tag / teardown / `Tversion`, exactly like
  `PendingRead` (a missed cancel = a stranded held tag).

The ring→smoltcp-TX copy (and the RX recv-into-ring copy) is netd-local; the true
zero-copy-to-NIC (no intermediate copy) is the F_NOTIF send, Weft-6b-3c/Weft-5.

### The synchronous path vs the batched descriptor ring

6b-2 drives the **synchronous** path — one descriptor per `SYS_WRITE`, the
buf-in-ring *is* the descriptor (no shared-ring slot to snapshot). The
descriptor RING + `weft_ring_drain`'s snapshot discipline is the **batched/async**
submission the native `libthyla_rs::net` API drives through Loom (Weft-6c). Both
validate through `weft_desc_valid` against the kernel view.

### The Loom data drive — `LOOM_OP_READ`/`WRITE` → `Tweftio` (Weft-6c-1)

The native async API rides Loom (NET-THROUGHPUT §4.4/§6): a `pop` is a
`LOOM_OP_READ`, a `push` is a `LOOM_OP_WRITE`, and the park is the existing
`SYS_LOOM_ENTER` CQ wait — **no new wait/wake ABI**. `kernel/loom.c::loom_submit_payload`
detects the zero-copy case at submit: a READ/WRITE whose pinned `/net` data fid
carries a weft binding (`dev9p_priv->weft`, read `__atomic`-acquire) **and** whose
registered Loom buffer **is that flow's whole shared ring** (`buf == wb->burrow &&
buf_reg_len == wb->ring_size`) routes to `loom_build_weftio` (a `Tweftio` off/len/dir
descriptor) instead of the byte-copying `loom_build_read`/`write`. The whole-ring
registration makes the SQE's `buf_off` ring-base-relative, so the slice runs the
**same** validator the synchronous `dev9p_weft_try_rw` uses (`weft_binding_validate_rw`,
on `wb->guest_va + buf_off` — the `guest_va` cancels, so only the offset *within*
the ring is load-bearing). A non-ring buffer (or a partial-ring registration) on a
weft fid falls through to the byte path — the §4.8 hybrid. An in-ring slice outside
the payload region is rejected `-EINVAL`. The completion copies **nothing**
(`loom_payload_result`, the `op->weft` branch: result = the `Rweftio` count, no
`loom_bufcopy`) — *that* is the zero-copy property: a READ's bytes are already in
the guest's ring slice (netd placed them there in place), a WRITE's were read out
of it. I-30 holds: the offset is computed from the SQE snapshot + the kernel-private
`wb->view`, never re-read from the shared ring; the ring Burrow is pinned (the
audited #847 payload-buffer `burrow_ref`) from submit to reap.

**The F_NOTIF realization** (the honest v1.0 vs v1.x split): a weft WRITE is a
zero-copy *send*. At v1.0 netd's `h_weftio` COPIES the ring into its socket buffer,
so the slice is reusable the instant `Rweftio` returns — the io_uring SEND_ZC
"copied" path: a **single terminal CQE, no `LOOM_CQE_MORE`**, is the reusability
signal (the consumer reuses the slice on that CQE). The deferred two-CQE
(result+`MORE`, then a `LOOM_CQE_F_NOTIF` CQE on the last `{netd,NIC,ACK}` holder
release) is the **v1.x** true-zero-copy seam — it needs a netd-holds-the-page TX
path + a netd→kernel holder-clear channel, neither of which exists at v1.0. The
`weft.tla` model already covers both (the copied case = `weft_notif_arm_copied` →
not-in-flight; the deferred case = `HolderRelease`/`ReleaseClean`), so no spec
changes; the buggy cfgs re-run green.

### The native `WeftFlow` push/pop/wait API (Weft-6c-2)

`libthyla_rs::net::WeftFlow` is the native, Demikernel-shaped consumer of the
6c-1 routing — **pure userspace; the kernel is byte-unchanged** (a weft flow is
just a Loom ring whose registered buffer is a `SYS_WEFT_MAP`'d region, which the
kernel already detects). `WeftFlow::open(&TcpStream)` does the four-step setup:
`SYS_WEFT_MAP(data_fd)` → `ring_va`; `weft::read_ring_geom(ring_va)` reads the
geometry mirror netd wrote (magic-validated; `ring_size = payload_off +
payload_size`, the whole-Burrow size the routing requires the registered length
to equal exactly); `Ring::setup(8, 0)`; then `register_handles(&[data_fd])`
(handle 0 = the I-30 fid pin) + `register_buffers(&[{va: ring_va, len:
ring_size}])` (buffer 0 = the whole shared ring). The trio:

- **`push(len)`** stages a `LOOM_OP_WRITE` SQE (`buf_off = payload_off`, `len`)
  into the SQ — the caller has filled the ring's payload region via `tx_buf()`
  (true zero-copy, no app→ring copy), or via the `send(data)` convenience that
  copies once into the ring (the io_uring registered-buffer fill).
- **`pop(max)`** stages a `LOOM_OP_READ` of up to `max` bytes into the ring.
- **`wait(ticket)`** does the io_uring submit-and-wait (`enter(1, 1,
  GETEVENTS)`) — the staged SQE is submitted and the call parks on the **Loom CQ
  wait** (NET-THROUGHPUT §6: "the consumer's park *is* the Loom CQ wait, not a
  separate weft Rendez"; death-interruptible, no per-op deadline). The terminal
  CQE's result is the byte count; a pop's bytes are read zero-copy from `rx_buf()`.

**Single-in-flight.** The shared ring's payload region is one buffer, so exactly
one op (push XOR pop) is in flight at a time — a second push/pop before its
`wait` returns `Error::WouldBlock`. The TX/RX region must not be mutated while an
op is in flight; the borrow checker enforces it (`tx_buf`/`rx_buf` borrow `self`,
the trio takes `&mut self`, no ring slice outlives a submit), and `WeftFlow` is
`!Send`+`!Sync` (the raw `ring_va`) so it cannot race across threads. Full-duplex
and multi-op pipelining (partitioning the payload region into slots) are v1.x
refinements.

**The readiness edge** (the §6 syscall-free busy-poll): netd
`weft_recv_into_ring`, on a real RX delivery into a flow's ring, bumps the ring's
single-cache-line `ready_seq` (`weftlib::ready_signal(WEFT_READY_RX)`), and the
client observes it without a syscall (`WeftFlow::rx_ready_seq` →
`weft::ready_observe`). The userspace `ready_signal`/`ready_observe` mirror the
kernel `weft_ready_signal`/`weft_ready_observe` orderings exactly (the store-buffer
register-then-observe of `specs/weft_readiness.tla`, I-9), so the cross-Proc
seq-cst story holds across the shared page. The native client's park is the Loom
CQ wait, so the producer's wake is the CQE, not a `wait_active` write — the
direct-park leg (`arm_park`/`unpark`) stays validated-not-wired (v1.x).

### Tests + what's deferred

`weft.binding_validate_rw` (a unit test of the direction-agnostic validator:
in-ring offset, below-region reject, past-`payload_size` reject, the byte-copy
fall-through — identical for read and write) + `9p_wire.tweftio_round_trip` /
`9p_session.weftio_round_trip` / `9p_client.weftio` (6b-2a). The **live E2Es** are
`net-echo`'s `weft_tx_e2e` (`net-echo: weft-6b TX E2E PASS`) and `weft_rx_e2e`
(`net-echo: weft-6b RX E2E PASS`) over the resident loopback. TX: the client
weft-writes a 4 KiB payload through the shared ring, netd reads it in place +
sends it, the server reads it back, the bytes are verified. RX: the server sends
a 4 KiB pattern, the client weft-reads it **into** the ring (`Tweftio` READ —
exercising the full blocking defer, since over the loopback the first read finds
the rx empty and netd parks + delivers on the next poll tick), and the bytes are
verified in the client's own ring mapping. **Weft-6c-1** adds the Loom data drive
(above) with `9p_client.loom_weft_{read,write,hybrid_fallback,oob_rejected}` —
the routing (READ/WRITE → `Tweftio`), the zero-copy completion (the ring slice
untouched by the kernel on a READ), the §4.8 byte-path fallback, and the in-ring
OOB reject, each over the loopback 9P client. **Weft-6c-2** adds the native
`libthyla_rs::net::WeftFlow` push/pop/wait API (above) + the live readiness edge,
proven in-guest by `net-echo`'s `weft_async_e2e` (`net-echo: weft-6c async E2E
PASS`): the client drives a `WeftFlow` (push = Loom WRITE → `Tweftio`, pop = Loom
READ → `Tweftio` READ) over the resident loopback, both directions — push a 4 KiB
pattern (the server reads it back + verifies), then the server sends a pattern
(the client pops it into its ring + reads it out of `rx_buf` zero-copy + verifies)
— asserting the readiness `ready_seq` advanced over the RX and that a second push
without a `wait` returns `WouldBlock` (single-in-flight). 965/965 PASS, boot OK, 0
EXTINCTION (kernel byte-unchanged). The deferred-F_NOTIF true-ZC path is a v1.x
seam; the focused buffer-lifetime audit + SMP gate + the throughput benchmark are
Weft-7 (6c-3).

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

## The Weft-7 focused audit close + the throughput benchmark

Weft-7 is the FIRST + FINAL formal prosecution of the whole Weft EL0 surface
(6a-6c) and CLOSES the Weft arc. An Opus-4.8-max adversarial soundness prosecutor
(the dedicated reviewer agent) + a concurrent self-audit. **Verdict: 0 P0 / 0 P1
/ 1 P2 / 3 P3 -- a clean close (the P2 fix is a localized cap-gate, not dirty).**

### Findings

- **F1 [P2] -- ungated `SYS_WEFT_SHARE` -> unprivileged registry-squat DoS. FIXED.**
  `sys_weft_share_for_proc` had no capability gate, and `weft_share_register`
  takes any free slot regardless of owner. Any EL0 Proc could loop
  `SYS_BURROW_ATTACH` + `SYS_WEFT_SHARE` to squat the fixed `WEFT_MAX_SHARES=64`
  registry (its share_id is never claimable -- only netd's `Rweft` hands the
  kernel an id to claim -- so the slots stay occupied for the squatter's
  lifetime), starving the trusted netd's `weft_ensure` so every flow system-wide
  silently falls back to byte-copy: a cross-Proc AVAILABILITY DoS (no
  UAF/corruption/leak -> P2). FIX: gate `SYS_WEFT_SHARE` on `CAP_HW_CREATE` (the
  driver tier netd holds, the same gate `SYS_MMIO/IRQ/DMA/PCI_CREATE` use; an
  ordinary user Proc lacks it). Regression: `weft.share_cap_gate` (cap-less ->
  -1 + no slot taken; cap'd -> id minted) -- fails on pre-fix code.
- **F2 [P3] -- a transient `SYS_WEFT_MAP` OOM permanently pins one flow to
  byte-copy** (the consumed-but-unmapped share_id is re-returned idempotently ->
  unclaimable). Graceful (byte-copy works; a new flow is unaffected). Same family
  as the self-audit's minted-but-unclaimed leak (bounded at the 64-slot registry,
  reclaimed at netd death). Fix = a per-flow `SYS_WEFT_UNSHARE` GC + re-mint
  (ABI-escalating -> v1.x, task #289).
- **F3 [P3] -- `dev9p_close` cleared `p->weft` non-atomically vs `__ATOMIC_ACQUIRE`
  readers. FIXED (hardening).** Sound today (last-ref excludes all readers; every
  reader holds a Spoor ref), now an `__ATOMIC_RELEASE` store + a comment naming
  the Loom-reader/reg-table-ref invariant.
- **F4 [P3] -- the netd raw-pointer slice sites rest on undocumented
  single-threadedness. FIXED (doc hardening).** Explicit "INVARIANT: netd
  single-threaded; a future concurrency lift MUST add a per-slot guard" notes at
  `weft_recv_into_ring`/`h_weftio`; the value safety (Eof on a vanished ring,
  never OOB) already holds unconditionally. (`WeftFlow` is auto-`!Send`+`!Sync`
  via `Ring`'s raw `*mut` -- the cross-thread property holds structurally.)

The SOUND set (the cross-Proc #847 dual-refcount ledger, the share_id
consume-once + unforgeability, the I-30 submit-time pin / ring-TOCTOU, the I-29
count clamp, the I-9 RX-defer + readiness seq-cst mirror, W^X, the wire codec +
kernel-owned geometry) all traced + survived; the `weft.tla` + `weft_readiness
.tla` spec gate is GREEN (clean + liveness + every buggy cfg). Full closed list:
`memory/audit_weft_closed_list.md`.

### The throughput benchmark (NET-THROUGHPUT section 8; #269 M6)

`netperf`'s new **MW** mode is the M2 twin: the SAME bulk transfer over the SAME
resident `lo`, but the SEND side rides a `WeftFlow` (push/wait over Loom ->
`Tweftio` -> netd reads the ring IN PLACE) instead of `TcpStream::write`
(byte-copy). The drain server stays a byte-copy reader, so MW isolates the
send-side zero-copy delta.

The HONEST in-guest result: **weft is ~10x SLOWER than byte-copy on loopback**
(MW ~2.4 MiB/s vs M2 ~24 MiB/s for the boot-probe transfer). This is not a bug --
it is the binding constraint the bench EXISTS to find: netd's smoltcp socket tx
buffer is **4 KiB**, so `data_send` accepts at most ~4 KiB per op. On loopback
that caps BOTH paths at 4 KiB windows, so weft pays the per-op Loom + `Tweftio`
round-trip overhead per 4 KiB **without the large-send amortization**, while the
copy it avoids is negligible at 4 KiB -- weft's WORST regime. The zero-copy
throughput win is gated behind (a) a larger socket tx buffer (so one weft push
absorbs a big send in one op -- the v1.x lever, task #288) or (b) the NIC path,
where the copy is the actual bottleneck (the slirp-bounded M6, not deterministic
in-guest). The bench's value is exactly this: it pins the lever, not a flattering
number.

## Known caveats / footguns (Weft-7 closed; v1.x seams)

- **Weft-7 v1.x seams.** (1) The per-flow share-GC: an unclaimed/transiently-
  failed-map share_id leaks its ring until netd death, bounded at
  `WEFT_MAX_SHARES=64` slots (~16 MiB), self-limiting to byte-copy, reclaimed at
  netd restart -- a per-flow `SYS_WEFT_UNSHARE` is the fix (task #289). (2) The
  socket-tx-buffer throughput lever (task #288, above). (3) The deferred two-CQE
  true-zero-copy-to-NIC F_NOTIF path (v1.0's weft send is the copied single-CQE
  realization). (4) netd single-threadedness is load-bearing for the raw-pointer
  slice sites (F4) -- a per-user-netd concurrency lift must add a per-slot guard.

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
