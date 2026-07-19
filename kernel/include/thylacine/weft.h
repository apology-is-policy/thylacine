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
    u32 ready_off;      // byte offset of weft_ready_hdr in the Burrow (mirror)
    u32 _pad[7];
};

_Static_assert(sizeof(struct weft_ring_hdr) == 64, "weft_ring_hdr pinned at 64 bytes");
_Static_assert(__builtin_offsetof(struct weft_ring_hdr, prod_tail) == 20, "prod_tail at 20");
_Static_assert(__builtin_offsetof(struct weft_ring_hdr, cons_head) == 24, "cons_head at 24");
_Static_assert(__builtin_offsetof(struct weft_ring_hdr, ready_off) == 32, "ready_off at 32");

// =============================================================================
// Readiness ring (Weft-4) -- the Shenango single-cache-line readiness poke
// (NET-THROUGHPUT.md section 5.2-2 / 4.3). The PRODUCER (netd) bumps a shared
// edge counter when a flow becomes RX-readable / TX-writable; the CONSUMER
// (guest) observes it at memory speed (acquire-load), retiring the ~50 ms
// RX-wake poll (NET-PERF N1). The PUSH counterpart of the dev9p.poll elicited
// PULL (net_poll.tla): no probe to keep outstanding -- the edge is written
// straight into shared memory.
//
// The no-lost-wake is the STORE-BUFFER register-then-observe (specs/weft_-
// readiness.tla; ARCH section 28 I-9): the consumer, before parking, publishes
// its park-intent (wait_active) and re-reads ready_seq; the producer, on an
// edge, bumps ready_seq and reads wait_active -- each side a seq-cst store then
// load, so at least one sees the other's write and no edge in the window is
// lost. weft_ready_arm_park / weft_ready_signal realize the two halves.
//
// Two cache lines, single-writer-per-word (no false sharing -- the producer
// never touches the consumer's line and vice versa):
//   ready_seq / ready_mask -- PRODUCER-owned (netd writes; guest reads).
//   wait_seq / wait_active -- CONSUMER-owned (guest writes; netd reads to
//                             decide a wakeup, NEVER writes -- the single-writer
//                             discipline; the consumer clears wait_active on
//                             resume).
// =============================================================================
#define WEFT_READY_RX 0x001u    // RX data available (POLLIN-aligned)
#define WEFT_READY_TX 0x004u    // TX space / send complete (POLLOUT-aligned)

struct weft_ready_hdr {
    u32 ready_seq;      // producer-owned: bumped per readiness edge (the poke)
    u32 ready_mask;     // producer-owned: WEFT_READY_* of the latest edge
    u32 _ppad[14];      // pad the producer words to a full cache line
    u32 wait_seq;       // consumer-owned: the seq registered before parking
    u32 wait_active;    // consumer-owned: 1 while parked, 0 while running
    u32 _cpad[14];      // pad the consumer words to a full cache line
};

_Static_assert(sizeof(struct weft_ready_hdr) == 128, "weft_ready_hdr pinned at 128 bytes (two cache lines)");
_Static_assert(__builtin_offsetof(struct weft_ready_hdr, ready_seq) == 0, "ready_seq at 0");
_Static_assert(__builtin_offsetof(struct weft_ready_hdr, wait_seq) == 64, "consumer words on their own cache line");
_Static_assert(__builtin_offsetof(struct weft_ready_hdr, wait_active) == 68, "wait_active at 68");

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
    u32    ready_off;     // byte offset of the readiness header (weft_ready_hdr)
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

// ---- Readiness ring (Weft-4) -- the netd<->guest single-cache-line poke. ----
// Direction-agnostic: Weft-6 instantiates one channel per direction (netd->guest
// RX-ready, guest->netd TX-queued). These are the shared-memory DECISION
// primitives; the live park (the Rendez sleep on a true arm_park) + the wake
// (the Rendez wakeup on a true signal) are wired at Weft-6.

// Zero the readiness header (ready_seq / mask / wait_seq / wait_active = 0).
// Called once at flow grant, alongside weft_ring_init_hdr.
void weft_ready_init_hdr(const struct weft_ring_view *rv);

