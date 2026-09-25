---
id: sub-kernel-territory
type: sub
title: "Territory — the per-Proc namespace (mount table, root, cwd)"
parent: moc-kernel-namespace
code: ["kernel/territory.c", "kernel/include/thylacine/territory.h", "kernel/test/test_territory_pivot_root.c", "usr/symlink-probe/src/main.rs"]
audit: hard
guarded-by: [inv-i1, inv-i3, inv-i33]
validated-by: [spec-territory, gate-smp]
locks: [lock-territory-ns-lock, lock-territory-dot-lock]
hazards: []
abis: []
design: ["docs/STALK-DESIGN.md", "docs/LIFE-SUPPORT.md"]
created: 2026-08-01
updated: 2026-09-24
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

Five structures on `struct Territory`, four of them live:

| Field | What | Reached by |
|---|---|---|
| `mounts[32]` | `(dc, devno, qid.path) -> source Spoor` grafts; a point may hold a UNION (several members in `MBEFORE`/`MAFTER` order) | `mount_lookup` / `mount_members_snapshot` from [[sub-kernel-stalk]]; `SYS_MOUNT`/`SYS_UNMOUNT` |
| `root_spoor` | the resolution floor + FROM_ROOT walk base | `territory_root_ref`; `SYS_CHROOT`/`SYS_PIVOT_ROOT` |
| `flags` | `TERRITORY_ROOT_PHENO_LINUX` — the namespace-level phenotype declaration (Design D) | `territory_root_pheno` from `stalk_core`'s `crossed_pheno` seed; `territory_declare_linux` at spawn |
| `dot_path` | the cwd string (`NULL` == `"/"`) | `SYS_CHDIR`/`SYS_GETCWD`; the `SYS_OPEN` relative join |
| `binds[8]` | Plan 9 path-to-path edges | **nothing — see Caveats** |

**The authority model is namespace-mediated, not capability-gated.** No
capability guards `SYS_MOUNT`, `SYS_UNMOUNT`, `SYS_CHROOT`,
`SYS_PIVOT_ROOT`, or `SYS_CHDIR`. The gates are exactly two, and both are
already-held authority:

1. **`RIGHT_READ` on the source handle** (`sys_lookup_spoor(..., RIGHT_READ)`
   in mount; chroot and pivot take theirs through `sys_lookup_root_source`,
   which adds the one non-authority condition — the source must be a
   DIRECTORY, `QTDIR`). A mount source you cannot read is
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
  full (a union's first mount needs two free slots) / `-3` would create a
  mount cycle.
- `unmount` — `0` / `-1` no entry at that identity.
- `territory_chroot` — `0` (stamped or idempotent) / `-1` NULL source.
- `territory_pivot_root` — `0` / `-1` NULL source **or no current root**
  (the precondition that distinguishes it from chroot).

## Mechanism

**Mount keying is the full Plan 9 `(type, dev, qid)` triple.** An entry
records the mount POINT's `(mp_dc, mp_devno, mp_qid_path)`; the
mountpoint Spoor itself is retained only as the covered member of a union
the mount starts (the covered directory, below); otherwise the caller stalks
it, `mount` copies the key, and the caller clunks it. All three components are
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

**MREPL displacement** now replaces the WHOLE union group at a point (the
UM-8 F6 correction): remove every existing member at the matching identity,
then install the new one, `spoor_clunk`ing each displaced source **outside**
the lock; a fresh `spoor_ref` on the new source and `mp_path`
ref-NEW-before-unref-OLD (so a degenerate shared `Path` object survives the
swap). `MBEFORE`/`MAFTER`/`MCREATE` are no longer inert — see the union-mount
section below.

**Two cycle checks, one modeled.** `would_create_cycle` guards the bind
graph (fixed-point reachability over `binds[]`) — it is what
`specs/territory.tla::NoCycle` proves. `would_create_mount_cycle` guards
the MOUNT identity graph with the identical algorithm over `(dc, devno,
qid.path)` keys, rejecting a self-mount or a cross-tree oscillation with
`-3`. It exists because [[fnd-stalk2-r1-f1]] falsified the claim that
I-3 held "by construction" on the mount table. Its cross-tree half is
**not modeled** — see [[spec-territory]] and [[seam-mount-graph-unmodeled]]
— and its self-mount half is, since B-1d-u (`NoSelfMount`). The covered
member a union's first mount installs is a self-edge by construction (its
source IS its point): it never adds a key to `reach`, and the resolver never
crosses it (below).

