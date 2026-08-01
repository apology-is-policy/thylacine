---
id: sub-kernel-territory
type: sub
title: "Territory — the per-Proc namespace (mount table, root, cwd)"
parent: moc-kernel-namespace
code: ["kernel/territory.c", "kernel/include/thylacine/territory.h"]
audit: hard
guarded-by: [inv-i1, inv-i3, inv-i33]
validated-by: [spec-territory, gate-smp]
locks: [lock-territory-ns-lock, lock-territory-dot-lock]
hazards: []
abis: []
design: ["docs/STALK-DESIGN.md", "docs/LIFE-SUPPORT.md"]
created: 2026-08-01
updated: 2026-08-01
---
## Purpose

The Territory is one Proc's namespace — Plan 9's `Pgrp`, renamed. It is
the STATE that [[sub-kernel-stalk]] resolves against: the mount table the
resolver crosses, the `root_spoor` that is I-28's containment floor, and
the `dot_path` cwd that relative paths join to. It is per-Proc,
refcounted, and deep-copied at `rfork` — which is the whole of
[[inv-i1]]: two Procs' namespaces are independent function values, so a
mount in one is unnameable in the other.

Everything a Proc can reach, it reaches through here. A confined Proc is
confined by what its Territory grafts, not by what it lacks permission to
read — visibility is the first wall.

## Contract

Four structures on `struct Territory`, three of them live:

| Field | What | Reached by |
|---|---|---|
| `mounts[20]` | `(dc, devno, qid.path) -> source Spoor` grafts | `mount_lookup` from [[sub-kernel-stalk]]; `SYS_MOUNT`/`SYS_UNMOUNT` |
| `root_spoor` | the resolution floor + FROM_ROOT walk base | `territory_root_ref`; `SYS_CHROOT`/`SYS_PIVOT_ROOT` |
| `dot_path` | the cwd string (`NULL` == `"/"`) | `SYS_CHDIR`/`SYS_GETCWD`; the `SYS_OPEN` relative join |
| `binds[8]` | Plan 9 path-to-path edges | **nothing — see Caveats** |

**The authority model is namespace-mediated, not capability-gated.** No
capability guards `SYS_MOUNT`, `SYS_UNMOUNT`, `SYS_CHROOT`,
`SYS_PIVOT_ROOT`, or `SYS_CHDIR`. The gates are exactly two, and both are
already-held authority:

1. **`RIGHT_READ` on the source handle** (`sys_lookup_spoor(..., RIGHT_READ)`
   in mount / chroot / pivot). A mount source you cannot read is
   structurally inert; `RIGHT_WRITE` is deliberately NOT required —
   pivot binds a name, it creates no edge ([[fnd-16c-r1-f10]]).
2. **Reaching the mount point at all** — `sys_resolve_mountpoint` stalks
   the path, so [[inv-i28]]'s per-component X-search is the mount-point
   gate. There is NO write permission check on the directory mounted
   over.

That is Plan 9-correct and deliberately unlike Linux: your namespace is
yours, so mounting over a directory you can merely *search* changes only
your own view. It is also the container keystone — a confined Proc
composes its own namespace with no privilege at all.

Return codes (the C API; **the SVC layer collapses every failure to
`-1`**, so the distinct codes serve the kernel callers and tests only):

- `bind` — `0` / `-1` cycle / `-2` duplicate / `-3` table full / `-4` self-bind.
- `mount` — `0` (added OR idempotent no-op) / `-1` bad arg / `-2` table
  full / `-3` would create a mount cycle.
- `unmount` — `0` / `-1` no entry at that identity.
- `territory_chroot` — `0` (stamped or idempotent) / `-1` NULL source.
- `territory_pivot_root` — `0` / `-1` NULL source **or no current root**
  (the precondition that distinguishes it from chroot).

## Mechanism

**Mount keying is the full Plan 9 `(type, dev, qid)` triple.** An entry
records the mount POINT's `(mp_dc, mp_devno, mp_qid_path)`; the
mountpoint Spoor itself is never retained (the caller stalks it, `mount`
copies the key, the caller clunks it). All three components are
load-bearing: `qid.path` is unique only *within* a `(dc, devno)`
instance, and every dev9p session shares `dc == '9'` with root
`qid.path == 0` — so `(dc, qid.path)` alone collides corvus against a
per-user stratum-fs. `devno` (minted per attach by `spoor_next_devno`)
is what separates them. This is the stalk-2 re-key; before it the target
was an abstract `path_id_t`.

