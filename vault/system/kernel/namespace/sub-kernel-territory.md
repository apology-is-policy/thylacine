---
id: sub-kernel-territory
type: sub
title: "Territory — the per-Proc namespace (mount table, root, cwd)"
parent: moc-kernel-namespace
code: ["kernel/territory.c", "kernel/include/thylacine/territory.h", "kernel/test/test_territory_mount.c", "kernel/test/test_territory_chroot.c", "kernel/test/test_territory_pivot_root.c", "kernel/test/test_sys_mount.c", "usr/symlink-probe/src/main.rs"]
audit: hard
guarded-by: [inv-i1, inv-i3, inv-i33]
validated-by: [spec-territory, gate-smp]
locks: [lock-territory-ns-lock, lock-territory-dot-lock]
hazards: []
abis: []
design: ["docs/STALK-DESIGN.md", "docs/LIFE-SUPPORT.md"]
created: 2026-08-01
updated: 2026-10-19
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
| the mount table (`mhash[64]`, `mfirst`..`mlast`) | `(dc, devno, qid.path)` mount points, each holding its `source` Spoor grafts in search order; a point with several members is a UNION. At most `PGRP_MOUNT_CAP` (1024) entries | `mount_lookup` / `mount_members_snapshot` from [[sub-kernel-stalk]]; `SYS_MOUNT`/`SYS_UNMOUNT` |
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

Return codes (the C API). `SYS_MOUNT` and `SYS_UNMOUNT` answer an errno
per refusal (operator vote 2026-10-19; ERRORS.md "Mount and unmount"):
`mount`'s `-2`, `-3` and `-4` reach EL0 as `ENOSPC`, `ELOOP` and `ENOMEM`,
`unmount`'s `-1` as `EINVAL`, and `sys_mount_for_proc` answers its own
gates (`EINVAL` for flags, `EBADF` for the source, `ENOTDIR` for Plan 9's
`Emount`) before the table op. Only the NULL guards EL0 cannot reach stay
a bare `-1` ([[sub-kernel-syscall-dispatch]]).

- `bind` — `0` / `-1` cycle / `-2` duplicate / `-3` table full / `-4` self-bind.
- `mount` — `0` (added OR idempotent no-op) / `-1` bad arg / `-2` the
  table holds `PGRP_MOUNT_CAP` entries, or would pass it (a union's first
  mount adds two) / `-3` would create a mount cycle / `-4` the entries
  could not be allocated (refused before `ns_lock`, so nothing changed).
- `unmount` — `0` / `-1` no entry at that identity.
- `territory_chroot` — `0` (stamped or idempotent) / `-1` NULL source.
- `territory_pivot_root` — `0` / `-1` NULL source **or no current root**
  (the precondition that distinguishes it from chroot).

## Mechanism

**Mount keying is the full Plan 9 `(type, dev, qid)` triple.** A mount
point (`struct PgrpMhead`, below) records its `(dc, devno, qid_path)`, and
its entries hang off it; the mountpoint Spoor itself is retained only as
the covered member of a union the mount starts (the covered directory,
below); otherwise the caller stalks it, `mount` copies the key, and the
caller clunks it. All three components are
load-bearing: `qid.path` is unique only *within* a `(dc, devno)`
instance, and every dev9p session shares `dc == '9'` with root
`qid.path == 0` — so `(dc, qid.path)` alone collides corvus against a
per-user stratum-fs. `devno` (minted per attach by `spoor_next_devno`)
is what separates them. It is 64 bits and never reused
([[sub-kernel-spoor]]), so the triple stays an identity for the life of
the boot; `territory_mount.devno_full_width` holds two points and two
sources 2^32 mints apart distinct, each refusal paired with its control.
This is the stalk-2 re-key; before it the target was an abstract
`path_id_t`.

**`SYS_MOUNT`/`SYS_UNMOUNT` resolve with `STALK_MOUNT`** — resolve, do
NOT cross the final mount, do NOT open. That carve-out is what makes
MREPL work: a re-mount onto an already-mounted point keys on the SAME
underlying identity, so the existing entry is found and replaced rather
than a second entry stacking on the crossed target.

**MREPL displacement** now replaces the WHOLE union group at a point (the
UM-8 F6 correction): unlink every existing member at the matching identity,
then link the new one at the same point, and release each displaced entry,
its source by `spoor_clunk`, **after** the lock is dropped; a fresh
`spoor_ref` on the new source and `mp_path` ref-NEW-before-unref-OLD (so a
degenerate shared `Path` object survives the swap). `MBEFORE`/`MAFTER`/`MCREATE` are no longer inert — see the union-mount
section below.