**chroot and pivot differ only in a precondition.** `territory_chroot`
establishes or replaces a root; `territory_pivot_root` REQUIRES an
existing `root_spoor` and refuses (`-1`) without one. The split is
semantic, enforced at this layer so `SYS_PIVOT_ROOT` cannot be used to
establish an initial root on a fresh Territory. Both share the
bump-before-swap discipline: `spoor_ref(new)` (which extincts on a
corrupted source, leaving `root_spoor` untouched), swap under
`ns_lock`, then `spoor_clunk(old)` outside it. `spoor_clunk`, not
`spoor_unref` — the displaced root may be its Spoor's last holder, and
the Dev's close hook must run. A chroot is **one-way at v1.0**: there is
no unchroot or `chroot(NULL)`, so the reference a chroot takes on its
root Spoor is held for the Proc's whole life. The consequence is a
caller discipline — a *persistent* Proc that chroots to a mounted Spoor
pins that Spoor (and the 9P session behind it) forever, so the
long-running init exercises chroot only through short-lived child probes
that release it on exit, never in its own persistent context, where the
pin would wedge a teardown that waits for the session's EOF.

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

**The cwd is a cleaned absolute string, not a handle** — but *joining* to it
and *cleaning* it are two jobs, and running them as one was a resolution
bug.

`cwd_lexical_resolve` is a pure, allocation-free, lock-free resolver: it
seeds from `dot` for a relative input (an absolute input ignores the
cwd), then walks components resolving `.` and `..` LEXICALLY, popping at
`olen` with a hard floor at 0 so excess `..` nets to `"/"`.

**It is no longer on the resolution path.** A lexical `..` pops a component
without proving it exists, so a cwd-relative path that traversed a directory
that was not there resolved successfully and handed back a working descriptor
— and the change-directory check validated the wrong object entirely, testing
directory-ness against the parent it had already massaged the path into. The
absolute spelling of the same path answered correctly, because it went through
the resolver's gates. Two code paths for one question, disagreeing.

The repair separates the jobs rather than adding a fourth gate:

- **`cwd_join` — resolution.** Emits the cwd, a separator, and the input
  **verbatim**. A `.`, a `..`, a trailing separator all survive into the path
  the resolver receives, so a cwd-relative path is subject to exactly the gates
  its absolute spelling gets.
- **`cwd_lexical_resolve` — canonicalization.** Narrowed to one production
  role: computing the string change-directory stores.

The entry point was renamed from *resolve* to *join*, because the old name
described the bug — it implied resolving the dots, which is precisely what it
must not do.

**Change-directory is the one caller needing both**, in three ordered steps:
join verbatim, resolve that, then canonicalize **the already-resolved join**
with no cwd seed — so the stored string is derived from the path that was just
validated, and a peer thread's concurrent change cannot make the two disagree.

**What this does to [[inv-i28]] is worth stating precisely, because the
dossier previously had the emphasis backwards.** Containment never rested on
this function, and still does not — the joined path resolves from `root_spoor`
and the resolver clamps `..` at its trail floor exactly as it does for an
absolute path. That was true before. What changed is that the clamp used to be
*unexercised* on cwd-relative paths, since the lexical cleaning consumed every
`..` before the resolver saw one. It was accurate to call it a redundant safety
net then, and it is the sole mechanism now.