**`SYS_MOUNT`/`SYS_UNMOUNT` resolve with `STALK_MOUNT`** — resolve, do
NOT cross the final mount, do NOT open. That carve-out is what makes
MREPL work: a re-mount onto an already-mounted point keys on the SAME
underlying identity, so the existing entry is found and replaced rather
than a second entry stacking on the crossed target.

**MREPL displacement** replaces the first entry at a matching identity:
capture the old source, install the new (with a fresh `spoor_ref`),
re-capture `mp_path` ref-NEW-before-unref-OLD (so a degenerate shared
`Path` object survives the swap), and `spoor_clunk` the displaced source
**outside** the lock. `MBEFORE`/`MAFTER`/`MCREATE` are stored but never
walked (see Seams).

**Two cycle checks, one modeled.** `would_create_cycle` guards the bind
graph (fixed-point reachability over `binds[]`) — it is what
`specs/territory.tla::NoCycle` proves. `would_create_mount_cycle` guards
the MOUNT identity graph with the identical algorithm over `(dc, devno,
qid.path)` keys, rejecting a self-mount or a cross-tree oscillation with
`-3`. It exists because [[fnd-stalk2-r1-f1]] falsified the claim that
I-3 held "by construction" on the mount table. It is **not modeled** —
see [[spec-territory]] and [[seam-mount-graph-unmodeled]].

**chroot and pivot differ only in a precondition.** `territory_chroot`
establishes or replaces a root; `territory_pivot_root` REQUIRES an
existing `root_spoor` and refuses (`-1`) without one. The split is
semantic, enforced at this layer so `SYS_PIVOT_ROOT` cannot be used to
establish an initial root on a fresh Territory. Both share the
bump-before-swap discipline: `spoor_ref(new)` (which extincts on a
corrupted source, leaving `root_spoor` untouched), swap under
`ns_lock`, then `spoor_clunk(old)` outside it. `spoor_clunk`, not
`spoor_unref` — the displaced root may be its Spoor's last holder, and
the Dev's close hook must run.

**`territory_clone` copies four things and takes three ref classes.**
Under the parent's `ns_lock`: the bind array, the mount array (one
`spoor_ref` per entry's source AND one `path_ref` per entry's
`mp_path`), and `root_spoor` (one `spoor_ref`). Then, under the parent's
`dot_lock` (a separate leaf), a `kmalloc`'d copy of `dot_path` — POSIX
fork semantics, the child gets an independent snapshot. On the
`dot_path` OOM the child is `territory_unref`'d, whose final release
drops every ref just taken; the parent is untouched.

**Final release** walks `mounts[]` in reverse dropping `mp_path` then
`spoor_clunk`ing each source, zeroes `nmounts` defensively, clunks
`root_spoor`, and frees `dot_path` — all before `kmem_cache_free`. This
is the spec's "DestroyTerritory requires `mounts[p] = {}`" precondition
discharged as a sequence of Unmounts. Skipping it is
`BUGGY_DESTROY_LEAK`.

**The cwd is a cleaned absolute string, not a handle.**
`cwd_lexical_resolve` is a pure, allocation-free, lock-free resolver: it
seeds from `dot` for a relative input (an absolute input ignores the
cwd), then walks components resolving `.` and `..` LEXICALLY, popping at
`olen` with a hard floor at 0 so excess `..` nets to `"/"`. Its output
is always absolute-from-root, which is why [[inv-i28]] gains no new
mechanism from LS-4: stalk's own `..` clamp becomes a redundant safety
net. `territory_setdot` is fed ONLY by that resolver's output, so
`dot_path` is always cleaned.

**`territory_format_ns`** renders `/proc/<pid>/ns` under `ns_lock`, one
whole `mount <point> <source>` line at a time: snapshot `off`, rewind on
any overflow to discard a partial line, and emit `binds: N` only when
the list rendered in full (a `binds:` line after a truncation would
falsely imply completeness). The source label is its Spoor's `->path`,
or `#<dc>` — the Plan 9 device spec — when the source is a device root
with no namespace name.

