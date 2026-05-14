# 55. SYS_ATTACH_9P — userspace 9P client attach (P5-attach-syscall)

The user-visible body of ARCH §9.6.1's `attach_9p(transport_fd, aname, n_uname) → spoor_fd`. Closes the last deferred SVC handler from the P5-attach plan; with this chunk plus the P5-mount-syscall that follows, userspace can compose Stratum-style mount workflows entirely through syscalls.

---

## Purpose

Until this chunk, `kernel/9p_attach.c::p9_attached_create` was kernel-internal — usable from tests + the future kernel-driven stratumd boot path, but not exposed to userspace. SYS_ATTACH_9P plugs the missing user-visible entry: takes a Spoor pair (tx + rx — produced by SYS_PIPE today, by a future SYS_SOCKETPAIR/SYS_VSOCK_CONNECT later), drives the 9P handshake against whatever server is on the other side, and hands back a KOBJ_SPOOR fd that represents the 9P tree's root.

Closing that fd tears down the entire attach session — the dev9p Spoor's `attached_owner` field (new at this chunk) wires the cleanup path.

---

## ABI

```
SYS_ATTACH_9P = 13

Args:
  x0 = tx_fd       — client → server byte pipe (KOBJ_SPOOR with RIGHT_WRITE)
  x1 = rx_fd       — server → client byte pipe (KOBJ_SPOOR with RIGHT_READ)
  x2 = aname_va    — user-VA pointer to the attach-name string
  x3 = aname_len   — bytes; 0 ≤ len ≤ SYS_ATTACH_ANAME_MAX (256)
  x4 = n_uname     — u32; 0 for no-auth attach at v1.0

Return:
  x0 = new fd (KOBJ_SPOOR pointing at the 9P tree's root) on success;
  x0 = -1 on failure (with all partial state rolled back).
```

### Duplex vs half-duplex transports

- **Duplex** (Unix socket, vsock — Phase 5+): pass the same fd as `tx_fd` and `rx_fd`. The handler detects equality and only spoor_ref's once.
- **Half-duplex** (Plan 9 pipes from SYS_PIPE today): create two pipe pairs and pass the matching write-end + read-end fds.

The kernel's spoor-transport adapter handles both shapes uniformly.

### Rights gates

- `tx_fd` requires `RIGHT_WRITE`.
- `rx_fd` requires `RIGHT_READ`.

SYS_PIPE grants `READ|WRITE|TRANSFER` on both ends, so freshly-pipe()'d fds pass both gates trivially.

### Returned fd rights

`RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER`. The returned Spoor is the dev9p-backed 9P root — userspace can read/write through it (which routes back across the wire via dev9p_read / dev9p_write), or pass it to SYS_MOUNT (Phase 5+; deferred), or transfer it across 9P sessions when that path lands.

---

## Userspace API — `<thyla/syscall.h>`

```c
__attribute__((always_inline))
static inline long t_attach_9p(long tx_fd, long rx_fd,
                               const char *aname, size_t aname_len,
                               unsigned long n_uname);
```

Same shape as the kernel ABI. Returns the new fd or -1.

---

## Implementation

`kernel/syscall.c::sys_attach_9p_handler`. The handler composes:

1. **Validate aname length** (`≤ SYS_ATTACH_ANAME_MAX = 256`) and user-VA range (same `UACCESS_USER_VA_TOP` bound as SYS_PUTS).

2. **Look up handles** via `sys_lookup_spoor` (handle_get + KOBJ_SPOOR check + rights check). On failure → -1.

3. **Take independent refs** on the transport Spoors. The original transport_fd handles still hold their own refs; this is in addition. If `tx == rx`, only one `spoor_ref`. The attach owns these refs; dev9p_close on the root Spoor will release them.

4. **Copy aname to kernel scratch** via per-byte `uaccess_load_u8`. Up to 256 bytes; stack buffer. On fault → -1 + rollback the spoor_ref's.

5. **Allocate the adapter struct** on the heap (`kmalloc(sizeof(struct p9_spoor_transport), KP_ZERO)`). Its lifetime is tied to the attach session.

6. **Initialize the adapter** with `owns_spoors=false`. The SVC handler manages the Spoor refs explicitly (see step 3 + the dev9p_close cleanup below); the adapter's own close becomes a no-op.

7. **Call `p9_attached_create`** with the adapter's transport_ops vtable + the default msize (4 KiB) + the default root_fid (1) + the user-supplied aname. The function drives Tversion + Tattach over the byte pipe. On failure → -1 + rollback (unref both spoors + kfree adapter).

8. **Get the root Spoor** via `p9_attached_root_spoor`. This is a dev9p-backed Spoor with `fid_owned=false`.

