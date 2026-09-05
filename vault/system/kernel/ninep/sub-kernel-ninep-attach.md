---
id: sub-kernel-ninep-attach
type: sub
title: "9P attach layer (p9_attached + srvconn_attach_dev9p_root)"
parent: moc-kernel-ninep
code: [kernel/9p_attach.c, kernel/include/thylacine/9p_attach.h]
audit: hard
guarded-by: []
validated-by: [prose, gate-smp]
locks: []
hazards: []
abis: []
design: []
created: 2026-07-31
updated: 2026-08-16
---
## Purpose

The mount-creation composition: wrap a transport in a heap `p9_client`,
drive Tversion+Tattach, and hand back a refcounted session holder
(`struct p9_attached`) whose root Spoor is dev9p-backed. Two entries: the
generic `p9_attached_create` (any transport_ops; the SYS_ATTACH_9P pipe
path and every test), and `srvconn_attach_dev9p_root` — the production
path shared by SYS_ATTACH_9P_SRV and devsrv's open=connect (stalk-3b),
which is how every real mount (Stratum system FS, per-user homes, netd
`/net`, corvus) comes to exist.

## Contract

- `p9_attached_create(transport_ops, recv_cap, root_fid, msize, uname,
  aname, n_uname, out_err)` → heap `p9_attached` or NULL. **`out_err`
  carries a negative POSIX errno on every NULL path** (A-3c/M6) — most
  importantly the Tattach Rlerror ecode (`-T_E_ACCES` on a per-user-stratumd
  dataset-scope refusal) rather than a collapsed `-1`. Allocation failures
  clean up all intermediate state (no partial leaks; the OOM ladder frees in
  reverse order).
- `p9_attached_ref/unref` — the F236 refcount: construction ref = 1; every
  dev9p_priv derived from the session (root AND walks) holds one; the LAST
  unref runs `attached_destroy_inner`. `p9_attached_destroy` is a legacy
  alias for unref. An unref past zero is swallowed (magic-guarded silent
  fail — the v1.0 disposition, noted in code).
- `p9_attached_install_transport(a, adapter, tx, rx)` — first-call-wins
  transfer of adapter + transport-Spoor ownership INTO the attached, so the
  last unref releases them in the right order.
- `p9_attached_root_spoor` → `dev9p_attach_client(client, root_fid)` (a
  root with `fid_owned = false`).
- `srvconn_attach_dev9p_root(cn, aname, aname_len, n_uname, loose,
  out_err)` → the dev9p root Spoor over a byte-mode SrvConn, or NULL.

## Mechanism

