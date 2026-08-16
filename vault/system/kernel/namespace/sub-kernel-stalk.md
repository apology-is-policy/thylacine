---
id: sub-kernel-stalk
type: sub
title: "stalk — the per-Proc pathname resolver"
parent: moc-kernel-namespace
code: ["kernel/stalk.c", "kernel/include/thylacine/stalk.h"]
audit: hard
guarded-by: [inv-i28, inv-i33]
validated-by: [gate-smp]
locks: []
hazards: []
abis: []
design: ["docs/STALK-DESIGN.md", "docs/POUNCE-DESIGN.md", "docs/FID-LIFECYCLE-DESIGN.md"]
created: 2026-08-01
updated: 2026-08-16
---
## Purpose

`stalk` is the multi-component pathname resolver — Plan 9 `namec`, renamed
for the bestiary (the predator **stalks** its **quarry** along a **trail**
through the per-Proc namespace). One engine resolves every absolute path in
the OS: `SYS_OPEN = 65` and `SYS_STAT = 88`, exec-from-namespace (#58 —
every spawn), `SYS_CHDIR`'s target check, and `SYS_MOUNT`/`SYS_UNMOUNT`'s
mount-point naming. It generalizes the audited single-hop
`sys_walk_open_handler` clone→walk→clunk lifetime to N hops, adds Plan 9
`domount` mount crossing keyed by the full `(dc, devno, qid.path)` Spoor
identity, and since POUNCE batches runs of components through one fused
walk+getattr RPC with the per-component checks preserved kernel-side.

## Contract

```c
#define STALK_WALK  0   // resolve only (O_PATH / navigation base); quarry crossed
#define STALK_OPEN  1   // resolve + Dev.open(quarry, omode); quarry crossed
#define STALK_MOUNT 2   // mount point's OWN identity (final NOT crossed); no open
#define STALK_STAT  3   // metadata only; final run may be the no-fid walk-QUERY
#define STALK_MAX_DEPTH 40

struct Spoor *stalk    (p, start, path, pathlen, amode, omode);
struct Spoor *stalk_err(p, start, path, pathlen, amode, omode, int *errp);
int           stalk_stat(p, start, path, pathlen, struct t_stat *out, int *errp);
int           stalk_cross_mounts(p, probe, struct Spoor **out);
```

- `start` is **BORROWED** — stalk never refs or clunks it. Since #844 every
  syscall caller holds a real ref across the call (`sys_lookup_spoor`
  transfers one; `FROM_ROOT` uses `territory_root_ref`, the RW-4 SA-F1
  atomic read+ref), retiring the stalk-1 F3 N-hop TOCTOU
  ([[fnd-stalk1-r1-f3]]).
- `path` is `pathlen` bytes, NUL-free (the caller copied it from user space
  and rejected embedded NUL). Separator runs collapse; `.` is a no-op;
  `..` pops the trail, contained at `start`; each real component is
  ≤ `SYS_WALK_OPEN_NAME_MAX`.
- Returns the quarry (`ref == 1`, opened iff `STALK_OPEN`) or NULL.
  `stalk_err` writes the cause to `*errp` as a POSITIVE `T_E_*` code —
  `T_E_NOENT` / `T_E_ACCES` / `T_E_INVAL` / propagated / `T_E_IO` default —
  and **never `T_E_PERM`** (== 1, which collides with the generic `-1`
  sentinel; `err_code` collapses `-1` to `T_E_IO` for exactly this reason).
  The ER-1 keystone: `SYS_OPEN` returns `-*errp`, so a missing path is
  `-T_E_NOENT` (Go `os.IsNotExist`) instead of a bare `-1` (which Go's
  Linux-shaped decode renders EPERM).
- The `amode` guard is **fail-closed**: anything outside
  {WALK, OPEN, MOUNT, STAT} returns NULL at entry (stalk-1 F1). A new amode
  MUST be added to the guard AND given its final-hop dispatch arm.
