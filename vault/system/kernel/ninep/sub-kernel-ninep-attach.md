---
id: sub-kernel-ninep-attach
type: sub
title: "9P attach layer (p9_attached + srvconn_attach_dev9p_root)"
parent: moc-kernel-ninep
code: [kernel/9p_attach.c, kernel/include/thylacine/9p_attach.h, kernel/test/test_9p_closer.c]
audit: hard
guarded-by: []
validated-by: [prose, gate-smp]
locks: []
hazards: []
abis: []
design: []
created: 2026-07-31
updated: 2026-09-29
---
## Purpose

The mount-creation composition: wrap a transport in a heap `p9_client`,
drive Tversion+Tattach, and hand back a refcounted session holder
(`struct p9_attached`) whose root Spoor is dev9p-backed. Two entries: the
generic `p9_attached_create` (any transport_ops; the SYS_ATTACH_9P pipe
path, which stamps its own cape the same way, and every test), and `srvconn_attach_dev9p_root` — the production
path shared by SYS_ATTACH_9P_SRV and devsrv's open=connect (stalk-3b),
which is how every real mount (Stratum system FS, per-user homes, netd
`/net`, corvus) comes to exist.

It also owns the **closer**, the pool of kernel threads that sends the
Tclunks a dying thread cannot (FID-LIFECYCLE section 9;
`dec-2026-09-28-tclunk-closer`). It lives here because the entry's session
reference, `p9_attached_ref`, is what keeps the client alive until the
Tclunk is sent.

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
  root with `fid_owned = false`). Before the root publishes it installs the
  client's orphan sink (`attached_orphan_sink`), so no walk runs on the
  client without a place for a flushed walk's late fid. Every production
  publisher goes through it (SYS_ATTACH_9P, `srvconn_attach_dev9p_root`).
- The closer: `p9_attached_defer_clunk(a, fid)` queues a bound fid whose
  Tclunk the caller could not send (`-P9_E_AGAIN` from the client), taking a
  session reference; it never sleeps (a non-blocking `kmalloc`, a leaf
  spinlock, a wakeup), so a dying thread may call it (I-24). `-1` when the
  node cannot be allocated: the caller reports the leak with
  `p9_clunk_refused(fid, rc)`, the one producer of
  `9p: close: clunk of fid N refused rc R`, which `tools/test.sh` fails on.
  `p9_closer_start()` makes the first closer (boot, after the poll pump;
  extinction on failure). `p9_closer_stats()` reports the pool.
- `srvconn_attach_dev9p_root(cn, aname, aname_len, who, flags, out_err)`
  → the dev9p root Spoor over a SrvConn, or NULL. `who` is the attaching
  Proc (its principal names the Tattach; with the cape, its principal and
  primary gid own every file); `flags` is the `/srv` attach's word, whose one
  admissible bit is `SYS_ATTACH_9P_LOOSE`; no bit of it capes the session. A
  NULL `cn` or `who` answers `-T_E_INVAL`.

  **The helper VALIDATES that word itself (audit F5, 2026-09-24)** rather than
  trusting the syscall to have done it, so the admissible domain is the helper's
  own property: `sys_attach_9p_flags_ok(flags, srv=true)` fails closed with
  `-T_E_INVAL` before anything is built. What that catches is precisely a word
  the `/srv` handler would not have admitted -- the CAPE bit, whose meaning
  belongs to the OTHER attach handler, and any unknown bit. It does NOT catch an
  unvalidated `LOOSE`, which is legal here and so indistinguishable from a
  validated one; the header's older claim that it did was corrected with the
  guard. It never fires for the two production callers (devsrv's literal `0`,
  and a word `syscall.c` already validated). Guard:
  `9p_srvconn_transport.cape_attach` asserts the refusal BY ERRNO for both the
  cape bit and an unknown bit, with an `SC_ERR_UNSET` sentinel so a fixture that
  never reached the call cannot satisfy the negative, plus the admitted control
  one variable away. That leaves `{0, LOOSE}` as the whole admissible domain and
  both members are asserted not to cape, so "no flag word capes a /srv session"
  is now covered over the entire domain rather than sampled.

## Mechanism

