# HOLOTYPE RW-4 -- Namespace / FS + 9P + Loom (DELTA)

**Status**: CLOSED. Tier STANDARD (+DELTA Loom). Fixes `ee30f55` (reviewer
findings 1/2) + `6cf5933` (SA-F1 ns_lock 2/2); doc/hygiene close in this
report's commit. Closed list: `memory/audit_holotype_rw4_closed_list.md`.

## Scope

The per-Proc namespace + filesystem + service spine, prosecuted as the
**resolver/driver internals** (RW-3 already cleared I-28 containment + the
two-axis rights+perm gate from the *syscall boundary*):

- **Namespace + resolver**: `territory.c`, `stalk.c` (the I-28 resolver),
  `spoor.c`, `dev.c` (the vtable contract).
- **FS dev drivers**: `devramfs.c`, `dev9p.c`, `devsrv.c`, `srvconn.c`,
  `devnone.c`, `devctl.c`.
- **9P client stack**: `9p_client.c`, `9p_session.c`, `9p_wire.c`,
  `9p_transport.c`, `9p_attach.c`, the 4 transport backends.
- **Loom (DELTA)**: `loom.c` + the `9p_client` pluggable-completion seam --
  re-examine drift since the loom-6d close + the neighbor interaction (6
  closed rounds + 3 spec modules already audited).

## Method

Four Fable `holotype-reviewer` agents split by sub-surface (never by lens) +
an Opus self-audit, then a **dirty-close round-2** on the fixes (they lift a
lock-order rule + change three wait/wake paths). Every reviewer self-reported
Fable at start AND end -- no mid-run model fallback. Three lenses per area
(Soundness + Completeness + local-SOTA).

## Findings + dispositions

| ID | Sev | Lens | Title | Disposition |
|---|---|---|---|---|
| **SA-F1** | **P1** | S | Per-Territory `mounts[]`/`root_spoor` unlocked -> multi-thread UAF (#848, promoted from P3-dormant) | **FIXED** `6cf5933` (ns_lock) |
| **R2-F1** | **P1** | S | Byte-mode `/srv` blocking-recv extincts on a 2nd concurrent reader (single-waiter Rendez) | **FIXED** `ee30f55` (busy-guard) |
| **R3-F1** | **P2** | S | 9p owned-reply dispatch failure leaks the `outstanding[]` slot + doesn't fail closed | **FIXED** `ee30f55` (mark-dead) |
| **R4-F1** | **P2** | S | Loom SQPOLL park-cond data race / falsified Loom-5 F4 single-admitter disposition | **FIXED** `ee30f55` (sqpoll guard) |
| **R1-F1** | P3 | S | `clone_walk_zero` accepts a 0-walk without asserting `nqid==0` | **FIXED** `ee30f55` |
| **R2-F2** | P3 | S/C | `#957`/A-4b regressed lseek to succeed on devsrv/devproc (`stat_native` heuristic) | **FIXED** `ee30f55` (`dev->seekable`) |
| R3-F2 | P3 | S/T | 9P fid allocator never reclaims (≈1 day to 2^32 burn, fail-safe) | REGISTERED `#23` (v1.x free-list + doc) |
| R3-F3 | H3 | C | ARCH 21.5 "block until a slot frees" not built (clean-fail `-EIO` instead) | REGISTERED `#23` (scripture call) |
| R3-F4 | H4 | C/T | Partial walk -> `-EIO` instead of Plan 9 last-bound-qid | REGISTERED (documented) |
| R4-F2 | P3 | C | SQPOLL back-pressure strands until next submit (busy-poll reap) | REGISTERED `#23` (doc + v1.x) |
| R4-F3 | P3 | C | Multi-client Loom ring pumps only the first client -> starvation | REGISTERED `#23` (v1.x round-robin) |
| R4-F4 | H2 | T | io_uring `SINGLE_ISSUER` + CQ overflow-list retires the over-admit residual + R4-F1 | REGISTERED `#23` (SURFACED, pre-rc) |
| R-A-F1 | P2 | C | `18-territory.md` + `104-stalk.md` teach the PRE-fix contract (borrow + "no lock") | **FIXED** (this commit) |
| R-A-F2 | P3 | -- | `sys_walk_open_handler` comment "src is NOT spoor_ref'd" contradicts the ref-held arms | **FIXED** (this commit) |
| R-A-F3 | P3 | -- | `joey.c:222` the only bare `root_spoor` read | **FIXED** (this commit, -> `territory_root_ref`) |
| R-B-F1 | **P2** | S | R3-F1's latch over-broad: a local TWALK fid-table-full latches the shared root-FS session dead | **FIXED** (this commit) |

### The headline: SA-F1 (the systemic theme)

The per-Territory mount table (`mounts[]`/`nmounts`), bind table, and
`root_spoor` carried **no lock** -- only `dot_lock`, which LS-4 added for the
cwd string alone. Peer Threads of a Proc share the Territory, and
`SYS_MOUNT`/`CHROOT`/`PIVOT_ROOT` have no capability gate (Plan 9
unprivileged-namespace model), so an unprivileged pthread program racing
`open(FROM_ROOT)` against `chroot`/`pivot`/`unmount` in another thread is a
`root_spoor` / mount-source **use-after-free** plus a torn `mounts[]` RMW. The
`#844` `spoor_ref(root)` added "to survive a concurrent pivot" sat *inside* the
read-then-ref window -- it narrowed the race, never closed it.

This is the **#848 race, promoted from P3-dormant to P1 by the P6
multi-thread-Proc lift** -- the same root cause as RW-2's 2C-F1 (poll waiter
outlives obj ref) and 2B-F1 (`wait_pid` single-waiter) and RW-4's own R2-F1
(SrvConn single-waiter Rendez): **the P6 multi-thread lift outran the
serialization of per-Proc SHARED state.** Recorded as a recurrent bug class in
`docs/DEBUGGING-PLAYBOOK.md` 6.15 + a CLAUDE.md self-audit hazard bullet.