**`srvconn_attach_dev9p_root`, step by step** (the production sequence —
each step's ordering is load-bearing):

1. kmalloc + `p9_srvconn_transport_init` (takes ONE srvconn_ref). Pre-init
   failures leave `cn` untouched (caller decides teardown); post-init
   failures go through the adapter's close, which tears `cn` down.
2. `srvconn_set_kernel_attached(cn)` **as early as the adapter commits**
   (16c R1-F4): from here a userspace close of the conn-endpoint handle
   skips `srvconn_teardown` — the rings are load-bearing for this kernel
   client.
3. `srvconn_set_client_deadline(cn, now + SRVCONN_HANDSHAKE_DEADLINE_NS)`
   (16c R1-F1): the serial handshake is wall-clock-bounded — a hung server
   times out instead of wedging the caller; a handshake timeout tears down
   an UNSHARED client, so no desync is possible.
4. `p9_attached_create` with **msize = recv_cap = `srvconn_msize(cn)`** —
   the CONNECTION's ring class (CF-3 B): a DMSRVBULK service negotiates
   128 KiB, a default one 32 KiB; the proposal can never exceed what the
   rings carry (ring cap = 2× msize class).
5. On handshake success: `srvconn_set_client_deadline(cn, 0)` — **the
   steady-state has NO per-op deadline** (#841): the pipelined elected
   reader blocks until reply / EOF / death, because a per-op timeout that
   abandons one in-flight op desyncs the stream every Proc shares.
6. `att->client->loose = loose` — the **B1 per-attach loose mode** (the
   I-38 opt-in consumed by the Larder write-behind/cached-open legs in
   [[sub-kernel-ninep-dev9p]]). Stamped on the still-private client BEFORE
   the root Spoor exists: the caller's handle publication orders it against
   every subsequent dev9p op, so the plain bool needs no atomics and never
   flips afterward.
7. `p9_attached_install_transport(att, adapter-as-spoor-cast, NULL, NULL)`
   — tx/rx NULL because the SrvConn's lifetime is the adapter's own
   srvconn_ref, not a Spoor pair.
8. Mint the root, stamp `root_priv->attached_owner = att`, take the root's
   ref, drop the construction ref. From here the session's lifetime IS the
   set of dev9p_privs holding it.

**`attached_destroy_inner`** (the last-unref teardown, in order): clunk
`root_fid` (client still alive — the wire round trip needs it) →
`p9_client_close` (fires the transport's close vtable — for srvconn:
teardown + unref; for spoor: clunk-if-owned) → `p9_client_destroy` →
clobber the attached magic → free recv_buf + client → release the installed
transport: clunk tx/rx (rx≠tx guarded), then the **dual destroy**:
`p9_spoor_transport_destroy(adp)` AND
`p9_srvconn_transport_destroy((cast)adp)` — each magic-guarded so exactly
one matches and the other no-ops. The discipline is pinned by two
`_Static_assert`s (16c R2-F5R2): the two magics are DISTINCT, and `magic`
sits at offset 0 in BOTH adapter types, making the wrong-typed read
layout-safe. Only after both destroys does the adapter kfree (the client's
ops vtable held it as `ctx` by value — destroy must run while it is alive).

### The session registry, and why the walk needs no refcount

Every attached session links itself into one global list at construction and
unlinks at the top of its last-unref teardown. That list is what makes live 9P
sessions visible for diagnosis — an instrument built during a reply-loss
investigation, when the question "which sessions exist and what are their ring
counters" had no answer at all.

**The lifetime argument is the elegant part, and it is a pairing rather than a
mechanism.** The walker holds the registry lock across its *entire* walk, and the
unlink runs *first* in the teardown — before the root clunk, before the client
destroy, before anything is freed. Those two facts together mean membership in
the list is itself the liveness proof: a session the walker can reach has not
begun tearing down, and a session that has begun tearing down is already
unreachable. No reference is taken and none is needed.

Reverse either half and it breaks. A walker that dropped the lock mid-walk could
resume into a freed entry; an unlink placed after any teardown step would leave a
window where the walker reaches a half-destroyed session and snapshots it.

Lock order is registry then client — the walker takes the registry lock and then
snapshots each session under its own client lock. Linking and unlinking take
*only* the registry lock, so there is no path that could invert them.

**The registry sees production sessions only.** Test loopback clients never
register, because they do not go through this layer. That is correct for the
instrument's purpose and worth stating as a coverage property rather than
leaving implicit: a bug visible only in the registry's output is a bug no test
can currently observe.

### The label is sanitized because an empty string is a sentinel elsewhere

Session labels default to the attach name, truncated to a small fixed field,
with every non-printable byte replaced — and **an empty result replaced by a
placeholder**.

That last clause is not tidiness. The consumer that renders this registry treats
*bytes written* as its overflow signal, so a zero-length field is
indistinguishable from a full buffer and aborts the entire listing. The
consumer-side guard exists too; this is the producer half of the same defence.

**The interesting part is the history.** That collision was found and fixed once,
as a literal empty string in a conditional. It came back here **as data** — a
label that happens to be empty at runtime rather than a constant written into the
source. Fixing the instance did not fix the class, and the second instance could
not have been found by looking for the first one's shape.

The connecting service path relabels with the peer's process id, because the
attach name is usually empty there — which is precisely the input that would have
produced the empty label.

**The client-struct economics**: `struct p9_client` is ~36 KiB (it inlines
the 32 KiB default-tier `out_buf`; a bulk session kmallocs an msize-sized
`out_buf` besides — CF-3 B), so kmalloc routes it through the alloc_pages
large-object bypass. recv_buf is msize-sized.

## Data structures

`struct p9_attached`: magic `0x50394154` "P9AT", atomic `ref`, `client`,
`recv_buf`/`recv_cap`, `root_fid`, `msize`, `handshake_ok`, and the
installed `adapter`/`transport_tx`/`transport_rx`. The ref uses RELAXED
add / ACQ_REL sub — the v1.0 syscall surface mutates it single-threaded,
but the atomics keep future SMP paths honest.

## Concurrency

The attach sequence itself is serial (one caller constructs a private
client; nothing is shared until the root handle publishes). The refcount is
the only cross-thread state: dev9p_privs across threads/Procs ref/unref it,
and the poll-pump + Loom borrow-guards take EXTRA refs to keep the client
alive across blocking pumps ([[sub-kernel-ninep-dev9p-poll]]). The
`loose` stamp's publication argument (step 6 above) is the one deliberate
non-atomic: publication-ordered, never flipped.

## Invariants enforced

None of §28 directly — the layer is composition. It carries three
disciplines other surfaces' invariants rest on: the **F236 refcount** (walk
Spoors outliving the root must never dangle the client — the R15-F236 UAF
class), the **handshake-vs-steady-state deadline split** (the #841 no-desync
premise), and **`kernel_attached`-before-publication** (16c R1-F4: no
window where a peer thread's handle-close can tear the rings out from under
the handshake).

## Error paths

Every `p9_attached_create` NULL carries `out_err`: `-T_E_INVAL` (bad
recv_cap/msize), `-T_E_NOMEM` (any of the three allocations), the client
init rc, or the handshake rc (server Rlerror ecode / `-P9_E_IO` on
transport death). `srvconn_attach_dev9p_root` failure paths: pre-adapter →
`cn` untouched; post-adapter → close-through-the-adapter (teardown + unref);
post-install → plain `p9_attached_unref` (the destroy chain owns cleanup).
The install-fail defensive path additionally destroys+frees the adapter
explicitly (16c R2-F2R2 — `a->adapter` was never set, so the destroy
chain's adapter block would skip and leak it).

## Performance

2 RTT per attach (Tversion + Tattach), three heap allocations. Attaches
are rare (mount-time); nothing here is hot.

## Prosecution

- **The failure-path ledger**: every exit must leave (adapter ref ×
  srvconn ref × attached ref × spoor refs) balanced — the 16c rounds found
  three distinct imbalances (R1-F5 missing destroys, R2-F2R2 adapter leak,
  and the pre-F236 walk-dangling root). Trace each `return NULL` against
  the ladder.
- **Teardown ordering**: root-clunk before client-destroy; client-destroy
  before adapter-free; unregister/close before unref. Reordering any pair
  is a UAF or a leaked server-side fid.
- **The deadline split**: the handshake MUST stay bounded (a hung server at
  boot must not wedge joey) and the steady state MUST stay unbounded (a
  per-op deadline desyncs the shared stream) — pressure in either direction
  has historically been wrong once each (16c F1 vs #841).
- **`kernel_attached` timing** (set before any blocking op, only after the
  adapter commits) and the dual-destroy magic contract (asserts in this
  TU).
- **The `loose` stamp's pre-publication window** — a stamp after the root
  handle publishes would race dev9p's relaxed reads.
- **The registry unlink must stay first in teardown, and the walk must hold its
  lock throughout.** Neither half is safe alone: together they make list
  membership the liveness proof, which is why the walk takes no reference. Moving
  the unlink after any teardown step, or releasing the lock mid-walk, reopens a
  use-after-free that nothing else in this layer would catch.
- **A label may never be empty.** The consumer reads bytes-written as its
  overflow signal, so an empty label aborts the whole listing. Both ends guard
  it; the producer's guard is the one that covers labels that are empty *by
  data* rather than by literal.

## Seams

- [[seam-848-pivot-walk-race]] — SYS_PIVOT_ROOT (which landed in the 16c
  chunk alongside this layer) vs a concurrent multi-thread walk from
  `root_spoor`: the 16c R1-F6 deferral, inherited from `territory_chroot`'s
  pattern, tracked as #848 (dormant; re-homes to the territory dossier at
  its sweep).
- The 16c R1-F11 (test refcount asserts) and R1-F13 (`territory_pivot_root`
  body duplication) hygiene notes remain as-recorded in the Record plane —
  code-hygiene wishes, not system debt.

## Caveats

- `p9_attached_create` captures `transport_ops` by value but the `ctx`
  pointer must outlive the attached.
- A `p9_attached_unref` past zero is silently swallowed (magic still valid
  → subsequent ops fast-fail on the freed-state check); the in-code note
  accepts the silent-failure shape at v1.0.
- `n_uname` is forwarded but v1.0-inert on the trusted-local path — the
  live identity channel is SO_PEERCRED (A-3); the n_uname trust-stamp gate
  is the recorded v1.x foreign-server seam (identity surface, swept there).

## Provenance

(generated from incoming `touched` edges — shaped by P5-attach-create,
SYS_ATTACH_9P/55, 16c [[chg-2026-05-26-16c-attach-srv]] + its two audit
rounds, stalk-3b's shared open=connect path, A-3c out_err, CF-3 B msize
classes, B1 loose, and #210's session registry --
[[chg-2026-08-16-ninep-attach-registry]].)

## Tests

`kernel/test/test_9p_attach.c` (`p9_attached.*`): lifecycle,
handshake-failure cleanup (the OOM/rollback ladder), root-walk-read
composition, and `p9_attached.walked_outlives_root_no_uaf` — the F236
regression (close the root BEFORE the walks; pre-fix UAF'd on the walked
clunk). `test_9p_srvconn_transport.c::kernel_attached_skips_teardown_on_handle_close`
covers the 16c integration half; the live path is exercised by every boot
(all mounts route through `srvconn_attach_dev9p_root`).