**Two cycle checks, one modeled.** `would_create_cycle` guards the bind
graph (fixed-point reachability over `binds[]`) — it is what
`specs/territory.tla::NoCycle` proves. `would_create_mount_cycle` guards
the MOUNT identity graph — the same reachability question over `(dc, devno,
qid.path)` keys — rejecting a self-mount or a cross-tree oscillation with
`-3`. It answers it with a pass over the points: from the source's key, each
point reached is looked up by key, marked, and queued on its own `work` link,
and every member's source key is checked against the mount point's, so the
check needs no array (the dynamic table, below). It exists because
[[fnd-stalk2-r1-f1]] falsified the claim that I-3 held "by construction" on
the mount table. Its cross-tree half is
**not modeled** — see [[spec-territory]] and [[seam-mount-graph-unmodeled]]
— and its self-mount half is, since B-1d-u (`NoSelfMount`). The covered
member a union's first mount installs is a self-edge by construction (its
source IS its point): it leads back to a point the pass has already
reached, and the resolver never crosses it (below).

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
Under the parent's `ns_lock`: the bind array, the mount table (one
`spoor_ref` per entry's source AND one `path_ref` per entry's
`mp_path`), and `root_spoor` (one `spoor_ref`). The child's mount nodes are
allocated before that lock is taken, from the counts read under it: the
clone counts, drops the lock, allocates, and takes it again, and a peer
thread that grew the table in the gap sends it round for the difference
(the pools only grow and the table only to the cap, so the rounds end). It
then copies the table in table order, appending each entry at its point,
which carries every point's search order over; what a shrinking peer left
over is freed after the unlock. Then, under the parent's
`dot_lock` (a separate leaf), a `kmalloc`'d copy of `dot_path` — POSIX
fork semantics, the child gets an independent snapshot. On a node
allocation failure (before any entry is copied) or the `dot_path` OOM the
child is `territory_unref`'d, whose final release drops every ref just
taken; the parent is untouched.

**Final release** has two stages, because a Territory has two kinds of
ref (2026-10-19). A Proc holds a STRONG ref (`ref`); devproc's ns read holds
a WEAK one, which keeps the struct and nothing else (`weak` counts the weak
holders plus one that the strong refs hold as a set). The last strong drop
releases the CONTENTS on the dropping thread: under `ns_lock` it detaches the
table (`mlast`, every point chain, the counts) and `root_spoor`, under
`dot_lock` it takes `dot_path`, and with both dropped it walks the entries
from `mlast` back — dropping each `mp_path`, `spoor_clunk`ing the source,
freeing the node — then frees the points, clunks `root_spoor`, frees
`dot_path`, and drops the strong refs' weak count. The last drop of either
kind `kmem_cache_free`s the struct. The detach is under `ns_lock` because a
weak holder may be rendering: it sees the table whole or empty, never a
released node. This is the spec's "DestroyTerritory requires
`mounts[p] = {}`" precondition discharged as a sequence of Unmounts.
Skipping it is `BUGGY_DESTROY_LEAK`.