9. **Patch the root's `dev9p_priv`** with `attached_owner = att` and `adapter_to_free = adapter`. These tell `dev9p_close` to run the attach-teardown path.

10. **Install** as a KOBJ_SPOOR handle with `RIGHT_READ|WRITE|TRANSFER`. On handle table full → roll back by calling `spoor_clunk(root)` (which fires `dev9p_close` → full teardown of attach + adapter + transport refs).

11. **Return** the new fd.

### dev9p_priv extension

```c
struct dev9p_priv {
    u32                magic;
    struct p9_client  *client;
    u32                fid;
    bool               fid_owned;
    // P5-attach-syscall: non-NULL on root Spoors created by
    // SYS_ATTACH_9P. dev9p_close on a Spoor with attached_owner
    // set tears down the entire attach session.
    struct p9_attached       *attached_owner;
    struct p9_spoor_transport *adapter_to_free;
};
```

Walk-derived Spoors (from `dev9p_walk`) AND kernel-internal callers (`dev9p_attach_client` from tests + the future stratumd boot path) leave both new fields NULL — they continue to use the existing `fid_owned` path.

### dev9p_close teardown discipline

```c
if (priv->attached_owner) {
    struct p9_spoor_transport *adp = priv->adapter_to_free;
    p9_attached_destroy(priv->attached_owner);
    if (adp) {
        struct Spoor *tx = adp->tx_spoor;
        struct Spoor *rx = adp->rx_spoor;
        if (tx) spoor_unref(tx);
        if (rx && rx != tx) spoor_unref(rx);
        kfree(adp);
    }
} else if (priv->fid_owned) {
    (void)p9_client_clunk(priv->client, priv->fid);
}
```

`p9_attached_destroy` clunks the root_fid via Tclunk + tears down the p9_client (including the transport's close hook — a no-op since `owns_spoors=false`). Then we explicitly unref the transport Spoors and kfree the adapter.

---

## Tests

The success path requires a 9P server on the other end of the transport — that's a userspace probe with a fork-and-respond pattern (deferred; needs a second Proc spawning + a Rversion/Rattach responder). Kernel-internal coverage at this chunk is the rejection paths:

| Test | Covers |
|---|---|
| `sys_attach_9p.rejection_paths` | Demonstrates that `handle_get` (the structural pre-check inside `sys_attach_9p_handler`) returns NULL for out-of-range and closed fds. The handler returns -1 before any allocation when this fires. |

The substantive success-path test will land in a follow-up chunk that introduces a 9P-responder userspace binary.

Test count 399 → 400. **400/400 PASS × default + UBSan**.

### Why no kernel-internal happy-path test

The SVC handler calls `p9_attached_create` which drives a synchronous Tversion + Tattach round trip against the byte pipe. To test the happy path kernel-internally, the test would need to either:
- Spawn a separate kernel thread acting as the 9P server (similar to the rendez-handoff pattern) — feasible but adds spawn-and-coordinate plumbing for a single test.
- Pre-stage canonical Rversion + Rattach bytes in the rx pipe before calling the handler. Works if the Tattach tag (allocated by `alloc_tag`) is predictable; it is (= 0 since freshly allocated after Tversion), but the test then encodes a fragile contract with the session's tag-allocation strategy.

Both are doable; both add scaffolding for limited additional confidence. The substantive correctness path is exercised by the existing `test_9p_attach.create_destroy` + `test_9p_spoor_transport.end_to_end_handshake` (which together cover `p9_attached_create` + the spoor-transport adapter against synthetic responders). What's new at SYS_ATTACH_9P is the **SVC dispatch shape** + **dev9p_priv attached_owner wiring** + **rollback discipline**. Those are testable structurally without a server.

A userspace probe binary that exercises SYS_ATTACH_9P end-to-end against a 9P-responder companion is the natural follow-up (similar to `/pipe-probe`). Deferred to P5-attach-probe.

---

## Spec posture

No new TLA+ module. Pure composition:

- `specs/handles.tla` — HandleAlloc + rights gates carry through.
- `specs/9p_client.tla` — session correctness invariants propagate through `p9_attached_create`.
- `specs/pipe.tla` — the transport Spoors' wait/wake protocol composes through the adapter.

No new invariants at the SYS_ATTACH_9P layer — it's plumbing composition. The lifecycle discipline (`dev9p_priv.attached_owner` wired to dev9p_close's teardown branch) is structurally enforced by code shape: only the SVC handler sets `attached_owner`, and only `dev9p_close` reads it.

---

## Audit posture

Extends three surfaces already on the trigger list (CLAUDE.md / ARCH §25.4):

