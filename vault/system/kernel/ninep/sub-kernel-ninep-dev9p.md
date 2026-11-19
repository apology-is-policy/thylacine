---
id: sub-kernel-ninep-dev9p
type: sub
title: "dev9p — the 9P Dev (walk/IO/mutation + Larder integration + write-behind + cached-open + weft arms)"
parent: moc-kernel-ninep
code:
  - kernel/dev9p.c
  - kernel/include/thylacine/dev9p.h
  - kernel/test/test_dev9p.c
audit: hard
guarded-by: [inv-i38]
validated-by: [spec-fs-cache, gate-smp]
locks: [lock-dev9p-wb-priv, lock-dev9p-wb-reg]
hazards: [haz-shared-stream-desync]
abis: []
design: [docs/LARDER-DESIGN.md, docs/FID-LIFECYCLE-DESIGN.md, docs/POUNCE-DESIGN.md]
created: 2026-07-31
updated: 2026-11-19
---
## The fid a cached open keeps, and locks on it (B-3h3, 2026-11-02)

FID-LIFECYCLE 3.3 and ARCH 9.6.11. A cached open still serves its snapshot with
no wire. What it gained is a way to reach the server, `lock_fid(p)`: a strict
open's kept fid, or the fid a loose open bound on its first need (`P9_NOFID`
until then). Any other open answers its own fid.
- **Strict** (operator vote 20, retain the walk fid unopened). The revalidating
  Twalkgetattr in `dev9p_open_cached` binds a fresh fid (mask BASIC|GEN), which
  `priv_alloc` keeps as `p->fid`, `fid_owned` and unopened. The walk teaches
  `lock_gen` and `lock_ident`, so `lock_ident` costs no wire. A mint that fails
  after the bind clunks the fid (`co_abandon`, `co_mint_fail`).
- **Loose** (operator vote B1, the SYSTEM mount). The mint binds nothing, so its
  open stays wire-free. It keeps `co_start` (a reference on the run's base),
  `co_names` (packed, each a u16 LE length and its bytes; `co_nname`,
  `co_names_len`) and the generation `larder_attr_gen` knows. A file with no
  known generation is not minted loose: the normal path opens it. The budget
  charge is the snapshot's size plus `co_names_len`.
- **`co_bind`** is a loose open's first need. It walks the kept names from the
  base (Twalkgetattr, BASIC|GEN) into a fresh fid and accepts only a full walk
  to the same `qid.path` with a valid generation equal to `lock_gen`. An accepted
  walk installs the leaf's attributes in the Larder (guarded by a pre-walk
  `seq0`), so the stat a `wstat` judges next is the walk's even if the Larder held
  a view another session has since changed. A name that
  no longer reaches the file (a partial walk, ENOENT, ENOTDIR, ESTALE) or reaches
  another one answers `-T_E_STALE`, and any fid goes back. A wire that failed says
  nothing about the file and answers its own errno through `dev9p_wire_errno`
  (`-T_E_INTR` for an interrupted walk; `-T_E_NOMEM` with nothing sent), so a table
  peer's failure refuses a take rather than letting that peer keep its snapshot
  under the lock. A run from an opened base is not minted loose: a server refuses
  a walk from an opened fid, so that open could never bind (strict learns it at its
  wire query). The fid is CAS-installed in
  `co_kfid`; a loser clunks its own (`dev9p.cached_open_install_race`).
- **`co_read_live`** ends the snapshot: a zero-name clone of the kept fid and a
  Tlopen O_RDONLY, CAS-installed in `co_rfid`, the loser clunking its own.
  `DEV_LOCK_SYNC_ACQUIRE` and `dev9p_loom_register` run it. A failure installs
  nothing and the snapshot still serves. `dev9p_read` and `dev9p_client_fid`
  address `co_rfid` once it is installed; before that `dev9p_client_fid` offers
  a ring `P9_NOFID`, which fixes OPEN-BUGS 2026-10-22 16:34Z's Loom path.
- **Locks.** `dev9p_lock` binds first, except that an unlock with nothing bound
  holds nothing and answers 0 with no wire. A failure to bind answers ESTALE or
  EINTR as they are and otherwise ENOLCK (`co_bind_lock_errno`).
  `dev9p_lock_owner` clones `lock_fid(p)`. `dev9p_lock_sync(ACQUIRE)` answers
  what a lock call can: ESTALE, EINTR, else ENOLCK.
- **Metadata.** `dev9p_wstat_bind` (the Dev's `wstat_bind`) binds before
  `wstat_apply` reads the stat its policy judges, because the open-time stat may
  name a former owner. `dev9p_wstat_native` binds too and sends Tsetattr on the
  kept fid.
  `dev9p_stat_native` serves the open-time stat until a fid is bound, then asks
  the server through it. `co_patch_stat` reports the snapshot's size and version
  while reads still serve the snapshot, so a reader sized by fstat reads what is
  there; the rest is the file's own, so a wstat through the open shows in it.
- **Close** clunks `co_rfid` and `co_kfid` asynchronously, frees the names, lets
  go of `co_start` and gives back the budget. A strict open's `p->fid` clunks as
  any walked fid does.

The residual, on the loose mount only: a read-only small cached file renamed,
unlinked or replaced since its open answers ESTALE to a lock or wstat, and never
reaches the wrong file; its reads keep serving the snapshot. Binding at every
loose open would close it at about 1604 wire walks per W1 run, the walks vote B1
removed ([[seam-co-fidless-wstat]], closed).

## Byte-range locks and lock-to-unlock consistency (B-3h2, 2026-10-29)

dev9p serves the five lock slots of [[sub-kernel-dev]] for
[[sub-kernel-filelock]] (ARCH 9.6.11):
- `dev9p_lock(c, type, start, len, test)` acts as the open file `c`. It sends
  Tlock on `c`'s fid, and Tgetlock for a test. BLOCKED on a take is
  `-T_E_AGAIN` (the wait loop is generic). ERROR, GRACE or an undefined status
  is `-T_E_NOLCK`. A Tgetlock type 9P2000.L does not define is `-T_E_IO`. The
  test answer's `pid` is -1.
- A flush with the append anchor gone drops the file's pages rather than installing
  them, and the job of a last close that left a run or a flush standing drops what that
  flush installed: that write is unordered with the file's later writers. The job goes by
  what stood at the close (`j->staged`), since a flush can retire before the closer looks,
  as the first of a lock's two jobs always does. It also gives back the buffer, and
  reports a failure itself, its own or the latch that flush left, since nobody reads the
  latch from there. A thread no death
  reaches sleeps out a peer's flush (`wb_flush_wait`, doubling to 64 ms) rather than
  yield for as long as a server hangs. A dying last close of an attach root queues the
  job with no holder, since no clunk follows it. Legs:
  `p9_closer.dying_last_close_with_holder`, `.job_loses_a_run_loudly`,
  `.job_waits_out_a_peer_flush`, `.job_drops_a_flush_that_retired_first`,
  `.job_reports_a_first_jobs_loss`, `.dying_last_close_of_a_root`.
- A parked directory fid carries the Larder's epoch: a forget says an op with no owner
  may have removed any directory, so `dirfid_take` clunks one from before it, and
  `dirfid_put` lets a fresh fid replace it. Leg: `dev9p.dirfid_forget_outdates_parked`.
- A take names the taking Proc's principal, in decimal, in Tlock's `client_id`
  (`lock_client_id`; operator vote 38), so a limit on the server can count what one
  identity holds. The kernel composes it, so a Proc cannot spend another identity's
  share. Every other request on the fid, a Tgetlock and an unlock on any thread, names
  what the latest take named (`lock_principal`), so a server keying owners by `proc_id`
  and `client_id` sees one owner. A caped session names nobody (IDENTITY-DESIGN 3.2).
  Legs: `dev9p.lock_one_name_on_a_fid`, `dev9p.lock_caped_names_nothing`.
- A grant drops the qid's Larder attributes and pages. The invalidates log
  the qid, so a fill in flight from before the grant is dropped rather than
  installed after it.
- Every unlock goes through `p9_client_unlock` ([[sub-kernel-ninep-client]]), which
  sends again one that never left for want of memory. `lock_unlock_resends` sets the
  bound: none on a thread a kill reaches (`dev9p_lock`'s unlock: a lock call's own, a
  close's through the Proc owner, and the one a lock call owes when its descriptor
  was closed during the call; and `lock_release_all` from the last close), and 0 in an
  exit's close (`exit_close_active`), whose Proc's memory is freed at reap, after the
  close, so a wait there for memory could wait on itself. The closer's job passes
  `P9_RESEND_CLOSER`, about a second. When it gives up on a fid the session still
  holds, it sets `lock_granted` again and reports `9p: close: unlock of fid N failed
  rc R` (`p9_close_unlock_lost`, counted); the open keeps the locks until its last
  reference clunks the fid. A refusal is the server's answer there as at the close
  (Stratum's ESTALE for a file unlinked under the lock holds nothing). `dev9p_lock_ident` learns the
  generation with `p9_client_getattr_patient`, so a close's whole-file unlock through
  the Proc owner is not lost one round trip earlier. Before 2026-11-06 a reply buffer
  that could not be allocated answered `-T_E_IO` with the range still held (the aux
  join's round 3 F2); the bounds are its round 1 (2026-11-09).
- `dev9p_fd_close`, the last descriptor's close (`Dev.fd_close`), flushes the
  open's run, then exchanges `lock_granted` (sticky until then) and, if it was set,
  sends a whole-file unlock and waits for it. `dev9p_close`, the last reference,
  clunks. The clunk would release the locks too, but it never waits (ARCH 7.9.1
  part A) and Stratum's pool may serve the next request first. A close that may not
  wait (`close_may_wait`) queues the flush and the unlock to a closer when another
  reference holds the Spoor (`dev9p_lock_flush_queue` with `release_locks`), and
  otherwise leaves them to the last reference, which follows at once and hands the
  run to a closer with its clunk. An unlock the server never applied (`-P9_E_UNSENT`:
  never sent; `-P9_E_INTR` with the call's mark, `wb_note_cancelled`: flushed) on a
  fid the session still holds stays owed the same way, as does one a death cut short;
  a server's refusal is an answer, its Rlerror(EAGAIN) and Rlerror(EINTR) included. A job that
  cannot be allocated for an owed unlock is reported with the unlock line.
  `dev9p_lock_held` tells the handle layer to land every open's run first (ARCH
  9.6.11).
- Every set-lock first lands `c`'s write-behind run (`dev9p_lock_sync`): an
  unlock gives its range up, and a conversion to a read lock admits readers. A
  test lands nothing. A flush a caught note cancelled answers `-T_E_INTR` and
  sends nothing: the run stays staged, and under `SA_RESTART` the call runs again
  (`dev9p.lock_cancelled_flush_says_eintr`, with a server's own EINTR and a mark
  with nothing staged as its controls; an fsync after each leg resends the
  cancelled run with nothing latched). A flush that fails otherwise still sends
  the request, and the error latches for the next write or fsync. The same
  `-T_E_INTR` from `dev9p_lock_sync` stops a lock call's flush of the file's other
  opens ([[sub-kernel-filelock]]); `P9_E_INTR == T_E_INTR` is asserted where the
  value crosses.
- `dev9p_lock_owner(c)` mints a second owner on `c`'s file: a zero-name
  Twalk clone of `c`'s fid into a fresh fid (Stratum admits the clone of an
  open fid). A clone that fails answers ESTALE or EINTR as they are and otherwise
  ENOLCK. The clone is unopened, never published, and flagged
  `fid_locks`, so `dev9p_close` never parks it in the directory-fid cache.
  It inherits `c`'s learned generation.
- `dev9p_lock_ident(c)` learns once, with one Tgetattr for
  `P9_GETATTR_GEN`, the inode generation of the file `c`'s fid names. It
  stores `lock_gen` and `lock_ident` (GEN, or NONE when the server reports
  no generation). A dead file's fid answers ESTALE and learns nothing.
  `dev9p_lock_same_file(a, b)` requires the same client and qid.path, and
  the same generation when both have learned one. Stratum does not keep a
  file a fid names, and reuses inode numbers. On a session that stages, a
  plain file's identity is learned where its fid is made (vote M33: **A file's
  name**), so a lock call asks nothing more; elsewhere the first asks once.
- `dev9p_lock_sync(c, RELEASE)` flushes `c`'s run when the thread may wait.
  A thread that may not (a death is pending) queues `dev9p_lock_flush_job`,
  which holds `c`, on the session closer as a job alone (`P9_NOFID`, see
  [[sub-kernel-ninep-attach]]). It is queued ahead of the owner's deferred
  Tclunk, and the closer serves a session's entries in order. Without it, a
  killed holder whose open file another Proc shares would release its lock
  before its staged bytes landed.

## Create admission by session end (2026-10-19, B-3e M7)

`dev9p_create_admits(c, principal)` answers whether `principal` may create
through the 9P session `c` belongs to
([[dec-2026-10-18-create-speaks-for-a-session-end]], IDENTITY-DESIGN 9.7 M7):
true only for one of the session's two recorded ends (`client->client_end`,
`client->server_end`, [[sub-kernel-ninep-client]]) and never for
PRINCIPAL_INVALID, so a session whose ends were never stamped admits no create.
A Spoor of any other Dev is not a 9P session and answers true; a dev9p Spoor
without a priv or a client answers false, and the dc test comes first so that
one whose priv fails the magic check refuses rather than passing as not-9P. The
reason is the server's: it stamps a create with its session's credential (the
conn opener's SO_PEERCRED, or the attacher's), never the caller's, so a third
principal sharing the mount would create entries recorded as an end's. Two
callers ask it, `spoor_create_install` ([[sub-kernel-syscall-dispatch]]) and
Loom's directory-mutation gate for `MKDIR`, `MKNOD` and `SYMLINK`
([[sub-kernel-loom]]), and each refuses before the wire: a refused create sends
no Tlcreate, Tmkdir, Tmknod or Tsymlink.