**The redundancy was itself the defect.** The cleaning that made the clamp
look superfluous was the code popping unwalked components. Removing it
promoted a net nobody was relying on into the thing doing the work — which is
the good outcome, and a reason to be wary of describing a second mechanism as
redundant when what makes it redundant is a duplicate of the first.

Three consequences, accepted rather than fixed: a cwd-relative path spelled
with `..` no longer takes the fused fast path (the same cost its absolute
spelling always paid, on exactly the paths that were resolving wrongly); the
joined path is longer, so the length bound is reached sooner, and it surfaces
as a bare failure because the too-long errno is not in the registry yet; and a
deleted working directory now fails relative resolution, matching POSIX.

`territory_setdot` is still fed ONLY by the canonicalizer's output, so
`dot_path` remains cleaned.

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

`struct Territory` is pinned at **1400 bytes** — a 24-byte header
(`magic`, `ref`, `nbinds`, `nmounts`, `_pad`), `root_spoor` at 24,
`binds[8]` at 32, `mounts[32]` at 96, then `dot_lock` / `dot_path`
(LS-4) and `ns_lock` (RW-4) appended at the tail. **The assertion is
written as an expression over the two array bounds rather than as a
literal**, so growing a table updates it automatically; only prose
restating the arithmetic can rot, and this dossier's previous figure of
920 was exactly that — correct for a twenty-entry table and stale the
moment the cap grew. **Every load-bearing
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

`PGRP_MAX_MOUNTS` is **32**, grown 8 → 12 → 16 → 20 → 32 (`git log -G`) —
every time for the same cause, and the fifth time the cause was fixed
instead (2026-09-21). A root swap used to leave the previous generation's entries
in the table. The boot namespace mounts `/srv`, `/proc`, `/ctl`, `/dev`,
`/hw`, `/hw/pci`, `/env` on devramfs synthetic directories; joey pivots to
the disk root and re-grafts each; and the seven originals stayed — their
mount points no longer the target of any name, and therefore
**un-unmountable**, because `unmount` takes a RESOLVED mount point.
`territory_clone` then copied them into every Proc in the system. Measured
on the device (`/proc/<pid>/ns` of a login session): 23 entries, the first
seven that generation. A container runner inherits the 23 and adds ten
recipe mounts; the tenth returned `-2` at `nmounts == 32`, and `viv run`
was broken on `main` from the day the 23rd entry arrived. **A leak that
only wastes a resource is invisible until something else wants that
resource** — and then it presents as the other feature's bug.

**The shed (ARCH 9.6.10; `territory_shed_unreachable_locked`).** In the
same `ns_lock` hold that installs a new root, `territory_pivot_root` and
`territory_chroot` drop every entry whose mount point lies in a device
instance unreachable from that root. Reachability is the least set `R` of
`(dc, devno)` instances containing the new root's — and, when the root is
a union handle, its mount point's (fact 4) — and closed under
"mount-point instance in `R` ⇒ source instance in `R`". Four facts make
that rule sound rather than hopeful:

1. **The resolver only descends.** stalk handles `..` itself: it pops its
   in-call trail and the pop is a no-op at the base (a device's own `..`
   arm IS reachable — the union readdir dedup hands server-supplied names
   to a member's walk — but it stays inside that device's instance, which
   per-instance granularity already covers)
   ([[sub-kernel-stalk]]), and a symlink re-anchors at `root_spoor`. So
   every Spoor a resolution from the new root can hold is in `R`, an entry
   keyed outside `R` can never fire for one, and every name resolved from
   the new root resolves identically with and without the shed — the
   spec's `ShedLosesNothing`.
2. **It is per instance, so it is conservative.** The kernel cannot see
   inside a 9P tree and cannot tell which directories of a reachable tree
   are reachable. It may keep an entry no walk reaches; it never drops
   one a walk can. `viv` shows the cost: it chroots into a bundle
   directory on the SAME Stratum session as the host root, so every host
   entry keyed in that session survives into the container's table.
