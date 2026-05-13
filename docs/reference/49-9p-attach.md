# 49. p9_attach — mount creation primitive

## Purpose

The kernel-internal machinery that the future `attach_9p` syscall handler will call. Per ARCH §9.6, the syscall decomposition is:

```
attach_9p(transport_fd, aname, n_uname) → spoor_fd
mount(source_spoor_fd, target_path, flags) → 0
```

`p9_attach` is the body of the first syscall, minus the user-visible fd-lookup boilerplate. Takes a transport_ops vtable + handshake parameters; allocates and initializes a heap-resident `p9_client`; drives `Tversion + Tattach`; returns a heap-managed `struct p9_attached` that owns the client and produces dev9p-backed Spoors for callers.

### Why kernel-internal at v1.0

At v1.0 the kernel has no fd-syscall surface — no `open`/`close`/`read`/`write` syscalls, no way for userspace to obtain a `KOBJ_SPOOR` handle pointing at a byte-pipe Spoor. The user-visible `SYS_ATTACH_9P` SVC handler is therefore **deferred** to a chunk that builds out the fd-syscall infrastructure (likely P5-fd-syscalls or P6-syscall-surface). The substantive work — wrapping a transport in a client, driving the handshake, producing a dev9p Spoor — lives here and is ready for the syscall handler to glue around.

Kernel callers that need 9P mounts before the syscall infrastructure exists:
- Tests (this chunk).
- The P5-stratumd boot path when it lands (init creates the transport Spoor + calls `p9_attached_create` directly, no SVC involved).

## Public API

```c
struct p9_attached {
    u32                  magic;
    struct p9_client    *client;       // heap-allocated; ~12 KiB
    u8                  *recv_buf;     // heap-allocated; sized to msize
    size_t               recv_cap;
    u32                  root_fid;     // the bound fid from Tattach
    u32                  msize;        // negotiated
    bool                 handshake_ok;
};

struct p9_attached *p9_attached_create(
    struct p9_transport_ops transport_ops,
    size_t                  recv_cap,
    u32                     root_fid,
    u32                     msize,
    const u8               *uname, size_t uname_len,
    const u8               *aname, size_t aname_len,
    u32                     n_uname);

struct Spoor *p9_attached_root_spoor(struct p9_attached *a);

void          p9_attached_destroy(struct p9_attached *a);

bool          p9_attached_is_open(const struct p9_attached *a);
```

### Allocation strategy

- `struct p9_attached`: small (~40 bytes); kmalloc'd.
- `struct p9_client`: ~12 KiB. kmalloc routes large requests through `alloc_pages` (slub.c bypass at `SLUB_MAX_OBJECT_SIZE`); allocator chooses order=2 (16 KiB) for a 12 KiB request.
- `recv_buf`: caller-specified size; kmalloc.

If any allocation or the handshake fails, `p9_attached_create` cleans up all intermediate allocations and returns NULL. No partial leaks possible.

### Lifecycle

1. **Create**: caller provides transport_ops (e.g., loopback-backed for tests; Spoor-backed when P5-spoor-transport lands). `p9_attached_create`:
   - Allocates the attached wrapper, client, recv_buf.
   - `p9_client_init` (no I/O — sets up state machines).
   - `p9_client_handshake` (drives `Tversion + Tattach`; transport activity happens here).
   - On success, returns ownership of the heap-resident `struct p9_attached`.
2. **Use**: `p9_attached_root_spoor(a)` returns a dev9p Spoor wrapping `(client, root_fid)` with `fid_owned=false`. Caller exercises walk/open/read/write/clunk on it as a normal Spoor.
3. **Destroy**: caller has released all dev9p Spoors derived from `a->client` (including the root + any walk-derived) — at v1.0 enforced by convention. `p9_attached_destroy`:
   - Clunks `root_fid` via the client (Tclunk over the transport).
   - `p9_client_close` (graceful close of the transport).
   - `p9_client_destroy` (clobbers magic, tears down session + transport).
   - Frees recv_buf, client, attached wrapper.

