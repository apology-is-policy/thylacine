# 47. 9P client high-level API

## Purpose

Consolidates the codec (`44-9p-wire`), state machine (`45-9p-session`), and transport (`46-9p-transport`) into one usable API. The client exposes a function per filesystem operation — handshake, walk, clunk, lopen, read, write, getattr, setattr, readdir, statfs, fsync, symlink, mknod, rename, readlink, link, mkdir, renameat, unlinkat — each returning `0` on success or `-errno` on failure. Callers above this layer (a future mount/Dev/syscall integration) don't need to know about tag allocation, message framing, or outstanding-request bookkeeping.

Layering:

```
syscall / mount / Dev      ← future caller of this layer
   │
p9_client  (this header)
   │
p9_session  (state machine)   p9_transport  (byte pipe)
   │                              │
p9_wire     (codec)           {loopback, Spoor-over-Unix-socket, ...}
```

## Public API

```c
// Lifecycle.
int  p9_client_init(struct p9_client *c, u32 root_fid, u32 msize,
                     struct p9_transport_ops transport_ops,
                     u8 *recv_buf, size_t recv_cap);
void p9_client_destroy(struct p9_client *c);
int  p9_client_close(struct p9_client *c);

// Handshake.
int  p9_client_handshake(struct p9_client *c,
                          const u8 *uname, size_t uname_len,
                          const u8 *aname, size_t aname_len,
                          u32 n_uname);

// Path.
int  p9_client_walk(struct p9_client *c, u32 src_fid, u32 new_fid,
                     u16 nwname, const u8 *const *names,
                     const size_t *name_lens,
                     u16 *out_nwqid, struct p9_qid *out_qids);
int  p9_client_walk_one(struct p9_client *c, u32 src_fid, u32 new_fid,
                         const u8 *name, size_t name_len,
                         struct p9_qid *out_qid);
int  p9_client_clunk(struct p9_client *c, u32 fid);

// IO.
int  p9_client_lopen(struct p9_client *c, u32 fid, u32 flags,
                      struct p9_qid *out_qid, u32 *out_iounit);
int  p9_client_lcreate(struct p9_client *c, u32 fid,
                        const u8 *name, size_t name_len,
                        u32 flags, u32 mode, u32 gid,
                        struct p9_qid *out_qid, u32 *out_iounit);
int  p9_client_read(struct p9_client *c, u32 fid, u64 offset,
                     u32 count, u8 *out_data, u32 *out_count);
int  p9_client_write(struct p9_client *c, u32 fid, u64 offset,
                      u32 count, const u8 *data, u32 *out_accepted);

// Metadata.
int  p9_client_getattr(...);  // ← Linux statx-shape result
int  p9_client_setattr(...);
int  p9_client_readdir(...);  // ← dirent-stream copy semantics
int  p9_client_statfs(...);
int  p9_client_fsync(...);

// Mutation.
int  p9_client_symlink(...);
int  p9_client_mknod(...);
int  p9_client_rename(...);
int  p9_client_readlink(...);
int  p9_client_link(...);
int  p9_client_mkdir(...);
int  p9_client_renameat(...);
int  p9_client_unlinkat(...);

// Query.
bool   p9_client_is_open(const struct p9_client *c);
size_t p9_client_inflight(const struct p9_client *c);

// Async (Loom) front-end -- the pluggable-completion seam (Loom-2b/3/4).
int  p9_client_submit_async(struct p9_client *c, struct p9_rpc *rpc,
                            p9_session_build_fn build, void *build_ctx);
int  p9_client_reader_pump_once(struct p9_client *c);          // 1 / 0 / -EIO
int  p9_client_reader_pump_once_deadline(struct p9_client *c,  // enum p9_pump_result
                                         u64 deadline_ns);
void p9_client_handoff_reader(struct p9_client *c);
void p9_client_abandon_async(struct p9_client *c, struct p9_rpc *rpc);

enum p9_pump_result {            // signed: DEAD is the only negative
    P9_PUMP_DEAD     = -1,       // EOF/recv error (marked dead) or death-interrupt
    P9_PUMP_IDLE     =  0,       // idle deadline lapsed at a frame boundary
    P9_PUMP_PROGRESS =  1,       // demuxed one reply frame
    P9_PUMP_BUSY     =  2,       // another thread holds the reader role
};
```