3. **One Dev breaks the premise, and says so.** A walk normally
   preserves `(dc, devno)` (`spoor_clone` copies it). `devenv` stamps the
   CALLING Proc's Env devno on every walk, including the 0-element mount
   cross, so a Spoor inside `/env` does not share a devno with the `/env`
   mount source. `Dev.devno_per_walker` ([[sub-kernel-dev]]) makes the
   closure match such a Dev on `dc` alone. The set of devno assigners was
   enumerated, not assumed: `spoor_alloc` (0), `spoor_clone` (copy),
   `dev9p` attach, `devsrv` attach, `env_alloc`, `devenv_walk`.
4. **One Spoor is consulted at the base without having been walked to: a
   union root's mount point** (audit round 1, F1 — a P1 the first version
   had). An open of a union directory — `O_PATH` or `OREAD`; both pass
   the directory gate — has member[0]'s identity and carries the point in
   `union_snap`; `chroot` / `pivot_root` take
   exactly such handles, and stalk routes every first component from a
   union base through the entries keyed AT THE POINT
   (`union_base = base->union_snap->point`). The point lives in the tree
   the union was mounted in. Seeded from the root alone, a chroot into
   `/bin` — a union in the default image — shed the union's own entries:
   every name under the new root was `ENOENT` and `open("/")` landed on
   the covered directory. The point's instance is therefore a second
   seed. `union_snap` is set once before the Spoor is published and freed
   with it, so the read under `ns_lock` needs only the root ref the
   caller holds. `reach[]` is sized for it (`PGRP_MAX_MOUNTS + 2`: two
   seeds + one new instance per entry), guarded, and a test lands exactly
   on the bound.

The spec could not have caught fact 4, and the reason is the lesson:
round 1's `territory_shed.tla` stated `ShedLosesNothing` against the same
closure `Keep` was built from, so it held for ANY rule — the auditor
replaced the closure with `{root}` and with "every tree" and TLC reported
no error both times. It now states it against an operational WALKER
(start / cross / per-walker restamp) that shares no operator with the
rule, and three sabotages of the closure each fail. A check that compares
a rule with itself is the spec-level form of a test that cannot fail.

Dropped entries release what `unmount` releases: `path_unref(mp_path)`
under the lock, `spoor_clunk(source)` deferred outside it (a Dev close
hook may sleep). Survivors keep their relative order, which IS the union
search order. The idempotent same-root call swaps nothing and sheds
nothing. `mount()` itself never sheds. Measured after the fix, same
probe: a shell in a login session holds **17** entries (15 of joey's,
login's `/home/<user>`, ut's `/tmp` bind), so a container's eleven at most
(`/dio`, eight fixed binds, `/net` and `/dev/tty` when granted) fit with
four to spare.

**Order matters, and the text says which order.** Reachability is
evaluated once, at the swap, over the table as it stands. joey binds the
OLD devramfs root at `/bin` *after* its pivot, which before the shed
revived the boot generation as aliases — `/bin/proc`, `/bin/srv`,
`/bin/dev/cons` were live device trees. They are bare synthetic
directories now. One alias had a real user: `/hw/pci` worked post-pivot
only because the orphaned entry was keyed on devhw's `pci` child, which
the `/hw` re-graft made reachable again — ARCH had recorded that re-graft
as "a v1.x seam" the whole time it was working by accident. joey now
re-grafts it explicitly ([[sub-stratum-boot]]).

