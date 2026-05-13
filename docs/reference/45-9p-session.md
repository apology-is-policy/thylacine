# 45 — 9P session state machine

## Purpose

`kernel/9p_session.c` + `kernel/include/thylacine/9p_session.h` compose the wire codec at `kernel/9p_wire.c` (see `44-9p-wire.md`) into the per-session tag pool + fid table + outstanding-request bookkeeping that `specs/9p_client.tla` describes. The session module is **what the spec's state machine looks like in code** — its preconditions, state transitions, and invariants all map directly onto spec actions.

Audit posture: spec-bearing per CLAUDE.md §"Audit-triggering changes". This module joins the trigger list when P5-transport composes it onto an actual transport. The unit-test matrix at `kernel/test/test_9p_session.c` covers each spec-discipline shape directly (the 4 buggy cfg variants in `specs/9p_client.tla` correspond to 4 specific rejection-path tests below).

## State machine

```
INIT
  │  p9_session_send_version → write Tversion(NOTAG) to caller buffer
  ▼
INIT  (Tversion in flight; outstanding[] empty — Tversion uses NOTAG, lives out-of-band)
  │  p9_session_dispatch_rmsg(Rversion) → negotiate msize
  ▼
VERSIONED
  │  p9_session_send_attach → alloc tag, write Tattach to caller buffer, outstanding[tag] = TATTACH
  │  p9_session_dispatch_rmsg(Rattach) → bind root_fid into bound_fids, clear outstanding
  ▼
OPEN  (root_fid bound; full surface available)
  │  p9_session_send_walk    — gated; outstanding[tag] = TWALK
  │  p9_session_send_clunk   — gated; Send-time-unbinds fid; outstanding[tag] = TCLUNK
  │  p9_session_dispatch_rmsg(Rwalk/Rclunk/Rlerror) — applies state mutation per kind
  │
  │  p9_session_close → drain Inflight first; transitions to CLOSED
  ▼
CLOSED  (terminal)
```

## Spec-action ↔ symbol cross-reference