### Deadline-aware reader pump (Loom-4)

`p9_client_reader_pump_once_deadline` is the SQPOLL idle pump (LOOM.md §8.6): it
drives the elected reader for one frame, but arms `deadline_ns` on ONLY the FIRST
recv -- the frame boundary, where a timeout consumes no bytes so the shared byte
stream stays synced (#841). The rest of the frame blocks unconditionally (a
mid-frame timeout would desync). It returns `P9_PUMP_PROGRESS` (demuxed a frame),
`P9_PUMP_IDLE` (the deadline lapsed at the boundary; the session stays alive +
synced -- the kthread parks + retries), `P9_PUMP_BUSY` (another reader holds the
role; the caller defers), or `P9_PUMP_DEAD` (EOF/error marks the session dead, or
a death-interrupt unwinds leaving it for survivors). On a backend with no
`set_recv_deadline` op (e.g. spoor) the deadline is inert and it blocks like the
plain pump. The mechanism rides the transport `set_recv_deadline`/`recv_timed_out`
vtable ops (reference 46).

### Composition pattern

Every operation follows the same shape:

```
1. session.send_*  → build Tmsg into c->out_buf, allocate tag,
                     mark outstanding.
2. transport.exchange → write Tmsg + read Rmsg + dispatch through
                        session.dispatch_rmsg, which fills in a
                        `struct p9_dispatch_result`.
3. result extraction → pull the relevant fields out of the
                       dispatch result and copy them to the
                       caller's output buffers.
4. error mapping → Rlerror's `ecode` → `-ecode`; lower-layer
                   failures → `-EIO`; bad args → `-EINVAL`.
```

The pattern is the only thing in this layer. The reason each op is a separate function (instead of a generic dispatch table) is that each op has different output-extraction logic (qid vs string vs data buffer vs no payload).

### Error convention

```c
0       — success
-errno  — failure (signed-errno convention; Linux errnos)

-EINVAL — bad arguments at the client layer (NULL, magic mismatch)
-EBUSY  — session not OPEN (handshake hasn't run)
-EIO    — lower-layer failure: send/recv error, frame malformed,
          tag pool full, fid bookkeeping conflict
-<n>    — Rlerror surface: n = the Linux errno the server returned
```

### Buffer ownership

- **Inline state**: `struct p9_client` embeds session (~4 KiB) + transport (~80 B) + an inline outbound buffer (8 KiB by default). Total struct size ≈ 12 KiB; must be allocated statically or heap-side, never on the stack.
- **Caller-provided recv buffer**: passed to `p9_client_init`; sized to negotiated msize.
- **Caller-provided I/O buffers**: read/write/readdir output buffers are passed per-call (no inline-data storage in the client struct).

## Implementation