**The observable changes — three shapes, no others.** (1) An fd-relative
walk from a directory fd opened BEFORE the swap, into a tree the new root
cannot reach, no longer crosses the shed mounts and sees the underlying
directory. (2) A ROOT-relative walk through joey's post-pivot `/bin` bind
(the aliases above). (3) A union dirfd whose point's entries were shed is a
plain handle on member[0]: names only a later member held are `ENOENT`,
and `"."` is member[0] — never a covered directory the handle did not
name (below). Such an fd is
a handle on a file tree, not on the old namespace.
A peer thread's resolution already in flight across the swap is the same
case: the lock makes each LOOKUP atomic with the swap, not each
resolution, and a resolution straddling a root swap never had a
consistent namespace. `SYS_PIVOT_ROOT` refuses a non-directory root as
`SYS_CHROOT` always did (one `sys_lookup_root_source`): with the shed, a
pivot onto a pipe strips the table for good instead of wedging the Proc
until it pivots back. `usr/symlink-probe` carries the deny-path legs (its
leg K is a real chroot, so it is where a root gate can be probed from
userspace): chroot AND pivot onto a regular file are refused and the
namespace still resolves afterwards. The shed's own regressions are the 12
`territory.shed_*` kernel tests in `test_territory_pivot_root.c` (count them:
`grep -c '"territory.shed_' kernel/test/test.c` — this sentence said 13 for
a day). Of the five added at audit round 1 only
`shed_union_root_keeps_point_entries` FAILS on the pre-fix shed
(survivors 0, want 2); the other four are gap-closers that pass on both and
are named as such (audit r2 F5). And that one test encodes the AUTHOR's
model of what stalk consults, so a new base-time consult leaves it green —
which is why the device witnesses exist: `usr/symlink-probe`'s `union-a` /
`union-b` stages run a REAL union (`/proc` + `/ctl` over a Stratum
directory: point and member[0] in different instances — a same-session
union passes on a kernel with no seed at all) through a real chroot.

**A dissolved union degrades to member[0], never to a covered directory the
handle did not name** (audit r2 F1; the rule is [[sub-kernel-stalk]]'s to enforce, stated here
because the shed is one of the two ways to reach it). A union handle's
`point` is the directory the union was mounted over. Round 1's correction
("a union dirfd whose point's entries were shed answers `ENOENT`") was true
for a NAME and false for `"."`: the zero-component walk cloned the point,
found nothing mounted there, and returned the covered directory. Reachable
with no shed at all — `unmount("/")` until the union is empty, then
`open("/")` — so it predates #80; the shed added a second route (hold the
dirfd, `chroot` into an instance that cannot reach the point's). A mount used as a MASK
over a directory in an unreachable tree therefore stops masking for
holders of such an fd; nothing in-tree relies on more. Since B-1d-u the
covered directory can itself be a MEMBER — first in order under `MAFTER`
(the covered-directory section below) — and then member[0] IS that
directory: the handle named it when it was opened, so degrading to it
reveals nothing (`stalk.union_covered_dissolved`).

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
`would_create_mount_cycle` on the mount identity graph. A union at a point is
a SET of members at one identity, not a cycle: the cycle check keys on the
mount identity, so stacking members at the same point adds no edge.

[[inv-i28]] (prose, the pheno-mount half) — the `MPHENO_LINUX` declaration is
detected by the RESOLVER ([[sub-kernel-stalk]]), scoped per mount POINT, and
so bounded by the same per-component X-search containment as every other
crossing; the phenotype is a property of HOW a file was named, not of its
bytes, so the SAME file reached by a non-pheno path stays native. Enforcement
of I-28 itself is stalk's.

[[inv-i43]] (prose, the declaration channel) — Design D and `MPHENO_LINUX` make
this Territory a phenotype DECLARATION channel (`territory_root_pheno`, the
mount crossing), conferring ABI SHAPE and no authority; the ENFORCEMENT half of
I-43 stays at the fork cap-strip and the exec-time stamp (a mis-declared Proc
mis-decodes its own calls behind its own gates). Not in `guarded-by` for the
exec-precedent reason: territory declares the shape, it does not enforce the
non-escalation.