// PRODUCER (netd): post a readiness edge -- bump ready_seq + publish `mask`
// (WEFT_READY_*), then read the consumer's park-intent. Returns true iff a
// parked consumer must be woken (the caller then issues the Rendez wakeup);
// false iff the consumer is running (busy-polling) and needs no wakeup. The
// seq-cst bump-then-load cannot miss a consumer that armed concurrently (the
// store-buffer barrier; weft_readiness.tla ProducerEdge). NEVER writes the
// consumer-owned wait_* words (single-writer-per-word).
bool weft_ready_signal(struct weft_ring_view *rv, u32 mask);

// CONSUMER (guest) fast path: acquire-load the readiness edge counter (and the
// mask, if `mask_out`). The caller compares the returned seq to its private
// last-seen cursor; a difference is a new readiness edge. No park, no syscall
// -- the lock-free Shenango cache-line read.
u32 weft_ready_observe(struct weft_ring_view *rv, u32 *mask_out);

// CONSUMER (guest) park decision: register-then-observe. Publishes the park-
// intent (wait_active, at `last_seen`) THEN re-reads ready_seq (seq-cst).
// Returns true iff it is safe to park (no edge raced in the window: ready_seq
// still == last_seen); false iff an edge arrived (the caller must NOT park --
// re-process; wait_active is cleared). The consumer's half of the store-buffer
// barrier (weft_readiness.tla ConsumerPark). The actual Rendez sleep on a true
// return is Weft-6.
bool weft_ready_arm_park(struct weft_ring_view *rv, u32 last_seen);

// CONSUMER (guest): clear the park-intent on resume (after a wakeup, or after
// deciding not to park). The consumer owns wait_active; the producer only reads
// it. Called from the consumer's post-sleep path (Weft-6).
void weft_ready_unpark(struct weft_ring_view *rv);

// =============================================================================
// F_NOTIF zero-copy-send completion contract (Weft-5) -- the two-CQE send +
// the multi-holder buffer-pin release (NET-THROUGHPUT.md section 4.6; weft.tla
// Holders / HolderRelease / ReleaseClean / ReleasePremature). A zero-copy send
// completing means only "queued": the registered payload page is still IN FLIGHT
// (the NIC may DMA from it; for TCP it stays pinned until the peer ACKs). So the
// send posts a RESULT CQE (LOOM_CQE_MORE = "queued") and, later, a NOTIFICATION
// CQE (LOOM_CQE_F_NOTIF = "buffer reusable"). The I-30 buffer pin releases at the
// NOTIFICATION-terminal -- the LAST of {netd stack done, NIC DMA done, peer ACK}
// -- NEVER at op-terminal. Releasing at op-terminal reuses a page the NIC may
// still be reading / TCP may still retransmit = the io_uring ubuf_info UAF
// (weft.tla PinHeldWhileInFlight + NoInFlightReuse).
//
// weft_notif is the KERNEL-PRIVATE per-send in-flight tracker (the spec's
// holders[b]): NOT a shared-page structure, NOT ABI -- the guest sees only the
// two CQEs. The holder bitmask alone is the complete state (in flight iff
// holders != 0); the release gate (weft_notif_clear -> WEFT_NOTIF_RELEASE) fires
// EXACTLY ONCE, on the last-holder transition, so a premature drop is structurally
// unreachable through this API: a caller that releases the pin ONLY on RELEASE
// cannot reuse an in-flight page. The live tracker rides the loom_async_op at
// Weft-6 (the two-CQE posting); Weft-5 lands the contract + the holder mechanism.
// =============================================================================

// The F_NOTIF in-flight holder set (weft.tla Holders). A zero-copied page is held
// until ALL THREE clear; each is a distinct completion event.
#define WEFT_HOLDER_NETD 0x1u   // netd's stack still references the page
#define WEFT_HOLDER_NIC  0x2u   // the NIC may still DMA from the page
#define WEFT_HOLDER_ACK  0x4u   // the peer has not yet ACKed (TCP may retransmit)
#define WEFT_HOLDERS_ALL (WEFT_HOLDER_NETD | WEFT_HOLDER_NIC | WEFT_HOLDER_ACK)

// Result-CQE indicator (the IORING_SEND_ZC_REPORT_USAGE / ZC_COPIED analog): set
// when the send FELL BACK to a copy, so the buffer is free immediately and netd
// is never silently-wrong about whether the page is in flight.
#define WEFT_NOTIF_COPIED 0x1u

// The action a holder-clear yields (the caller's completion step).
enum weft_notif_phase {
    WEFT_NOTIF_HELD = 0,    // the pin stays held -- holders remain (or a stray/late event)
    WEFT_NOTIF_RELEASE,     // the LAST holder cleared: release the I-30 pin + post the
                            // notification CQE NOW (returned EXACTLY ONCE; weft.tla ReleaseClean)
};