The ordering discipline ("destroy attached AFTER all Spoors are clunked") is by convention at v1.0; a future spec extension (P5-attach-mount's namespace.tla extension) will formalize.

## Implementation

| File | Purpose |
|---|---|
| `kernel/include/thylacine/9p_attach.h` | Public API + `struct p9_attached` shape |
| `kernel/9p_attach.c` | Create / destroy / root_spoor + is_open helpers |
| `kernel/test/test_9p_attach.c` | 4 lifecycle tests against loopback transport |

## Compile-time invariants

`_Static_assert` in `kernel/9p_attach.c`:
- `P9_ATTACHED_MAGIC == 0x50394154u` ("P9AT" little-endian).

## Tests

4 tests in `kernel/test/test_9p_attach.c`:

| Test | Covers |
|---|---|
| `p9_attached.create_destroy` | End-to-end lifecycle; `is_open` is true post-create |
| `p9_attached.handshake_failure_returns_null` | Server Rlerror on Tattach; create returns NULL with all intermediate allocations cleaned up |
| `p9_attached.root_spoor_walk_read` | root_spoor → walk → open → read end-to-end through the attached wrapper |
| `p9_attached.query_helpers` | is_open NULL-safety; basic state queries |

The handshake-failure test specifically exercises the cleanup path on partial-init failure — if the bug were a leak of the half-built client or buffers, the heap would grow across test runs (caught by future leak-tracking; here verified by completion).

## Error paths

`p9_attached_create` returns NULL on:
- `recv_cap < P9_HDR_LEN` (transport frame buffer too small).
- `msize == 0`.
- kmalloc OOM on wrapper / client / recv_buf.
- `p9_client_init` failure (arg validation).
- `p9_client_handshake` failure (transport error, Rlerror on Tversion/Tattach).

All NULL-return paths clean up partial state.

## Performance characteristics

- One transport round trip for `Tversion`, one for `Tattach` — total 2 RTT for create.
- Three heap allocations per create (wrapper + client + recv_buf).
- Steady-state cost (after handshake): same as the underlying transport for each Spoor op.

## Status

| Component | State |
|---|---|
| Kernel-internal create / destroy / root_spoor / is_open | **Landed (P5-attach-create)** |
| User-visible `SYS_ATTACH_9P` SVC handler | Deferred until fd-syscall infrastructure exists |
| Spoor-backend transport (Spoor as the transport_ops ctx) | **P5-spoor-transport** (next; required for real `stratumd` integration) |
| Mount syscall + Territory mount table | **P5-attach-mount** |

## Known caveats / footguns

1. **Ordering discipline**: `p9_attached_destroy` must be called AFTER all dev9p Spoors derived from this attached have been `spoor_clunk`'d. Walk-derived Spoors clunk their own fids on close; the root Spoor's `fid_owned=false` means its close doesn't touch root_fid (the attached owns it). Destroy then clunks root_fid + tears down. Calling destroy with live Spoors leaves dangling client pointers in their privs — subsequent ops will see magic mismatch and fail, but the failure mode is messy. Future spec extension formalizes.

2. **The user-visible syscall is deferred.** The body of `attach_9p` is implemented here; the SVC dispatch + handle-table glue is not. At v1.0 the only callers are kernel-internal (tests + future boot path). Userspace 9P mounts are gated on fd-syscall infrastructure.

3. **Single-ref dev9p Spoors only.** The root Spoor returned by `p9_attached_root_spoor` is single-ref at v1.0 — `spoor_ref`-ing it for use across multiple kernel call sites is not supported (the first clunk would free the priv; later refs would observe NULL aux). Mirrors the dev9p caveat (47-9p-client.md #5).

4. **Handshake errors include transport-level failures.** A transport that fails between Tversion and Tattach (e.g., loopback's pending-response-not-drained discipline catching a test bug) returns NULL from create. The error type isn't surfaced to the caller — they just see NULL. Future revisions may add an out-param for the underlying errno.

5. **The transport_ops vtable is captured by value.** `p9_attached_create` copies the struct into `client->transport`; the caller's ops struct can be released after the call returns. The ops's `ctx` pointer (e.g., the loopback struct) must outlive the attached.

## Naming rationale

`p9_attached` is direct ("an attached 9P session"). No thematic marsupial alternative reads more clearly. The struct name parallels `p9_client` / `p9_session` / `p9_transport` — the namespace prefix is consistent across the 9P stack.

## Spec cross-reference

No new TLA+ module. `p9_attached` is pure composition over `specs/9p_client.tla`-spec'd lower layers; mount-lifecycle invariants land at **P5-attach-mount** in the planned `specs/namespace.tla` extension (per ARCH §9.6.6).

## Reference

- ARCH §9.6 (filesystem-as-Spoor + decomposed mount).
- ARCH §11.2 (syscall table — `attach_9p` row).
- `docs/reference/47-9p-client.md` (the underlying client API).
- `docs/reference/48-dev9p.md` (the Dev vtable this produces Spoors for).
- ROADMAP §7.2 (the P5-attach sub-chunk plan).