The mode a create carries is taken as before, `perm & 0777` into Tlcreate or
Tmkdir, but since M8 ([[dec-2026-10-19-create-mode-bound-and-umask]]) those
nine bits arrive already reduced by the creator's umask and bounded by the
parent's mode.

The test fake gained three knobs: `g_twalk_enoent_n` answers the next N named
Twalks Rlerror(ENOENT), so a create's existence probe can find a name absent;
`g_ga_mode` sets the mode both Rgetattr and Rwalkgetattr report;
`g_create_mode` captures the mode the last Tlcreate or Tmkdir carried.
Witnesses: `dev9p.create_admits_session_ends`,
`dev9p.create_refuses_invalid_principal`, `dev9p.create_refusal_keeps_eexist`,
`dev9p.create_refusal_without_rwx` (a copy of the Dev with `perm_enforced`
false still refuses), `dev9p.open_create_refuses_non_end`,
`dev9p.client_init_clears_ends` and `dev9p.create_mode_on_the_wire`. Every M7
creator holds CAP_DAC_OVERRIDE, so only M7 can refuse, and each refusal sits
beside an admitted control one variable away.

A create whose parent descriptor names a file answers ENOTDIR before any RPC,
whatever the file's mode: `spoor_create_install` tests the parent's QTDIR before
the W|X check and the clone-walk (#81). `dev9p.create_into_a_file_is_enotdir`
witnesses it with the session root's QTDIR cleared, beside the same create from
the directory, which reaches the wire.

F-5 ([[dec-2026-10-19-create-group-from-parent]]): the gid a create carries is
the parent directory's, which `spoor_create_install` reads from the parent's
stat ([[sub-kernel-syscall-dispatch]]); `dev9p_create` still replaces it with
`P9_NOGID` on a caped session. The fake's Rgetattr reports `g_ga_gid` (0x5678
by default, the primary group of the identity under test, so the witness moves
it), and `g_ga_fail_ecode` answers every Tgetattr Rlerror; `m8_wire_mode` arms
it from `g_create_ga_fail` around its create alone. Witness:
`dev9p.create_group_from_parent`: both create doors, a file and a directory, a
copy of the Dev with `perm_enforced` false (the stat taken for the group
alone), a parent whose stat leaves the gid out (GID_INVALID on the wire), and a
parent that cannot be stat'd (-EIO, and no Tlcreate reaches the server).

## Purpose

The Dev (`dc='9'`, `.name "9p"`) that makes a remote 9P server
indistinguishable from a kernel-internal filesystem: every vtable op routes
through a [[sub-kernel-ninep-client]] to the server. Since the FS-perf
arcs it is also where the guest-side cache policy lives — the Larder
serve/populate/invalidate calls, the write-behind staging, the cached-open
fast path, and the dir-fid cache are ALL dev9p policy over Larder/client
mechanism. Every mount in the system (Stratum system FS, per-user homes,
netd `/net`, corvus) is a tree of dev9p Spoors.

## Contract