- `stalk_cross_mounts` is public (since #957) so the single-hop
  `SYS_WALK_OPEN` crosses identically: at the SOURCE (before X-search +
  walk) and the RESULT (before open). `probe` is never consumed; `*out`
  is owned when non-NULL; `-1` means "is a mount point but the cross
  failed" — the caller fails the walk.

## Mechanism

### The per-component loop

For each tokenized component: `.` continues; `..` does
`spoor_clunk(trail[--depth])` — at `depth == 0` a hard no-op, so resolution
can never escape above `start` (the chroot/pivot boundary, I-28). A real
component: X-search the parent (on a `perm_enforced` Dev,
`spoor_stat_native` + `perm_check(p, &st, PERM_X)` — fail-closed if the Dev
cannot vouch); reject trail-full BEFORE the push; `nc = spoor_clone(parent)`;
NUL-terminate into `namebuf`; `parent->dev->walk(parent, nc, {name}, 1)`;
on success `spoor_path_extend(nc, ...)` (I-33, never load-bearing) and push
`nc` — now owning its own fid — onto the trail.

**The lifetime discipline** (the audit-critical part): `spoor_clone` copies
`aux` SHALLOWLY — for dev9p a SHARED fid — until a successful walk replaces
it. Three failure shapes:

| shape | cleanup | why |
|---|---|---|
| walk returns NULL | `nc->aux = NULL; spoor_unref(nc)` | nc still shares the parent's fid — detach, never clunk |
| `w->spoor != nc` (reuse-nc violated) | free w; detach + unref | same shared-aux state; defense-in-depth |
| `w->nqid != 1` (devramfs miss) | free w; `spoor_clunk(nc)` | nc was reused with a non-heap aux — clunk-safe |

On return `stalk_unwind` clunks every trail ancestor exactly once; the
quarry is popped first so it is never double-clunked; the `fail:` path
clunks the quarry (if popped) then unwinds. Zero real components (`"/"`,
`"."`, a netted-out `..` run) mint the quarry via `clone_walk_zero(start)`
— Plan 9 cclone: clone + 0-element walk so dev9p allocates a fresh fid
(9P forbids Twalk from an opened fid). `clone_walk_zero` validates
`w->nqid == 0` with the same rigor as the main loop (RW-4 R1-F1,
[[fnd-rw4-rev1-f1]]).

### The POSIX shape gates, and the one ordering rule behind all of them

Five commits added four gates, and they read as one design rather than four
patches because a single rule governs every one: **type before permission,
always.**

| gate | asks | subject |
|---|---|---|
| through-a-file | is the parent I am walking through a directory? | the **crossed** parent |
| dot-out-of-a-file | is the position `.`/`..` resolve in a directory? | the **uncrossed** tip |
| trailing slash | is the quarry a directory, as the path asserted? | the **crossed** quarry |
| dot-search | may the caller search the directory the dot resolves in? | the **uncrossed** tip |

**The ordering is argued, not conventional.** The x bit on a non-directory says
nothing about whether it can be traversed, so gating on permission first would
answer `EACCES` for a path that can never resolve *as written*. A `0000`
regular file is `ENOTDIR`, and an unreadable `file/` is `ENOTDIR` rather than
`EACCES`. Every one of the four sits before the permission check it is adjacent
to. For `..` there is a second ordering: the search check runs **before the
pop**, while the directory being popped out of is still the subject.

**Two gates read the same field and disagree about crossing, deliberately.**
The dot gates read `qid.type` **uncrossed**; the trailing-slash gate reads it
**crossed**. That is not an inconsistency to tidy — they ask different
questions of one field. `.` and `..` are about **where resolution stands**, so
`/mnt/.` must equal `/mnt` and crossing would move it; a trailing slash is
about **what the quarry is**, so a directory mounted over a file legitimately
makes `file/` resolve. Unifying them would silently break whichever one lost.

The dot gates also had to be written **separately from** the through-a-file
gate, for a structural reason worth keeping: `.` and `..` are handled by stalk
itself and **never reach `Dev.walk`**, so a gate sitting on the real-component
arm cannot see them. `a/b/..` popped back to `a` and `a/b/.` handed back `b`
while `/bin/ls/foo` was correctly refused.

Containment is **strengthened, never touched**. At depth 0 the subject of a dot
gate is `start` itself, and answering `ENOTDIR` there is strictly more
restrictive than the old no-op — the gate can only turn a success into a
failure, never move a pop further up. So no path that previously stopped at
`start` can now pass it ([[inv-i28]]).

### A gate binds only what it SEES

All four gates live here, so they bound only the paths that reach here — and
the **cwd join did not let the commonest form in a shell reach here**. It
resolved `.` and `..` and dropped a trailing separator *before* calling stalk,
so every gate above was bypassed for every relative path.

Seven consequences were **measured on the pre-fix tree**, from a working
directory holding a regular file `f`: `open("f/..")`, `open("f/.")`,
`open("f/")` all returned working descriptors; `stat("f/")` and `stat("f/..")`
succeeded; and the two that make it a resolution bug rather than a conformance
nit — **`open("nope/..")` returned a working fd**, because a lexical `..` pops
a component without proving it exists, and **`chdir("f/..")` succeeded**,
having run its directory check against the parent it had already massaged the
path into. Checking the wrong object entirely.

**The fix was a unification, not a fifth gate.** Joining and canonicalising had
been one function; they were separated, leaving exactly one production caller
of the canonicalising half. That is the same move [[sub-libthyla-rs]] made at
the same time and for the same reason: **three layers independently normalised
paths — this join, the ported libc's splitter, the native runtime's — and each
was wrong differently.**

**Correction, same day, from the third layer** ([[chg-2026-08-16-pouch-trailing-slash]]):
this note first stated the lesson as "all three were repaired by DELETING the
normalisation". Two of them were. The **pouch splitter could not be** —
splitting `(parent, leaf)` is structurally required, because the kernel's
mutation primitives take a parent fd and a leaf name rather than a path, so the
separator genuinely has to come off there.

The rule that covers all three is not *never transform*, it is **never
DECIDE**: a layer that must transform a path may strip, but it may not
adjudicate — it reports what the original asserted and re-asks the authority.
Pouch does exactly that, spending one extra `SYS_STAT` of the un-stripped path
so **this** resolver's audited gate answers. For the two layers with nothing to
transform, "do not decide" simply collapses to "delete the cleaning", which is
why the collapsed form looked like the general one until a third instance
disagreed.

### Mount crossing (Plan 9 domount, stalk-2)

Crossing is **on descent**: a trail Spoor is crossed the moment it is used
as a directory to walk THROUGH (replaced in place by the mounted root,
which is then X-checked — the MOUNTED root's perms govern, not the shadowed
point's); the base is crossed before the loop (the owned crossed clone
becomes `trail[0]` since `start` is borrowed); the quarry is crossed at the
end — EXCEPT under `STALK_MOUNT`, so `SYS_MOUNT`'s MREPL re-keys the same
underlying point. `stalk_cross_mounts` loops a mount-over-mount chain to
the leaf, bounded by `PGRP_MAX_MOUNTS` (20) — I-3 acyclicity is ENFORCED at
`mount()` time (`would_create_mount_cycle`, the stalk-2 F1 fix,
[[fnd-stalk2-r1-f1]]); the bound is a defensive backstop. `mount_lookup`
returns a **ref-held** source under the Territory `ns_lock` (RW-4 SA-F1,
[[fnd-rw4-sa-f1]]) so a concurrent unmount cannot free it mid-cross; the
crossed clone takes the MOUNT-POINT's namespace name via
`spoor_path_transplant` (I-33, cosmetic).

### The POUNCE (fused component batching)

When the path has no `..` (`path_has_dotdot` — a pop into a compressed run
has no Spoor to land on), the Dev has `walk_attrs`, and logical depth
remains, the resolver gathers the maximal run of real components (budget =
`min(STALK_MAX_DEPTH - logical_depth, DEV_WALK_ATTRS_MAX = 16)`; a `.` /
`..` / over-long token ends the run and is left for the outer loop) and
issues ONE `Dev.walk_attrs` call — on dev9p one `Twalkgetattr` RPC
returning each walked component's full attributes. Then:

- **Base X-check** — one `stat_native` per run, or the previous run's
  `carried` leaf record (both server-fresh samples of THIS resolution; the
  same unsynchronized-snapshot TOCTOU envelope as the per-component loop).
- **The sentinel latch**: `DEV_WALK_ATTRS_UNSUPPORTED` (a STATIC object
  that must NEVER reach `walkqid_free`) means this session's server lacks
  the extension (dev9p latches the first ENOSYS). Release the untouched
  clone, rewind to `ends[0]`, `goto per_component` — the whole resolution
  degrades to the per-component loop with identical results.
- **The shape contract** (P-3 SA-11): a full BIND walk must have
  transitioned `nc` (`w->spoor == nc`); partial and query forms must carry
  `w->spoor == NULL`. Violations fail closed — pushing an untransitioned
  `nc` would later clunk the parent's SHARED fid.
- **The fail-ordering post-scan, LEFT-TO-RIGHT** (the audit's #1 target):
  component j's X-gate is `sts[j-1]` (the base pre-batch); an X-denial at
  j MASKS everything past j INCLUDING a partial walk's miss —
  `T_E_ACCES`, never `T_E_NOENT` — so a caller cannot existence-probe
  under a forbidden directory. The mount test of j precedes j's own
  X-check, byte-identical in order to the per-component loop.
- **Mount-mid-run split**: the batch may have walked PAST a mount point
  server-side (everything deeper is the UNDERLYING tree's answer — junk).
  On a `mount_is_point_id` hit: discard the batch, re-walk the validated
  prefix `[0..split_at]` to materialize the mount point (one extra RPC),
  push it, resume after that component — the next iteration's
  cross-on-descent does today's exact cross + X-check of the mounted
  root. A full BIND walk's leaf is exempt (the quarry/descent machinery
  owns it). A prefix re-walk that no longer matches (racing
  unlink/rename) fails `T_E_NOENT`, as a sequential racer would.
- **Partial, no split**: the miss at component k is real; report NOENT
  only after `sts[k-1]` passes the X-check (the invariant's last
  obligation).
- **Carried attrs**: a run's leaf record seeds the next run's base X-check
  and the `STALK_OPEN` final R/W check (open = walkgetattr + lopen = 2
  RPCs); invalidated on EVERY tip-changing event — cross-on-descent,
  quarry cross, `..` pop, split push, any per-component hop.
- **`logical_depth`** counts REAL components consumed (runs compress the
  trail array), enforcing the `STALK_MAX_DEPTH` surface `depth` no longer
  measures; monotone because `pounce_ok` excludes `..`. The per-component
  arm enforces BOTH caps when pouncing is possible, so a
  `walk_attrs`-less tail cannot resolve where the pure loop would INVAL.

### STALK_STAT — the walk-query (1-RPC stat)

The FINAL run of a `stalk_stat` resolution passes `nc == NULL` — dev9p
sends `newfid = P9_NOFID`, nothing binds on either end, and the leaf's
fused record is the answer: no handle, Spoor, or fid ever exists.
`stat_out->devno` is stamped from the run parent (#100 — the fused leaf
inherits the session identity `Chan.dev`, matching `spoor_stat_native`).
The success exit never touches `*errp`. Fallbacks (a `walk_attrs`-less
final Dev, a mount-point leaf — POSIX: stat reports the MOUNTED root — or
a zero-component path) materialize a quarry, `spoor_stat_native` it, and
clunk it: the old O_PATH+fstat shape minus the handle-table round trip.

### The FID-LIFECYCLE cached-open arm

The FINAL run of a plain read-only `STALK_OPEN` (`omode == 0` exactly —
OTRUNC / write / OEXEC / O_PATH never take it) on a Dev with `open_cached`
may be served FIDLESS: the Dev revalidates server-fresh (a forced-wire
query walk; on a B1 LOOSE client the records may be cache-served),
snapshots the fully-page-cached content, and returns an OPENED Spoor plus
the walk's records in `sts`. **The resolver keeps every permission
decision** — the same left-to-right X post-scan, then the final R/W gate,
identical fail ordering. ANY mount hit in the run (leaf included — this
path has no quarry-cross) discards the cached open (`spoor_clunk` is
wire-free) and falls back to the normal walk, whose split/cross machinery
owns the crossing. A NULL return reveals nothing — the observable outcome
is the normal path's.

### The final hop

`STALK_WALK` / `STALK_MOUNT` / `STALK_STAT` return the quarry unopened
(navigation / create / chroot / mount / metadata bases are exempt from the
R/W gate — the single-hop O_PATH carve-out; POSIX stat authority is the
path X-search only). `STALK_OPEN` runs `perm_check(perm_want_for_omode)`
(consuming `carried` when valid — the quarry's own walk-fused record) then
`Dev.open`. Open returns EITHER the same Spoor opened in place (dev9p /
devramfs) OR a DIFFERENT owned Spoor that REPLACES the quarry (devsrv
open=connect consumes the `/srv/<name>` node and returns the connection
endpoint — stalk-3b-β). The adoption arm clunks the spent quarry, adopts
the replacement, and transplants the walked name onto it (#66a F2,
[[fnd-66a-r1-f2]] — fd2path must report `/srv/corvus`, not the endpoint's
born-"/" name).

## Data structures

None persistent. Per call: `struct Spoor *trail[STALK_MAX_DEPTH]` (40
pointers), `char namebuf[SYS_WALK_OPEN_NAME_MAX + 1]` per component, and
the POUNCE run arrays (`names`/`lens`/`ends[16]` + `struct t_stat sts[16]`
≈ 1.6 KiB) — all on the 16 KiB kernel stack. The mount-table entry
(`struct PgrpMount`: source + `mp_path` + the `(mp_dc, mp_devno,
mp_qid_path)` key, 40 B since #66b) and `Spoor.devno` (Plan 9 `Chan.dev`,
minted per attach session by `spoor_next_devno`) belong to the territory
and spoor dossiers; the key's `devno` axis exists because every dev9p
session shares `dc = '9'` and every attach root has `qid.path == 0` — two
concurrent sessions' mount points are indistinguishable without it.

## Concurrency

stalk itself takes NO locks and keeps all resolution state on the stack.
Its two shared-state touchpoints:

- **The mount table** — read via `mount_lookup` (ref-held return) and
  `mount_is_point_id` (membership only), both under the per-Territory
  `ns_lock` (RW-4 SA-F1; the lock lives territory-side and is never held
  across `clone_walk_zero` or a walk). Any new `mounts[]`/`root_spoor`
  reader MUST go through `mount_lookup`/`territory_root_ref` — a bare
  lock-free read reopens the multi-thread UAF the P6 lift exposed.
- **The borrowed `start`** — pinned by the CALLER's ref since #844
  (`sys_lookup_spoor` transfers one; `territory_root_ref` for FROM_ROOT).
  stalk's borrow is safe exactly because the callers hold it.

The X-search is open-time-only: perms are snapshotted at resolve time;
`SYS_READ`/`SYS_WRITE` re-check only the handle RIGHT (the A-3 model).

## Invariants enforced

- **[[inv-i28]]** — containment (`..` pops the in-call trail, clamped at
  `start`; the cwd join is convenience, not authority — stalk re-clamps),
  the per-component X-search on every hop including crossed roots, the
  POUNCE fail-ordering equivalence, and mount-cross by full Spoor
  identity.
- **[[inv-i33]]** — the resolver is WRITE-ONLY to `Spoor.path`
  (`spoor_path_extend` / `spoor_path_transplant` at the walk/cross/adopt
  hooks); no resolution or permission decision reads it; a path-alloc
  failure leaves it NULL and the walk still succeeds.
- **I-3** (consumed) — cycle-freedom is enforced at `mount()`
  (`would_create_mount_cycle`); the cross loop's bound is a backstop, not
  the proof.
- **I-22** (composed) — no `principal_id` bypasses `perm_check`; only
  `CAP_HOSTOWNER`/`CAP_DAC_OVERRIDE` do, inside `perm_check` itself.

## Error paths

`stalk_err` writes exactly one cause per NULL return: `T_E_NOENT` (walk
miss — dev9p NULL, devramfs `nqid != 1`, partial-run miss, split re-walk
race), `T_E_ACCES` (any X-search or final R/W `perm_check` denial),
`T_E_INVAL` (unknown amode, NULL start/path, over-long component, trail /
logical-depth overflow, Dev without walk/open), a propagated stat errno
via `err_code` (magnitude of `[-4095,-2]`; `-1` and out-of-range collapse
to `T_E_IO` — never errno 1), or `T_E_IO` (OOM, cross failure, transport).
`stalk_stat` additionally returns `-1` with `*errp` on a NULL `out`.
The walk-NULL arm deliberately reports NOENT for the rare deep failures
(session-dead, OOM) too: the kernel's own `perm_check` is the ACCES
authority, so a walk NULL is never a masked permission denial (ER-2 — a
walk-vtable errno out-param — is the in-flight refinement; see Seams).

## Performance

Pre-POUNCE: one `Tgetattr` (X-search) + one `Twalk` per component on
dev9p. Post-POUNCE: one base `Tgetattr` + one `Twalkgetattr` per ≤16-run
(the 1:1 residual signature — the remaining Tgetattr IS the per-stalk base
X-check), with `carried` eliding the next run's base and the open's R/W
stat. The P-4 close: gofmt-91-pkg cold 21.8/22.1 s (−19–21% vs pre-POUNCE
fresh-pool), warm 8.3/8.2 s; the RT recount collapsed Twalk 23.3k → 10
(warm window) and the metadata trio ~75k → ~17k (cold); go4c build2-warm
4192 → 3006 ms (−28%) on the controlled pair. The Phase-2 attr cache was
DEFERRED on the recount (the fd-fstat class it targeted collapsed into
the fused op); the base-Spoor attr memo is the recorded seam. The
cached-open arm makes a fully-cached re-open 0-RT; devramfs and fixtures
resolve locally.

## Prosecution

Standing obligations for any change (the ARCH §25.4 POUNCE row is the
authoritative audit-trigger copy):

- **The fail-ordering invariant**: an X-denial at component k masks
  everything past k including a deeper miss — ACCES never NOENT. Pinned
  by `stalk.pounce_acces_masks_noent`.
- **Pounce ≡ per-component parity** on every observable (`.`/`..` runs,
  mount-mid-run split, partial walk, both depth caps) — the whole stalk
  battery runs THROUGH the pounce (the fixture implements `walk_attrs`)
  plus the explicit A/B `stalk.pounce_parity_nowa`.
- **The three-shape lifetime discipline** (detach-vs-clunk on every
  failure branch; the sentinel intercepted before `walkqid_free` at BOTH
  call sites; the split re-walk's four exit shapes).
- **The amode guard**: a new amode must extend the entry guard AND land a
  final-hop arm — a missed arm must fail closed, not skip an open / a
  cross / a parent check (stalk-1 F1).
- **Carried-attrs invalidation completeness**: set only at full-push;
  cleared at every tip change. A stale record consumed after a cross is a
  wrong-Spoor permission check.
- **The cached-open arm keeps resolver-side checks**: any change must
  preserve the post-scan + final R/W gate + the ANY-mount discard
  (crossing is never the Dev's decision).
- **`..` containment**: the pop guard (`depth > 0`) and the borrowed-start
  no-op are I-28's floor; the #957 single-hop crosses must stay
  symmetric with stalk's base/quarry crosses.
- **Type before permission, at every gate.** A non-directory answers
  `ENOTDIR`, never `EACCES` — the x bit on a non-directory says nothing
  about traversability, so a permission-first order answers the wrong
  question about a path that can never resolve as written. And `..`'s
  search check runs **before the pop**, while the directory being popped
  out of is still the subject.
- **The dot gates read the tip UNCROSSED; the trailing-slash gate reads the
  quarry CROSSED.** They ask different questions of one field — *where
  resolution stands* versus *what the quarry is* — and unifying them
  silently breaks whichever loses. `/mnt/.` must equal `/mnt`; a directory
  mounted over a file must make `file/` legal.
- **A dot arm needs its own copy of every gate.** `.` and `..` never reach
  `Dev.walk`, so anything added to the real-component arm does not cover
  them. Both tokens, not just `..`: two separate tasks named only `..` and
  the measurement showed both behaved identically in every row.
- **No caller may DECIDE before calling stalk.** Every gate here binds only
  what stalk sees, so a caller that resolves dots or strips a trailing
  separator and then acts on its own reading bypasses all of them for
  whatever path form it handles. This is the single defect class that has
  recurred at three independent layers. Joining and canonicalising stay
  separate operations and only stalk does the second — but a caller that
  *must* transform (the pouch splitter, whose kernel primitives take a
  parent fd and a leaf, not a path) is not thereby exempt: it strips, carries
  the assertion forward explicitly, and spends a `SYS_STAT` so **this**
  resolver adjudicates. Transforming is sometimes forced; adjudicating never
  is.
- **`STALK_STAT` and the cached-open arm are separate success exits and
  each needs the quarry-shaped gates repeated.** The trailing-slash check
  lives at three sites for exactly this reason — two of the three exits
  never reach the ordinary `return quarry`, and they read the leaf's fused
  record instead.

## Seams

- [[seam-372-latched-double-xcheck]] — on a `wga_unsupported`-latched
  session the pounce block pays its base X-check Tgetattr before the
  sentinel returns, then `per_component` re-stats the same parent (2×
  Tgetattr per component on `/net`-class fallbacks; idempotent).
- [[seam-posix-pathname-form-gates]] — the POSIX pathname-form family
  (resolution through a file reports NOENT not ENOTDIR; `.`/`..` out of a
  file resolve; a trailing slash is dropped; name-op errno loss). The fix
  family (#79–#84 + ER-2) is landed on the unmerged vivarium branch
  (`aux-2`, commits `6790c125..a0d146a7`, 2026-07-29) and reconciles into
  this lineage at its merge.
- [[seam-fid-monotonic-reclaim]] — a failed clone-walk / partial walk
  abandons a monotonic fid NUMBER (stalk-2 F2; RW-4 R3-F2 re-registered
  the ~2^32 burn bound; G2's dirfid cache recycles walk-fresh DIR fids
  but the general allocator never reclaims).
- The base-Spoor attr memo (the `carried` idea extended across stalks) —
  recorded at the P-4 close, NOT built: it needs an invalidation story
  and the data said the next lever was elsewhere.
- `SYS_WALK_OPEN` retirement — single-component callers are unchanged;
  migrating them to `SYS_OPEN` is a deferred cleanup.
- The handle-based dot + cross-mount `..` fidelity (Plan 9 `Chan->mh`
  back-pointers) — v1.x, lands with symlinks; v1.0's in-call trail
  containment is the audited invariant ([[fnd-stalk2-r1-f3]]).

## Caveats

- **`..` is contained at `start`, not the dirfd's real parent** — for a
  relative resolve from a dirfd, `..` at the base is a no-op.
  Over-restrictive vs POSIX `openat` (safe: it cannot escape).
- **The cwd join is a layer above** (LS-4, territory-owned):
  `territory_resolve_cwd` joins + lexically cleans `dot_path` with a
  relative path BEFORE stalk runs (only for the FROM_ROOT sentinel), so
  I-28 gains no new mechanism — but the join is combined-length-bounded
  (`SYS_OPEN_PATH_MAX` for dot + input together, LS-4 audit F1), cleans
  `.`/`..`/trailing-`/` lexically (why the in-flight #83 parity task
  exists), and the name-based `dot_path` goes stale if an ancestor is
  renamed. The handle-based dot is the v1.x upgrade. Mechanism detail
  pends the territory sweep's dossier.
- **O_PATH handles are navigation-only** (#81, June): `STALK_WALK` skips
  the final R/W check AND `Dev.open`, so the walk-open sites set
  `CWALKONLY` on the returned Spoor and `sys_read`/`sys_write`/
  `sys_readdir` reject it — else the perm_check-exempt O_PATH open is a
  read-bypass (it once leaked the 0400 `/system.key`). Never inherited by
  `spoor_clone` (the create-from-O_PATH-base pattern must do its own
  I/O). Enforcement lives syscall-side ([[adt-81-r1]] carries the round).
- **Superseded doc claims** (`docs/reference/104-stalk.md`, now a stub,
  carried these stale): stalk-3 is LANDED (see [[sub-kernel-devsrv]]);
  the "no batching at v1.0" performance note predates POUNCE; the
  "borrowed-start TOCTOU deferred" caveat predates #844 (closed); the
  `#848` pivot-vs-walk note predates RW-4's `ns_lock` (closed).
  `PGRP_MAX_MOUNTS` is 20 today.

## Provenance

(generated — incoming `touched` backlinks, newest first; never
hand-written)