The union-mount invariants are [[spec-territory]]'s `WalkFirstHit` /
`ReaddirDedupFirstWins` / `CreateTargetCorrect` / `RemoveTargetCorrect` /
`OrderCorrect` (each with a buggy cfg); this file owns the ordering + the
atomic snapshot they rest on, [[sub-kernel-stalk]] owns the walk/readdir/
create/remove that consume it.

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
- **The cwd join stays verbatim.** Any cleaning re-introduced there resolves
  dots the resolver never walks, and the failure is silent success rather than a
  refusal — a path through a directory that does not exist opening a working
  descriptor. The canonicalizer keeps exactly one caller, and it runs on an
  already-resolved path.
- **The shed's premise is the resolver's, not the table's.** It is sound
  ONLY while no resolution can climb out of a mounted tree: `..` must keep
  popping the in-call trail, never reaching `Dev.walk`, and symlinks must
  keep re-anchoring at `root_spoor`. A resolver change that lets a
  device-level `..` escape a mount source makes the shed drop LIVE
  mounts. Likewise a new Dev whose walk re-stamps `devno` without setting
  `devno_per_walker`. Re-run `territory_shed.tla`'s two buggy cfgs
  (`nontransitive` must violate `ShedLosesNothing`, `keeps_all` must
  violate `NoResidueAfterPivot` — check WHICH invariant, a bare
  conjunction hides a wrong one) and `territory.shed_*`, whose
  `per_walker` test carries its own control one variable away.
- **Shedding an `MNOEXEC` entry can un-cover an instance still reachable
  through a second, unflagged mount** (`mount_noexec_covers` is an
  any-scan over the surviving table). It is the loosening the ungated
  `unmount` already gives the same caller, and the Linux phenotype serves
  neither `chroot` nor `mount`, so a container cannot trigger it. If
  either of those ever changes, this becomes a hole.
- **Nothing may become the only enforcement by accident.** The `..` clamp was
  described as a redundant net while a duplicate mechanism upstream consumed its
  inputs; when that duplicate was removed as a defect, the clamp became load-
  bearing without anyone changing it. Before calling a second mechanism
  redundant, check whether the thing making it so is a duplicate of it.

## Seams

- [[seam-union-mount-walk]] — CLOSED (the UM arc): MBEFORE/MAFTER/MCREATE
  are walked, ordered, and spec-modeled (see the union-mount section).
- [[seam-rfnameg-shared-territory]] — cross-Proc namespace sharing.
- [[seam-80-pivot-orphan-mounts]] — CLOSED 2026-09-21: the shed at pivot /
  chroot (above). What it leaves is recorded there: a container still
  inherits its host's reachable-by-instance entries, and the
  Fuchsia-shaped fix is a clean spawn-time Territory.
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

## Union mounts (the UM arc; 2026-09-05) — MBEFORE / MAFTER / MCREATE walked

The seam that said these flags were "stored and never walked"
([[seam-union-mount-walk]]) is CLOSED: a point may hold a UNION of members and
the whole stack is walked, ordered, and spec-modeled. This file owns the
ordering + the snapshot; [[sub-kernel-stalk]] owns the walk/readdir/create/
remove that consume them.

- **Placement dispatch (`mount`).** A non-MREPL mount at an existing point
  places by its ordering flag: `MBEFORE` inserts at the index of the point's
  FIRST existing member (Plan 9 prepend; later MBEFOREs go after earlier ones),
  `MAFTER` and the flagless default append. Members are searched in `mounts[]`
  ARRAY ORDER — so MBEFORE members precede MAFTER members by construction, which
  is `OrderCorrect`. An existing pair re-mounted with a new ordering flag is
  MOVED, not duplicated (UM-8 F6). `unmount` shifts down rather than swap-removes,
  because order stopped being cosmetic.
- **MREPL replaces the WHOLE group** (above) — the spec's `MountRepl` collapses
  a union to one member, so it must remove every member at the point, not just
  the first.
- **`mount_members_snapshot` — the atomic whole-union snapshot.** The resolver
  cannot walk `mounts[]` member-by-member under a lock it must release to cross
  (a cross may block on 9P), so it snapshots the point's members + their flags
  ATOMICALLY under `ns_lock` into a caller array, then crosses them lock-free.
  The atomicity is load-bearing: a member shifting into slot k between two
  separate reads would land a create on the wrong member (the UM-8 F4 hazard the
  snapshot closes). Ordered: MBEFORE members first, MAFTER/default last.