| File | Purpose |
|---|---|
| `kernel/include/thylacine/9p_client.h` | Public API + `struct p9_client` shape |
| `kernel/9p_client.c` | 25 op functions + internal `copy_qid` / `copy_attr` / `copy_statfs` helpers (field-by-field struct copies; kernel doesn't link libc → no `memcpy`) |
| `kernel/test/test_9p_client.c` | 13 tests covering composition end-to-end through loopback |

Internal struct-copy helpers (`copy_qid`, `copy_attr`, `copy_statfs`) replace implicit `*dst = *src` assignments that the compiler would otherwise lower to `memcpy`. The kernel doesn't link libc, so `memcpy` is undefined; field-by-field copies sidestep the issue. This mirrors the same discipline applied in P5-session (where all init paths are explicit instead of `memset`-ed).

### Elected-reader pipeline (#841)

The as-built client is **pipelined and multi-Proc-shared** (a single `p9_client` backs a dev9p mount, so every Proc whose territory resolves through that mount drives the same client concurrently from different CPUs). It uses the Plan 9 `devmnt`/`mountio` **elected-reader** model rather than the serial single-in-flight stand-in the P5-client first shipped (the "R15-c F230" regression that #841 restored away from). The full design + invariants are in `ARCH §21.10`; the load-bearing points for a maintainer:

- Each op allocates a stack `struct p9_rpc` {tag, `done`/`dead`/`be_reader` flags, own single-waiter `Rendez`, `reply_buf`} and registers it in the tag-indexed `c->inflight[]` under `c->lock`.
- `client_run` is the core: submit under `c->lock` (alloc tag, build, `transport_send` frame-atomic), then the **reader-election loop** — a submitter with no reply yet becomes *the* reader (one at a time via `c->reader_active`), drops `c->lock`, `recv`s one frame, retakes the lock, demuxes it by tag to the owning rpc (copies frame → that rpc's `reply_buf`, wakes it), and repeats until its own reply lands; otherwise it sleeps on its own rpc's `Rendez`. On departing the reader role it **hands off** to one still-pending rpc.
- **`c->lock` is NEVER held across the blocking `recv`** (the soundness fix — the serial regression held a spinlock across the sleep). The deadline is caller-set: `HANDSHAKE_DEADLINE` for the serial fresh-client handshake, then `0` (block until reply / EOF / death) for steady state. Death-interruptible via #811.
- **Reply-buffer lifetime (audit F1):** the read/readdir/readlink dispatch results zero-copy alias into the rpc's `reply_buf`, and the caller copies them out *after* `client_run` returns — so `client_run` does NOT free the buffer on the DONE path; it stashes it in `c->done_reply_buf` (freeing the prior), freed at the next completion or at destroy (both under `c->lock`, holds at most one buffer).
- The session is marked `c->dead` on transport EOF/error OR a demux-level protocol violation; every in-flight rpc is then failed `-EIO`. A dead session rejects all subsequent ops.
- **Tflush-on-abandon (#845):** when a Proc dies mid-op (`CLIENT_WAIT_DIED`), `client_run` NULLs `inflight[tag]`, frees the rpc's `reply_buf`, and sends `Tflush(oldtag=T)` under `c->lock` (`p9_session_send_flush` + `p9_transport_send`). The abandoned tag T stays *reserved* (`session.outstanding[T].awaiting_flush`) and is freed **only** by its `Rflush` — never by a late original reply (the I-10 reuse-race guard: per 9P, `oldtag` is not reusable until `Rflush`, so a stray reply can't be mis-attributed to a reused tag). The flush is ownerless (no `inflight[F]`); a survivor's elected reader drains the `Rflush` via the ownerless demux path. `dispatch_rmsg` consumes a late original on an `awaiting_flush` tag without clearing it, and on the `Rflush` frees both T and the flush tag F. If the flush can't be built/sent the client falls back to the pre-#845 leave-active path (no regression). See ARCH §21.10 "Tflush-on-abandon".

## Compile-time invariants

`_Static_assert` in `kernel/9p_client.c`:

- `P9_CLIENT_MAGIC == 0x50394354u` ("P9CT").
- `P9_CLIENT_OUT_BUF_MAX >= 256u` (smallest reasonable Tmsg fits).

## Tests

24 tests in `kernel/test/test_9p_client.c` (the sync ops below, plus the
`lock_released_between_ops` reentrancy test, 3 async-seam tests, 3 Loom-engine
tests, and 4 Loom-4 deadline-pump tests). The canonical responder handles every
op type with a sensible canned response; the rlerror responder always returns
`Rlerror{ecode=2}` (ENOENT). One representative test per op category:

| Test | Covers |
|---|---|
| `9p_client.init_destroy` | Lifecycle + magic clobber + invalid-arg refusal |
| `9p_client.handshake` | Tversion + Tattach end-to-end; state machine reaches OPEN; `total_ops == 2` |
| `9p_client.walk_and_clunk` | `walk(nwname=0)` clone + `walk_one(name)` + `clunk` |
| `9p_client.lopen_read` | walk → lopen (iounit round-trip) → read with payload extraction |
| `9p_client.write` | walk → write with accepted-count round-trip |
| `9p_client.getattr` | walk → getattr; struct p9_attr fields round-trip (mode, size) |
| `9p_client.readdir` | walk → readdir; empty dirent stream → count = 0 |
| `9p_client.statfs` | walk → statfs; bsize round-trip |
| `9p_client.mkdir` | walk → mkdir; created_qid surfaced |
| `9p_client.unlinkat` | walk → unlinkat |
| `9p_client.readlink` | walk → readlink; target string surfaced (caller-cap-bounded copy) |
| `9p_client.rlerror_propagates_to_negative_errno` | Server returns Rlerror{ecode=2}; client returns -2 |
| `9p_client.op_before_handshake_returns_ebusy` | Op before handshake → -EBUSY |
| `9p_client.pump_deadline_idle` | Loom-4: empty stream + armed deadline → `P9_PUMP_IDLE`; session stays open + reusable |
| `9p_client.pump_deadline_data_ready_progresses` | Reply on the wire beats the deadline → `P9_PUMP_PROGRESS` + on_complete |
| `9p_client.pump_deadline_chunked_frame_completes` | Frame atomicity: chunked frame under a deadline aggregates → `P9_PUMP_PROGRESS` |
| `9p_client.pump_deadline_busy_when_reader_active` | Another reader holds the role → `P9_PUMP_BUSY` (no-op) |

The canonical responder is intentionally permissive — it accepts every standard 9P2000.L op and returns a fixed canned response. This lets a single test fixture verify any client op without per-test responder customization. Tests that need specific behavior (Rlerror) supply their own responder.

## Error paths

Every public op returns negative on:
- NULL client pointer, magic mismatch → -EINVAL
- session not OPEN (state != OPEN) → -EBUSY
- session.send_* failure (e.g., bad fid, tag pool full) → -EIO
- transport.exchange failure (backend error, frame malformed) → -EIO
- session.dispatch_rmsg surfacing Rlerror → -<server's ecode>

## Performance characteristics

- Per-op cost: 1 backend send + 1+ backend recv calls (depending on partial-read aggregation) + one `kmalloc(recv_cap)` per op for the rpc reply buffer + one frame copy (reader → owner buffer). A buffer pool / read-into-owner-buffer is a v1.x optimization.
- Locking: one per-client spinlock `c->lock`, taken per op and DROPPED across the blocking `recv` (the elected-reader, #841). SMP-safe and shared across Procs (no caller serialization required — the client serializes internally).
- Memory: `sizeof(struct p9_client)` ≈ 12 KiB + at most one `recv_cap`-sized `done_reply_buf` held between completions. Most of the struct is the embedded session's fid + outstanding tables (~4 KiB) + the 8 KiB inline out_buf.

## Status

| Component | State |
|---|---|
| Lifecycle (init / destroy / close) | **Landed (P5-client)** |
| Handshake | **Landed (P5-client)** |
| Path ops (walk / walk_one / clunk) | **Landed (P5-client)** |
| IO ops (lopen / lcreate / read / write) | **Landed (P5-client)** |
| Metadata ops (getattr / setattr / readdir / statfs / fsync) | **Landed (P5-client)** |
| Mutation ops (symlink / mknod / rename / readlink / link / mkdir / renameat / unlinkat) | **Landed (P5-client)** |
| Lock / xattr / Stratum-extension wrappers | Phase 5+ (await codec extensions) |
| Async dispatch (multi-in-flight) | **Landed (#841 elected-reader)** — tag-demuxed, out-of-order, lock-not-held-across-recv |
| Partial-walk handling (Rwalk's nwqid < requested nwname) | Phase 5+ (currently returns -EIO if partial) |

## Known caveats / footguns

1. **`struct p9_client` is ~12 KiB.** Allocate it statically (or heap-side once kalloc exists); never declare it on a stack frame. The kernel's test-thread stack is 16 KiB; one client struct + a few small locals leaves no margin.

2. **`p9_client_read` / `readdir` / `readlink` do COPY semantics** — the lower-layer Rread/Rreaddir/Rreadlink surface a zero-copy pointer that, under the #841 pipeline, aliases the per-op rpc `reply_buf` (NOT the transport recv_buf as in the old serial client). `client_run` keeps that buffer alive past its return via `c->done_reply_buf` (audit F1) so the public op's copy-out is valid; the caller still ends up with an owned copy in its output buffer. A maintainer adding a new zero-copy-aliasing op MUST ensure the buffer outlives the caller's read of the alias.

3. **Partial walks are reported as -EIO.** If a Twalk with `nwname=3` returns `Rwalk{nwqid=2}` (server walked partway then hit a missing component), `p9_client_walk_one` and the general `p9_client_walk` may produce surprising behavior. At v1.0 we treat partial walks as failures; nuanced "you walked partially, here's what got bound" handling is a Phase 5+ extension.

4. **Rlerror's ecode is unconditionally treated as a Linux errno.** The wire protocol allows any u32; servers that send out-of-range values (e.g., the Stratum-specific STM_E* codes — see `stratum/v2/docs/REFERENCE.md` for the canonical list) get propagated as-is. Callers may need to translate.

5. **No retry / reconnect.** A single transport-layer failure puts the transport in ERROR state and the next client op fails with -EIO indefinitely. Recovery requires destroy + re-init at a higher layer. Future work: P5-attach may add a Proc-level "reconnect" affordance.

6. **The client IS internally locked and multi-Proc-shared (#841).** A single `p9_client` backs a dev9p mount and is driven concurrently by every Proc resolving through it; `c->lock` + the elected-reader serialize access. Callers do NOT serialize. (The old serial client required external serialization; that contract is retired.) A Proc that dies mid-op now sends a `Tflush` to reclaim its tag promptly (#845) rather than leaking it until the late reply drains it (the old #841-F2 behavior); the abandoned tag is reserved until its `Rflush`.

7. **Duplicate-reply trust assumption (#845 audit F1, untrusted-server only).** The client trusts the server to send **exactly one reply per tag** — a duplicate same-type reply on a tag the client has since reused is mis-attributed to the new op. This is inherent to 9P (the wire tag is the only correlation key; there is no per-tag generation) and applies to *every* op kind. `Tflush` adds `Rflush` to that set: a non-conformant server that sends a duplicate `Rflush` after the flush tag was freed-and-reused-as-a-flush would prematurely free the new flush's reserved `oldtag`. Does NOT arise with the v1.0 trusted servers (stratumd / kernel dev9p send one `Rflush` per `Tflush`); closing it for an untrusted/remote 9P server needs wire-level tag generations (a v1.x ABI lift, the `n_uname` trust-stamp seam). See `memory/audit_845_closed_list.md` F1.

8. **A walk abandoned after the server bound `new_fid` leaks the fid server-side (#845 audit F3).** If a Proc dies after its `Twalk` was processed by the server, the late `Rwalk` is consumed without a client-side `fid_bind` (the dead Proc can't clunk it anyway). The server holds the fid for the connection's lifetime; bounded per shared client. A v1.x session-teardown sweep could clunk reserved-but-orphaned fids if abandon-heavy server-fid pressure is ever observed.

## Naming rationale

`p9_client_*` is the natural extension of the `p9_session_*` / `p9_transport_*` / `p9_wire_*` family naming. No thematic marsupial name applies — this is mechanical infrastructure that consolidates lower layers. The Plan 9 heritage uses "client" generically for the 9P consumer side; Thylacine's per-Proc 9P consumer follows that convention.

## Spec cross-reference

No new TLA+ module. The client is a pure composition over session + transport, and the spec (`specs/9p_client.tla`) covers the session-level invariants that propagate through this layer unchanged.

## Reference

- ARCH §10 (9P client architecture).
- `docs/reference/44-9p-wire.md` (codec — the layer 3 below this one).
- `docs/reference/45-9p-session.md` (session state machine — the layer 2 below).
- `docs/reference/46-9p-transport.md` (transport byte pipe — the layer 1 below).
- `specs/9p_client.tla` (session-level invariants; transport + client compose unchanged).
- Stratum's `stratum/v2/docs/OS-INTEGRATION.md` (the eventual stratumd target).