| Spec action | Impl |
|---|---|
| `OpenSession` | `p9_session_send_version` + `p9_session_send_attach` + `p9_session_dispatch_rmsg` |
| `CloseSession` | `p9_session_close` (refuses while `Inflight ≠ {}`) |
| `SendWalk(t, src, new)` | `p9_session_send_walk` — Send-time precondition: src bound, new not bound, new ≠ root, no other in-flight op on new |
| `SendClunk(t, fid)` | `p9_session_send_clunk` — Send-time precondition: fid bound, fid ≠ root, no other in-flight op on fid. **Send-time-unbinds** fid (the spec's canonical client discipline). |
| `SendIO(t, fid)` — open/create | `p9_session_send_lopen` + `p9_session_send_lcreate` — Send-time precondition: fid bound, no other in-flight op on fid (server-side fid state mutation is exclusive). |
| `SendIO(t, fid)` — read/write | `p9_session_send_read` + `p9_session_send_write` — Send-time precondition: fid bound. Concurrent ops on same fid permitted (offset is explicit on the wire). |
| `ReceiveOp(t)` | `p9_session_dispatch_rmsg` — tag-indexed lookup pairs Rmsg with correct outstanding op; Rlerror surfacing; type-mismatch rejected. IO R-msgs (Rlopen/Rlcreate/Rread/Rwrite) populate the corresponding fields in `struct p9_dispatch_result` without mutating the fid table. |

## Invariants the module upholds

| Spec invariant | Mechanism |
|---|---|
| **I-10 TagUniqueness** | Bitmap allocator (`alloc_tag`) scans `outstanding[0..MAX-1]` for the first `!active` slot. Refuses to return a slot already marked `active`. Each Send marks its tag's slot `active`; each Receive clears it. The "lost-op" form of tag collision (overwriting an active slot) is structurally prevented because `mark_outstanding` is called only after `alloc_tag` returns a free slot. |
| **I-11 FidStability** | `fid_bind` / `fid_unbind` explicit. `SendClunk` Send-time-unbinds. `SendWalk` / `SendIO` (future) require `fid_bound(src)`. `BuggyFidAfterClunkSend` shape impossible at the API: every Send-side function checks `fid_bound(fid)` first. |
| **OutOfOrderCorrectness** | `p9_session_dispatch_rmsg` looks up `outstanding[tag]` (NOT some other index); the op kind + fid + new_fid stored at Send time determine the state mutation applied at Receive time. Pipelined out-of-order Rmsgs are paired to their original Tmsg's metadata. |
| **FlowControl / BoundedOutstanding** | `alloc_tag` returns `-1` when no slot is free. `p9_session_send_*` short-circuits on the negative. Back-pressure surfaces as a send-side `-1` (caller retries or escalates); no silent overflow. The cap is `P9_SESSION_MAX_OUTSTANDING = 64` at v1.0. |

## Public API

```c
// Lifecycle.
int  p9_session_init(struct p9_session *s, u32 root_fid, u32 msize);
void p9_session_destroy(struct p9_session *s);   // clobbers magic
int  p9_session_close(struct p9_session *s);     // requires Inflight = {}

// Send-side: write Tmsg into caller buffer; return total bytes or -1.
int  p9_session_send_version(struct p9_session *s, u8 *out, size_t cap,
                              const u8 *version, size_t version_len);
int  p9_session_send_attach (struct p9_session *s, u8 *out, size_t cap,
                              const u8 *uname, size_t uname_len,
                              const u8 *aname, size_t aname_len,
                              u32 n_uname);
int  p9_session_send_walk   (struct p9_session *s, u8 *out, size_t cap,
                              u32 src_fid, u32 new_fid, u16 nwname,
                              const u8 *const *names, const size_t *name_lens);
int  p9_session_send_clunk  (struct p9_session *s, u8 *out, size_t cap, u32 fid);
// IO family (P5-wire-io).
int  p9_session_send_lopen  (struct p9_session *s, u8 *out, size_t cap,
                              u32 fid, u32 flags);
int  p9_session_send_lcreate(struct p9_session *s, u8 *out, size_t cap,
                              u32 fid, const u8 *name, size_t name_len,
                              u32 flags, u32 mode, u32 gid);
int  p9_session_send_read   (struct p9_session *s, u8 *out, size_t cap,
                              u32 fid, u64 offset, u32 count);
int  p9_session_send_write  (struct p9_session *s, u8 *out, size_t cap,
                              u32 fid, u64 offset,
                              u32 count, const u8 *data);

// Receive-side: dispatch by tag, apply state mutation, surface result.
int  p9_session_dispatch_rmsg(struct p9_session *s,
                              const u8 *rmsg, size_t len,
                              struct p9_dispatch_result *out);

// Query helpers.
bool   p9_session_is_open    (const struct p9_session *s);
bool   p9_session_fid_bound  (const struct p9_session *s, u32 fid);
size_t p9_session_inflight   (const struct p9_session *s);
size_t p9_session_n_bound_fids(const struct p9_session *s);
```

## Sizing constants

| Constant | Value | Rationale |
|---|---|---|
| `P9_SESSION_MAX_OUTSTANDING` | 64 | Comfortably covers pipelined-32 workloads (VISION §4.5 throughput target) with margin. Tags allocated 0..63; tag value past MAX reserved. Tversion's NOTAG=0xFFFF lives out-of-band. Bumpable; sized conservatively at v1.0. |
| `P9_SESSION_MAX_FIDS` | 256 | Client-side cap; Stratum's server caps at 4096. Most procs use few fids — 256 is comfortable. Bumpable. |
| `P9_SESSION_MAGIC` | `0x50395345` ("P9SE") | Magic for lifetime discipline (mirrors R9 F148 — see `39-hw-handles.md` caveat #2). Clobbered on destroy. |

## State variables (`struct p9_session`)

```c
struct p9_session {
    u32                   magic;                  // P9_SESSION_MAGIC; clobbered on destroy
    enum p9_session_state state;                  // INIT / VERSIONED / OPEN / CLOSED
    u32                   root_fid;               // caller-supplied at init
    u32                   msize;                  // proposed at init
    u32                   negotiated_msize;       // set after Rversion
    u32                   bound_fids[P9_SESSION_MAX_FIDS];
    size_t                n_bound_fids;
    struct p9_outstanding outstanding[P9_SESSION_MAX_OUTSTANDING];
    u32                   next_op_id;             // monotonic
    u32                   total_sent;             // diagnostics
    u32                   total_completed;
};

struct p9_outstanding {
    bool active;
    u8   kind;     // P9_TVERSION / P9_TATTACH / P9_TWALK / P9_TCLUNK /
                   // P9_TLOPEN / P9_TLCREATE / P9_TREAD / P9_TWRITE
    u32  fid;      // primary target
    u32  new_fid;  // walk dest (== fid for non-walk)
    u32  op_id;    // spec op_id
};
```

Allocation: caller-managed. The kernel handle-table layer (P5-attach) wraps each `struct p9_session` per-Proc. The session struct is currently 4 KiB +; future SLUB cache fits this.

## Tests

25 tests in `kernel/test/test_9p_session.c`:

| Test | Covers |
|---|---|
| `9p_session.init_destroy` | Lifecycle + magic clobber + invalid-arg refusal |
| `9p_session.version_handshake` | INIT → VERSIONED; Tversion uses NOTAG; msize negotiated DOWN to server's value |
| `9p_session.attach_handshake` | VERSIONED → OPEN; root_fid bound; Rattach qid round-trip |
| `9p_session.walk_round_trip` | Twalk(clone)→Rwalk; new_fid bound at Receive |
| `9p_session.clunk_round_trip` | Tclunk→Rclunk; fid fully released |
| `9p_session.clunk_send_time_unbinds` | Tclunk Send-time-unbinds; subsequent walk-from refused |
| `9p_session.dispatch_rlerror` | Rlerror surfaces ecode; fid NOT bound on Rlerror for Twalk |
| `9p_session.walk_to_root_refused` | new_fid == root_fid → `-1` |
| `9p_session.walk_to_bound_fid_refused` | new_fid already bound → `-1` |
| `9p_session.walk_from_unbound_fid_refused` | src_fid not bound → `-1` |
| `9p_session.clunk_root_refused` | fid == root_fid → `-1` |
| `9p_session.clunk_with_inflight_on_fid_refused` | other in-flight op on same fid → `-1` (spec discipline) |
| `9p_session.fid_after_clunk_refused` | spec `BuggyFidAfterClunkSend` shape — refused |
| `9p_session.dispatch_wrong_tag_rejected` | Rmsg with tag not in outstanding[] → `-1` (catches OOO-mispair-like bugs) |
| `9p_session.dispatch_wrong_type_rejected` | type-mismatch (e.g., Rclunk dispatched against outstanding Twalk) → `-1` |
| `9p_session.close_with_inflight_refused` | close() while Inflight ≠ {} → `-1` |
| `9p_session.state_gate_send_walk_before_open` | send_walk in INIT or VERSIONED → `-1` |
| `9p_session.lopen_round_trip` | Tlopen→Rlopen; qid + iounit surfaced; fid stays bound |
| `9p_session.lcreate_round_trip` | Tlcreate(name)→Rlcreate; qid + iounit surfaced |
| `9p_session.read_round_trip` | Tread→Rread; zero-copy data pointer + count surfaced |
| `9p_session.write_round_trip` | Twrite(payload)→Rwrite; accepted count surfaced |
| `9p_session.lopen_with_inflight_on_fid_refused` | concurrent Tlopen on same fid → `-1` (server-side state mutation exclusive) |
| `9p_session.read_permits_concurrent` | two Tread on same fid, different offsets → both accepted (offset is explicit) |
| `9p_session.io_from_unbound_fid_refused` | all four IO ops on unbound fid → `-1` |
| `9p_session.io_before_open_refused` | all four IO ops in INIT state → `-1` |

The tests use synthesized Rmsg buffers (helper `synth_*` functions in the test file) to drive the dispatcher without an actual server. P5-transport will provide the actual byte-pipeline.

## Error paths

`-1` returned from:
- Lifecycle: NULL session, magic mismatch, invalid args.
- Send-side: state mismatch (e.g., send_walk in INIT), unbound src, bound new, root fid violation, in-flight conflict, window full (`alloc_tag` returns -1), buffer too small, wire-codec failure.
- Receive-side: NULL session / NULL rmsg / NULL out, malformed header, type mismatch with outstanding kind, tag out of range or inactive, parse failure.

## Performance characteristics

- Per-op cost: O(1) for tag alloc (linear scan over 64-element bitmap; tight loop fits in cache), O(N) for fid table operations where N = `n_bound_fids` (typically < 32; linear scan is fine).
- No allocation, no locking. Per-Proc serialization is the kernel's responsibility (handle-table mutex).
- Memory: `sizeof(struct p9_session)` ≈ 4 KiB (256 fids × 4 B + 64 outstanding × 32 B + bookkeeping). One SLUB cache slot per Proc with an active session.

## Status

| Component | State |
|---|---|
| State machine (INIT/VERSIONED/OPEN/CLOSED) | **Landed (P5-session)** |
| Tag pool + fid table + outstanding bookkeeping | **Landed (P5-session)** |
| Version + Attach handshake | **Landed (P5-session)** |
| Walk + Clunk + Rlerror | **Landed (P5-session)** |
| Spec invariants I-10 + I-11 + OutOfOrderCorrectness + FlowControl | **Pinned (P5-session)** |
| IO family Send/Receive (Tlopen / Tlcreate / Tread / Twrite) | **Landed (P5-wire-io)** |
| Metadata family Send/Receive (Tgetattr / Tsetattr / Treaddir / Tstatfs / Tfsync) | Phase 5+ (P5-wire-meta) |
| Mutation family | Phase 5+ (with P5-wire-mutation) |
| Lock family + Xattr family | Phase 5+ (with P5-wire-lock / -xattr) |
| Stratum extensions (Tsync / Treflink / Tbind / Tunbind / Tfallocate / Tfadvise) | Phase 5+ (with P5-wire-stratum-ext) |
| Partial-walk semantics (Rwalk's nwqid < requested nwname) | Phase 5+ (P5-session-walk-partial) |

## Known caveats / footguns

1. **`p9_dispatch_result` is a 200+ byte struct**; the dispatcher zeros it on every call (via internal `zero_result`). Tests declare the struct UNINITIALIZED at function scope — the dispatcher's zero is the load-bearing init. Tests must not read `r.field` if dispatch returned `-1`.
2. **Tversion lives out-of-band**: it uses NOTAG (0xFFFF) which is outside the `outstanding[0..63]` range. The dispatcher special-cases Rversion (handles it without an outstanding-table lookup). State must be INIT to dispatch Rversion.
3. **Tclunk's Send-time unbind is intentional**. The fid is removed from `bound_fids` BEFORE the Rclunk arrives. Subsequent sends targeting that fid fail at the precondition check. Rclunk's dispatch is a no-op on `bound_fids`. Rlerror on Tclunk: fid stays unbound (the client already treated it as gone).
4. **Partial-walk binding** (where `nwqid < nwname` in Rwalk): at P5-session, we bind `new_fid` unconditionally on any Rwalk success. The nuanced 9P2000.L semantic — server binds at the LAST walked qid; if no qids returned, new_fid stays unbound — lands in a future P5-session-walk-partial chunk. At v1.0 with all current tests using `nwname=0` (clone), this is benign.
5. **Concurrency**: the session struct is NOT thread-safe internally. Per-Proc serialization (via Proc lock, or per-session mutex if needed) is the kernel's responsibility. The libstratum-9p reference at `stratum/v2/docs/reference/23-9p_client.md` §"Concurrency model" frames this as "One client = one connection = one fid namespace."

## Naming rationale

`p9_session_*` API; struct fields lowercase per the kernel's C99 style (see `CLAUDE.md §"Style policies"`). The session module is named to match the spec's terminology — `specs/9p_client.tla` talks about "session", "tag pool", "outstanding"; the impl uses the same nouns.

## Spec cross-reference

The session is the kernel-side realization of every spec action in `specs/9p_client.tla`. The state-machine cross-reference table at the top of this doc enumerates the mapping; `specs/SPEC-TO-CODE.md`'s 9p_client section consolidates it for the full Phase 5 stack.
