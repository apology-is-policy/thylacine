# 48. dev9p — Dev vtable proxying to the kernel 9P client

## Purpose

The kernel-side realization of ARCHITECTURE.md §9.6: every filesystem entity in Thylacine is a Spoor. `dev9p` is the Dev whose vtable routes every operation (walk / open / read / write / clunk) through a `p9_client` instance to a remote 9P server. With dev9p, a 9P-mounted filesystem becomes indistinguishable from a kernel-internal Dev above the vtable — the Spoor walk algorithm, the syscall surface, and downstream callers all use the same dispatch.

Per the §9.6 design, dev9p is the foundation that unifies the kernel's filesystem ABI. The two-syscall mount decomposition (attach_9p + mount) is built on top of this Dev in subsequent chunks (P5-attach-syscall, P5-attach-mount); dev9p itself is the byte-pipe-to-Dev adapter.

## Public API

```c
// Device character; distinct from all kernel-Dev characters.
#define DEV9P_DC  '9'

// The Dev struct (vtable). Registered in the bestiary by dev9p_init.
extern struct Dev dev9p;

// Per-Spoor private state.
struct dev9p_priv {
    u32                magic;       // DEV9P_PRIV_MAGIC; clobbered on free
    struct p9_client  *client;      // lifecycle externally managed
    u32                fid;         // the 9P fid this Spoor represents
    bool               fid_owned;   // true → close clunks the fid
};

// Boot init: register dev9p in the bestiary. Called from kernel/main.c
// after dev_init.
void dev9p_init(void);

// Construct the root Spoor of a 9P-mounted tree. `client` must be in
// OPEN state (handshake completed; root_fid bound). Returns a Spoor
// whose fid_owned is FALSE — closing it does NOT clunk root_fid (the
// higher layer manages the root fid + client lifecycle).
struct Spoor *dev9p_attach_client(struct p9_client *client, u32 root_fid);
```

The standard Dev `attach(spec)` slot is implemented as a stub returning NULL — dev9p Spoors are constructed via `dev9p_attach_client` directly. The spec string isn't expressive enough to carry (client_ptr, root_fid).

## Vtable implementation

| Vtable slot | Routed to |
|---|---|
| `walk(c, nc, names[], nname)` | `p9_client_alloc_fid` + `p9_client_walk` — allocates a fresh fid, sends Twalk, installs (`client`, `new_fid`, `fid_owned=true`) into `nc->aux` |
| `open(c, omode)` | `p9_client_lopen` — flags translated from Plan 9 omode to Linux O_* (low 2 bits passthrough; OTRUNC→O_TRUNC); updates cached qid + sets COPEN |
| `read(c, buf, n, off)` | `p9_client_read` — count clipped to `INT32_MAX`; offset cast to u64; returns the byte count or -1 |
| `write(c, buf, n, off)` | `p9_client_write` — same clipping; returns accepted count |
| `close(c)` | `p9_client_clunk` (only if `fid_owned`); `kfree` the priv; null-out `aux` |
| `stat(c, dp, n)` | Returns -1 (Tgetattr → Plan 9 wire stat is deferred to syscall chunk) |
| `create / remove / wstat / bread / bwrite / power` | Stubbed at v1.0; deferred to syscall chunks |

## Lifecycle

dev9p does NOT own `p9_client` lifetime when constructed via `dev9p_attach_client` directly (the test path; the client is externally allocated + destroyed). For Spoors created through `SYS_ATTACH_9P` the client is wrapped in a refcounted `p9_attached` (see `49-9p-attach.md`); every dev9p_priv carries an `attached_owner` pointer + holds one ref via `p9_attached_ref`; dev9p_close drops the ref via `p9_attached_unref`. The LAST unref runs the full session teardown.