Reviewer R1 prosecuted the same race and rated it "still dormant" (no *current*
in-tree multi-thread Proc both walks and mutates its namespace). **Overruled to
P1**: R2-F1 is the identical class and R2 rated it P1 without hesitation; the
kernel must be sound against *any* EL0 program, not the current in-tree set, and
the stewardship doctrine forbids "tracked dormant" for a reachable soundness
threat. The split was surfaced to the user.

**Fix**: a near-leaf `spin_lock_t ns_lock` on `struct Territory` (placed last,
size assert +16 -> +24). `mount_lookup` changed contract borrow -> **owned**
(ref under the lock; caller clunks); a new `territory_root_ref()` reads
`root_spoor` + refs it atomically; the 6 syscall FROM_ROOT readers use it.
`ns_lock` is never held across `stalk` (blocks on 9P) or a `spoor_clunk` (the
Dev close hook may sleep) -- the displaced source is captured under the lock and
clunked outside it (the `dot_lock` discipline).

## Verified SOUND (survived prosecution)

- **Resolver (I-28)**: `..` clamps at `start` via trail-pop (depth-0 no-op);
  per-component PERM_X on the CROSSED (mounted) root; base + quarry crossed
  exactly once; full trail/quarry refcount balance across every failure branch;
  STALK_MAX_DEPTH bound; amode fail-closed. The cwd-join is convenience, not
  authority (stalk re-clamps).
- **`ns_lock` fix (round-2 R-A, path-by-path)**: lock-order leaf; `dot_lock` /
  `ns_lock` never nest; no sleep under the lock; deferred-clunk complete (no
  `spoor_clunk` in any locked region); `mount_lookup` migration complete (2
  callers, both clunk); all 6 FROM_ROOT sites balance on every exit; `rc` set on
  every `goto`; pre-fix semantics (return codes, MREPL, idempotency, I-3,
  9.6.6 refcount algebra) byte-preserved; 2-thread interleavings replayed;
  `territory_root_ref(NULL)->NULL`; `p->territory` set-once (RFNAMEG unsupported
  at v1.0, so the unlocked `p->territory` deref is safe -- the multi-thread
  reachability is via shared Threads, which the fix covers).
- **I-3**: `would_create_mount_cycle` + `would_create_cycle` enforce the DAG at
  mount/bind time. mount-key `(dc, devno, qid.path)` disambiguates concurrent
  dev9p sessions. `SYS_BIND` has no EL0 surface.
- **spoor.c**: ref/unref/clunk atomic ACQ_REL; magic-clobber on free; close hook
  on last drop with storage intact.
- **Dev drivers**: dev9p create/dir fid lifecycle; `dev9p_walk` `attached_owner`
  refcount (R15-F236 UAF closed); rename cross-Dev+same-session reject; readdir
  `#955` (u64 offset, no sign-clamp); devsrv registry-ref + `open_connect`
  refcount dance (lock-free deadline-bounded handshake); srvconn ring bounds +
  teardown lock-order + death-interrupt arms + byte_mode publication.