The split keeps a reader of ANOTHER Proc's namespace from running that
Proc's release (OPEN-BUGS SA-5, the votes 5-7 round 2 F1): with one count, a
reader whose ref outlived the target's exit dropped the last ref, so the
target's mount clunks ran on the reader, where a walked 9P source's
`Tclunk` could wait on the dead Proc's own server, and the namespace was
released when the read ended rather than at the death
([[sub-kernel-death]] Part D). Now the exit's strong drop always releases
the namespace, and the reader's drop at most frees the struct, which never
sleeps. `territory_ref` extincts on a Territory whose contents are gone (a
strong ref is never revived from a weak one) and `territory_weak_ref` on a
freed one. Witnesses: `territory.weak_ref_keeps_the_struct_not_the_contents`
(both drop orders: the root's ref goes at the strong drop, the struct at the
last drop, and the weak holder's render is the empty listing) and
`devproc.ns_admission_after_render`'s exit leg.

**The cwd is a cleaned absolute string, not a handle** — but *joining* to it
and *cleaning* it are two jobs, and running them as one was a resolution
bug.

Until #83 one lexical resolver did both jobs. It seeded from `dot`, then
popped `.` and `..` LEXICALLY. A lexical `..` pops a component without proving
it exists, so a cwd-relative path through a directory that was not there
resolved and handed back a working descriptor. The change-directory check then
tested directory-ness against the parent it had already massaged the path
into. The absolute spelling of the same path answered correctly, because it
went through the resolver's gates. Two code paths for one question, disagreeing.

**`cwd_join` -- resolution.** It emits the cwd, a separator and the input
**verbatim**. A `.`, a `..` and a trailing separator all survive into the path
the resolver receives, so a cwd-relative path gets exactly the gates its
absolute spelling gets. The entry point was renamed from *resolve* to *join*,
because the old name described the bug.

**The stored name comes from the resolver, not from cleaning text**
(2026-10-06, [[dec-2026-10-06-chdir-physical]]). From #83 to 2026-10-06 the
canonicalizer kept one role, computing the string change-directory stored, and
its comment argued that with no symlinks its lexical pop matched the
resolver's trail pop. Symlinks then landed and the argument quietly stopped
holding. `cd link/..` stored the link's lexical parent while the resolver's
`..` had climbed out of the link's target, so the cwd named a different
directory from the one validated. A retargeted link also moved a cwd entered
through it. Change-directory now joins verbatim, resolves with `stalk_landed`
and stores the name the resolver reports for where it landed
([[sub-kernel-stalk]]). That name holds no `.`, `..` or link component, and it
is what was validated: the resolver builds it alongside the trail and walks it
once more, refusing a name that lands elsewhere. The canonicalizer had no other
production caller and was deleted.

**What this does to [[inv-i28]] is worth stating precisely.** Containment
never rested on these functions and still does not. The joined path resolves
from `root_spoor`, and the resolver clamps `..` at its trail floor exactly as
it does for an absolute path. Before #83 that clamp was *unexercised* on
cwd-relative paths, because the lexical cleaning consumed every `..` first. It
is the sole mechanism now. The stored name is only ever resolved again from
`root_spoor`, so a wrong name could misplace a cwd but never escape.

**The redundancy was itself the defect.** The cleaning that made the clamp
look superfluous was the code popping unwalked components. Removing it
promoted a net nobody was relying on into the thing doing the work. That is
the good outcome, and a reason to be wary of calling a second mechanism
redundant when what makes it redundant duplicates the first.

Accepted consequences, not fixed:
- A cwd-relative path spelled with `..` no longer takes the fused fast path.
  Its absolute spelling always paid that cost.
- The joined path is longer, so the length bound is reached sooner, and the
  failure is bare because the too-long errno is not in the registry.
- A deleted working directory fails relative resolution, matching POSIX.
- A stored name longer than the buffer at any point of the walk fails the
  change-directory, even when a later `..` would have shortened it.

`territory_setdot` is fed only by `stalk_landed`'s name (SYS_CHDIR) and the
boot's literal `/bin`, so `dot_path` stays clean.

**`territory_format_ns`** renders `/proc/<pid>/ns` under `ns_lock`: one
`mount <point> <source>` line per entry in table order, then `binds: N`.
The listing is as long as the table, so the function copies a window of
it — the bytes from the read's offset, at most the caller's room, ending at
the last line end among them (#66b F2: no partial line where a whole one
fits), or, holding no line end, all of them, so a read shorter than a line
still advances. A reader that loops to the end receives every line, with
`binds:` last; a reader that stops early holds whole lines and no
`binds:`, which is how a cut list is told from a whole one. Each read
renders afresh, so two windows of a table edited between them are two
renders. The source label is its Spoor's `->path`,
or `#<dc>` — the Plan 9 device spec — when the source is a device root
with no namespace name. A 9P session's root is the exception (operator vote
2026-09-28, Plan 9's form): its label is the file its session came over, the
origin its attach handler stamped on the root's priv, asked through
`dev9p_spoor_origin` -- that file's name (`/srv/home-<user>`) or, for a file
with none, its device spec (`#|`, a pipe) ([[sub-kernel-ninep-dev9p]]). Never
on the covered entry, whose line names its own directory even when that
source is a stamped root. The root itself stays named "/", so a bind of it,
which crosses with a fresh walk, reads the name it was reached by. On the CI
image a login shell's list reads `mount /home/michael /srv/home-michael`, and
a Haul mount `mount /tmp/haul-post /srv/haul-e2e remote` (the shell's mount of
the posted service) or `mount /tmp/host2 #| remote` (Haul's private form).

A line ends in its suffixes, each a word after a space, in a fixed order:
` noexec`, ` pheno-linux`, ` covered`, then ` remote` (LR-1, HAUL-DESIGN
4.8). ` remote` marks a member whose source belongs to a 9P session
declared remote at its attach or its /srv post, asked of the source through
`dev9p_spoor_remote`. It is never written on the covered entry: nobody
mounted that entry, and its line already names the directory, even when the
directory itself lies in a remote session (main's rule, and the test
asserts the premise so the missing suffix is the rule's doing). Each suffix
belongs to its line: a window that ends inside the next line ends at this
one's newline, suffix and all, and `binds:` stays the proof of a whole
list. Readers therefore treat a list without `binds:` as cut: the
coreutils listing tools say `mount list incomplete` rather than show a
mount point by its plain realm. This is the kernel's only use of the
declaration. `dev9p.remote_format_ns` renders real Territories over
unstamped roots: an unmarked
session's `mount /m /`, a marked one's `mount /m / remote`, windows of 13,
17, 18 and 20 bytes, an MREPL of a local tree over the remote one, and a union at a
remote directory whose covered line carries no suffix. The LR-1 sabotage
boots turned it red when the covered guard was dropped and when the suffix
write's result was ignored. `dev9p.origin_format_ns` renders the origin the
same way: an unstamped root's `/`, a stamped root's `mount /m /srv/home-joey`
(then with ` remote` after the name), windows inside the name, short of the
newline and inside the binds line, a walked clone reading its own name, a covered entry on a stamped
root keeping its own, and a nameless transport's device spec.

## Data structures

The mount table's two node types are private to `territory.c`, so nothing
outside it walks the table without `ns_lock`; `territory.h` declares them
only. Both are pinned at **64 bytes**, the figure the cap's memory cost is
stated in:
- `struct PgrpMount`, one per mount: `tnext`/`tprev` (table order),
  `next`/`prev` (its point's members, in search order), `head`, `source`,
  `mp_path`, `flags`, and a pad.
- `struct PgrpMhead`, one per mount point: `hnext` (its hash chain),
  `first`/`last` (its members), `work` (a pass's worklist link), the key
  `devno` / `qid_path` / `dc`, `nmembers`, and `visit` (the pass that last
  reached it). Its devno and `struct mkey`'s are pinned by a
  `_Static_assert` to the Spoor field's width: a narrower copy would read
  instances 2^32 mints apart as one.

`struct Territory` is pinned at **656 bytes** — a 32-byte header
(`magic`, `ref`, `nbinds`, `nmounts`, `flags`, then `root_spoor` at 24),
`binds[8]` at 32, the mount table at 96 (`mhash[64]`, `mfirst`, `mlast`,
`npoints`, `mvisit`), then `dot_lock` / `dot_path` (LS-4) and `ns_lock`
(RW-4). **The assertion is
written as an expression over the bounds (`PGRP_MAX_BINDS`,
`PGRP_MOUNT_HASH`) rather than as a literal**, so resizing a table updates
it automatically; only prose restating the arithmetic can rot, and this
dossier's earlier figures of 920 and 1400 were exactly that — each correct
for one array size and stale the moment the cap grew. **Every load-bearing
offset is individually `_Static_assert`ed**, not just the size — a field
reorder that preserved the total would otherwise silently break the
FROM_ROOT path and the mount iteration ([[fnd-stube2-r1-f5]]). The dynamic
table took the array's place at offset 96, and the offsets after it moved
with their asserts.

`magic` sits at offset 0 deliberately — SLUB's freelist write on
`kmem_cache_free` clobbers exactly that word, so a double-free is caught
at the next `territory_ref`/`unref` rather than corrupting silently.
`territory_clone` additionally range-checks `nbinds` against its
compile-time cap before the bind copy, and `nmounts` / `npoints`
(`nmounts <= PGRP_MOUNT_CAP`, `npoints <= nmounts`) before sizing its
pools from them; a table list that holds more entries or points than the
counts said is an extinction when the pools run dry.

The table was a fixed array, `mounts[PGRP_MAX_MOUNTS]`, until 2026-10-19,
and its size went 8 → 12 → 16 → 20 → 32 (`git log -G`) —
four times for the same cause, and the fifth time the cause was fixed
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
   caller holds. The closure is a pass over the points (the dynamic table,
   below): each seed marks and queues every point in its instance, and
   each queued point's members' sources do the same, so it allocates
   nothing and a root swap cannot fail. The array it replaced was sized
   `PGRP_MAX_MOUNTS + 2` (two seeds + one new instance per entry);
   `territory.shed_full_table_boundary` now runs a chain of
   `PGRP_MOUNT_CAP` entries through the pass.

The spec could not have caught fact 4, and the reason is the lesson:
round 1's `territory_shed.tla` stated `ShedLosesNothing` against the same
closure `Keep` was built from, so it held for ANY rule — the auditor
replaced the closure with `{root}` and with "every tree" and TLC reported
no error both times. It now states it against an operational WALKER
(start / cross / per-walker restamp) that shares no operator with the
rule, and three sabotages of the closure each fail. A check that compares
a rule with itself is the spec-level form of a test that cannot fail.

Dropped entries release what `unmount` releases: each is unlinked under
the lock and chained on a `struct mount_dead`, with any point it leaves
empty, and `path_unref(mp_path)`, `spoor_clunk(source)` and the frees run
after the lock is dropped (a Dev close hook may sleep). Survivors keep their
relative order, which IS the union
search order. The idempotent same-root call swaps nothing and sheds
nothing. `mount()` itself never sheds. Measured after the fix, same
probe: a shell in a login session holds **17** entries (15 of joey's,
login's `/home/<user>`, ut's `/tmp` bind), so a container's eleven at most
(`/dio`, eight fixed binds, `/net` and `/dev/tty` when granted) fit with
four to spare. By 2026-10-19 joey's share was 17 and the shell's 19, which
left a container's eleven two to spare under the array's 32, and the table
became dynamic (below).

**Order matters, and the text says which order.** Reachability is
evaluated once, at the swap, over the table as it stands. joey bound the
OLD devramfs root at `/bin` *after* its pivot, which before the shed
revived the boot generation as aliases — `/bin/proc`, `/bin/srv`,
`/bin/dev/cons` were live device trees. The shed left them bare synthetic
directories, and since B-1d the bind's source is the initrd's `bin/`, so
`/bin` holds no mount-point directory at all
([[dec-2026-09-25-initrd-bin-directory]]). One alias had a real user: `/hw/pci` worked post-pivot
only because the orphaned entry was keyed on devhw's `pci` child, which
the `/hw` re-graft made reachable again — ARCH had recorded that re-graft
as "a v1.x seam" the whole time it was working by accident. joey now
re-grafts it explicitly ([[sub-stratum-boot]]).

**The observable changes — three shapes, no others.** (1) An fd-relative
walk from a directory fd opened BEFORE the swap, into a tree the new root
cannot reach, no longer crosses the shed mounts and sees the underlying
directory. (2) A ROOT-relative walk through joey's post-pivot `/bin` bind
(the aliases above), moot since B-1d: that `/bin` has no mount-point
directory to walk. (3) A union dirfd whose point's entries were shed is a
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
alone would be safe in place — it is refcount + `kfree`, no close hook,
non-sleeping — but it releases with the rest, since the node holding the
`Path` is freed there.

**Nothing is allocated or freed under `ns_lock`** (the dynamic table,
below). `mount()` allocates the most it can need — two entries and a
point — before taking the lock and frees what it did not use after it;
`territory_clone` allocates the child's nodes outside the parent's lock;
every edit that removes nodes chains them on a `struct mount_dead`, which
`mount_dead_release` frees once the lock is dropped. The final strong
release is the one remover that takes `ns_lock` on a Territory no Proc
holds any more: a weak holder's render may hold it, so the release
detaches the table under it and frees the nodes after it.

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

The `g_proc_table_lock -> ns_lock` edge that `/proc/<pid>/ns` introduced
is gone (2026-10-19): a shed holds `ns_lock` for about a million steps, so
a reader waiting for it under the IRQ-masked table lock stalled every CPU
queued there, and devproc now renders after dropping the table lock
([[lock-territory-ns-lock]]). The `ns_lock -> slub c->lock` edge that
`path_unref`'s `kfree` under the lock once made is gone with the deferred
release. `kmalloc` under `dot_lock`
in `territory_clone` is sound: SLUB knows nothing of Territory, and
neither lock is taken from an IRQ handler.

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
and it is introspection-ONLY. Every keying decision (`mhead_find`, under
`mount_lookup`, `mount_member_at`, `mount_members_snapshot`,
`mount_is_point_id`, `would_create_mount_cycle` and `mount` itself) reads
a point's `(dc, devno, qid_path)` key; `territory_format_ns` is the only
reader of `mp_path` anywhere. A wrong, stale, or NULL `mp_path`
misreports `/proc/<pid>/ns` and nothing else.

`MountRefcountConsistency` ([[spec-territory]]) — every mount entry and
every `root_spoor` holds exactly one ref on its Spoor, maintained at
five sites: `mount` (bump), `unmount` (drop), `territory_chroot` /
`territory_pivot_root` (bump new, drop old), `territory_clone` (bump per
cloned entry + per cloned root), `territory_unref`'s last STRONG drop
(drop all; a weak ref never holds a Spoor). Four of the spec's five buggy configs are exactly the "forgot one
of these" classes.

## Error paths

Argument faults return; state faults extinct. NULL or corrupted-magic
Territory is an `extinction` at every entry point (a corrupted Territory
is a kernel invariant violation, not a caller error). NULL or
corrupted-magic Spoor arguments return `-1`. `spoor_ref` extincts on a
corrupted source, which is why the bump precedes every swap — the
extinct leaves state unchanged.

The refusals (`-2` full, `-3` cycle, `-4` allocation) take no ref, so a
rejected mount cannot leak one, and `-4` is answered before `ns_lock` is
taken, so it cannot have edited anything. A refusal that follows an
unlink cannot strand it: only an edit at the mount's own point unlinks
anything, and that edit has already freed at least the one entry the
mount adds (`starts_union` is false at a point that hosted a member).
Linking is infallible after the `spoor_ref`s, so no rollback exists or is
needed; `mount`'s exit extincts if a hashed point is left with no member,
the one state its paths are built never to leave. `territory_clone`'s
fallible steps are its node allocations and the `dot_path` `kmalloc`,
and each failure path is a full `territory_unref` of the child.

`SYS_GETCWD` deliberately accepts an OVERSIZED buffer: it computes the
cwd first, then requires only `len + 1 <= buf_len` and copies exactly
that many bytes. The pre-fix `buf_len > SYS_OPEN_PATH_MAX + 1 -> -1`
rejection broke the near-universal `getcwd(buf, PATH_MAX)` idiom and
surfaced as `make: getcwd: I/O error`
([[chg-2026-07-24-getcwd-oversized]]).

## Performance

The bind cycle check is fixed-point reachability: O(N²) worst case at
N = 8 binds (~64 inner iterations). The mount cycle check visits each
point reachable from the source's key once and looks up each of its
members' source keys by hash, so it costs the reachable members, at most
the table. Both run once per `bind`/`mount`, never on the resolution
path.

The shed finds a device instance's points by scanning every point (the
hash is keyed on the whole identity), so it costs (members of the reached
points) × (points): about a million steps at `PGRP_MOUNT_CAP`, all under
`ns_lock`, with preemption off and IRQs on. `pivot_root` is ungated, so a
Proc can repeat that hold at will on its own Territory. It stalls that CPU
and whoever waits for this Territory's `ns_lock`. Until 2026-10-19 that
included every CPU queued on `g_proc_table_lock`, with IRQs masked, because
devproc rendered `/proc/<pid>/ns` under it (audit round 1 F1). The cost
itself is enqueued (OPEN-BUGS, P3). A per-point chain of the points that
share an instance cannot make it linear: a source instance with no points,
hashed into a large instance's chain, still costs that whole chain per
member. The fix is to group the points by instance.

`mount_lookup` runs at EVERY component descent in `stalk` — it is the
hottest thing in this file. It hashes the probe's key to one of 64 chains
and takes the point's first member, so it costs one chain. Measured use
(19 entries in a login shell) is under one point per chain, and at the cap
the average is 16. The hash is not keyed, though, and a user's own 9P
server picks the qid.paths it serves, so it can put every point of its
Territory in one chain: the worst case is the table's points (OPEN-BUGS,
P3, with the shed). `mount_member_at` adds a walk to its index, and
`mount_noexec_covers` is a scan of every entry (a source's instance is not
a key), bounded by the cap.

`territory_format_ns` takes `ns_lock` with IRQs enabled, and asserts it.
A read renders one window: a line before the window costs a few
comparisons and no copy, the render stops at the window's end, and a
window is at most `DEVPROC_READ_BUF`, 2 KiB; no sleep, no allocation.
Until the dynamic table's 5d step that 2 KiB was also the listing's limit:
devproc rendered the whole file into it, so a namespace past about forty
entries was cut, a limit only long names had reached under the array. Now
the read's offset reaches the renderer, which serves that window of the
whole listing, as `/proc/<pid>/environ` already did for an environment
past the buffer (`devproc.ns_reads_past_the_buffer`,
`territory_mount.format_ns`). devproc renders after dropping
`g_proc_table_lock` and then asks the read's admission again
([[sub-kernel-devproc]]).

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
- **Nothing is allocated or freed under `ns_lock`.** A node allocated under
  it would add a slab edge and a failure path inside an edit; a node freed
  or a source clunked under it can sleep in a Dev close hook. `mount()`
  allocates first, every removal goes through a `struct mount_dead`, and
  `territory_clone` allocates outside the parent's lock.
- **A point's members appear in the table in their search order.**
  `/proc/<pid>/ns` prints table order as the union's order, and
  `territory_clone` rebuilds every point's order by appending in table order.
  Only `mount_link`'s `first` arm (MBEFORE at a point with members) inserts
  mid-table, and it inserts just before the point's old first member;
  anything else that inserts mid-table breaks the copy.
  `territory_mount_check` (rule 8) checks it, and
  `territory_mount.nodes_follow_every_edit` runs it after every edit.
- **Every point in the hash hosts a member.** `mount_lookup` reads
  `h->first->source` unguarded on that strength. `unmount` and the shed
  unhash a point they empty; `mount` relinks at the point it emptied, and
  its exit extincts if it did not.
- **No array is sized from `PGRP_MOUNT_CAP`.** At the cap such an array
  does not fit a 16 KiB kernel stack; a new consumer of a whole union or
  of the table takes the count and allocates, as stalk's `union_members`
  does, or walks the lists under the lock.
- **The visit marks.** A pass is valid only while every point starts it
  unmarked: `mvisit_next` clears every mark when the counter wraps, and a
  new pass must open with it.
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
  places by its ordering flag: `MBEFORE` links the entry first in the point's
  member list (Plan 9 prepend; a later MBEFORE goes ahead of an earlier one),
  `MAFTER` and the flagless default link it last. Members are searched in the
  point's list order — so MBEFORE members precede MAFTER members by
  construction, which is `OrderCorrect`. An existing pair re-mounted with a new
  ordering flag is MOVED, not duplicated (UM-8 F6). `unmount` unlinks, which
  keeps every other member's order. (While the table was an array, MBEFORE
  inserted at the index of the point's first member and `unmount` shifted down
  rather than swap-removed; the lists keep both orders the array kept.)
- **MREPL replaces the WHOLE group** (above) — the spec's `MountRepl` collapses
  a union to one member, so it must remove every member at the point, not just
  the first.
- **`mount_members_snapshot` — the atomic whole-union snapshot.** The resolver
  cannot walk a point's members one by one under a lock it must release to
  cross (a cross may block on 9P), so it snapshots the point's members + their
  flags ATOMICALLY under `ns_lock` into a caller array, then crosses them
  lock-free. The atomicity is load-bearing: a member shifting into slot k
  between two separate reads would land a create on the wrong member (the UM-8
  F4 hazard the snapshot closes). Ordered: MBEFORE members first,
  MAFTER/default last. It returns the point's member COUNT and takes the
  members only when the array holds them all: every caller needs the whole
  union, and a cut one would hide its later members. stalk's `union_members`
  ([[sub-kernel-stalk]]) holds 16 inline and retakes into a heap array sized
  from the count when a union is larger.
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
  && (mountpoint->qid.type & QTDIR) && !h`, where `h` is the point hashed at
  the mount point's key (a point is hashed only while it hosts a member),
  judged BEFORE the UM-8 F6 reposition: re-mounting the sole member of an
  `MREPL` group with `MBEFORE` finds the point hosting a member, so it grows
  no covered one (`territory.tla` `BUGGY_FRESH_AFTER_REMOVE`). `MREPL` wins
  over the ordering flags and replaces the whole group, covered entry
  included; a flagless mount adds none. No union starts at a file point
  (`NoCoveredFile`), and since the 2026-09-25 votes
  ([[dec-2026-09-25-mrepl-only-at-a-file]]) `SYS_MOUNT` installs only a
  replacement at a point that is not a directory:
  `sys_mount_for_proc` refuses Plan 9's `Emount` cases with `-T_E_NOTDIR`
  before the table op -- a source whose type (directory or not) differs from
  the point's, under any flag, and any mount but `MREPL` at a point that is
  not a directory. A flagless mount appends, so a second one at a file would
  make a two-member group there, which `stalk` treats as a union directory
  whose listing skips the files. Only `MREPL` of a file over a file stays
  legal, so `SYS_MOUNT` never gives a file point a second member
  (`sys_mount.refuses_a_type_mismatch`,
  `sys_mount.refuses_all_but_mrepl_at_a_file`). The check is made once, at
  install, on the point's own Spoor: a 9P server the caller attached can
  answer a later walk to the same point with the other type, so a directory
  point with several members can read as a file afterwards, and `stalk`'s
  use-time `QTDIR` gates stay. The key must also name the point alone:
  devsrv gives each posted service a `qid.path` of its own, since a mount at
  `/srv/<name>` was once keyed at the registry root and at every other
  service (`devsrv.service_keys_distinct`). The QTDIR test in
  `starts_union` stays for kernel-internal callers, whose boot mounts (joey's
  `/dev` and `/srv`) are all directories over directories (`territory.tla`
  `KERNEL_MOUNTS` and `CovGuard`; `territory_mount.covered_file_point_stays_plain`).
- **Order and room.** Both entries append in Plan 9's order (`mount_link`):
  `<new, covered>` for `MBEFORE`, `<covered, new>` for `MAFTER`. Later ordered
  mounts place around the covered entry like any member. The first mount
  needs room for two entries under `PGRP_MOUNT_CAP` (`-2` otherwise, nothing
  installed), and `mount()` allocates both before taking `ns_lock`.
- **The one retained point.** The covered entry's source IS the mount-point
  Spoor, `spoor_ref`'d before install. `unmount`, `MREPL`, the shed and
  `territory_destroy` drop it exactly once and `territory_clone` refs it, as
  for any source.
- **Never crossed.** [[sub-kernel-stalk]]'s `stalk_cross_src` answers
  `MCOVERED` with a clone of the point, and a mount-over-mount chain stops at
  a point whose member[0] is covered. It is therefore not an edge of the
  mount graph (`would_create_mount_cycle` reaches no new point through it),
  and I-3 holds over the edges that are crossed.
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

## The dynamic table (2026-10-19) — Plan 9's `Mhead` and `Mount`

The fixed array is gone ([[dec-2026-10-19-mount-table-dynamic]]). Use had
reached its 32: a login shell held 19 entries before any mount of its own, a
container's recipe adds up to 11, and a gate's seventh `bind -bc` layer was
refused. The operator voted Plan 9's shape under a DoS cap (ARCH 9.1).

- **Shape.** One `PgrpMount` per mount under one `PgrpMhead` per mount
  point, as Plan 9's `Mount` hangs off its `Mhead`. A point is found by
  hashing its key into `Territory.mhash[PGRP_MOUNT_HASH]` (64 chains; Plan 9
  hashes into a 32-chain `mnthash`), holds its members in search order, and
  is hashed only while it hosts one. Every entry is also on the table-order
  list, `mfirst` .. `mlast`, in the order the mounts were placed: MBEFORE at
  a point with members places the entry just before that point's first
  member, and every other placement appends. So a point's members appear in
  the table in their search order, the order the array kept, and
  `/proc/<pid>/ns` prints the same text it printed from the array.
- **The cap.** `PGRP_MOUNT_CAP` is 1024 entries, a DoS bound in the shape of
  Linux's `fs.mount-max` (100,000 per mount namespace), not a usage limit.
  At the cap a Territory holds 1024 × (64 + 64) B = 128 KiB of mount nodes,
  copied at every spawn from it: 1/32 of the per-Proc VMA ceiling
  (`PROC_VMA_MAX`, 4 MiB of Vma slab). A mount past it answers `ENOSPC`.
  4096 was weighed and refused: 512 KiB per Territory, and a shed of about
  16 million steps.
- **Allocation and release.** `mount()` allocates two entries and a point —
  the most a union's start needs — before taking `ns_lock`. A failure is
  `-4` (`ENOMEM`) with nothing changed, and whatever goes unused is freed
  after the unlock. What an edit unlinks (a reposition, MREPL's group,
  `unmount`'s member and covered entry, the shed's dropped entries) is
  chained in removal order on a `struct mount_dead`, with any point it left
  empty, and `mount_dead_release` drops each entry's `mp_path`, clunks its
  source and frees the nodes once the lock is dropped. The node caches are
  created without `KMEM_CACHE_PANIC_ON_FAIL`: an allocation failure refuses
  a mount or a spawn, never panics.
- **Passes, not arrays.** The cycle check and the shed walk the points as a
  graph. `mvisit_next` opens a pass by bumping `Territory.mvisit`; when it
  wraps it clears every point's mark first. A point reached is marked with
  the pass and queued on its own `work` link, so neither check allocates,
  and a root swap cannot fail. A heap-allocated reach set was the other
  candidate; it would have given `chroot` and `pivot_root` an allocation
  failure they do not have.
- **Readers.** `mount_lookup` (the point's first member),
  `mount_member_at` (a walk to the index), `mount_members_snapshot` (the
  count; all members or none), `mount_is_point_id` (a hash lookup),
  `mount_noexec_covers` (a scan of the table list) and `territory_format_ns`
  (the table list) all hold `ns_lock`. `territory_nmounts` and
  `territory_npoints` read a count without it, for diagnostics only.
- **Diagnostics.** `territory_mount_check` walks the table under `ns_lock`
  and returns the number of the first of nine rules it finds broken: the
  table list, the counts, hash placement, the member lists and their
  heads, the member sum, search order against table order, and the covered
  entry's form. `territory_mount_entries_live` / `_points_live` read the
  node caches' live counts. `territory_mount_fail_nodes_for_test(n)` fails
  the next `n` node allocations, after the `p9_closer_fail_nodes_for_test`
  precedent.
- **Tests.** `territory_mount.nodes_follow_every_edit` makes nine edits and
  checks after each the return, the rule check, the point count and the
  exact `/proc/<pid>/ns` text; then the snapshot's short and full legs, a
  clone's order, an MREPL's refs, a shed, and the live counts back where
  they started. `territory_mount.refuses_enomem` fails each of a mount's
  three allocations and then a clone's, each with its control.
  `sys_mount.enomem_answers_enomem` takes `ENOMEM` through SYS_MOUNT's
  errno map. `stalk.union_past_the_inline_snapshot` builds a 17-member
  union and walks, creates and opens through it. `territory_mount.table_full`,
  `territory_mount.covered_needs_two_slots`, `territory.shed_full_table_boundary`
  and `sys_mount.full_table_answers_enospc` run at the real cap, on heap
  arrays.
- **Specs.** `territory.tla` and `territory_shed.tla` model each point's
  member sequence and the shed's closure, not the storage, so neither
  changes; both re-run with their buggy configs before the merge.

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