// weft_notif -- the kernel-private F_NOTIF multi-holder tracker for one in-flight
// ZC send. `holders` is the set still pending (0 == not in flight: free / copied /
// notification-terminal). `result_flags` carries WEFT_NOTIF_COPIED for a copied
// send. Zero-initialize before arming (an all-zero tracker is "not in flight").
struct weft_notif {
    u32 holders;        // WEFT_HOLDER_* still pending; 0 == not in flight
    u32 result_flags;   // WEFT_NOTIF_* for the result CQE
};

// Arm the tracker for a ZERO-COPIED send: the page is in flight under `holders`
// (a non-empty subset of WEFT_HOLDERS_ALL, typically all three) until the
// notification-terminal. Clears result_flags (zero-copied, in flight). weft.tla
// NetdAct. A degenerate empty/over-wide `holders` is masked to WEFT_HOLDERS_ALL's
// domain; an empty result leaves the tracker not-in-flight (no-op send).
void weft_notif_arm(struct weft_notif *n, u32 holders);

// Arm for a COPIED send (the ZC path fell back to a copy): the payload is already
// copied, so the buffer is free IMMEDIATELY -- no in-flight holders. The
// notification fires at once carrying WEFT_NOTIF_COPIED; weft_notif_inflight is
// false right away (the buffer is reusable).
void weft_notif_arm_copied(struct weft_notif *n);

// Clear one holder (a netd-stack-done / NIC-DMA-done / peer-ACK event; `holder`
// is one WEFT_HOLDER_* bit). Returns WEFT_NOTIF_RELEASE iff this clear emptied the
// holder set -- the caller then releases the I-30 buffer pin + posts the
// notification CQE (EXACTLY ONCE: a stray/duplicate/late event, or a clear of an
// already-clear holder, is a no-op returning WEFT_NOTIF_HELD). weft.tla
// HolderRelease (-> ReleaseClean on the last).
enum weft_notif_phase weft_notif_clear(struct weft_notif *n, u32 holder);

// PinHeldWhileInFlight query: true iff the page is still in flight (a holder
// pending) -- the I-30 pin must stay held + the page must NOT be reused. A reuse
// path checks this; a true result means a reuse would be the ubuf_info UAF
// (weft.tla NoInFlightReuse).
bool weft_notif_inflight(const struct weft_notif *n);

// The result-CQE flags (WEFT_NOTIF_COPIED if the send fell back to a copy, else 0)
// -- OR'd into the result CQE so userspace/netd knows whether the buffer is in
// flight (the never-silently-wrong fallback indicator).
u32 weft_notif_result_flags(const struct weft_notif *n);

// =============================================================================
// Weft-6a-2: the per-flow EL0 delivery (grant-is-the-share). The kernel-scoped
// share_id registry + the per-data-fd ring binding. NET-THROUGHPUT.md section 6.
//
// share_id is the kernel-internal join key between netd's SYS_WEFT_SHARE (which
// registers a per-flow ring Burrow it allocated in its own AS) and the kernel's
// SYS_WEFT_MAP (which, on a guest's first zero-copy use of a flow, issues
// Tweft(F) -> netd returns the share_id in Rweft -> the kernel claims it here +
// maps the ring into the guest via burrow_share_into). The share_id NEVER
// reaches the guest: it travels netd->kernel inside the kernel's OWN Tweft
// round-trip (the RDMA-rkey shape, NET-THROUGHPUT.md section 4.6), so a guest
// cannot forge one, and a claim consumes it exactly once.
//
// The registry struct (struct weft_share) is private to kernel/weft.c. The
// lifetime authority is the I-30 REGISTRATION PIN (a burrow_ref / handle_count):
// SYS_WEFT_SHARE takes it so the ring Burrow survives the netd<->kernel
// correlation window even if netd drops its own mapping; the claim TRANSFERS it
// to a weft_binding; dev9p_close drops it. The guest's ring MAPPING
// (mapping_count) is owned by the guest VMA and reclaimed by vma_drain at guest
// exit (the Loom-ring precedent, kernel/loom.c::loom_free) -- NOT by the binding
// release. The #847 dual refcount frees the ring when BOTH reach 0, in any order
// (weft.tla ShareBoundedByFlow).
// =============================================================================

struct Proc;
struct Burrow;