- **MCREATE — the writable member.** The create path crosses the FIRST member
  carrying `MCREATE` (stalk's `stalk_union_create`); a union with no MCREATE
  member answers `-T_E_ACCES` (no writable member), never a silent misplacement.
  That is `CreateTargetCorrect`; picking the holder instead of the MCREATE member
  is `RemoveTargetCorrect`'s buggy twin.

The spec grew a SEQUENCE-valued `mounts` (the set-valued model could not express
order) with `WalkFirstHit` / `ReaddirDedupFirstWins` / `CreateTargetCorrect` /
`RemoveTargetCorrect` / `OrderCorrect` and their buggy cfgs.

## The covered directory (B-1d-u; 2026-09-24) — Plan 9's `cmount`, exactly

Until B-1d-u a union searched only its GRAFTED members (the section above):
the directory mounted over was never one, a lone `MBEFORE` was a one-member
mount, and `bind -b dir /lib` hid the disk's `/lib`. The operator chose Plan 9
unions ([[dec-2026-09-24-union-covered-directory]]): an `MBEFORE` / `MAFTER`
mount at a DIRECTORY point that hosts no member also installs the covered
directory as a member.

- **`MCOVERED` (0x40) is kernel-internal.** `SYS_MOUNT`'s valid mask excludes
  it, so no EL0 caller can mint or clear one. The entry carries no other
  flag: never `MCREATE` (Plan 9 adds it with flag 0), so a create in a union
  whose mounted members lack `MCREATE` stays `-T_E_ACCES`; never `MNOEXEC`
  (the restriction scopes to the instance that was mounted —
  `territory_mount.covered_noexec_scoped`).
- **When (`starts_union`).** `!(flags & MREPL) && (flags & (MBEFORE|MAFTER))
  && (mountpoint->qid.type & QTDIR) && !mount_point_hosts_member(...)`,
  judged BEFORE the UM-8 F6 reposition: re-mounting the sole member of an
  `MREPL` group with `MBEFORE` finds the point hosting a member, so it grows
  no covered one (`territory.tla` `BUGGY_FRESH_AFTER_REMOVE`). `MREPL` wins
  over the ordering flags and replaces the whole group, covered entry
  included; a flagless mount adds none. A file point stays a plain mount
  (`NoCoveredFile`); Plan 9 refuses it (`Emount`), and whether `SYS_MOUNT`
  should is owed to the operator.
- **Order and slots.** Both entries append in Plan 9's order (`mount_install_at`):
  `<new, covered>` for `MBEFORE`, `<covered, new>` for `MAFTER`. Later ordered
  mounts place around the covered entry like any member. The first mount
  needs two free slots (`-2` otherwise, nothing installed).
- **The one retained point.** The covered entry's source IS the mount-point
  Spoor, `spoor_ref`'d before install. `unmount`, `MREPL`, the shed and
  `territory_destroy` drop it exactly once and `territory_clone` refs it, as
  for any source.
- **Never crossed.** [[sub-kernel-stalk]]'s `stalk_cross_src` answers
  `MCOVERED` with a clone of the point, and a mount-over-mount chain stops at
  a point whose member[0] is covered. It is therefore not an edge of the
  mount graph (`would_create_mount_cycle` adds no key for it), and I-3 holds
  over the edges that are crossed.
- **Leaving.** `unmount` removes the first NON-covered entry at the point,
  then the covered entry if no mounted member remains, so the point is a
  plain directory again, never a union of itself (`NoOrphanCovered`).
- **Rendering + the shed.** `territory_format_ns` prints the entry as
  `mount <pt> <pt> covered`. The shed sees a self-edge: it adds nothing to
  the closure and is kept or shed with the members at its point
  (`territory.shed_covered_shares_fate`).

The first consumer is joey's post-pivot `/lib`: the initrd's `lib/` `MBEFORE`
the disk's `/lib` ([[sub-stratum-boot]]). Spec: `UnionHasCovered` /
`CoveredOnlyInUnion` / `CoveredIsItsPoint` / `CoveredPlacement` /
`NoOrphanCovered` / `NoSelfMount` / `NoCoveredFile`, each failed by its own
buggy config ([[spec-territory]]). Tests: ten `territory_mount.*` cases
(`union_keeps_covered`, `no_covered_unless_fresh`, the `covered_*` eight),
seven `stalk.union_covered_*`, and `territory.shed_covered_shares_fate`.

## VIVARIUM Design D + the pheno-mount (2026-09-05) — the phenotype declaration

Two namespace channels declare a loaded image's Linux phenotype (ABI SHAPE, not
authority — [[inv-i43]]); this file owns both.