- **kernel/syscall.c** — new SVC handler. The rollback discipline is the load-bearing piece: every failure point unwinds back to a clean "no partial state" state.
- **kernel/dev9p.c** — new attach-owner branch in `dev9p_close`. Only fires when `attached_owner` is set (by the SVC handler). For walk-derived Spoors + test-side callers, the branch is dead — they leave both fields NULL.
- **kernel/include/thylacine/dev9p.h** — added 2 fields to `dev9p_priv` (now 32 bytes vs 24).

No new attack surfaces. The user-VA aname copy uses the existing `uaccess_load_u8` primitive with the same bound and the same fault-fixup discipline as SYS_PUTS / SYS_WRITE.

---

## Status

| Component | State |
|---|---|
| SYS_ATTACH_9P handler in kernel/syscall.c | **Landed (P5-attach-syscall)** |
| dev9p_priv extension + dev9p_close teardown branch | **Landed (P5-attach-syscall)** |
| Userspace libt stub `t_attach_9p` | **Landed (P5-attach-syscall)** |
| Kernel-internal rejection-path test | **Landed (P5-attach-syscall)** |
| Userspace probe binary with a 9P responder server | Deferred to P5-attach-probe |
| SYS_MOUNT / SYS_UNMOUNT user-visible (consumes KOBJ_SPOOR fd) | Deferred to P5-mount-syscall |

---

## Known caveats / footguns

### Hardcoded msize = 4096 + root_fid = 1

The handler uses fixed defaults: `msize = 4096` (matches `SYS_RW_MAX` + `PIPE_BUF_SIZE`), `root_fid = 1`. For Stratum integration these are the right defaults. Future iterations may expose them as additional SVC args; v1.0 omits the complexity.

### aname max = 256 bytes

Empirically generous for typical 9P attach strings ("/", "pool/data", "tcp!host!port"). Phase 5+ can lift the cap if a use case demands it; would need to switch from a stack scratch to a heap or page-aligned buffer.

### Transport Spoor refs

The SVC handler takes independent `spoor_ref`s on the transport Spoors. The original transport_fd handles still hold their own refs via the handle table. Userspace closing the original transport_fd after a successful `t_attach_9p` is safe — the attach holds its own refs. Closing the returned root fd tears down the attach AND drops the SVC-handler-side transport refs.

### Half-duplex pipe testing requires two pipe pairs

For tests/probes using `SYS_PIPE` (half-duplex), the userspace pattern is:

```c
long rd_A, wr_A, rd_B, wr_B;
t_pipe(&rd_A, &wr_A);   // client → server
t_pipe(&rd_B, &wr_B);   // server → client
// Server side reads from rd_A, writes to wr_B.
// Client side does:
long root = t_attach_9p(wr_A, rd_B, "", 0, 0);
```

For duplex Unix sockets / vsock (when those land): `tx_fd == rx_fd`.

### handle_alloc failure rolls back via spoor_clunk

If `handle_alloc` fails (table full), the handler calls `spoor_clunk(root)` to roll back. The clunk fires `dev9p_close` which sees `attached_owner` set and tears down the entire session (attached + adapter + transport refs). This is the same path userspace's eventual close takes — single cleanup discipline.

### Userspace probe is the integration test

Until P5-attach-probe lands with a 9P-responder companion, the happy-path correctness rests on the kernel-internal `test_9p_attach.create_destroy` + `test_9p_spoor_transport.end_to_end_handshake` tests + the rejection-path test landed here.

---

## Naming rationale

`SYS_ATTACH_9P` = 13 — appends to the existing enumeration. The `_9P` suffix distinguishes from future Linux-style `attach(fd)` syscalls that might land for non-9P backends; matches ARCH §11.2's syscall table verbatim.

`t_attach_9p` userspace stub mirrors the kernel name.

---

## Reference

- ARCH §9.6 (Mount: the filesystem-as-Spoor principle) + §9.6.1 (attach decomposition) + §9.6.5 (authentication discussion — n_uname).
- ARCH §11.2 (Syscall table — SYS_ATTACH_9P row).
- `docs/reference/49-9p-attach.md` (kernel-internal `p9_attached_create`).
- `docs/reference/48-dev9p.md` (the Dev whose vtable backs the returned Spoor).
- `docs/reference/50-9p-spoor-transport.md` (the adapter wiring Spoor read/write into transport_ops).
- `docs/reference/52-sys-pipe.md` (SYS_PIPE — produces the transport Spoor pair).
- `specs/9p_client.tla` (session correctness).
- `specs/handles.tla` (RightsCeiling enforcement on the returned fd).
