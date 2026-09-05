---
id: chg-2026-08-15-diorama-vivarium
type: chg
title: "diorama re-swept: a second operating mode the dossier never saw, and a read buffer half its source"
date: 2026-08-15
arc: arc-vault
commits: ["2a0c3699"]
touched: [sub-diorama]
established: []
closed: []
opened: []
mirrors-checked: [usr/diorama/src/server.rs, usr/diorama/src/main.rs, kernel/devctl.c, kernel/devproc.c, kernel/proc.c]
depth: rich
created: 2026-08-15
---
Five commits, +657/-134, and they did not arrive the way the churn figure
implies: every one of them is dated July, and they landed on this branch in
one merge on 2026-08-05, the day after the dossier was written. So this is
not a dossier that decayed — it is one that was **born describing a version
of the file the branch was about to replace**. The `--first-parent` dating
rule earns its keep here: the stale tool was right about *when it became my
problem* and would have been badly wrong about *when it was written*.

## The largest miss is a whole second operating mode

`--vivarium <runner-pid>` (V-7) makes the server per-container: it posts
`/srv/viv-dio` rather than `/srv/diorama`, and both pid **enumeration** and
per-pid **existence** answer only for the container's process tree.
Membership is ppid-descent, and the entrypoint is **located rather than
passed** — it is the runner's child that is not this server — because the
entrypoint does not exist yet at the moment the diorama must already be up to
serve the pre-spawn territory mounts. Before it exists the set is **empty**,
fail-closed.

[[sub-diorama]] recorded the opposite as a seam: *"Pid visibility is not
container-scoped… scoping it here alone would be theatre, since a contained
process that can reach native `/proc` would read around it."*

The premise was the error. A container's **territory** is what withholds
native `/proc`; the diorama is mounted *inside* that territory, so scoping it
is not theatre but the closing of the last hole — the code's own words are
"so the diorama cannot be a read oracle for the surface the container's
territory withheld," citing the section 7.1 F6 audit close. The dossier
reasoned about the mechanism correctly and about the **surrounding
containment** wrongly, which is the more dangerous half to get wrong: it
produces a confident argument for not building the thing.

## The environ caveat's trigger fired, and the answer is worth more than the prediction

The dossier's caveat read: *"If a per-container instance ever runs as its
container's principal… the per-pid form becomes servable — and until then the
absence is the whole protection."*

A per-container instance landed. It does **not** run as the container's
principal, and `/proc/self` stays peer-based and unfiltered, so a non-member
reader reads only itself. The code now names that outcome — *"the
/self/environ authority-coincidence argument, never a cross-boundary leak"* —
which is the dossier's own reasoning arriving in the source under its own
name. The caveat was right about the trigger and right to name what would
change; what it could not predict is that the design would take the third
option, keeping `self` peer-scoped *while* filtering everything else.

## The attach gate, and what fails open without it

`/srv/viv-dio` is a **fixed** name, so it is first-come-first-served: a second
concurrent `viv run` lands on the **first** container's server, and ungated it
would mount container A's `/proc` into container B. That fails **open**.

Two details are the interesting part. The check cannot live in the runner —
at the point the runner holds the connection it has nothing to compare
against, since `SYS_SRV_PEER` is server-side only, the registry's poster pid
is never exposed to EL0, and every diorama's member set is equally empty
pre-entrypoint. And it gates **attach**, not each op, which makes the
cross-mount *impossible* rather than merely detectable: every fid descends
from the attach root, so the refusal fails the opener's `SYS_OPEN` outright.

## The finding: a mirror that was true when written

`server.rs:870` reads `/ctl/procs` into `[0u8; 2048]` with the comment
**"matches the kernel's DEVCTL_READ_BUF"**. `kernel/devctl.c:817` says
**4096**.

It was true when written. The constant went 512 -> 2048 at prowl-1
(2026-07-23), the diorama landed against 2048 (2026-07-27), and #210 lifted
it to 4096 (2026-08-11) for the `/ctl/9p-sessions` instrument — a change with
no reason to open this file. [[chg-2026-08-14-merge-fold-124]]'s rule,
landing again: **a lifted constant voids every proof that named it.**

Two consumers ride the 2048 window, and the second is the one that matters:
`viv_read_pairs` feeds `compute_members`, so it decides container membership
— enumeration *and* per-pid existence. The arithmetic makes the cost exact
rather than estimated: rows carry 29 bytes of fixed separators plus eight
fields, so the shortest possible row is 43 bytes against 1977 usable, an
absolute ceiling of **45 rows** — which means `VIV_PROCS_MAX = 64`, the
declared capacity of both the pair table and the member set, is
**unreachable by arithmetic**, and would be reachable at the kernel's actual
4096. The stale mirror is precisely what keeps the declared capacity
fictional.

**The security claim survives and should be said plainly**: the callback
stops at first overflow and `compute_members` only ever adds pids present in
the snapshot, so a lost row can only *narrow* the set. The code's "never the
reverse" is correct.

The correctness consequence is worse than a missing tail, because
`proc_for_each` is a **pre-order DFS from kproc**, not a pid scan. The
container's rows sit at whatever position the runner occupies in the tree, so
a cut landing before the runner's subtree empties the member set entirely —
the container's `/proc` shows nothing, including its own siblings, while
`/proc/self` keeps working. The trigger is how many processes precede the
runner in tree order, so it degrades silently and with host business. Task
#182.

**And this one came with its own control.** `server.rs:1628` is
`[0u8; 2048]  // matches the kernel's DEVPROC_READ_BUF` — identical idiom,
same file, 758 lines apart — and it is **correct**: `DEVPROC_READ_BUF` has
been 2048 since 2026-05-07 and never moved. Two mirrors written the same way,
one right and one wrong, and nothing about the writing distinguishes them.
The difference is entirely external — which kernel constant happened to get
lifted — which is the argument against the idiom rather than against the
author.

## Three smaller mechanisms the dossier predates

- **The offset contract (#72).** `render`'s contract is now one sentence for
  every node — "the bytes at `[off, ...)`" — held by `drop_prefix` for the
  files that build themselves whole and by a real positioned read for
  `environ`. The reasoning is explicit and good: *"a caller that had to know
  which is a caller that can get it wrong."* `read_native_at` is deliberately
  **not** a drop-in for `read_native`, because `t_pread` fails `ESPIPE` on a
  Dev that is not `.seekable` and devctl is not — so every `/ctl` source must
  keep the cursor reader, and devproc's seekability is what makes the one
  positioned caller legal.
- **`environ` stats as size 0**, and the reason is the self-sufficient one:
  its content is a window, so there is no total without reading the whole file
  on every stat, and the pre-#72 answer — the *truncated* length — was a lie
  the moment an environment passed 4 KiB. Deliberately not extended to the
  rest, since a stat that agrees with its read beats symmetry with a zero.
- **Nothing is a child of a file (#71)**, `.` and `..` included — four sites,
  and the fourth is the cache leaf that the dossier's own enumeration caveat
  already records as having read as an empty directory.

## A narrowing that did not happen

Unlike [[chg-2026-08-15-build-targets]], nothing was dropped from `code:`
here: all three claimed paths are genuinely this dossier's, and
`Cargo.toml` is small enough that its absence from the churn list is honest
rather than a false claim.