// The binding KIND (G-2; TAPESTRY.md §18.11 F10). Derived at claim time from
// the KERNEL-MINTED Burrow type -- never from the server's declaration alone:
//   WEFT_BIND_RING  -- a netd flow ring (BURROW_TYPE_ANON): carries the ring
//                      view; drives the Tweftio data fast-paths.
//   WEFT_BIND_WEAVE -- a tapestryd framebuffer weave (the DMA weave subtype):
//                      NO ring view, NO Tweftio drive (weft_binding_validate_rw
//                      is kind-gated, which closes all three fast-path
//                      consumers -- the sync dev9p_weft_try_{read,write} AND
//                      the Loom routing -- at one chokepoint); the map is the
//                      whole deliverable (the client draws into it directly).
enum weft_bind_kind {
    WEFT_BIND_RING  = 0,
    WEFT_BIND_WEAVE = 1,
};

// The per-data-fd binding recorded in dev9p_priv once a flow's ring / a
// surface's weave has been mapped into the guest. Holds the registration pin
// (transferred from the registry at claim); a weak record of the guest VA for
// the idempotent SYS_WEFT_MAP return. Allocated by weft_binding_alloc /
// weft_binding_alloc_weave at SYS_WEFT_MAP; released (pin dropped + struct
// freed) by weft_binding_release at dev9p_close -- which for a WEAVE binding
// additionally unmaps the client mapping when the closer IS the mapping Proc
// (§18.1 "the weave fid's clunk drops the client mapping"; the RING mapping
// keeps its audited vma_drain-at-exit lifetime).
struct weft_binding {
    struct Burrow *burrow;     // the ring / weave; the registration pin is held HERE
    u64            guest_va;   // where it is mapped in the guest (idempotent-MAP return)
    u32            ring_size;  // mapped byte length (== burrow size; diagnostics)
    u32            kind;       // enum weft_bind_kind (claim-time, immutable)
    // The Proc that holds the mapping (the SYS_WEFT_MAP caller), for the
    // WEAVE clunk-unmap: dev9p_close unmaps only when the closing Proc's pid
    // matches (an inherited-fd closer in another Proc leaves the mapping to
    // the mapper's own vma_drain -- the ring precedent). Sound as a bare pid:
    // g_next_pid is a monotonic u32 (no reuse within any realizable runtime,
    // the proc.c precedent), so a matched pid IS the mapping Proc.
    u32            map_pid;
    // Weft-6b-2: the kernel's PRIVATE trusted geometry of this ring (computed
    // at SYS_WEFT_MAP via weft_ring_layout from the shared Burrow's contiguous
    // KVA + the Rweft-reported ring_entries). The data-drive validate reads the
    // payload-region geometry from HERE, never from the guest-mutable shared
    // header mirror (the I-30 validator-once; the Weft-3 snapshot discipline).
    // ZERO for a WEAVE binding (no ring; never consulted -- the kind gate).
    struct weft_ring_view view;
    // G-3 (TAPESTRY.md section 18.12 R2-F3): the orphaned-weave reaper's
    // registry linkage. A WEAVE binding registers after the SYS_WEFT_MAP
    // CAS-install (weft_reap_register) and unregisters at dev9p_close
    // (weft_reap_unregister, BEFORE the close touches the binding -- the
    // g_weft_reap_lock serialization that makes reaper-vs-close sound).
    // sess_att/sess_cl let the reaper test the SERVING session's liveness
    // (att when the session is SYS_ATTACH-owned -- production; cl alone on
    // the externally-owned test path). Both are BORROWED, valid while the
    // binding is registered: the owning dev9p_priv holds the att ref /
    // client lifetime, and it unregisters before releasing either. RING
    // bindings never register (their audited vma_drain lifetime stands).
    struct weft_binding      *reap_next;
    u64                       orphan_since_ns;
    const struct p9_attached *sess_att;
    const struct p9_client   *sess_cl;
};

// Register a per-flow ring `v` (netd's backing ANON Burrow) owned by `owner`
// (the registering netd Proc, for owner-death GC). Takes the I-30 registration
// pin (burrow_ref) and returns a fresh kernel-scoped share_id (monotonic; never
// 0). Returns 0 on NULL inputs or a full registry (the caller then fails the
// flow's setup; the byte-copy path still works). The pin is the caller's-burrow
// liveness across the claim window.
u64 weft_share_register(struct Proc *owner, struct Burrow *v);