**`srvconn_attach_dev9p_root`, step by step** (the production sequence —
each step's ordering is load-bearing):

0. **The cape decision** (IDENTITY-DESIGN 3.2), before anything is built:
   the attach is caped if and only if the conn carries the service's
   DMSRVCAPE mark AND is byte-mode; no bit of `flags` enters (B, 2026-09-24:
   the attacher's flag was withdrawn, so over /srv the cape is the poster's
   decision alone, whatever word a caller hands in). The mark is read off the
   CONNECTION, so every attach over a caped byte conn is caped whichever
   caller drives it -- SYS_ATTACH_9P_SRV passes its flags through, devsrv's
   9P-mode connect passes 0. The byte-mode half is the no-escalation
   argument: a byte-mode attacher holds the raw transport (it could speak
   9P to the server itself), so owning every file grants it nothing new; a
   9P-mode opener never holds the transport. The /srv post already refuses
   DMSRVCAPE without DMSRVBYTE, so the gate is the helper's own second half.
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
4. `p9_attached_create` with n_uname = `who->principal_id`, or
   `PRINCIPAL_NONE` when caped (nothing identity-bearing crosses to a
   server whose ids are foreign), and **msize = recv_cap = `srvconn_msize(cn)`** —
   the CONNECTION's ring class (CF-3 B): a DMSRVBULK service negotiates
   128 KiB, a default one 32 KiB; the proposal can never exceed what the
   rings carry (ring cap = 2× msize class).
5. On handshake success: `srvconn_set_client_deadline(cn, 0)` — **the
   steady-state has NO per-op deadline** (#841): the pipelined elected
   reader blocks until reply / EOF / death, because a per-op timeout that
   abandons one in-flight op desyncs the stream every Proc shares.
6. `loose` from `SYS_ATTACH_9P_LOOSE` — the **B1 per-attach loose mode**
   (the I-38 opt-in consumed by the Larder write-behind/cached-open legs in
   [[sub-kernel-ninep-dev9p]]) — and, when caped,
   `p9_client_set_cape(client, who->principal_id, who->primary_gid)`, and,
   when the conn carries the service's DMSRVREMOTE mark,
   `p9_client_set_remote(client)` (LR-1, HAUL-DESIGN 4.8). All three are
   stamped on the still-private client BEFORE the root Spoor exists: the
   caller's handle publication orders them against every subsequent dev9p
   op, so the plain fields need no atomics and never flip afterward. No stat
   runs in this layer, so no conversion can precede the cape. The remote
   mark is read off the conn like the cape's, but in EITHER mode: it is a
   label and grants nothing, so the cape's byte-mode argument has nothing to
   protect. No bit of `flags` can ask for it: step 0's
   `sys_attach_9p_flags_ok(flags, true)` refuses REMOTE with -EINVAL, as it
   does the cape, because over /srv the poster declares.
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

### The closer pool (FID-LIFECYCLE section 9, 2026-09-29)

Plan 9's `closeproc`, serialized per session. A session with deferred
Tclunks waits on a run-queue (`closer_queued`) until a closer takes it
(`closer_busy`); that closer sends every entry, oldest first, through
`p9_client_clunk_async` like any live thread, parking on back-pressure if it
must. So a server that never answers holds only its own session's closer.
The closer that takes work spawns a spare when no other closer is idle, and
a closer that finds no work retires when another is idle, so one idle closer
is kept. A hand-off that finds a session waiting, no closer idle and none
starting spawns the spare itself: that state means the last spawn failed,
and without the retry every session queued behind a closer that waits on a
silent server would wait with it. `g_closer_spawning` keeps it to one spawn
at a time and stays set until the new closer's first loop top (`started`),
where it can take work -- cleared at creation, a hand-off in between would
spawn a duplicate. A hand-off made while a spawn runs sees the flag and
leaves the spare to that spawn, so a failed attempt tries again while a
session waits and no closer is idle, up to `CLOSER_SPAWN_TRIES` (3)
attempts; then it clears the flag. A
kernel thread cannot free itself: a retired closer parks terminally (the
loom SQPOLL shape -- IRQs masked across `THREAD_EXITING` and the RELEASE of
`exited`, then a switch that never returns) after kicking the idle closer,
which reaps it with `thread_free` once `exited` reads true (ACQUIRE); while a
retired closer is unreaped the idle one sleeps with a 10 ms deadline, in case
the kick arrived before the retiree's switch.

- An entry's reference is dropped after its send and outside every lock: the
  last drop runs `attached_destroy_inner`, which may close Spoors and queue
  again. While entries remain they hold references, so the session outlives
  every unref but the last, and the closer lets go of the session
  (`closer_busy = false`) before that one.
- A closer is kproc's and never dies, so its own `-P9_E_AGAIN` is a spill
  buffer that could not be allocated: it retries 10 times from 1 ms,
  doubling (~1 s), then reports the fid as a live leak.
- A session that died while its entry waited refuses the Tclunk with
  `-P9_E_IO`; its fids died with it, and the entry is dropped quietly
  (`dropped`, no refusal line).
- The orphan sink runs under `c->lock` holding no reference: it allocates
  first, then takes one with `attached_tryref` (a CAS that fails once the
  count reached 0), so a failed tryref never needs an unref. A failed
  allocation leaves the fid bound, and on a live session -- not dead, still
  open, and not being torn down (`ref > 0`) -- it prints the refusal line; a
  dead session's fids died with it, as the closer's `dropped` entries do. It never spawns (a stack
  allocation and the Proc table lock under `c->lock`): with every closer busy
  and the last spawn failed, its entry waits for the next hand-off or for a
  closer to finish.
- If a spare cannot be spawned in three attempts (memory is short), the
  queued sessions wait for a closer to finish or for the next hand-off's
  spawn; nothing is lost. Test knobs: `p9_closer_fail_spawns_for_test(n)` and
  `p9_closer_fail_nodes_for_test(n)` fail the next `n` spawns or queue nodes
  (each returns the count still unconsumed), and
  `p9_closer_hold_spawn_for_test` holds the next spawn at its end -- before
  it readies its closer or takes its failure -- until released.
- `attached_destroy_inner` sends no Tclunk for the root fid: the session
  refuses to clunk the root, so the old call was dead, and the transport
  close releases the root with every other fid.

The spec step: `specs/net_poll_teardown.tla` `DyingClose` / `CloserSend`,
with weak fairness on `CloserSend` (a closer runs); `NO_CLOSER` is the red
cfg.

## Data structures

`struct p9_attached`: magic `0x50394154` "P9AT", atomic `ref`, `client`,
`recv_buf`/`recv_cap`, `root_fid`, `msize`, `handshake_ok`, and the
installed `adapter`/`transport_tx`/`transport_rx`, and the closer's queue:
`closer_head`/`closer_tail` (the session's deferred fids),
`closer_next`/`closer_queued` (its run-queue link), `closer_busy` (a closer
has it). The ref uses RELAXED add / ACQ_REL sub, and `attached_tryref` a CAS
loop that refuses at 0. The pool: `struct p9_closer` (thread, a Rendez only
it sleeps on, the `kicked` flag, `exited`, `started`), `g_closer_idle` (at
most one),
`g_closer_retired`, and `struct p9_closer_stats` (threads, idle,
idle_parked, retired, runq, pending, sent, dropped, refused, spawned,
spawn_failed, reaped, live_refusals).

## Concurrency

The attach sequence itself is serial (one caller constructs a private
client; nothing is shared until the root handle publishes). The refcount is
the only cross-thread state: dev9p_privs across threads/Procs ref/unref it,
and the poll-pump + Loom borrow-guards take EXTRA refs to keep the client
alive across blocking pumps ([[sub-kernel-ninep-dev9p-poll]]). The
`loose` and cape stamps' publication argument (step 6 above) is the one
deliberate non-atomic: publication-ordered, never flipped. The conn's cape
mark is read with ACQUIRE (`srvconn_cape`, paired with its RELEASE setter at
mint), and `byte_mode` likewise.

`g_closer_lock` guards the run-queue, every session's entry list and the
pool. It is a leaf below `c->lock` (the orphan sink takes it under
`c->lock`); nothing is sent, slept on, freed or unreffed under it, and the
wakeups under it take only rendez and scheduler locks. Every wakeup of a
closer's Rendez happens under it, because a retired closer's struct is freed
by its reaper once the closer has left the lists. A kick sets `kicked`
before the wakeup, so a closer between its unlock and its tsleep sees it at
the sleep's first cond check (I-9).

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
- **Teardown ordering**: client-destroy before adapter-free;
  unregister/close before unref. Reordering any pair is a UAF or a leaked
  server-side fid. (No root Tclunk: the session refuses the root, and the
  transport close releases it.)
- **The closer**: the entry's reference must be taken before the caller's
  own is dropped (dev9p_close unrefs after `dev9p_clunk_fid`); nothing may
  sleep, free or unref under `g_closer_lock`; a Rendez wake of a closer must
  happen under that lock; the reap must read `exited` with ACQUIRE before
  `thread_free`. A new path that sends a Tclunk for a dying thread inline,
  or drops the fid on `-P9_E_AGAIN`, re-opens the leak.
- **The deadline split**: the handshake MUST stay bounded (a hung server at
  boot must not wedge joey) and the steady state MUST stay unbounded (a
  per-op deadline desyncs the shared stream) — pressure in either direction
  has historically been wrong once each (16c F1 vs #841).
- **`kernel_attached` timing** (set before any blocking op, only after the
  adapter commits) and the dual-destroy magic contract (asserts in this
  TU).
- **The `loose` and cape stamps' pre-publication window** — a stamp after
  the root handle publishes would race dev9p's relaxed reads, and a cape
  stamped after a stat would leave the Larder holding the server's ids.
- **The cape decision's two halves.** The flag capes any attach; the conn's
  mark capes only a BYTE conn. Dropping the byte-mode half would let a
  9P-mode opener -- who never holds the transport -- own a server's files;
  deciding the cape anywhere but here would let one caller of this helper
  forget it. n_uname must be PRINCIPAL_NONE whenever the cape is.
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
  is the recorded v1.x foreign-server seam ([[seam-nuname-trust-stamp]]).
  A caped attach sends `PRINCIPAL_NONE` there instead of the principal.

## Provenance

(generated from incoming `touched` edges — shaped by P5-attach-create,
SYS_ATTACH_9P/55, 16c [[chg-2026-05-26-16c-attach-srv]] + its two audit
rounds, stalk-3b's shared open=connect path, A-3c out_err, CF-3 B msize
classes, B1 loose, #210's session registry --
[[chg-2026-08-16-ninep-attach-registry]] -- and (L) the Haul identity cape,
which gave the helper the attaching Proc and the flags word; B (2026-09-24)
then withdrew the attacher's cape flag, leaving the conn's mark the only
input.)

## Tests

`kernel/test/test_9p_closer.c` (`p9_closer.*`, 2026-09-29, each seen red by a
sabotage): `dying_close_delivers_tclunk` (a dying thread drops a walked
Spoor's last reference over an owned session, and a closer sends the
Tclunk), `stalled_session_holds_one_closer` (a server that stops answering
holds one closer while a spare sends another session's Tclunk; its death
drops the entry quietly; both spares are reaped; the test releases the
stalled server even when it fails, so no closer is left waiting), and
`flushed_walk_fid_clunked` (flush(5) end to end: the late Rwalk's fid goes
through the orphan sink to a closer), `failed_spawn_retried_by_hand_off`
(the stalled shape with the spare's spawn failed: the next hand-off spawns
it, and the other session's Tclunk goes out while the silent server holds
its closer), `hand_off_inside_failed_spawn_retried` (the hand-off lands while
that spawn is held before its failure: it spawns nothing, and the failing
spawn's retry serves it), `hand_off_inside_spawn_no_duplicate` (the hand-off
lands while a successful spawn is held before its `ready`: the flag is still
set, so no second spare), `orphan_oom_on_dead_session_quiet` (the sink's node
fails on a session a peer marked dead: -1, the fid stays bound, no refusal
line), and `clunk_killed_while_self_pumping` (a sender reading the
replies itself, killed in that read over the stall transport: the session
stays live and the Tclunk is taken back, fid bound). Each leaves the pool as
it found it -- one closer, idle and asleep (`idle_parked`), nothing queued.

`kernel/test/test_9p_attach.c` (`p9_attached.*`): lifecycle,
handshake-failure cleanup (the OOM/rollback ladder), root-walk-read
composition, and `p9_attached.walked_outlives_root_no_uaf` — the F236
regression (close the root BEFORE the walks; pre-fix UAF'd on the walked
clunk). `test_9p_srvconn_transport.c::kernel_attached_skips_teardown_on_handle_close`
covers the 16c integration half; `9p_srvconn_transport.cape_attach` covers the
cape decision through the helper (a DMSRVCAPE service capes without a flag,
the cape flag handed straight to the helper capes nothing, the uncaped control
names the principal, LOOSE alone is not the cape, a cape mark on a raw 9P-mode
conn capes nothing), and `9p_srvconn_transport.cape_attach_srv` covers it
through SYS_ATTACH_9P_SRV's inner (a DMSRVCAPE service capes with flags 0, the
cape flag is refused and sends nothing, LOOSE reaches the helper uncaped, an
unknown bit sends nothing) -- two tests, so neither half's early return can
hide the other's -- both reading the Tattach's n_uname off the ring; the live path is exercised by every boot
(all mounts route through `srvconn_attach_dev9p_root`).
`9p_srvconn_transport.remote_attach` covers the remote mark through the helper
(a DMSRVREMOTE byte service marks without a flag and is not the cape, the
plain control stays unmarked, a caped remote service carries both, LOOSE
over a remote service is loose and remote, the REMOTE flag is refused as
-EINVAL and the call did run, and a remote mark on a 9P-mode conn marks the
session while a cape mark there still capes nothing), and
`9p_srvconn_transport.remote_attach_srv` covers it through
SYS_ATTACH_9P_SRV's inner (the poster's declaration marks with flags 0, the
REMOTE flag is refused and sends nothing, even over a service already
declared remote). The LR-1 sabotage boots turned both red when the helper's
stamp was deleted, when the post stopped recording the declaration, and when
connect stopped carrying it onto the conn.