## Data structures

`struct PgrpMount` is pinned at **40 bytes**: `source` (8) + `mp_path`
(8) + `mp_qid_path` (8) + `mp_dc` (4) + `mp_devno` (4) + `flags` (4) +
`_pad` (4). Two pointers first for 8-alignment; the pad gives the array
its 8-byte stride. It was 16 bytes before stalk-2 re-keyed it and 32
before #66b added `mp_path`.

`struct Territory` is pinned at **920 bytes** — a 24-byte header
(`magic`, `ref`, `nbinds`, `nmounts`, `_pad`), `root_spoor` at 24,
`binds[8]` at 32, `mounts[20]` at 96, then `dot_lock` / `dot_path`
(LS-4) and `ns_lock` (RW-4) appended at the tail. **Every load-bearing
offset is individually `_Static_assert`ed**, not just the size — a field
reorder that preserved the total would otherwise silently break the
FROM_ROOT path and the mount iteration ([[fnd-stube2-r1-f5]]). That is
also why both later additions were APPENDED: the tail-append discipline
keeps every pinned offset stable.

`magic` sits at offset 0 deliberately — SLUB's freelist write on
`kmem_cache_free` clobbers exactly that word, so a double-free is caught
at the next `territory_ref`/`unref` rather than corrupting silently.
`territory_clone` additionally range-checks `nbinds`/`nmounts` against
their compile-time caps before the copy loops, so a torn count cannot
walk past the arrays.

`PGRP_MAX_MOUNTS` is **20**, grown 8 → 16 → 20. joey is the high-water
mark and the reason: the kproc boot namespace mounts `/srv`, `/proc`,
`/ctl`, `/dev`, `/env`, and the pre-pivot mounts ORPHAN at pivot (their
devramfs mount points stop being reachable from the disk root) while
staying in the table — so the cost is pre+post per re-grafted directory.
The real fix is a pivot-time GC ([[seam-80-pivot-orphan-mounts]]); the
cap growth is the holding action.

## Concurrency

Two locks, both per-Territory, both leaves, deliberately separate:
[[lock-territory-ns-lock]] (mounts + binds + root_spoor) and
[[lock-territory-dot-lock]] (the cwd string alone).

The load-bearing rule for both: **captured-and-deferred release.** A
displaced or removed `source` Spoor is captured under the lock and
`spoor_clunk`'d outside it, because the Dev close hook may sleep and a
spinlock must never be held across a sleep. `dot_path`'s old string is
freed outside `dot_lock` for the same shape (readers copy under the lock
and never retain the pointer past their critical section). `path_unref`
is the exception that proves the rule — it is refcount + `kfree`, no
close hook, non-sleeping, so it runs in place.

`ns_lock` is NEVER held across `stalk` (which blocks on 9P). That is why
`mount_lookup`'s contract is OWNED, not borrowed: the lookup and the
`spoor_ref` happen atomically under the lock, the caller gets a
ref-held Spoor to cross with, and the lock is long released before
`clone_walk_zero` runs. `territory_root_ref` is the same pattern for
`root_spoor` — and it is the ONLY sound way to take a FROM_ROOT walk
base in a multi-thread Proc.

Both properties are the RW-4 SA-F1 fix ([[fnd-rw4-sa-f1]]). Before it
these fields were unlocked, and a peer thread's `pivot_root` or
`unmount` could free a Spoor a walking thread was mid-read on. The
hazard was known and tracked as dormant ([[seam-848-pivot-walk-race]])
until RW-4 overruled the dormancy: the kernel must be sound against any
EL0 program, and the P6 multi-thread lift had made the program
writable.