// Claim a registered share by id (consume-EXACTLY-once). On success removes the
// registry entry, TRANSFERS the registration pin to the caller (who must
// burrow_unref it or hand it to a weft_binding), and returns the Burrow. Returns
// NULL if no live entry has `share_id` (a replay / forged / already-claimed id).
// Because the pin is transferred, the caller owns exactly one burrow_ref after a
// non-NULL return -- no extra ref/unref.
struct Burrow *weft_share_claim(u64 share_id);

// GC every UN-claimed share owned by `owner` (drops each registration pin). The
// share_outlives_flow backstop: a netd that registered a ring but died before
// the kernel claimed it must not leak the pinned ring Burrow. Called at the netd
// Proc's exit (kernel/proc.c exit-notify, alongside srv/cap_proc_exit_notify).
void weft_share_release_owner(struct Proc *owner);

// G-2 (SYS_WEFT_UNSHARE; TAPESTRY.md §18.11 F3 + §18.12 R2-F5; closes the #289
// seam): explicitly disarm ONE un-claimed share. Owner-gated -- the entry is
// removed only when `owner` matches its registrant. Returns 0 (entry removed,
// pin dropped -- a subsequent claim of this id fails closed: the spec's
// Map-guard `wstate ∈ {woven,live}` realized as registry-removal-before-free +
// the claim's live-registry lookup) or -1 (no live entry with this id under
// this owner -- already claimed [the client holds a legitimate mapping; retire
// proceeds by quiesce], already GC'd, forged, or another owner's). The pin
// drop runs OUTSIDE the registry leaf lock (the release_owner discipline).
// The two consumers: tapestryd's surface retire/reweave (a weave's share_id
// must not linger claimable past the RETIRING transition -- the NoStaleMap
// guard), and netd's per-flow GC of a minted-but-never-claimed id (#289).
int weft_share_unregister(struct Proc *owner, u64 share_id);

// G-2: the claim-time kind decision (pure; used by SYS_WEFT_MAP's claim path +
// unit-testable in isolation). Derives the binding kind from the KERNEL-MINTED
// Burrow type -- the single source of truth -- and CROSS-CHECKS the server's
// declared geometry: an ANON ring must declare ring_entries != 0 (the layout
// validates the exact value later); a DMA weave must declare ring_entries == 0
// (a weave has no descriptor ring). Returns WEFT_BIND_RING / WEFT_BIND_WEAVE,
// or -1 on a type/declaration mismatch or an inadmissible type (the caller
// fails closed: unmaps nothing, drops the claimed pin). A mismatch means a
// buggy/hostile server declared a geometry its own registered Burrow
// contradicts -- never mapped.
int weft_claimed_kind(const struct Burrow *v, u32 ring_entries);

// Allocate a RING weft_binding owning the (already-claimed) registration pin on
// `burrow`, recording the guest VA + size + computing the kernel-private ring
// view (geometry) from the Burrow's contiguous KVA + `ring_entries` (the
// netd-reported descriptor-slot count from Rweft). ANON Burrows only (the ring
// view derives from the ANON contiguity guarantee). Returns NULL on OOM OR on
// an invalid geometry (the caller then drops the pin + the guest mapping
// itself). The binding is stored in the data Spoor's dev9p_priv.
struct weft_binding *weft_binding_alloc(struct Burrow *burrow, u64 guest_va,
                                        u32 ring_size, u32 ring_entries);

// G-2: allocate a WEAVE weft_binding (the §18.11 F10 framebuffer map branch) --
// no ring view, no Tweftio drive; records the mapping Proc's pid for the
// clunk-unmap. Same pin-ownership contract as weft_binding_alloc. Returns NULL
// on OOM or a non-weave Burrow (defense-in-depth: the caller's kind decision
// already derived WEAVE from the type).
struct weft_binding *weft_binding_alloc_weave(struct Burrow *burrow,
                                              u64 guest_va, u32 size,
                                              u32 map_pid);

// Weft-6b-2/6b-3 data drive: validate a guest syscall buffer against the flow's
// ring. Direction-agnostic -- a Tweftio WRITE source and a Tweftio READ
// destination both name a `[off, off+len)` window in the payload, so one
// validator serves both. `ubuf_va` is the caller's SYS_WRITE/SYS_READ buffer; if
// it lies within this flow's shared-ring payload region and `[off, off+len)` is
// in bounds, returns 0 and sets `*out_off` to the payload-region-relative offset
// (the descriptor's `addr`, the same domain as weft_desc.addr). Returns -1 if the
// buffer is not in the ring or the window is out of bounds (the caller falls back
// to the byte-copy path). The geometry read is the kernel-private view (never the
// guest-mutable shared header) -- the I-30 validator-once.
int weft_binding_validate_rw(const struct weft_binding *b, u64 ubuf_va,
                             u32 len, u32 *out_off);