P5-stratumd-stub-bringup audit close (F2 / R15 F236): pre-fix the root Spoor's close ran `p9_attached_destroy` immediately, freeing the client while walked dev9p_priv still pointed at it — UAF on subsequent walked clunk. Post-fix `dev9p_close` is uniform: clunk fid (fid_owned branch) THEN `p9_attached_unref(attached_owner)`. The walked Spoor's `attached_owner` is inherited from the parent's `src_priv->attached_owner` at `priv_alloc` time. Adapter + transport-Spoor ownership lives INSIDE `p9_attached` now (was: `dev9p_priv.adapter_to_free`); the LAST unref releases them. The pre-fix discipline ("close walks first, then root") is no longer load-bearing — userspace can close fds in any order; the kernel teardown is correct regardless.

The root Spoor (constructed via `dev9p_attach_client`) has `fid_owned = false` — closing it does NOT clunk root_fid. For SYS_ATTACH_9P-created roots, `root_fid` is clunked by the attached_destroy_inner path at the LAST unref. For test-path roots (dev9p_attach_client with externally-owned client), `root_fid` is owned by the external caller.

Walk-derived Spoors have `fid_owned = true` — their close DOES clunk their fid (via the client, before the attached unref). The priv struct is `kmalloc`'d per Spoor; `kfree`'d in close. Magic is clobbered before free for UAF defense (mirrors `kobj_*` discipline at §39 caveat #2).

## Compile-time invariants

`_Static_assert` in `kernel/dev9p.c`:

- `DEV9P_PRIV_MAGIC == 0x44395050u` ("D9PP" little-endian).

The dev character `'9'` is registered in the bestiary at boot; collision with another Dev is a `dev_register` extinction (caught at init).

## Tests

9 tests in `kernel/test/test_dev9p.c`. A canonical responder synthesizes valid Rmsgs for every op type dev9p might issue.

| Test | Covers |
|---|---|
| `dev9p.registered` | Bestiary lookup by dc='9' returns &dev9p; vtable slots non-NULL |
| `dev9p.attach_client_root_spoor` | dev9p_attach_client returns a Spoor with dc=9 + aux populated |
| `dev9p.walk_one_component` | walk(root, nc, "etc", 1) sends Twalk + nc->aux is a fresh priv (not shared with root) |
| `dev9p.walk_clone` | walk(root, nc, NULL, 0) is the fid-clone shape; 0 qids returned |
| `dev9p.open_lopens_fid` | open routes through p9_client_lopen; qid updated from Rlopen; COPEN set |
| `dev9p.read_routes_through_client` | read returns the loopback's payload bytes |
| `dev9p.write_routes_through_client` | write returns the accepted count |
| `dev9p.close_clunks_owned_fid` | walk-derived Spoor close clunks its fid (verified via session.fid_bound) |
| `dev9p.close_does_not_clunk_root_fid` | root Spoor close does NOT clunk root_fid (higher layer owns it) |

## Error paths

Every vtable op returns the Dev-vtable failure convention (NULL or -1):
- NULL `dev9p_priv` (Spoor not dev9p-backed, magic mismatch) → NULL or -1.
- Client not OPEN → NULL.
- p9_client_alloc_fid exhausted → walk returns NULL.
- p9_client_walk partial result (server walked fewer components than requested) → walk returns NULL; the fresh fid is best-effort clunked. Phase 5+ extension may surface partial walks.
- p9_client_lopen / _read / _write returning -errno → vtable returns NULL / -1; the Dev-vtable layer's error reporting is coarse-grained at v1.0.

## Performance characteristics

- Each op = one network round-trip (Twalk / Tlopen / Tread / Twrite / Tclunk) through the loopback or future Spoor-over-Unix-socket transport. The cost is dominated by the underlying transport, not by the dev9p shim (the shim adds a few function calls + a `kmalloc` per walk).
- No allocation in the read/write hot path — only walk + close touch `kmalloc` / `kfree`.

## Status

| Component | State |
|---|---|
| Dev vtable + Spoor private state | **Landed (P5-attach-dev)** |
| walk / open / read / write / close | **Landed (P5-attach-dev)** |
| stat / create / remove / wstat | Deferred to P5-attach-syscall + P5-attach-mount |
| bread / bwrite (block I/O) | Not implemented at v1.0 (byte-stream path is sufficient) |
| Partial-walk handling | Deferred — walk returns NULL on partial-walk result at v1.0 |
| attach_9p syscall (user-visible entry) | Next chunk: **P5-attach-syscall** |
| mount syscall + Territory mount table | After: **P5-attach-mount** |

## Known caveats / footguns

1. **Client lifecycle is refcounted for SYS_ATTACH_9P-backed Spoors.** Pre-fix (P5-stratumd-stub-bringup audit close F2 / R15 F236), closing the root Spoor ran `p9_attached_destroy` immediately, requiring the higher layer to close all walks BEFORE the root. Post-fix every dev9p_priv (root + walks) carries an `attached_owner` ref; the LAST unref runs the destroy. The kernel handles arbitrary close orders correctly; the prior "close-walks-then-root" discipline is no longer required. Test-path Spoors (dev9p_attach_client with externally-owned client) still have NULL attached_owner and rely on external lifecycle management.

2. **`fid_owned` is the load-bearing flag.** The root Spoor has it false; walk-derived Spoors have it true. Getting this wrong causes either double-clunk of the root_fid (if root is mis-flagged true) or leaked fids (walk-derived is mis-flagged false).

3. **Partial walks return NULL at v1.0.** The 9P wire allows a server to walk fewer components than requested (Rwalk with `nwqid < nwname`); Plan 9's semantics is "the Spoor at the deepest successful component is returned via Walkqid + the missing components are an error." dev9p currently treats partial walks as a hard failure. This is too strict for production but sufficient for tests; the syscall chunks will refine.

4. **stat / wstat / create are stubs.** They return -1 (or do nothing) at v1.0. The full semantics involve mode-bit translation between Plan 9 and Linux conventions + the 9P stat-wire-format encoding; deferred to the syscall chunks where the syscall layer carries the mode/perm context.

5. **No iounit awareness.** Rlopen returns an iounit (server's recommended max single-IO count); dev9p ignores it. Callers that exceed iounit will get the server's truncated response (handled correctly by p9_client_read/write's returned count). A future optimization could chunk large I/Os internally.

6. **The Dev's `.init` slot is a no-op.** Registration happens via the standalone `dev9p_init()` called from `kernel/main.c` after `dev_init()`. The `.init` slot is filled with `dev9p_init_noop` to satisfy `dev.vtable_slot_coverage`'s "every slot non-NULL" check.

## Naming rationale

`dev9p` follows the kernel-Dev naming convention (`devcons`, `devnull`, etc.). The `dc='9'` character is a natural extension of the single-char device space (avoiding collisions with `-`, `c`, `0`, `z`, `r`, `p`, `C`, `m`).

The struct field name `fid_owned` is direct and unambiguous — there was no obvious thematic name from the marsupial vocabulary that would communicate this more clearly than the English word.

## Spec cross-reference

No new TLA+ module. dev9p is mechanical composition over the spec'd lower layers — `specs/9p_client.tla`'s I-10 / I-11 / FlowControl / OutOfOrderCorrectness all propagate through unchanged. Mount-lifecycle invariants land at **P5-attach-mount** in `specs/namespace.tla`.

## Reference

- ARCH §9 (Territory and device model) — especially §9.6 (filesystem-as-Spoor).
- ARCH §11.2 (syscall table) — for the attach_9p + mount syscalls dev9p will be reached through.
- `docs/reference/45-9p-session.md` (session state machine).
- `docs/reference/47-9p-client.md` (high-level client API — what dev9p calls into).
- `docs/reference/30-dev-spoor.md` (Dev vtable + Spoor primitives — what dev9p extends).
- Stratum's `stratum/v2/docs/OS-INTEGRATION.md` (the eventual server-side target).