The `g_proc_table_lock -> ns_lock` edge introduced by `/proc/<pid>/ns`
is ACYCLIC — nothing under `ns_lock` takes `g_proc_table_lock`, and the
secondary `ns_lock -> slub c->lock` edge (via `path_unref`'s `kfree`)
has no reverse. `kmalloc` under `dot_lock` in `territory_clone` is
sound for the same reason: SLUB knows nothing of Territory, and neither
lock is taken from an IRQ handler.

## Invariants enforced

[[inv-i1]] — the isolation this dossier IS. Every operation takes one
`struct Territory *`; no call mutates two. `territory_clone` reads the
parent and writes only the child. RFNAMEG (cross-Proc sharing) does not
exist at v1.0, so a Territory has exactly one Proc except for the peer
Threads that share it.

[[inv-i3]] — the DAG. Enforced twice: `would_create_cycle` on binds,
`would_create_mount_cycle` on the mount identity graph.

[[inv-i33]] — `mp_path` is the territory-side mirror of the Spoor Path,
and it is introspection-ONLY. Every keying decision (`mount_key_eq`,
`mount_is_point_id`, `would_create_mount_cycle`, `mount_lookup`) reads
`(mp_dc, mp_devno, mp_qid_path)`; `territory_format_ns` is the only
reader of `mp_path` anywhere. A wrong, stale, or NULL `mp_path`
misreports `/proc/<pid>/ns` and nothing else.

`MountRefcountConsistency` ([[spec-territory]]) — every mount entry and
every `root_spoor` holds exactly one ref on its Spoor, maintained at
five sites: `mount` (bump), `unmount` (drop), `territory_chroot` /
`territory_pivot_root` (bump new, drop old), `territory_clone` (bump per
cloned entry + per cloned root), `territory_unref` final release (drop
all). Four of the spec's five buggy configs are exactly the "forgot one
of these" classes.

## Error paths

Argument faults return; state faults extinct. NULL or corrupted-magic
Territory is an `extinction` at every entry point (a corrupted Territory
is a kernel invariant violation, not a caller error). NULL or
corrupted-magic Spoor arguments return `-1`. `spoor_ref` extincts on a
corrupted source, which is why the bump precedes every swap — the
extinct leaves state unchanged.

The table-full paths (`-2`/`-3`) take no ref, so a rejected mount cannot
leak one. The `mount` append is infallible after its `spoor_ref`, so no
rollback exists or is needed. `territory_clone`'s only fallible step is
the `dot_path` `kmalloc`, and its failure path is a full
`territory_unref` of the child.

`SYS_GETCWD` deliberately accepts an OVERSIZED buffer: it computes the
cwd first, then requires only `len + 1 <= buf_len` and copies exactly
that many bytes. The pre-fix `buf_len > SYS_OPEN_PATH_MAX + 1 -> -1`
rejection broke the near-universal `getcwd(buf, PATH_MAX)` idiom and
surfaced as `make: getcwd: I/O error`
([[chg-2026-07-24-getcwd-oversized]]).

## Performance

Both cycle checks are fixed-point reachability: O(N²) worst case at
N = 8 binds (~64 inner iterations) and N = 20 mounts (~400). They run
once per `bind`/`mount`, never on the resolution path.

`mount_lookup` is a linear scan of up to 20 entries under a spinlock,
and it runs at EVERY component descent in `stalk` — it is the hottest
thing in this file. The flat array is the right shape at N = 20; the
RB-tree-keyed-on-qid replacement waits on a count that justifies it.

`territory_format_ns` holds `ns_lock` inside `g_proc_table_lock`'s
IRQs-off window. Bounded (≤ 20 lines into a 512-byte buffer, no sleep,
no allocation) and comparable to the pre-existing `format_status` hold —
a latency note, not a defect.

## Prosecution

On any change to this file, prosecute:

- **The five refcount sites stay matched** (mount / unmount / chroot /
  pivot / clone / final-release). Four of the spec's buggy cfgs are the
  miss-one classes; re-run them.
- **The three ref classes are independent** — `source` (spoor),
  `mp_path` (Path), `root_spoor` (spoor). A new field with a lifetime
  needs its own hook at all four mount-table sites, not three.
- **Capture-and-defer holds**: no `spoor_clunk` under either lock, no
  `stalk` under `ns_lock`, no sleep under either. A new caller that
  takes `ns_lock` and then blocks is the RW-4 bug re-introduced.
- **`mount_lookup` / `territory_root_ref` keep returning OWNED refs**
  and every caller clunks. A borrow-shaped refactor reopens
  [[seam-848-pivot-walk-race]].
- **Both cycle checks survive** — the bind one is spec-pinned, the mount
  one is not, so only the tests and this row protect it.
- **The pinned offsets** — any new field goes at the TAIL, and the
  `_Static_assert` set grows with it.
- **`mp_path` gains no reader that makes a decision** ([[inv-i33]] is a
  grep-complete obligation).
- **The mount-point gate stays search-only** — adding a write check
  would break the Plan 9 model and every confined Proc's own-namespace
  composition; adding a capability gate would break the container
  keystone.

## Seams

- [[seam-union-mount-walk]] — MBEFORE/MAFTER/MCREATE are stored and
  never walked.
- [[seam-rfnameg-shared-territory]] — cross-Proc namespace sharing.
- [[seam-80-pivot-orphan-mounts]] — pre-pivot mounts accumulate; the cap
  grows instead of a GC running.
- [[seam-handle-based-dot]] — the cwd is a string, not a Spoor; symlinks
  force the upgrade.
- [[seam-mount-graph-unmodeled]] — the live cycle check has no model.
- The bind table itself (see Caveats) — dead scaffolding whose removal
  or revival is an open call.

## Caveats

**The bind table is structurally dead at v1.0.** There is no `SYS_BIND`;
`bind()` and `unbind()` have NO caller anywhere outside this file and
the kernel tests; and neither `kernel/stalk.c` nor `kernel/syscall.c`
so much as names `binds`, `PgrpBind`, or `path_id_t`. What the boot
chain calls "binding `/bin`" is a `mount(..., MREPL)`. So `binds[]` is
allocated, cloned, cycle-checked, size-asserted, and rendered as a count
in `/proc/<pid>/ns` — while being unreachable and unread. This matters
beyond tidiness: it means `specs/territory.tla::NoCycle`, the only cycle
invariant the model proves, is about the DEAD table, while the live
mount graph's check is unmodeled ([[seam-mount-graph-unmodeled]]).

**`path_id_t` is an abstract `u32` that nothing mints.** The header's
"the fd-syscall surface (deferred) populates these with real path
identifiers" describes a plan that stalk-2 superseded — mounts are keyed
by Spoor identity now, and the type survives only on the dead bind
table.

**`source_is_valid` is a tautology** — it null-checks and returns true,
delegating the magic check to `spoor_ref`'s own extinct. Harmless, and
noted as such by the LS-4 round; the name overpromises.

**`unmount` removes ONE entry per call** — the first match. Plan 9's
`unmount(name)` can clear everything at a name; to clear a union here,
call until `-1`.

**Superseded doc claims.** `docs/reference/18-territory.md` was
materially stale when absorbed, and self-contradictory in places. It
stated `PGRP_MAX_MOUNTS 8` (is 20) and `sizeof(PgrpMount) == 32` (is
40); showed `struct Territory` with no `dot_lock`/`dot_path`/`ns_lock`
and `struct PgrpMount` with no `mp_path`, while its own Status table
said `mp_path` and `ns_lock` had landed; omitted `-3` from `mount`'s
return table and `would_create_mount_cycle` from its cycle section
entirely; omitted `territory_pivot_root`, `territory_root_ref`,
`mount_is_point_id`, `territory_format_ns`, and the whole LS-4 cwd API
from the public-API block; showed `mount`/`unmount` code sketches with
the pre-stalk-2 `path_id_t target` signature; claimed "~290 LOC"
(is 988) and "16 tests" (are 29); listed `pivot_root` as "v1.x per
CORVUS-DESIGN §10.1 Q2" though it landed at 16c, and "multi-component
walker consuming mount table" as Phase 5+ though that is `stalk`; and
carried a literal duplicate Status row.

`56-sys-mount.md` was stale in a DIFFERENT and worse mode: PARTIALLY
updated. Its ABI section had been correctly revised at stalk-2, while
everything beneath it stayed at P5-mount-syscall — `PGRP_MAX_MOUNTS = 8`,
"9 tests" (are 13), path IDs described as the live keying, and a caveat
teaching that walking a mount point "still uses the Plan 9 bind table
(already implemented)", which the walk has never done. A current section
lends authority to the stale ones below it, so the partial update is
harder to catch than wholesale rot.

## Provenance