// Release a data-fd's ring binding at dev9p_close: drop the registration pin
// (burrow_unref) + free the binding struct. Does NOT touch the guest's ring
// mapping (vma_drain owns it, the Loom-ring precedent). NULL-safe.
void weft_binding_release(struct weft_binding *b);

// G-2 (§18.1 ClunkMap; the G-2 audit F1 close): the WEAVE clunk-unmap --
// drop `closer`'s mapping of the weave at fid-close, iff (a) the binding is
// WEAVE-kind, (b) `closer` IS the mapping Proc (pid match; the monotonic-u32
// identity), and (c) the VMA at the recorded guest_va is STILL backed by this
// weave's Burrow (the F1 guard: after an explicit SYS_BURROW_DETACH, an
// unrelated fresh mapping can land at the same VA -- it must survive the
// close untouched). Takes closer->vma_lock; the unmap uncharges the shared-in
// budget via the SHARED_IN pairing. Returns 0 (unmapped) or -1 (skipped --
// wrong kind / wrong Proc / no live weave mapping at the VA). Called by
// dev9p_close BEFORE weft_binding_release; unit-driven directly (the closer
// param exists so a test drives it against a synthetic Proc).
int weft_binding_clunk_unmap(struct weft_binding *wb, struct Proc *closer);

// =============================================================================
// G-3: the orphaned-weave force-reclaim (TAPESTRY.md section 18.12 R2-F3;
// the ServerDeath leg of specs/tapestry_present.tla, kernel half).
// =============================================================================
//
// When the SERVING compositor dies, a claimed weave's client mapping keeps
// the pixel pages alive (#847 mapping_count -- no UAF) but semantically dead:
// every fid op returns the session-dead error ("compositor gone"). A client
// that never closes the fd would pin the pages UNCHARGED forever (the R2-F3
// leak). The reaper kthread sweeps the registered WEAVE bindings; one whose
// serving session has been dead longer than WEFT_REAP_GRACE_NS is FORCE-
// RECLAIMED: the client's stale mapping is cross-Proc unmapped (the client
// was warned -- a later touch takes snare:segv), the shared-in budget
// uncharges (inside burrow_unmap), and the binding's registration pin drops
// so the pixel chunk frees at once. The binding STRUCT itself is freed only
// by dev9p_close (weft_binding_release) -- the reaper NULLs wb->burrow under
// g_weft_reap_lock, and the close path unregisters under the same lock
// before reading the binding, so neither side sees a half-reclaimed state.
//
// Lock order: g_weft_reap_lock -> g_proc_table_lock(irqsave) -> vma_lock ->
// v->lock -> buddy. Acyclic: register runs lock-free (after the map's
// vma_lock drop), unregister runs lock-free (dev9p_close), and nothing
// under gptl/vma takes the reap lock. The deferred pin drop (burrow_unref
// outside the reap lock) follows the weft_share_unregister discipline.

// The dead-session grace before a force-reclaim, and the sweep cadence.
#define WEFT_REAP_GRACE_NS  (2ull * 1000 * 1000 * 1000)
#define WEFT_REAP_SWEEP_NS  (1ull * 1000 * 1000 * 1000)

struct p9_attached;
struct p9_client;

// Register an installed WEAVE binding for orphan tracking. Call AFTER the
// SYS_WEFT_MAP CAS-install wins, with no locks held. att/cl are borrowed
// (see the struct comment); att may be NULL (test path), then cl is the
// liveness source.
void weft_reap_register(struct weft_binding *wb,
                        const struct p9_attached *att,
                        const struct p9_client *cl);

// Remove a binding from the reaper's registry (idempotent). Call from
// dev9p_close BEFORE touching the binding's fields: returning from this is
// the guarantee no reaper sweep still holds wb.
void weft_reap_unregister(struct weft_binding *wb);

// One sweep pass at `now_ns` (the kthread's body; test-drivable). Returns
// the number of bindings force-reclaimed this pass.
int weft_reap_sweep(u64 now_ns);

// Boot init + the reaper kthread main (spawned as a kproc thread).
void weft_reap_init(void);
void weft_reaper_main(void);


#endif