- **9P stack**: I-9 per-rpc rendez register-then-observe; elected-reader `#841`
  F6/F7 + `#845` Tflush (re-prosecuted from a 2-in-flight + mid-RPC-death
  interleaving -- closed-list claims hold); `done_reply_buf` deferred-free; wire
  malformed-frame rejection (strict `off!=len`, caller-cap bounds, ecode
  `[1,4095]`); all-or-nothing send; death-interruptible steady recv; p9_attached
  lifecycle + dual-destroy asserts; Loom seam lock-order `c->lock -> l->lock`
  (no ABBA, cross-confirmed by R3 + R4 + Opus).
- **Loom DELTA (still holds since 6d)**: borrow-guard; `#898` quiesce-before-free;
  the 3 I-30 pins balance + goto-fail epilogue; ring TOCTOU both queues; count
  /payload clamps; registered-buffer slice + W^X; multishot
  `async_inflight<->rearm_pending`; chain memory-safety; SQPOLL EXITING handshake
  + join; CQ wait-list register-then-observe; KObj_Loom lifecycle.

## Round-2 dirty-close (the fixes)

- **R-A (ns_lock fix), Fable, CLEAN**: 0 P0 / 0 P1 / 1 P2 (doc staleness) / 2 P3
  (a stale comment + the one bare `root_spoor` read) -- all doc/hygiene, all
  fixed in this commit. The SA-F1 fix is CORRECT (the verified-SOUND list above
  is the reviewer's path-by-path confirmation). The recursion terminates.
- **R-B (the three wait/wake fixes), Fable**: 0 P0 / 0 P1 / **1 P2** / 1 P3.
  R2-F1 (busy-guard) and R4-F1 (SQPOLL guard) verified SOUND -- every
  interleaving replayed (leaked-`reading` wedge traced + withdrawn; the elected
  reader never self-refuses; SQPOLL lost-wake-free; no give-up-abandons-stream;
  no CQ-full deadlock). **R-B-F1 [P2] -- R3-F1 was over-broad** (the dirty-close
  working as designed): its "drc<0 == protocol violation" premise is false for
  the TWALK `fid_bind`-full leg (a *local* 256-fid table exhaustion, not a server
  fault), so the `mark_dead` would take the whole shared root-FS session down on
  the 257th concurrent fid -- strictly worse than the round-1 leak it closed.
  **FIXED** (this commit): a `send_walk` fid-capacity pre-check (fail-closed at
  send) + the dispatch-time `fid_bind` failure now surfaces as a synthetic per-op
  error (`is_error` + EIO, reusing the audited Rlerror path) instead of `-1`, so
  the op completes with `-EIO` and the tag is cleared *without* latching the
  session. Regression: `9p_session.walk_fid_full_no_latch` (pre-fix: dispatch
  returns `-1` -> session latched dead; post-fix: `0` + `is_error`, alive). The
  fix is a non-invasive classification refinement (no new lock, no wait/wake
  protocol change), so no round-3 is owed -- verified by the test + the re-run
  SMP gate. Residual (pre-existing, out of scope): a local `fid_bind` failure
  leaks the server-side fid (the client treats the walk as failed and won't
  clunk it) -- bounded by the client's own 256-fid cap + the trusted server;
  v1.x closes it with a Tclunk-on-local-bind-failure.

## Posture

- Default suite **814/814 PASS** (+3 deterministic regressions: the 2 SA-F1
  `territory_mount.{lookup_ref_survives_unmount, root_ref_survives_pivot}` + the
  round-2 `9p_session.walk_fid_full_no_latch`).
- **SMP gate PASS -- 0 corruption** across default-smp4 / default-smp8 /
  ubsan-smp4 / ubsan-smp8 (the standing witness for the ns_lock + 9p-dispatch
  interleavings; the timing entries are the documented benign host flakes).
  Re-run after the round-2 R-B-F1 9p fix.
- Boot OK incl. the full pivot + login + per-user-stratumd path; 0 EXTINCTION.

## Registered (carried to RW-11/12/13)

R3-F2/F3/F4, R4-F2/F3/F4 (tasks `#23`); the owed reference-doc currency for
71-srvconn / 47-9p-client / 107-loom (task `#24`). R4-F4 (`SINGLE_ISSUER`) is the
near-term (H2) SOTA item surfaced to the user.