- **Design D — the namespace-level declaration (`Territory.flags`).**
  `TERRITORY_ROOT_PHENO_LINUX` (a bit in `flags`, occupying the old alignment
  pad so the pinned offsets hold) means every image load whose resolution starts
  from this Territory decides `PHENO_LINUX`. `territory_root_pheno(t)` reads it
  (NULL-safe false); `territory_declare_linux(t)` sets it (idempotent,
  release-ordered), called ONCE on a child's freshly cloned Territory in the
  FULL_ARGV spawn thunk before EL0, and copied by `territory_clone` under
  `ns_lock` next to `root_spoor`. `stalk_core` seeds `crossed_pheno` from it at
  its `restart:` label, so the seed covers the first pass AND every symlink
  re-anchor. A container declares it on the namespace object itself because
  `territory_chroot` swaps `root_spoor`, so a crossing can never fire from inside
  a rootfs mount. `/proc/<pid>/ns` renders `root: pheno-linux` when set.
- **The pheno-mount (`MPHENO_LINUX`).** A binary whose exec RESOLUTION CROSSES a
  mount carrying this flag is stamped `PHENO_LINUX` — how `/viv/bin` ships bare
  Linux binaries (git) on the user's PATH: the location IS the declaration (a
  static binary has no reliable intrinsic ABI marker, so a phenotype is declared,
  never sniffed; FreeBSD's `/compat/linux` path-brand in a Plan 9 mount-flag
  form). SCOPE is per mount POINT, detected by the RESOLVER, NOT the
  `(dc, devno)` device-instance key `MNOEXEC` uses — a devno is minted per 9P
  session, so a device-instance key would declare EVERY file in the shared pool,
  native `/bin/ut` included. `stalk_cross_mounts` reports the crossing via its
  `crossed_pheno` out-param, giving exact `/viv/bin`-subtree semantics: the SAME
  file reached another way stays native.
- Both are OR-combined at the single exec-time stamp with the container
  manifest's `pheno_flags` spawn arg. Both are FAIL-SAFE (a resolution crossing
  no pheno-mount leaves the binary native — a Linux binary that misses the
  phenotype makes Linux-numbered calls that hit native handlers and fails cleanly,
  never a silent privilege gain) and deliberately UNGATED (composing a mount is a
  namespace edit that confers no authority; `/viv/bin` is composed by
  `PRINCIPAL_SYSTEM` at boot). Execve re-decides the phenotype at every image
  load; the exec-side consumer is [[sub-kernel-exec]] (its Legs A/B + the
  RELEASE-store commit live in [[sub-kernel-proc]]).

## Provenance

[[chg-2026-08-16-territory-verbatim-join]].

[[chg-2026-09-06-chroot-doc-absorb]] folds the one-way-chroot lifetime caveat
absorbed from docs/reference/77: a persistent Proc that chroots pins its root
Spoor (and the 9P session behind it) for life, since v1.0 has no unchroot -- the
reason long-running init uses short-lived child probes, not its own chroot.
