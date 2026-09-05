---
id: sub-kernel-ninep-dev9p
type: sub
title: "dev9p — the 9P Dev (walk/IO/mutation + Larder integration + write-behind + cached-open + weft arms)"
parent: moc-kernel-ninep
code: [kernel/dev9p.c, kernel/include/thylacine/dev9p.h]
audit: hard
guarded-by: [inv-i38]
validated-by: [spec-fs-cache, gate-smp]
locks: [lock-dev9p-wb-priv]
hazards: [haz-shared-stream-desync]
abis: []
design: [docs/LARDER-DESIGN.md, docs/FID-LIFECYCLE-DESIGN.md, docs/POUNCE-DESIGN.md]
created: 2026-07-31
updated: 2026-08-16
---
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
`write`, `poll` (→ [[sub-kernel-ninep-dev9p-poll]]), `fsync`, `readdir`,
`rename`, `unlink`, `stat_native`, `wstat_native`, plus `seekable = true`
(positioned I/O honors the byte offset — the #37 pread/pwrite gate) and
`perm_enforced = true` (A-3b: kernel rwx enforcement ACTIVE on dev9p; the
in-struct comment records the reconciliation that made the flip sound).
Legacy stubs: `.stat`/`.wstat` (Plan 9 wire-stat — native slots supersede),
`.remove` (wrong shape; `.unlink` replaces), `.bread`/`.bwrite`, `.attach`
(dev9p Spoors are minted via `dev9p_attach_client`, not a spec string).

Exports beyond the vtable: `dev9p_client_fid` (the Loom I-30 submit pin
resolve), `dev9p_weft_try_write`/`_read` (the zero-copy data-drive arms),
`dev9p_priv_of`, `dev9p_create_errno` (#99), the cached-open/write-behind
budget diagnostics + test bias.

## Mechanism

### The priv and its lifetimes

`struct dev9p_priv` (kmalloc KP_ZERO, magic "D9PP", clobbered on free):
`client` + `fid` + `fid_owned`, `attached_owner` (one `p9_attached_ref` per
priv — the F236 discipline; walks inherit the parent's), `create_errno`
(#99 transient), `fid_gen`/`fid_suspect` (G2), `poll` (lazily-allocated
readiness state, refcounted independently — #294), `weft` (the lazily-bound
flow ring; CAS-installed, ACQUIRE-read), the cached-open triple
(`cached_open`/`co_buf`/`co_size`/`co_stat`), and the write-behind block
under `wb_lock` ([[lock-dev9p-wb-priv]]).

**`dev9p_close` teardown order** (each step's position is load-bearing):
1. `dev9p_poll_priv_release` — cancel any outstanding readiness op BEFORE
   the fid clunk, so the netd `ready`-fd Tclunk lands deterministically
   (#294).
2. Weft release: ACQUIRE-load + NULL (RELEASE) → `weft_reap_unregister`
   (leave the G-3 reaper registry FIRST — after it returns no sweep holds
   the binding) → the G-2 weave clunk-unmap (pid-matched, VMA-identity
   guarded) → `weft_binding_release`.
3. Cached-open: free `co_buf` + uncharge the global budget.
4. Write-behind: flush the staged run (the fid must still be live for the
   flush Twrites) — best-effort; a failure latches-and-drops (`Dev.close`
   is void at v1.0, [[seam-wb-close-flush-slot]]) — then free the buffer +
   uncharge.
5. `fid_owned`: **G2 donate or async clunk.** An unopened (COPEN clear)
   DIRECTORY fid on a cacheable client, not `fid_suspect`, and not staled
   (`larder_qid_staled_since` over the G4 ring since `fid_gen`) PARKS in
   the client's dir-fid cache instead of clunking; everything else takes
   `p9_client_clunk_async` (fire-and-forget; the fid unbinds at send and
   its number is never reused; the ownerless Rclunk drains via a later
   op's reader). Dedup/evict victims from the park are async-clunked
   outside the table lock.
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
  `DEV_WALK_ATTRS_UNSUPPORTED` sentinel; first `-T_E_NOSYS` reply sets it.
  **Non-support is the MAJORITY case and must be read as a class, not as a
  named example.** Stratum is the only v1.0 server that implements the op;
  every native userspace one (netd, ptyfs, tapestryd, the VIVARIUM diorama)
  falls through its unknown-op arm to ENOSYS, and **the list only grows** —
  each new native server joins the non-supporting side by default, since
  implementing the op is the deliberate act. The resolver then uses the
  per-component loop for the session's lifetime, RPC-free.
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
  diorama, and each future native server. **The ENOSYS latch above is
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

`dev9p_create` (file: Tlcreate creates-and-opens; dir: Tmkdir → walk to
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
after the run is gone) — then Tfsync with real-errno propagation.

`dev9p_wstat_native` (Tsetattr; `T_WSTAT_* == P9_SETATTR_*` pinned by four
`_Static_assert`s): cached-open fails LOUD ([[seam-co-fidless-wstat]]);
write-behind: flush first (a truncate must land after the staged bytes)
then de-eligibilize (a size change destroys the append anchor); on success
attr invalidate (CRITICAL — the base X-check perm_checks the cached mode,
so the invalidate keeps the guest's own chmod window at zero) + whole-file
page invalidate when SIZE changed.

### rename, unlink, readdir

`dev9p_rename` (Trenameat; same-client gate — a renameat is within one
session) / `dev9p_unlink` (Tunlinkat; `SYS_UNLINK_REMOVEDIR ==
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
invalidate on BOTH arms; a failed/partial flush drops the whole file's
pages fail-safe; a FULL land **installs the run's pages as OWN** (G1a —
`larder_page_install_own`, the write-populate: the build's read-back of
just-written archives serves from cache; the `err==0` coupling is pinned by
the `fs_cache_buggy_populate_unflushed` counterexample cfg). An own page
serves without the cvers gate and the boundary page extends only an OWN
page ending exactly at the run start — sound under the loose single-writer
premise the wb already asserts.

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
  `create_errno` is handler-local by construction (a fresh clone-walk priv,
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
rename/unlink (#80). `dev9p_create`'s errno record + accessor (#99).
NULL/`-1` per the Dev convention elsewhere. `walk_attrs` returns the
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

The write path documents its collision as a known residual owed to the
rollout. The create accessor's window silently excludes it, so a
permission-denied create and a permission-denied unlink give the same
caller different answers to the same class of denial. Neither is a bug
today; both are the shape the remaining ER stages exist to converge.

**One comment on this surface asserts immunity it does not have.** The
native-stat arm claims the integrity invariant bounds the code into
`[-4095,-2]` so `-1` can never arrive, and claims both its named callers
propagate. The mapper's clamp refutes the first; the resolver propagates
but the stat syscall flattens to `-1`, refuting the second — and the
resolver's own converter **guards explicitly against the value the leaf says
cannot occur**, which is the strongest available evidence that it can.
[[seam-fstat-errno-flattened-above-the-leaf]].

## Performance

The read stack's serve precedence is the measured FS-perf ladder (POUNCE
→ Larder L1c/d/e → task-#44 EOF/alignment → G1/G2/G3/G4 → wb F1): warm
gofmt 1352→1147 ms at L1f; S3 wire reads 6.7k→3.5k at G1; the arc ledgers
live in the go-build measurement series. Hot-path allocations: one priv
kmalloc per walk; the attrs scratch on the walk_attrs RPC path (heap — 16
× ~152 B is too big for the 16 KiB kstack above stalk's own arrays).

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
  needs an explicit capability (recorded v1.x).
- **Read the ENOSYS latch as a CLASS.** Non-support is the majority and the
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

## Seams

- [[seam-wb-close-flush-slot]] — the close-flush is best-effort (void
  `Dev.close`); fsync is the reliable channel; the bounded/abortable
  close-flush error slot is the v1.x lift.
- [[seam-co-fidless-wstat]] — fchmod/fchown on a cached-open fd fails
  loud (no fid to Tsetattr; no v1.0 consumer does this).
- [[seam-larder-loom-bypass]] — the Loom-async-mutation bypass of the
  wb/Larder invalidates (self-inflicted-reachable only, L1f-F2 wording);
  [[seam-larder-stale-child-attr]] — rename/unlink leave the moved
  file's OWN attr stale in metadata-only fields (L1f-F3);
  [[seam-larder-cacheable-proxy]] — the Twalkgetattr-success latch as
  the cacheability proxy. All three live on [[sub-kernel-larder]].
- [[seam-fstat-errno-flattened-above-the-leaf]] — the native-stat arm's
  comment asserts an immunity the mapper does not grant and a propagation
  the stat syscall does not perform. Comment-only; the behaviour matches
  the rollout's own staging.

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
  per-component loop.
- The dispatch of every partial-walk/create failure leaves `p->fid` at a
  defined owner (parent or swapped child) so the caller's clunk hits the
  right fid — the per-arm comments track which.

## Provenance

(generated from incoming `touched` edges — the shaping chunks:
P5-attach-dev, FS-alpha/beta/gamma, A-2a/A-3b, #37, #99, POUNCE P-3,
Larder L1c/L1d/L1e, wb F1, G1/G2/G3/G4, FID-LIFECYCLE cached-open,
Weft-6b-2/6b-3a, net-6b QTPOLL wiring, #955, D44, task-#44, #80 the
name-op errno propagation, and the V-4c-3 self-audit's class correction.)
[[chg-2026-08-16-dev9p-errno-class]] records the last two.

## Tests

`kernel/test/test_dev9p.c` — ~56 registered `dev9p.*` cases over a
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
cached-open (`dev9p.cached_open_*`), the poll teardown
(`dev9p.poll_cancel_at_close`, `dev9p.poll_regular_file_always_ready`),
and the prw wire-offset capture (`dev9p.prw_wire_offset_and_cursor`).
Boot-level: every boot exercises the full stack against live Stratum; the
go-build oracle is the standing stress ([[gate-smp]] the SMP witness).