The vtable (all slots non-stub today unless noted): `walk`, `walk_attrs`
(POUNCE), `open_cached` (FID-LIFECYCLE), `open`, `create`, `close`, `read`,
`write`, the three remote-readiness slots `poll_snapshot` /
`poll_snapshot_release` / `poll_arm` (→ [[sub-kernel-ninep-dev9p-poll]]; there
is no `.poll` since #98: a QTPOLL file asks its server, any other file answers
always-ready without a wire op), `fsync`, `readdir`,
`rename`, `unlink`, `stat_native`, `wstat_native`, plus `seekable = true`
(positioned I/O honors the byte offset — the #37 pread/pwrite gate) and
`perm_enforced = true` (A-3b: kernel rwx enforcement ACTIVE on dev9p; the
in-struct comment records the reconciliation that made the flip sound).
Legacy stubs: `.stat`/`.wstat` (Plan 9 wire-stat — native slots supersede),
`.remove` (wrong shape; `.unlink` replaces), `.bread`/`.bwrite`, `.attach`
(dev9p Spoors are minted via `dev9p_attach_client`, not a spec string).

A CAPED session (the identity cape, below) changes what four of these slots
report or send; no slot changes shape.

Exports beyond the vtable: `dev9p_client_fid` (the Loom I-30 submit pin
resolve), `dev9p_loom_register` (a Loom registration of a plain file first
runs the changing holder leg, pinned. A cached open then reads through a
read fid; an eligible one fails on a latched flush error, flushes its staged
run, stops staging by clearing only `wb_known` as wstat does and frees the
dead staging buffer. A registration of a plain file that succeeds drops the
file's Larder pages, an ineligible open's too: the leg's flush may have
installed the holder's run as own pages, and the ring's writes bypass the
Larder (M15 round 5, R5b-F1); witnesses
`p9_closer.loom_register_flushes_staged_run`,
`dev9p.wb_dying_loom_register_keeps_staging`,
`dev9p.wb_loom_register_keeps_the_latch`; its errno is the registration's,
`SYS_LOOM_REGISTER` returning it since B-2b), `dev9p_weft_try_write`/`_read` (the zero-copy data-drive arms),
`dev9p_priv_of`, `dev9p_create_errno` (#99), the cached-open/write-behind
budget diagnostics + test bias.


**File mappings** (operator votes 113 and 114, 2026-11-13). dev9p sets
`may_back_mapping` beside `may_back_exec`, but a dev9p session is whatever server
attached it. Stratum serves versioned content; netd serves stream and control
files whose open holds a connection slot (`/net/tcp/clone` takes one of the
box's 16) and whose bytes are rendered per read under a constant `qid.vers` of 0.
So `content_versioned` answers per session, from the Larder's L1e latch
(`p9_client.cacheable`, set when the server answers a Twalkgetattr): a latched
session's images are shared and cached; any other session's mapping is private,
its own Burrow freed with its last mapping, so it holds a slot no longer than an
fd would and no mapper reads another's rendering (round 31 F1). A Haul or
host-share export, which never latches, still maps and executes, unshared.
An open that can stage answers no on any session (vote M29; **A holder's
mappings are private**, below).
`dev9p.content_versioned_follows_latch` holds the answer to the latch. The wire
qid's append bit reaches the Spoor as `QTAPPEND` (round 31 F5), so both mapping
cores refuse a Plan 9 server's append-only file; `dev9p.qid_type_from_wire` pins
the translation.

## Mechanism

### The priv and its lifetimes

`struct dev9p_priv` (kmalloc KP_ZERO, magic "D9PP", clobbered on free):
`client` + `fid` + `fid_owned`, `attached_owner` (one `p9_attached_ref` per
priv — the F236 discipline; walks inherit the parent's), `create_errno`
(#99 transient), `fid_gen`/`fid_suspect` (G2), `poll` (the lazily-allocated
arm state: the hook list an arm's answer walks, refcounted independently — #294), `weft` (the lazily-bound
flow ring; CAS-installed, ACQUIRE-read), the cached-open triple
(`cached_open`/`co_buf`/`co_size`/`co_stat`) with what it reaches the server by
(`co_rfid`, `co_kfid`, `co_start`, `co_names`, section above), and the write-behind block
under `wb_lock` ([[lock-dev9p-wb-priv]]).

**`dev9p_close` teardown order** (each step's position is load-bearing):
1. `dev9p_poll_priv_release` — cancel the outstanding readiness ARM BEFORE
   the fid clunk (a snapshot never outlives its poll call), so the netd
   `ready`-fd Tclunk is not refused on a live read and lands
   deterministically (#294).
2. Weft release: ACQUIRE-load + NULL (RELEASE) → `weft_reap_unregister`
   (leave the G-3 reaper registry FIRST — after it returns no sweep holds
   the binding) → the G-2 weave clunk-unmap (pid-matched, VMA-identity
   guarded) → `weft_binding_release`.
3. Cached-open: free `co_buf` + uncharge the global budget.
4. Write-behind: flush the staged run (the fid must still be live for the
   flush Twrites); a failure latches and drops the run, and the close
   returns it -- or the failure the latch kept from an earlier flush -- which
   `close(2)` reports as `EIO` (2026-10-07, ARCH section 21.11; until then
   `Dev.close` was void and the failure was silent,
   [[seam-wb-close-flush-slot]]) — then free the buffer + uncharge.
   A flush that fails on a thread dying inside `write`, `fsync` or `wstat`
   keeps the run staged and latches nothing (a death refused the send; the
   bytes were acknowledged), so this close sends it (`wb_flush_locked`,
   LARDER-DESIGN section 12; witness `dev9p.wb_dying_flush_keeps_run`).
   A flush a caught note interrupted is the same case (2026-10-07, B-2b):
   the client's flush(5) cancelled the Twrite and returned `-P9_E_INTR`, so
   the call returns `EINTR`, the run stays staged and unlatched, and a retry
   or the last close sends it; a close whose own flush is interrupted hands
   the run to the closer and returns 0. Until then the `EINTR` latched and the
   run was dropped, so every later write, fsync and close on the file
   returned `EINTR`. The cancellation is told by the call's mark
   (`wb_note_cancelled`, `note_call_unwound`: every caught-note unwind sets it,
   a thread-directed note's too, which takes no claim, and it holds to the
   EL0-return tail); a server's own Rlerror(EINTR) has no mark, since `client_run`
   takes it back when the op's own reply stands, so it latches like any failure and
   the last close reports it (B-2b audit F1, item 11c r2 F1). Witnesses
   `dev9p.wb_interrupted_flush_keeps_run` (the mark held) and its control
   `dev9p.wb_server_eintr_latches` (no mark, with a claim and without).
   **`dev9p_close` never flushes** (B-3h audit round 3, 2026-11-02): the last
   reference can fall on any thread, an image-cache eviction or a Loom reap, so
   the flush lives in `dev9p_fd_close` on the closing thread. That one flushes
   only when `close_may_wait()` holds, which is false on a die-pending thread (a
   killed thread's own last close, or a final close a second kill forced;
   2026-10-07, ARCH 7.9.1 part C). A run still staged at the last reference --
   never tried, or kept by a death that ended the flush -- goes to a closer with the fid's
   clunk (`wb_close_hand_off` -> `p9_attached_defer_close`, a
   `dev9p_close_job` owning the buffer and its budget charge), and the close
   returns 0: nothing is lost yet. Until its release the job stands in the
   open's place in the holder registry (`wb_close_swap`; "The file's holder"
   below), so the client's other opens wait for the run. The closer writes the run
   (`wb_write_run`, the loop `wb_flush_locked` uses), then drops the file's
   cached attr and pages rather than installing them, since that write lands
   unordered with the file's later writers; then it clunks. A hand-off that
   cannot be made (no session owner -- a test's bare client -- or no memory)
   loses the run loudly: `9p: close: flush of fid N failed rc R`, and the
   close returns the errno. The kernel-thread arm keys on the flag, not on
   kproc, because the in-kernel test runner is kproc's boot thread and waits
   on its fixtures by design.
5. `fid_owned`: **G2 donate or async clunk.** An unopened (COPEN clear)
   DIRECTORY fid on a cacheable client, not `fid_suspect`, and not staled
   (`larder_qid_staled_since` over the G4 ring since `fid_gen`) PARKS in
   the client's dir-fid cache instead of clunking; everything else takes
   `dev9p_clunk_fid` (fire-and-forget; the fid unbinds at send and its
   number is never reused; the ownerless Rclunk drains via a later op's
   reader). Dedup/evict victims from the park are clunked the same way,
   outside the table lock.

   **`dev9p_clunk_fid` is every dev9p clunk (2026-09-29, FID-LIFECYCLE
   section 9)**, all ten sites: the close's fid and G2 victims, the rename
   and create G2 drops, and the rollback clunks in walk, walk_attrs and
   create (asynchronous now too, so no dev9p Tclunk waits for its Rclunk). A
   thread whose Proc is dying cannot send; the client then answers
   `-P9_E_AGAIN` with the fid still bound, and the helper hands the fid to
   the closer through the priv's session owner (`p9_attached_defer_clunk`,
   [[sub-kernel-ninep-attach]]) -- before `p9_attached_unref` in step 6, so
   the entry's reference is taken while the priv's still holds. gopls's kill
   of a `go` child still in its spawn thunk was the measured case: three
   leaked fids a boot. A handed-off close skips this step: the closer clunks
   after its write. A thread no death reaches (`thread_death_reaches` false: a
   kernel thread such as the Loom SQPOLL reap, or an unforced exit close) never waits
   here: the helper clunks through `p9_client_clunk_nowait`, so a full op
   share or a full request ring sends the fid to the closer instead of
   holding the close on the server (`dec-2026-10-07-exit-close`, part A). Only a fid the live session still holds after that
   is reported (`p9_clunk_refused`); a dead session's fids died with it, and
   a fid a failed walk never bound had nothing to leak.
6. `p9_attached_unref` — possibly the last ref → the whole session tears
   down ([[sub-kernel-ninep-attach]]).
7. Magic clobber + kfree.

### walk and walk_attrs (POUNCE)

`dev9p_walk` (the plain per-component path): Walkqid carrier allocated
FIRST (the F3 resource-order rule — an OOM must not consume a fid number),
then `p9_client_alloc_fid` (monotonic, never reused), then the wire walk.
Partial walk (`nwqid != nname`) is a hard failure at this arm. The walked
priv inherits `attached_owner` and refs it.

`dev9p_walk_attrs` (Twalkgetattr — the stalk fast path; contract pinned by
`_Static_assert(DEV_WALK_ATTRS_MAX == P9_MAX_WALK)`):

- **Capability latch**: `client->wga_unsupported` → return the
  `DEV_WALK_ATTRS_UNSUPPORTED` sentinel; either `-T_E_NOSYS` or
  `-T_E_OPNOTSUPP` sets it, at both fused-walk and cached-open sites.
  A real transport error such as EIO does not latch non-support.
  **Non-support is the MAJORITY case and must be read as a class, not as a
  named example.** Stratum is the only v1.0 server that implements the op;
  every native userspace one (netd, ptyfs, tapestryd, the VIVARIUM diorama)
  falls through its unknown-op arm to ENOSYS, and **the list only grows** —
  each new native server joins the non-supporting side by default, since
  implementing the op is the deliberate act. The resolver then uses the
  per-component RPC loop for the session's lifetime without retrying the
  unsupported fused operation. npxf uses EOPNOTSUPP (95); classifying it
  prevents an absent operation from appearing to Haul as an absent file.
- **L1d dentry serve**: a fully-cached run serves RPC-free — a negative
  (miss) in EITHER form; a full positive ONLY in the query form
  (`nc == NULL`; a bind form must RPC to mint the server fid) and only if
  the leaf is not `perm_only` (a G3-downgraded leaf serves intermediate
  X-checks, not leaf consumers).
- **G2 consume**: a bind-form full-positive run whose leaf is a DIRECTORY
  with a parked fid re-issues the parked fid with ZERO wire ops. The gen
  snapshot is taken BEFORE the take (an invalidation landing in the
  serve→take window then falls inside the donate gate's staleness scan —
  the fail-safe direction; term-4 close F1).
- **The RPC path**: gen snapshot → `p9_client_walkgetattr` → on success:
  latch `client->cacheable = true` (**the L1e cacheability gate**: a
  successful Twalkgetattr is the v1.0 proxy for a content-versioned,
  offset-stable FS — the latch is what admits the attr/dentry/page caches
  for this session, and its absence is what keeps **every non-supporting
  server's tree** out of them: netd's streams, ptyfs, tapestryd, the
  diorama, and each future native server. **The non-support latch above is
  therefore the cache-admission decision**, which is why the class matters
  and a single named example actively misleads — a reader who takes one
  instance for the rule concludes the other sessions are cached. They are
  not), then per-component: fill `sts[i]`, attr-install (gen-guarded),
  positive-dentry-install (parent = previous component). A partial walk
  negative-installs the missing name under the last walked parent; a
  first-component `-T_E_NOENT` negative-installs under `c`. Bind form
  installs the fid into `nc` only on a FULL walk (the session layer's
  walkgetattr arm binds server-side only then too).

### open, create, and the errno/coherence choreography

`dev9p_open`: Plan 9 omode → Linux flags (low 2 bits; OEXEC(3) → O_RDONLY —
9P2000.L has no exec-open and 3 is the INVALID Linux accmode; the X check
already happened identity-side in stalk — #58; OTRUNC → O_TRUNC). OTRUNC
additionally drops the file's attr + pages (an own-write — the D44-F3 rule:
truncate coherence must not rest on the server bumping qid.version) and
arms write-behind eligibility (end known = 0) on a loose+cacheable client.

`dev9p_create` (file: Tlcreate creates-and-opens, always asking O_EXCL,
0200: both create paths -- an exclusive create, and open-or-create's create after its open missed -- count on
EEXIST for a name that exists, which a server honouring the flags would otherwise open; Stratum keys only the access
mode and O_APPEND, and the Mac app adds O_EXCL itself, so neither answers differently today (M15 round 3, F11; test
`dev9p.create_file` reads the flags); dir: Tmkdir → walk to
the child → swap fids → lopen OREAD): on ANY failure arm it records
`p->create_errno = rc` (#99 — read once by the create handler via
`dev9p_create_errno`, clamped to the [-4095,-2] passthrough window) and
latches `fid_suspect`; on `-T_E_EXIST` it ALSO drops the (parent,name)
dentry — EEXIST *proves* existence, so any cached negative is stale, and
without the drop a racing loser's retry-Open re-serves the stale negative
and spuriously ENOENTs (the #99-F1 P1, found convergently by the SMP gate,
the holotype, and the self-audit). Success-path coherence, in order:
parent attr DOWNGRADE (G3 — a child create cannot edit the parent's
mode/uid/gid, so the perm-servable core survives for mid-hop X-checks);
child attr INVALIDATE + child page INVALIDATE (ino-reuse: a recycled
qid.path may carry a prior occupant's attr/pages, and cvers collision
cannot be ruled out cross-project — the L1f-F1 rule); G2 dir-fid DROP on
the child's qid (a parked fid for the dead prior occupant must die);
(parent,name) dentry drop; write-behind arming (create-born ⇒ end known 0)
on loose+cacheable plain files.

`dev9p_open` resets and records `open_errno` on the unpublished walked
Spoor. `dev9p_open_errno` returns a server errno in [-4095,-2], otherwise the
unspecified sentinel. Both `SYS_WALK_OPEN` and stalk read it before failure
cleanup clunks the private state; netd's ENOMEM admission refusal therefore
reaches callers as ENOMEM. Success clears the previous failure. Non-9P devices
keep their generic EIO fallback. The pointer-returning `Dev.open` interface
still requires this per-operation record (as `Dev.create` already does); a
future typed vtable result would remove that interface debt. No global or
thread-local errno state is introduced.

### read (the serve/overlay/populate stack, in precedence order)

1. **cached-open serve**: the immutable open-time snapshot (no lock, no
   RPC).
2. **Write-behind overlay**: a read WITHIN the staged run serves from the
   buffer (short to the run end); BELOW it falls through (the append-anchor
   discipline: the server holds every byte under `wb_off`); AT/PAST the run
   end falls through so EOF comes honestly from the server/attr.
3. **Attr-served EOF** (task #44): a FRESH cached attr (cvers == the fid's
   open-time qid.vers) with `offset >= size` answers the sequential
   reader's final 0-probe RPC-free — plain files only (a dir's cached size
   must not convert the server's read-on-directory error into a silent 0).
4. **Page serve**: the one cached page containing `offset` (bounds the
   under-lock copy at 4 KiB; a short serve is a legal short read).
5. **The wire** — with the task-#44 aligned-read rule: a `count >
   LARDER_PAGE_SIZE` read on a cacheable client issues at the containing
   page's ALIGNED start (the msize payload 131049 is not page-multiple, so
   sequential streams otherwise leave a permanent partial-page hole that
   defeats re-serves — the measured 82%-of-misses class). Populate installs
   every page from its aligned start. The lead-shift tail: `got > lead`
   shifts down (overlap-safe forward copy); `got == 0` is true EOF;
   `0 < got <= lead` — the server short-returned before the caller's offset
   — RETRIES UNSHIFTED at the original offset and returns that verbatim,
   because a single Rread may legitimately short-return mid-file and
   returning 0 manufactured a false mid-file EOF (the D44-F1 P1:
   zero-filled REVENANT text pages).

### write, fsync, wstat_native

`dev9p_write`: cached-open rejects (defense-in-depth; omode-derived rights
already deny). Write-behind staging via `wb_write_prepare` (below). The
through path propagates the real `-errno` (#3: `-T_E_NOSPC` etc. reach
userspace; the EPERM/-1 collision residual is the ER-rollout's job), then:
attr invalidate + **range-scoped** page invalidate over
`[offset, offset+accepted)` (G1b — the ~100-byte buildid pwrite must not
nuke a just-populated archive; a zero-accepted write keeps the whole-file
drop), then `wb_note_through` advances the append anchor.

`dev9p_fsync`: flush the staged run first — **fsync is the reliable error
channel** (the voted NFS model: a latched flush error surfaces here even
after the run is gone) — then Tfsync with real-errno propagation. A cached open (read-only: nothing is
staged) sends its Tfsync on the fid its fstat and wstat address -- a strict open's kept fid, or the one a loose open
binds now (`co_bind`) -- because the server may hold the file's content uncommitted and fsync takes either right
(vote M22). A loose open whose name no longer reaches the file (renamed or removed since) still syncs, as POSIX
fsync on such a descriptor does: its Tfsync goes on the start the open kept, because a cached open exists only on a
server that walks with Twalkgetattr (Stratum), whose Tfsync commits the whole pool whichever fid carries it (M15
round 3, F8; it answered ESTALE before). Tests: `dev9p.cached_open` (one Tfsync, on the kept fid),
`dev9p.cached_open_loose_fsync` (on the start's fid while the name is gone, on a bound fid once it resolves).

`dev9p_wstat_native` (Tsetattr; `T_WSTAT_* == P9_SETATTR_*` pinned by eight
`_Static_assert`s, and a ninth keeps the wire's CTIME bit out of
`T_WSTAT_VALID`): every refusal is a `-errno`, the slot's contract. The
times (G12) go out as the 9P bits they equal: an explicit time carries the
caller's `t_timespec` in the atime/mtime fields, and "now" carries its bit
with zeroed fields, which the server answers with its own clock (Stratum
honours that since its 2026-10-22 Tsetattr fix; before it stored the
fields, the epoch). A caped session refuses UID/GID before anything else
with EACCES (the cape, below); a cached open sends it on its kept fid, and a
loose one binds first and answers ESTALE if the file moved since its open; a
failed flush or Tsetattr answers the wire's
errno through `dev9p_wire_errno` (its EPERM folded to EACCES);
write-behind: flush first (a truncate must land after the staged bytes;
a time must land after the write that would stamp it). The flush is the
NAMED Spoor's run only: a time set by path, through a fresh O_PATH Spoor,
while another open of the same file still stages bytes is overwritten when
that open flushes, so `write; utimensat(path); close` ends at the flush's
clock where Linux keeps the time set (OPEN-BUGS 2026-10-22, the G12 round's
F3; `cp -p` sets times on the handle that wrote). It then stops staging by clearing the append anchor (`wb_known`; a size change
destroys it) -- never the eligibility flag, which gates the read overlay,
fsync's flush and the write ordering of a run a death kept, and the latch's
report on every write and fsync, for another Proc sharing the fd too
(witnesses `dev9p.wb_dying_wstat_keeps_staging`,
`dev9p.wb_wstat_keeps_the_latch`), and with no run left it frees the
staging buffer and its budget share at once (`wb_release_dead_locked`;
nothing can stage into it again); on success
attr invalidate (CRITICAL — the base X-check perm_checks the cached mode,
so the invalidate keeps the guest's own chmod window at zero) + whole-file
page invalidate when SIZE changed.

### rename, unlink, readdir

Each attach session is its own device: `dev9p_attach_client` (once per session) mints the session's devno, which
`SYS_RENAME`'s one-device check compares and a stat reports as st_dev ([[sub-kernel-syscall-dispatch]]).

`dev9p_rename` (Trenameat; same-client gate — a renameat names two fids of one
session, so two sessions answer `-T_E_XDEV` before anything is sent, vote M15; test `dev9p.rename_two_sessions`) /
`dev9p_unlink` (Tunlinkat; `SYS_UNLINK_REMOVEDIR ==
P9_UNLINK_AT_REMOVEDIR` pinned): both resolve the G2 victim's qid from the
dentry cache BEFORE the wire op mutates the dirent set (rename-replace
kills the DEST inode; the SOURCE keeps its inode — fids track inodes, so
its parked fid survives), then drop+async-clunk the victim's parked fid,
invalidate the victim's own attr (the event arms the donate gate), G3
DOWNGRADE the parent dirs, and drop the touched (dir,name) bindings.
Errors latch `fid_suspect` on the borrowed dir privs. Neither allocates a
transient fid (borrow-only — the create-path leak class structurally
absent).

**Both now report WHY they failed** (#80): local argument rejects answer
`-T_E_INVAL`; the server's verdict passes through `dev9p_wire_errno` and
reaches userspace as itself. **No side-channel was needed and that is the
whole shape of the fix** — these slots return `int` and the client already
returned `-errno`, so unlike the create path (#99, whose `Dev.create`
returns a `Spoor *` with nowhere to put a cause) the value was merely being
discarded. `return rc` plus one bounding helper; no new state.

`dev9p_wire_errno` folds exactly one value: a server EPERM arrives as `-1`,
which **is** the flat generic-failure sentinel, so it becomes `-T_E_ACCES`
— the registry's sanctioned permission-denied stand-in. A deliberate small
lie, chosen over reporting a permission problem as an I/O error. Everything
else in `[-4095,-2]` crosses unchanged, including codes with no `T_E_*`
name (EISDIR 21, ENOTEMPTY 39, EXDEV 18): **a SERVER errno crosses by
value**, and both boundary lines map numerically, so the motivating cases
needed zero registry appends. Only errnos the KERNEL originates must be
named.

`dev9p_readdir`: the Treaddir offset is an **opaque resume cookie, not a
byte position** — Stratum derives it from an entry hash, so real cookies
exceed INT64_MAX; the bits pass straight through (`(u64)off`), and clamping
a "negative" cookie to 0 restarts enumeration forever (#955). This is why
byte reads clamp negative offsets but readdir must not.

### The identity cape (IDENTITY-DESIGN 3.2; HAUL-DESIGN 4.7)

A server whose ids are foreign -- a Haul export of a host tree, owned by the
host's uid 501 and group staff, which no Thylacine principal is -- would leave
every principal "other" under kernel DAC, and a private (0700/0600) tree
unreadable to the user who mounted it. Such a session is CAPED:
`client->cape` with `cape_uid` / `cape_gid`, stamped once by
`p9_client_set_cape` before the root Spoor publishes, never flipped
([[sub-kernel-ninep-attach]] decides when; [[sub-kernel-ninep-client]] holds
the fields). dev9p consults it at exactly three points:

- **The conversion.** `t_stat_from_p9_attr(out, attr, client)` is the ONE
  place a server attr becomes a `t_stat`: `stat_native` and both
  `walk_attrs` record sites call it, the Larder caches its output, and a
  cached-open's `co_stat` is a converted leaf -- so the cape applies there
  and nowhere else. uid = `cape_uid` (the attaching principal) and gid =
  `cape_gid` (its primary gid), whatever the server said or left out; the
  mode stays the server's under the same valid-mask fail-closed rule (an
  omitted mode is 0). perm_check, the resolver's X-search, the Larder serve
  and fstat therefore all see one owner. The Larder is per-client and the
  stamp precedes the first stat, so a caped session can never hold an
  uncaped conversion.
- **create.** `dev9p_create` sends gid `P9_NOGID` ((u32)-1) on a caped
  session: chown(2)'s "leave it", which a POSIX server hands to its
  set-group call as a no-op (npxf's `try_set_gid` passes it to
  `fchownat` as the group; `haul-cape.exp`'s create and mkdir land on the
  host through it). The creator's group never crosses.
- **wstat_native.** UID or GID in `valid` is refused FIRST -- ahead of the
  cached-open seam and any write-behind flush -- with EACCES. chmod,
  truncate and the times pass: they are the server's own.

Kernel DAC stays on. The owner bits of the server's mode now apply to the
mounter, so a 0600 host file is readable by the principal that mounted it,
and any other principal reaching the mount is "other". The Tattach's
PRINCIPAL_NONE is the attach layer's; the Loom GETATTR copy and the Loom
create gid are [[sub-kernel-loom]]'s.

### The write-behind engine (F1/G1; the deepest block)

State per priv under `wb_lock`: one contiguous append run
`[wb_off, wb_off+wb_len)` in `wb_buf` (cap ≤ `DEV9P_WB_CAP` 256 KiB, grown
by doubling with the global-budget delta charged), the append anchor
`wb_base` (valid iff `wb_known`; born 0 at create/OTRUNC; advanced by
completed flushes and through-writes), `wb_flushers` (freezes the run),
`wb_err` (the latched NFS-model errno).

`wb_write_prepare` decides: latched error → return it; stageable (anchored
+ exactly-append + ≤ `DEV9P_WB_STAGE_MAX` 32 KiB + no flusher) → stage;
run-full → inline flush then retry the stage; non-append with a live run →
if DISJOINT from a frozen mid-flight run, write through immediately
(appends-never-wait); if overlapping, flush FIRST (ordering: the staged
older bytes must land before an interior pwrite) then write through;
budget/alloc denial → write through (graceful — the budget is a DoS floor,
not correctness).

`wb_flush_locked` is **single-flight** (the SA-F1 close): a second
flush-needing party yield-waits (`sched()` loop — the on_cpu-spin class:
bounded by the flusher's independent progress incl. its #811 death-unwind;
no Rendez → no new I-9 leg). A duplicate flush would be UNSOUND, not
redundant: it completes while the first flusher's stale residual chunks
still fly, an ordering-dependent through-write lands between, and the
residuals silently overwrite it. The flusher count freezes the run (no
stage, no growth realloc) so the out-of-lock reads of the captured buffer
cannot race a kfree. Flush I/O: msize-max Twrites, wire outside the lock;
`acc == 0` fails (no progress must not spin). Coherence at the flush: attr
invalidate on BOTH arms; a failed or partial flush, or one past the open's
last close, drops the whole file's pages fail-safe; a FULL land inside the
open **installs the run's pages as OWN** (G1a — `larder_page_install_own_run`,
the write-populate: the build's read-back of just-written archives serves
from cache; the `err==0` coupling is pinned by the
`fs_cache_buggy_populate_unflushed` counterexample cfg). The install has a
gen guard (LARDER-DESIGN 13, votes M24, M24a): `wb_flush_locked` snapshots
the Larder's gen after freezing the run and before sending it, and an event
naming the file since then, a change on this client that may have reached
the server after the run, skips the install and drops the file's pages
(`page_own_skips`). An own page serves without the cvers gate and the
boundary page extends only an OWN page ending exactly at the run start:
nothing on the client changes the file while its holder's run is frozen (a
changing leg waits the flush out, or pinned the file before the open became
the holder), and the guard catches what gets past.

### The file's holder (LARDER-DESIGN 12.6, vote M23, 2026-11-15)

On one client a file's held bytes are the client's: another open's operation
on the file lands them first, as one page cache per file per client would.
**The registry**: 64 buckets keyed by client and qid.path
([[lock-dev9p-wb-reg]], ordered before `wb_lock`), each with a chain of
entries, at most one per file (its number and generation: **A file's name**,
below), and a chain of pins. An entry is a holder's
`wb_reg` -- `wb_register`, at the eligibility gate of a create-born open
(`dev9p_create`) or an OTRUNC open (`dev9p_open`) -- or a closer job's. A
second open of a held file, or of a file a pin stands on, stays ineligible
and writes through for its life. `wb_register` sets the open's anchor
(`wb_eligible`, `wb_known`, `wb_base`) and its entry's fields before it links
the entry, so a leg that finds the entry finds the anchor. The holder leaves
at its last close once nothing is staged (`dev9p_fd_close`), when the
lock-flush job its last close queued has left no run (past the last close
nothing stages again, though a map or a Loom op keeps the open), or at its
last reference with no run (`dev9p_close`, first thing). With a run
at the last reference, `wb_close_swap` moves the run into the hand-off job and
the job into the holder's place, under the bucket lock and before the job is
queued; the job's release leaves (`wb_reg_leave`), after the closer's write and
Tclunk. A hand-off that fails leaves the registry too. `g_wb_reg_count` (all
entries) is the observing legs' fast path.

**The pin** (votes M24, M24a, 2026-11-16; M26, 2026-11-17): a change through
any open, the holder's own included, pins the file, found holder or not, from
its leg's first look until its wire operation has answered and its Larder
drops are done (`struct wb_pin`, on the operation's stack, linked in its bucket's pin
chain; the operation's `wb_unpin` after them). While a pin stands
`wb_register` refuses, so no open becomes the holder between a change's leg
and the change reaching the server, whose answer the new holder's flush
could otherwise be installed over as own pages; Linux holds the inode lock
across a write or a truncating open for the same reason. A truncating
open's own pin is taken over by its registration, under the bucket lock
that decides it. Only a pin's owner links or unlinks it. The holder's own
wstat, Loom registration and write-through pin too (M26): their wire operation
runs outside the holder's `wb_lock`, and its entry can leave meanwhile (a last
close on another thread with nothing staged), so without the pin another open
could become the holder before the change lands. A write the holder stages
holds its pin only while it stages. A strict client never stages (`loose` is
fixed at attach), so its operations run no leg and take no pin (round 6 F9).

**`wb_holder_flush(cl, key, self, pin, refused)`** -- the leg, by the caller's
name (`struct dev9p_fkey`: **A file's name**, below). Observing
legs pass no pin: `dev9p_open` (any mode but OTRUNC, before the Tlopen),
`dev9p_open_cached`, `dev9p_read`, `dev9p_stat_native`, `dev9p_fsync`, and the
walk-fused getattr; with no entry on any file (`g_wb_reg_count`) they return
at once. Changing legs pass their pin and also drop the holder's anchor so
it stops staging: `dev9p_write`, an OTRUNC `dev9p_open`,
`dev9p_wstat_native`, `dev9p_loom_register`. Under the bucket lock the leg links
its pin at its first look, then visits every entry its key matches, newest
registration first (each entry carries `seq`, its bucket's count at its
registration). A key with a generation matches at most one entry, since a
registration refuses a name the same as a standing one; a key with no name
(`unnamed`: the open's Tgetattr failed or was never asked) matches every entry
at the number, and a leg that stopped at the first would pass a live holder
standing behind a deleted file's (M15 round 9 S-15). The holder's own
operations pass their own entry (`self`), keeping their pin (M26). A strict
client's leg returns 0 at once (F9). A holder with nothing held is passed (a
changing leg drops its anchor in place). A holder holding a run or a flush:
the leg takes a reference on its Spoor only while one is held
(`spoor_tryref`; a closing holder is refused) and collects it, at most eight
in a pass (`WB_LEG_VISIT_MAX`), then flushes each outside every lock through
`wb_flush_locked` (single-flight). A changing leg drops the anchor of every
holder it collected (vote M35a: a change through an open with no name may be
any of them's own file's). A job, or a holder refused as closing:
`wb_leg_wait`, a sleep doubling from 1 ms to 64 ms on every thread
(`tsleep_noteintr`: a death ends it with `-T_E_IO`, a caught note with
`-T_E_INTR`; counted in `g_wb_leg_waits`, and in `g_wb_leg_sleeps` when the
sleep timed out, round 6 F8), then look
again, past the entries already passed (`lim`: a pass takes only registrations
older than the one it stopped at), so each entry is passed at most once and the
leg ends. A yield would spin a CPU for as long as a closer holds the run, about
a second for a refused write and without end on a hung server (M15 round 5,
F3). Returns 0, 1 (a run landed; or the server refused it, and it was dropped,
latched on the holder and recorded as a lost run (below), the refusal also
put in `*refused` when the caller asks, as is a refusal latched between the
leg's look, which found the run held, and its flush, which found nothing left
(round 6 F2a); or a run was waited out), `-T_E_INTR` (this thread's caught
note cancelled the flush or ended the wait: `wb_note_cancelled`,
`thread_caught_note_unwinds`) or `-T_E_IO` (a death). On a negative return
the operation does not happen, its pin is released, and a run it flushed
stays held. A server's own Rlerror(EINTR), with no caught note behind it, is
a refusal like any other (R5b-F6). A refusal is latched, put in `*refused`
and recorded as `EIO`, unless the server refused for space (`ENOSPC`,
`EDQUOT`), which pass as they came (`wb_refusal_errno`, vote M28: Linux's
`mapping_set_error` rule; a server's `EINTR` or `EAGAIN` would invite a retry
that finds nothing held and returns 0). A caller with no name takes no refusal
from a holder named by a generation, which may be a deleted file's (vote M35,
round 9 F2): it lands the run, and hears of the loss only through the record
past its sample.

**The stat by name** (`walk_attrs_pass(..., asked)`, wrapped by
`dev9p_walk_attrs`): the Larder serve skips a held leaf (asks the server
instead); a wire answer naming a held file lands the run, gives back a bound
fid (bind form) and walks once more with `asked`, so it reports the file after
the flush.

**What a leg's result means to its caller.** An fsync whose leg flush the
server refused returns the refusal (vote M25): fsync through any descriptor
writes back what the holder holds and reports a failed writeback, as Linux
reports one to every descriptor's next fsync; the holder keeps its latch for
its own next write, fsync or last close. A third open's fsync, which flushes
nothing, is told of the refusal once, by its record (below). A cached open whose leg failed fails the open:
`dev9p_open_cached` records the errno with `dev_walk_failed` and returns
NULL, and the resolver fails the open with it rather than take the normal
path with the run still held (M15 round 5, R5b-F3; [[sub-kernel-stalk]]). A
leg that landed a run returns NULL with no errno, and the normal path opens
the file after the flush.

**Lost runs** (votes M27, M30a, 2026-11-17). A run is lost when the server
refuses it or a closer gives it up, and the loss may reach no program: an
exit's last close, a closer's spent retries, a hand-off that could not be
queued. So every lost run is recorded against the file on its client, as
Linux's errseq records a writeback error against the inode: `struct
p9_wb_losses` in `struct p9_client` ([[sub-kernel-ninep-client]]), at most
`P9_WB_LOSS_MAX` (16) records, one per file name (below), each a sequence, the
errno and whether an open has been told. `wb_loss_record` records a refused run at its
refusal, in `wb_flush_locked` under the holder's `wb_lock`, which every leg
reads the latch under, so no leg reports a refusal before it is recorded; and
a closer's run where its job gives it up -- the close job's release when its
last try failed, each failure arm of `wb_close_hand_off` -- before the job or
the open leaves the registry, so a leg waiting on it finds the record when it
stops waiting. A full table evicts its oldest record, and that loss is then
told only where it was before: the holder's latch, the fsync whose leg met it,
the console line. Every open samples the record under its name into
`wb_errseq` where its fid is made (`wb_open_sample`: the walk's mint, a
navigation handle's included, round 7 F3; the walk_attrs mint; the create; the
cached open's mint), and `dev9p_open` samples again under the same name: the
record's sequence once an open has been told, the one before while none has,
so an open made after a loss nobody heard is told too. `dev9p_fsync` calls
`wb_loss_report` after its leg and after its own run is written back (round 7
F5: Linux writes back, then reports; a told loss skips only the Tfsync): a
record past the open's sample is returned once (`EIO`, or the space errno),
and the open catches up and marks the record told -- the holder's own fsync
too, so an open made after it is not told the same loss again (round 7
S-4). An open with no name catches up but marks nothing told: it may be a
deleted file's, which found the live file's record by number, and that file's
own later opens are told still (round 10 F2). The report runs before the
fsync returns its leg's refusal, so no open is told of one loss twice, and the
leg's refusal is the answer even when its record was evicted meanwhile. A peer
is told of a holder's refusal even when the holder heard it from a write or an
fsync, as Linux tells every descriptor's next fsync. The table's lock is a
leaf, taken under an open's `wb_lock` at a refusal and with no lock held
elsewhere ([[lock-dev9p-wb-priv]]).

**A file's name** (votes M31, M33, 2026-11-18). Stratum's qid.path is the
inode number alone: it frees the number at the last unlink, while opens of the
file stand (their fids then answer `ESTALE`), and gives it to the next file
created. So write-behind names a file by number and generation, as Linux's NFS
client names a file by a handle that carries the generation: `struct
dev9p_fkey` (`path`, `gen`, `has_gen`). Every registry entry, pin and lost-run
record carries the name, and `wb_same` compares two: the same number, and the
same generation where both carry one. An open's name is set where its fid is
made, before the priv is published, and never changes (`wb_name`, `wb_gen`;
`wb_key_of` reads them): the walk_attrs mint takes the Twalkgetattr's
generation; a plain walk that lands on a plain file, on a session that stages
(loose and cacheable: `wb_session_names`), asks one Tgetattr on the new fid
before the walk publishes it, for the basic attributes and `P9_GETATTR_GEN`
together (`wb_name_learn`, votes M33 and M36), as Linux's 9P client asks
whenever it makes an inode from a fid, and a reply that names the file the
walk bound and carries the whole basic set fills the Larder's attribute entry
under the freshness guard the walk that asks attributes uses (M36, a populate
route of [[sub-kernel-larder]]), so a stat after the walk asks nothing more --
the resolver walks one
name at a time on any path holding `..`, at a union member, and on a server
without the fused walk; a clone that walks no name copies its source's name
and lock identity; a create asks once; a cached open takes the Larder's
identity for the leaf. `wb_name` is GEN, NONE (the server reports no
generation: the number names the file, vote M31's allowance), FAILED (the
Tgetattr failed: no memory, the file gone, a server's refusal), or UNASKED (a
session that does not stage, or not a plain file). A caught note or a death
during the plain walk's Tgetattr fails the walk instead: it clunks the fid it
bound (a dying thread sends no Tclunk; the closer sends it) and frees the priv
it never published (ARCH 8.8.3: a call that sees a caught note returns EINTR;
round 9 F3, round 10 F4). An open with no name (FAILED or
UNASKED) registers nothing and writes through (`wb_named` gates the
registration), samples the current sequence so it is told only losses recorded
after it was made, and its legs and report go by number: its legs visit every
holder at the number, take no refusal from one named by a generation (vote
M35), and on a change stop every one of them staging (vote M35a). The answer names the file for the file locks
too (`wb_name_from_attr`), which then ask nothing more; a generation the locks
learn later for an open that has no name stays theirs, so its sample and its
report read the same records (round 8 F2). State of another generation is a
deleted file's: a leg skips it, it refuses no registration, and its refusal is
recorded under its own name, so only its own file's opens are told of it.
A strict session never stages and asks nothing.

**A handle is served by its name** (votes M34, M34a, 2026-11-18). The Larder
keys its entries by number, so a handle of a deleted file was served the next
file's own pages and attributes (M15 round 9 F1). A read, a mapping fill and
an fstat through a handle ask the Larder with the handle's name (`wb_who_of`,
a `struct larder_who`): its generation for an open named GEN; by number for
one named NONE, and for every directory handle (vote M34a: the root and a
directory a plain walk reached have no name, and the resolver's per-directory
search check stays a cache hit); UNNAMED for any other, which is served no own
page and no attribute and asks the server, which answers a deleted file's fid
`ESTALE`. `dev9p_read` asks the end-of-file size (`larder_attr_fresh_size_who`)
and the pages (`larder_page_serve_who`), `dev9p_stat_native` the attributes
(`larder_attr_serve_who`), and the cached open its hint and its snapshot under
the identity it would mint (`larder_attr_gen`, `larder_pages_cover_who`,
`larder_pages_snapshot_who`; an identity it does not know fails closed). A
page read from the server keeps its version check and serves any handle. The
own pages a flush installs carry the name their holder registered under
(`wb_holder_key`). The rules are [[sub-kernel-larder]]'s.

**A ring FSYNC is told** (vote M32, 2026-11-18). `dev9p_fsync_report`, the
Dev's `fsync_report` ([[sub-kernel-dev]]), is what a Loom FSYNC whose Tfsync
succeeded is told: the record under its open's name past the open's sample,
once, as `dev9p_fsync` reports it. The ring lands nothing first (it runs no
leg). Loom calls it under the client's lock ([[sub-kernel-loom]]): the report
takes only the table's leaf lock and never blocks.

**A failed change drops what it may have staled** (round 6 F6). A Tsetattr the
server failed may have applied (a reply this thread never read), as a failed
write may have landed: `dev9p_wstat_native` drops the attr, and the pages too
when the size was asked. A failed truncating Tlopen drops the attr and the
pages, under its pin, before the pin goes.

**A holder's mappings are private** (vote M29). `dev9p_content_versioned`
answers false for an open that can stage (`wb_eligible`, fixed before the open
is published): its pages would fill through its own read, which overlays bytes
the server may yet refuse. Its mappings are private Images
([[sub-kernel-image]]); another open's mapping fills through its own read,
whose leg lands the run first.

Witnesses: `dev9p.m23_peer_open_lands_the_run`,
`.m23_peer_read_fstat_fsync_land_the_run`,
`.m23_stat_by_name_lands_and_asks_again` (wire, Larder and bind arms),
`.m23_peer_write_lands_the_run_first`, `.m23_peer_write_stops_an_idle_holder`,
`.m23_truncating_open_wstat_loom_change_the_file`,
`.m23_one_holder_per_file`, `.m23_leg_flush_failure_latches_on_the_holder`,
`.m23_interrupted_leg_keeps_the_run` (EINTR and EIO),
`.m23_cached_open_of_a_held_file_falls_back`, and
`p9_closer.peer_waits_out_a_handed_off_run` (the job's place); and for round
5, `dev9p.m24_change_pins_the_file`, `.m24_pin_refuses_a_holder` (M24a's pin
with no holder found among its arms), `.m24_guard_skips_the_own_install`,
`.m25_peer_fsync_reports_a_refused_run`, `.m23_server_eintr_is_a_refusal`,
`.m23_leg_wait_sleeps`, `.m23_cached_open_interrupted_fails`, the page arms of
`.m23_wstat_changes_the_file` and `.m23_loom_registration_changes_the_file`
(R5b-F1), the bind form's
clunk in `.m23_stat_by_name_lands_and_asks_again` (F4), and
`p9_closer.lock_flush_job_leaves_the_registry` (F2); and for round 6,
`.m23_truncating_open_changes_the_file`, `.m23_wstat_changes_the_file` and
`.m23_loom_registration_changes_the_file` (one per changing leg, so a red
names its leg), `.m26_holder_changes_pin`,
`.m27_lost_run_reaches_the_next_fsync`, `.m27_unhanded_run_is_recorded`,
`.m30_closing_refusal_reaches_a_peer`, `.m30a_a_loss_is_told_once`,
`.m25_refusal_inside_the_leg_window` (F2a, with an evicted leg),
`.m25_refusal_outlives_its_record`, `.m29_holder_maps_privately`,
`.mapping_keys_on_the_probe_version`,
`.m24_failed_change_drops_what_it_staled` (F6),
`.m24_strict_client_runs_no_leg` (F9), `.m23_stalk_eintr_names_no_component`
(F4), `.m23_stalk_cached_open_interrupted_fails`,
`p9_closer.close_job_loss_reaches_a_peer_fsync` and
`p9_closer.lock_flush_job_loss_reaches_a_peer_fsync`; and for round 7,
`.m31_walk_names_the_file`, `.m31_a_new_file_at_a_dead_files_number`,
`.m31_a_dead_files_loss_is_not_the_new_files`,
`.m31_cached_mint_samples_by_generation`,
`.m31_create_that_cannot_learn_writes_through`, `.m31_legs_skip_a_dead_holder`,
`.m32_ring_fsync_is_told`, `.m27_navigation_handle_samples_at_its_walk` (F3),
`.m27_create_samples_the_record`, `.m27_told_fsync_writes_back_first` (F5),
`.m30a_holders_fsync_marks_the_loss_told` (S-4),
`.m26_holder_loom_registration_pins`,
`.m28_edquot_through_the_leg_and_the_latch`,
`.m28_sys_close_passes_space_errnos`, and
`p9_closer.hand_off_job_oom_reaches_a_peer_fsync` and
`p9_closer.hand_off_node_oom_reaches_a_peer_fsync` (F7); and for round 8,
`.m33_plain_walk_names_the_file`, `.m33_plain_walked_holder_is_named`,
`.m33_plain_walked_open_skips_a_dead_holder`, `.m33_clone_keeps_the_name`,
`.m33_unnamed_open_writes_through`, `.m33_the_name_does_not_move` (F2),
`.m31_a_generation_leg_lands_a_numbered_holder` and
`.m32_ring_fsync_names_the_file` (F4); and for round 9,
`.m34_dead_open_no_other_files_own_page`, `.m34_dead_open_no_other_files_attr`,
`.m34_unnamed_file_misses_m34a_unnamed_dir_by_number`,
`.m35_unnamed_leg_takes_no_named_refusal`,
`.m35a_unnamed_change_stops_the_holder`, `.s15_unnamed_leg_visits_every_holder`,
`.f3_plain_walk_note_fails_the_walk`, `.m36_plain_walk_fills_the_larder` and
`.m33_plain_walk_to_a_dir_asks_nothing` (F5); and for round 10,
`.m35_unnamed_report_marks_no_record_told` (F2),
`.s15_leg_visits_past_its_batch`, `.s15_leg_resumes_after_a_closer` and the
idle arm of `.s15_unnamed_leg_visits_every_holder` (F3), the raced arm of
`.m36_plain_walk_fills_the_larder` and `.f3_plain_walk_death_fails_the_walk`
(F4). KERNEL_TESTS
diagnostics: `dev9p_wb_holder_flushes`, `dev9p_wb_holders`,
`dev9p_wb_leg_waits`, `dev9p_wb_leg_sleeps`, `dev9p_wb_pins`; hooks:
`dev9p_wb_test_pin`, `dev9p_wb_test_job` and `dev9p_wb_test_job_gen` (a closer
job's entry, by number or under a generation, so it can stand between two
holders), `dev9p_wb_test_leg_window` (once,
between a leg's look and its flush), `dev9p_wb_test_report_window` (once,
between an fsync's leg and its loss report), `dev9p_wb_test_loss` and
`dev9p_wb_test_loss_gen` (a lost run by hand, by number or under a
generation), `dev9p_wb_fail_close_jobs_for_test` (the hand-off's no-job arm),
`dev9p_wb_test_release_knobs` (the runner's CLOSER-KNOB release after every
test: the close-job failures, the two windows and the budget bias, named when
a test left one set; round 8 F5, round 9 F4), and `dev9p_wb_test_purge`, which
the runner's WB-REGISTRY release calls after every test, so a test that
fails with a holder registered on the suite's shared client leaves no entry
for the next one to find; it unlinks only the test's own static pin, and
counts any other pin of the client, which belongs to a running operation whose
`wb_unpin` would find it gone (round 6 S-1). Spec: `fs_cache.tla`'s named opens (OneHolder;
the `buggy_peer_skips_staged`, `buggy_peer_unordered`,
`buggy_peer_unordered_lost` and `buggy_unpinned` cfgs, with `wb_nopop` the
green control); the cached-open leg and the guard are beneath the model. Since
round 8 (F3) it models the reuse of a number too: a file is its number and
generation, every open is named where its fid is made, and a dead holder's run
is recorded under its own name (`NoMisdirectedLoss`, `DeadStateSkipped`; the
`reuse` cfg green, its `reuse_told` and `reuse_coexist` witnesses, the
`buggy_number_key` and `buggy_unnamed_walk` cfgs red); a server that reports
no generation is beneath it, pinned by the m33_ tests. Since round 9 (F1, F7) a deleted file's open reads through the Larder
too, whose serve compares the open's name with the entry's (`buggy_number_serve`
red, the `reuse_served` witness); a live holder's flush can be refused, and is
recorded as a dead one's is; and an fsync is answered by its own leg's refusal
(the `reuse_told_leg` witness, `buggy_number_key_told_leg` red). Since round
10 (S-17, F2, F5) it models the open with no name (`EnableUnnamed`: served
nothing, staging nothing, its legs and report by number, marking no record
told -- `NoMisdirectedSeen`; the `reuse_unnamed` cfg green, its
`reuse_unnamed_told` and `reuse_unnamed_residue` witnesses, the
`buggy_unnamed_serve`, `buggy_unnamed_leg_told` and `buggy_unnamed_marks_seen`
cfgs red), and another client's or session's reuse of a number, whose Larder
entry stands (`ExternalReuse`: the `reuse_external` cfg green,
`buggy_serve_older` red). Those cfgs run one file.

### The weft data-drive arms

`dev9p_weft_try_write`/`_read`: ACQUIRE-load `p->weft`; below the hybrid
threshold or outside the ring → 0 (byte-copy fallback);
`weft_binding_validate_rw` (the I-30 validator-once over the kernel-private
ring view) → `p9_client_weftio(WRITE/READ)`. A weft failure returns -1
(dead flow), never a silent fallback. On the read arm the syscall handler
does NO uaccess_store — netd already wrote the guest's shared mapping.
The binding lifetime rests on: every reader holds a Spoor ref, and
`dev9p_close` (the last ref) is therefore excluded while any reader runs.

### The dir-fid cache mechanics (G2, table on the client)

`dirfid_take` (exclusive: entry removed — one Spoor per live fid, I-11),
`dirfid_put` (dedup keeps the resident, returns the incoming for the
caller to clunk; overflow evicts round-robin via `hand`), `dirfid_drop`
(the reuse-hazard kill: create-at-reused-ino / rmdir / rename-replace
victims MUST be clunked — a fresh walk re-resolving a reused qid.path must
never be served a fid for the dead object). All returns are clunked by the
CALLER outside the leaf lock.

### The remote declaration's two readers, and the name a session root's mount carries (LR-1 2026-09-28; the origin 2026-09-29; served links 2026-10-06)

`dev9p_spoor_remote(c)` answers whether `c` belongs to a session declared
remote at its attach or its /srv post (HAUL-DESIGN 4.8). It reads the
client's `remote` flag through `priv_of`, so it is false for NULL, for a
Spoor of another Dev, for a dev9p Spoor with no priv, and for a priv that
does not carry `DEV9P_PRIV_MAGIC`. The read is lock-free: the flag is
stamped before the session's root publishes and never flips, and the
caller's reference on `c` keeps the priv and the client alive. It has two
callers: `territory_format_ns` ([[sub-kernel-territory]]), which renders the
` remote` suffix, and, as this Dev's `remote` slot (`.remote =
dev9p_spoor_remote`, 2026-10-06), the resolver, which contains a link the
session serves beneath the mount it was reached through ([[sub-kernel-stalk]],
DISTRO 4.6) and so only narrows. Nothing else in this Dev consults the flag.

Every session root is born named "/" (`dev9p_attach_client`) and keeps that
name: it is the namespace root's name when joey pivots to one, and a pivot
never re-stamps a published Spoor. A mount line names the root otherwise
(operator vote 2026-09-28, [[dec-2026-09-28-ns-session-root-names]]): by the
file its session came over. `dev9p_stamp_origin(root, transport)` records it
on the root's priv -- `origin`, a reference on the transport's `->path`, or,
when the transport has no name, `origin_dc`, its device character -- and
`dev9p_spoor_origin` reads it back. Both attach handlers stamp between minting
the root and `handle_alloc` ([[sub-kernel-syscall-dispatch]]), so the plain
fields are set before publication and never change; `dev9p_close` drops the
reference. Only an unstamped root takes the stamp: a walked priv (`fid_owned`)
and a cached-open priv (not a root, and with no fid on a loose client) refuse it, and a
walk -- the zero-name clone every mount crossing makes included -- always
builds a fresh priv, so no copy of the root carries the name. A root reached
by opening a 9P-mode /srv service is named through its `->path` instead
(stalk's adoption arm transplants the opened path onto it). `#9` appears only
when `path_make_root` fails on an unstamped root. The name is display only
(I-33); its one reader is `territory_format_ns` ([[sub-kernel-territory]]).

Tests: `dev9p.remote_format_ns` opens on this function (NULL, another Dev, a
bare dev9p Spoor, a priv naming a marked session under the wrong magic and,
as the control, under the right one) before its rendering legs; the REMOTE
rows of `dev9p.walk_create_refuses_dmsrv_bits` and
`dev9p.path_create_refuses_dmsrvcape` keep the bit out of a Tlcreate perm.
The LR-1 sabotage boots turned `remote_format_ns` red both when the function
answered for every dev9p Spoor and when it read a priv without its magic.
The slot itself is held on the device by `haul-links`, which follows a real
npxf export's links over both Haul forms: with the `.remote` line deleted from
the vtable, its first leg read the guest's own decoy (2026-10-06).
`dev9p.origin_format_ns` pins the origin: the accessor's negatives (NULL,
another Dev, a bare dev9p Spoor), an unstamped root's `/`, the stamp sharing
the name (its count rises by one) and refusing a second stamp, the rendered
name with and without ` remote`, windows inside the name, short of the
newline and inside the binds line (the listing is read in windows since the
dynamic mount table, [[sub-kernel-territory]]), a clone walk carrying no origin and refusing the stamp, a covered entry on a
stamped root keeping its own name, the root's close releasing the reference,
a nameless transport's device spec, an origin named `/srv/a b\c` written as
`/srv/a\040b\134c` (operator vote 97's escapes, at the origin as at the other
names), and a cached-open priv refusing what the same priv shaped as a root
takes.

## Data structures

`dev9p_priv` as above. Budgets: `DEV9P_CO_MAX_SIZE` 128 KiB /
`DEV9P_CO_BUDGET` 8 MiB (cached-open), `DEV9P_WB_CAP` 256 KiB /
`DEV9P_WB_STAGE_MAX` 32 KiB / `DEV9P_WB_BUDGET` 8 MiB (write-behind). Both
budgets are **GLOBAL, not per-Proc** — a priv crosses Proc boundaries
(handle_dup, rfork inheritance, the #926/#68 close-at-exit runs in
whichever holder dies last), so a per-Proc charge has no sound uncharge
site; exhaustion degrades the fast path only. CAS charge / atomic
uncharge; test accessors assert balance.

## Concurrency

- `wb_lock`: a PURE LEAF spinlock — byte copies + state only; wire I/O
  never under it; the only lock below is the buddy zone via kmalloc/kfree
  ([[lock-dev9p-wb-priv]]).
- The Larder's own leaf lock serializes serve/populate/invalidate (that
  surface's dossier owns it); dev9p passes gen snapshots across its RPCs
  (captured BEFORE the wire op, checked at install — the
  populate-after-invalidate resurrection close).
- `p->weft`: CAS install + ACQUIRE readers + RELEASE clear-at-close;
  non-racy today via the ref-exclusion argument, ordering kept as
  future-proofing (Weft-7 F3).
- `p->poll`: RELEASE-publish / ACQUIRE fast-path read (net-6b F5); the
  rest under the poll registry lock ([[sub-kernel-ninep-dev9p-poll]]).
- `cached_open` state is immutable post-mint (lock-free serves);
  `create_errno` and `open_errno` are handler-local by construction (a fresh clone-walk priv,
  read once before the clunk — #99's no-sharing argument).
- Everything else rides the client's `c->lock` via the `p9_client_*` calls.

## Invariants enforced

![[inv-i38#Statement]]

dev9p is I-38's write-side enforcement: every mutation path pairs its wire
op with the exact invalidate/downgrade set above (OwnWrite), walk_attrs is
the revalidate-by-overwrite (Open), and the wb/cached-open legs are the
staged refinements ([[spec-fs-cache]] models StageWrite/FlushClose; the
`EnableStaging => ~EnableExternalWriter` premise is the loose-mount
single-writer assertion). The fail-ordering half of I-28 (which dev9p
FEEDS via fresh `sts` records but which is ENFORCED in stalk) is
deliberately not owned here — the resolver post-scans.

## Error paths

Real-errno propagation on read/write/fsync/getattr (#3/Area-F) and on
rename/unlink (#80), and on wstat (G12): a chown/chgrp on a caped session
answers EACCES before the wire, a loose cached open that cannot reach its file
ESTALE, and a flush or
Tsetattr failure the wire's errno. `dev9p_create`'s errno record + accessor (#99).
NULL/`-1` per the Dev convention elsewhere. A failed walk records its reason with
`dev_walk_failed` ([[sub-kernel-dev]]): the server's errno by value, except that its
`ENOSYS` or `EOPNOTSUPP` to the base op reads `EIO` (a broken server, not a missing
call: a Linux guest's T2 row reads `ENOSYS` as its own decline; the fused op's two are
its latch and never get here); `EIO` for no priv or an over-count; `EINVAL` for a walk
from an open fid or bad arguments; `ENOMEM`; `EMFILE` for fid exhaustion. A short Rwalk
stays a miss: it is how 9P says a later name is missing. Only `ENOENT` installs a
negative dentry. `walk_attrs` returns the
distinct `DEV_WALK_ATTRS_UNSUPPORTED` sentinel for the capability miss (NOT
a walk failure — nothing about the path was learned).

**The EPERM collision is handled THREE different ways in this file, and the
difference is not decoration.** A server EPERM is ecode 1, and the client's
mapper rejects only ecode 0 and anything above 4095 — so it arrives as
`-1`, which is the flat generic sentinel. Each path decided separately:

| Path | On server EPERM | Userspace sees |
|---|---|---|
| rename / unlink (#80) | folded to `-T_E_ACCES` | EACCES |
| write (#3) | returned raw | the flat sentinel → EIO |
| create (#99) | falls outside the `[-4095,-2]` accessor window → `-1` | EIO under pouch; a blanket EPERM on go's native seam |
| stat_native (ER-1, ER-2) | returned raw as `-1`; stat and fstat read it as `-T_E_IO` | EIO |

The write path documents its collision as a known residual owed to the
rollout. The create accessor's window silently excludes it, so a
permission-denied create and a permission-denied unlink give the same
caller different answers to the same class of denial. Neither is a bug
today; both are the shape the remaining ER stages exist to converge.

**The native-stat arm's comment states the one mapping both its callers
apply.** A server EPERM reaches `dev9p_stat_native` as `-1`, because the
mapper's clamp passes ecode 1 through by value, and the comment says so: the
resolver's `err_code` names that value, so the frame below knew it before the
frame above. stalk's `err_code` for `*errp` (ER-1) and `sys_fstat_for_proc`'s
return (ER-2, 2026-10-20) map the arm's answer alike: a -errno in `[-4095,-2]`
crosses as itself, and the `-1`, a server EPERM included, reads `-T_E_IO`.
A server's EBADF never reaches the arm: the client reads it as `-EIO` at
`map_error` (vote 2026-10-21 #16), so stalk and fstat both see `-T_E_IO`.
`dev9p.wb_server_ebadf_reads_eio` drives it through an open file: a flush
Twrite the server answers EBADF makes the fsync and the close read EIO, and
`wb_fsync_flush_and_error`, whose ENOSPC crosses as itself, is the control.
On a close the closer finishes (an exit close), that EIO takes the closer's
bounded retry, as a server's EIO always did ([[sub-kernel-ninep-attach]]).
[[seam-fstat-errno-flattened-above-the-leaf]] records the comment's earlier
claim that both callers propagated, when the fstat half flattened to `-1`.

## Performance

The read stack's serve precedence is the measured FS-perf ladder (POUNCE
→ Larder L1c/d/e → task-#44 EOF/alignment → G1/G2/G3/G4 → wb F1): warm
gofmt 1352→1147 ms at L1f; S3 wire reads 6.7k→3.5k at G1; the arc ledgers
live in the go-build measurement series. Hot-path allocations: one priv
kmalloc per walk; the attrs scratch on the walk_attrs RPC path (heap — 16
× ~152 B is too big for the 16 KiB kstack above stalk's own arrays).

## Last close and last reference (B-3h audit round 3, 2026-11-02)

The image cache keeps a file's first map's own Spoor until 128 other images
churn, so a lock or a staged run tied to the last reference outlived the Proc,
and a flush at the last reference waited on a stranger's thread. `dev9p_fd_close`
now does what the closing thread owes, and `dev9p_close` does what any thread may.

- `dev9p_close`'s clunks go through `dev9p_close_clunk`, always
  `p9_client_clunk_nowait`. A live thread's clunk used to wait out a full tag
  pool; it now spills to a closer, which waits instead.
- A last close's failure with no run left goes to its caller. When no program will hear it,
  on a dying thread or because the caller says so (`heard` false: an exit, a replace, an
  endowment no table took), the close reports it with `p9_close_job_lost`. A latch a write or
  an fsync already returned prints again. Legs: `p9_closer.dying_close_reports_a_retired_flush`,
  `.unheard_close_reports_its_loss`.
- A job with no run behind it only unlocks: what landed inside the open keeps its pages. Leg:
  `p9_closer.job_keeps_what_landed_inside_the_open`.
- `dev9p_fd_close` is idempotent: `sys_file_lock_handler` runs it again for a take
  that lost the race with the close (`spoor_fd_open`; [[sub-kernel-filelock]]). The
  grant store and the close's exchange are sequentially consistent for that.
- `dev9p_lock_flush_job` gains `release_locks`. A job with it (a last close's) leaves the
  holder registry once its flush has left no run: past the last close nothing stages again,
  so another open of the file may hold it while a map or a Loom op keeps this one (M15
  round 5, F2). Leg: `p9_closer.lock_flush_job_leaves_the_registry`.
- `wb_flush_locked`'s wait for another flusher leaves on `thread_die_pending` with
  `-P9_E_IO`; a killed thread used to burn a CPU there.
- `dev9p_lock` records `lock_granted` on a take's `-P9_E_IO`: a death may have
  abandoned a reply that granted it.
- `dev9p_write`'s failure arm drops the file's attr and pages: a server can fail a
  write it applied in part. `larder_forget_all` covers the write nobody answered
  ([[sub-kernel-larder]]).
- `dev9p_walk_attrs` answers `DEV_WALK_ATTRS_UNSUPPORTED`, unlatched, for a walk of
  several names that fails with anything but ENOENT: the reply cannot say at which
  name, and [[sub-kernel-stalk]] runs its X gates and mount split in order by walking
  name by name.

Legs: `dev9p.lock_close_order`, `dev9p.wb_last_close_with_holder`,
`dev9p.wb_dying_waiter_leaves_flush_wait`, `dev9p.wga_failure_at_an_unknown_name`,
`p9_closer.dying_last_close_with_holder`, and the added arms of
`dev9p.lock_close_releases`.

## Prosecution

- **The coherence pairing**: every mutation path must carry its exact
  invalidate/downgrade/drop set — a missed arm is a stale serve (the
  #99-F1, L1f-F1, D44-F3 family); a widened one (invalidate where
  downgrade suffices) silently re-opens the re-stat storm.
- **Ino-reuse discipline**: child attr + child pages + parked fid all drop
  at create; rename/unlink resolve victims BEFORE the wire op. Data
  integrity never rests on cross-project cvers uniqueness.
- **The wb single-flight + freeze protocol**: a duplicate flush, a stage
  under a nonzero `wb_flushers`, or a growth realloc during a flush is
  memory-unsafe or silently reordering; the close-flush must precede the
  async clunk (a Tclunk racing ahead writes to a dead fid).
- **Read-path EOF honesty**: the overlay must fall through at/past the run
  end; the attr-served EOF stays plain-file + fresh-cvers gated; the
  aligned-read short-return arm must retry unshifted (D44-F1).
- **G2's three stale-fid layers** (drop hooks / donate staleness gate /
  `fid_suspect` backstop) — weakening any one re-opens the wrong-object
  serve.
- **The cacheability latch**: every serve/populate path gates on
  `cacheable`; the latch is set ONLY by a successful Twalkgetattr. A
  future POUNCE-speaking-but-streaming server breaks the proxy — that
  needs an explicit capability (recorded v1.x). So does one whose
  `Tfsync` syncs only the fid's own file: the latch also stands for a
  whole-pool commit on any fid, which `dev9p_fsync` relies on to sync a
  loose cached open whose name is gone through its start (audit round 4
  of M15, F5).
- **Read the non-support latch as a CLASS.** Non-support is the majority and the
  default; a new native server is non-supporting unless someone implements
  the op. Any statement here that names one server is wrong the moment a
  second lands, and it fails in the dangerous direction — it invites the
  reader to conclude the unnamed sessions ARE cached.
- **A server errno crosses by value; only kernel-originated ones need
  registry names.** Adding a `T_E_*` for a code the server supplies is
  wasted ABI surface. The one exception is the EPERM/`-1` collision, and
  the three paths above answer it differently — check which one you are on
  before copying a neighbour's treatment.
- **Budget balance** on every path (grow, fallback, error, close, death).
- **The cape has one conversion point.** A new path that turns a
  `p9_attr` into a `t_stat` -- or hands the raw attr to userspace, as Loom's
  GETATTR does -- without the client's cape reports the host's ids on a
  caped session, and perm_check then denies the mounter its own files. The
  create gid and the wstat refusal are the other two consults; nothing
  identity-bearing may reach a caped server.

## Seams

- [[seam-wb-close-flush-slot]] — the close-flush is best-effort (void
  `Dev.close`); fsync is the reliable channel; the bounded/abortable
  close-flush error slot is the v1.x lift.
- [[seam-larder-loom-bypass]] — the Loom-async-mutation bypass of the
  wb/Larder invalidates (self-inflicted-reachable only, L1f-F2 wording);
  [[seam-larder-stale-child-attr]] — rename/unlink leave the moved
  file's OWN attr stale in metadata-only fields (L1f-F3);
  [[seam-larder-cacheable-proxy]] — the Twalkgetattr-success latch as
  the cacheability proxy. All three live on [[sub-kernel-larder]].

## Caveats

- The `.stat` slot still returns -1 (Plan 9 wire-stat never landed;
  `stat_native` is the real surface — its `valid`-mask gating on
  mode/uid/gid is fail-closed for the A-3 enforcement, and `qid_type` maps
  through `qid_type_p9_to_kernel` including the QTPOLL carry-through).
- `wb_patch_stat_size` patches fstat on the STAGING fd only (the Go
  buildid truncate-gate); path-stats via other fids see last-flushed state
  (close-to-open-legal).
- `t_stat_from_p9_attr` is shared by stat_native and walk_attrs — the two
  MUST report identical shapes or the pounce X-search diverges from the
  per-component loop. It takes the client because the identity cape is
  applied inside it.
- The dispatch of every partial-walk/create failure leaves `p->fid` at a
  defined owner (parent or swapped child) so the caller's clunk hits the
  right fid — the per-arm comments track which.

## Provenance

(generated from incoming `touched` edges — the shaping chunks:
P5-attach-dev, FS-alpha/beta/gamma, A-2a/A-3b, #37, #99, POUNCE P-3,
Larder L1c/L1d/L1e, wb F1, G1/G2/G3/G4, FID-LIFECYCLE cached-open,
Weft-6b-2/6b-3a, net-6b QTPOLL wiring, #98 NP-4c (the three readiness slots
replace `.poll`), #955, D44, task-#44, #80 the
name-op errno propagation, the V-4c-3 self-audit's class correction, and (L)
the Haul identity cape.)
[[chg-2026-08-16-dev9p-errno-class]] records the last two.

2026-10-29 (thread-model C7-0): `dev9p.h` declares two KERNEL_TESTS-only hooks, `dev9p_poll_test_under_lock` and `dev9p_poll_test_lock_held`, which `poll.dev9p_poll_lock_defers_its_walks` uses ([[sub-kernel-ninep-dev9p-poll]]).

## Tests

`kernel/test/test_dev9p.c` — 200 registered `dev9p.*` cases over a
canonical loopback responder: the vtable basics, errno propagation
(`dev9p.create_errno_propagates_eexist` — non-vacuous, asserts the -17
return AND the dentry drop), the Larder integration
(`dev9p.create_invalidates_reused_child`,
`dev9p.create_invalidates_reused_child_pages`,
`dev9p.page_cache_serve_and_gate`), the wb battery
(`dev9p.wb_{coalesce_one_twrite,overlay_read,flush_at_close,
fsync_flush_and_error,nonappend_writethrough,fstat_staged_size,cap_flush,
budget_fallback,populate_readback,append_chain,failed_flush,
writethrough_range}`), the G2 dirfid battery
(`dev9p.dirfid_{consume_and_recycle,perm_only_leaf_consume,
create_reuse_drop,rmdir_drop_and_no_stale_repark,suspect_not_reparked}`),
cached-open (`dev9p.cached_open_*`), the remote-readiness battery (ten
`dev9p.poll_*` cases on a message-queue loopback with a scripted server and
the live poll kthread: `regular_file_always_ready`,
`snapshot_answers_at_zero_timeout` (the #98 closure),
`local_and_remote_both_reported`, `snapshot_shortage_is_resent`,
`unanswered_snapshot_fails_safe`, `arm_wakes_the_parked_poller`,
`retry_timer_is_a_wake`, `widen_keeps_the_old_arm_until_replaced`,
`cancel_at_close`, `gc_flushes_with_the_unlink`; each seen red with its rule
removed; the harness is detailed in [[sub-kernel-ninep-dev9p-poll]]),
the prw wire-offset capture (`dev9p.prw_wire_offset_and_cursor`), and the
identity cape: `dev9p.cape` (an uncaped control whose chown reaches the
wire; the caped stat with the mode kept; a server that omits the trio;
chown, chgrp and chmod+chgrp refused with no Tsetattr; chmod passing; the
`walk_attrs` records; the Tlcreate/Tmkdir gid, `P9_NOGID` caped and the
caller's uncaped) plus the two DMSRV* create refusals that sit on this
fixture, `dev9p.path_create_refuses_dmsrvcape` and
`dev9p.walk_create_refuses_dmsrv_bits` (the fd create through
`sys_walk_create_kname_for_proc`, a plain create as its control).
Boot-level: every boot exercises the full stack against live Stratum; the
go-build oracle is the standing stress ([[gate-smp]] the SMP witness).

### Append, directory errors, and close diagnostics

Open and create forward `SYS_WALK_OPEN_OAPPEND` as the 9P/Linux `02000` flag.
The server chooses EOF for each write; the kernel's tracked cursor is advisory.
Append descriptors are excluded from write-behind at both eligibility sites:
a client-side staging offset cannot safely predict concurrent server appends.
Writes therefore pass through instead of installing own-pages at guessed offsets.

Readdir propagates the translated wire errno, preserving interrupted reads as
EINTR rather than fabricating EPERM from a flat -1. Readdir failure does not
mark the fid suspect: its cursor is supplied again from the unchanged offset.
A clunk the closer cannot take (a test attach with no session owner, or a
node that cannot be allocated) on a session that still holds the fid prints
`9p: close: clunk of fid N refused rc R`; close still releases local
ownership. Since 2026-09-29 the line means a real leak, and `tools/test.sh`
fails on it. Before, it also printed for dead sessions, whose fids leak
nothing, and for every dying close, so it could not tell the two apart.
