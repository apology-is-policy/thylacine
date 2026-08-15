---
id: chg-2026-08-15-joey-boot
type: chg
title: "joey re-swept after Warp-2: a mount deliberately not made, and the scope of what this dossier owns"
date: 2026-08-15
arc: arc-vault
commits: ["*(pending)*"]
touched: [sub-stratum-boot]
established: []
closed: []
opened: []
mirrors-checked: [usr/joey/joey.c, kernel/devdev.c]
depth: rich
created: 2026-08-15
---
Eleven commits on `usr/joey/joey.c` since the dossier was written, +1510 lines.
Almost none of them touched the sequence the dossier describes.

## The nine contract steps are unchanged

Verified rather than assumed: the readiness token, the loose-mode attach flag,
and the seven carried O_PATH handles (`/srv`, the boot root for `/bin`, `/proc`,
`/ctl`, `/dev`, `/hw`, `/env`) are all exactly as recorded. Of the eleven
commits only one touched the pivot region at all.

## What is new: a mount deliberately *not* made

That one commit is the interesting one, and it went in both directions.

The compositor daemon posts two services. `Warp-2c` mounted both — the
compositor tree onto `/dev/tapestry` and the GPU seam onto `/dev/warp`. The
Warp-2 audit then rated the second a **P1** and removed it.

The reason generalises past this file. The GPU seam's authority keys on the
**connection**: the owning connection gates every context and buffer resolve,
and one context per connection *is* the [[inv-i45]] exposure bound. A shared
mount is one server-side connection. So mounting it once for everyone aliased
every process on the box onto init's single connection — one process could
submit an arbitrary command stream into another's rendering context, read its
buffers back, or destroy them, and no second process could obtain a context at
all.

Same daemon, two services, opposite mount treatment, and the discriminator is
where the served tree's authority lives. That turns the dossier's mount rule
from a checklist into a decision, and the prosecution list now carries the
complement: **before asking how to carry a mount across the pivot, ask whether
it should be global at all.**

Two details of the corrected shape are recorded with it: the boot probe reads
the seam's control file over the connection it just opened (walking the returned
descriptor, never a namespace path) so no context is minted and the hazard never
arises transiently; and init holds no standing connection afterwards, because a
second listener sharing the daemon's pool had already starved the compositor's
own listener — eight opens against eight slots — now bounded per-root.

## The near-miss worth recording

I found the `/dev/warp` mount by reading `git show` of the commit that added it,
and the kernel's device-directory comment says init "deliberately never mounts
over it". That reads as a flat contradiction between code and comment, and it
was one sentence from being written up as a finding.

It is not a contradiction. The mount was **added** by one commit and **removed**
by a later one; the current tree has the comment and the code in exact
agreement. The diff was three commits stale and I had treated it as the present.

Second time in two sweeps that a *derived* view nearly produced a false finding —
the first was a word-unbounded grep matching `sid` inside `ASID`. Both were
caught the same way, by going and looking at the current artifact instead of the
thing that described it.

## The scope finding

[[sub-stratum-boot]] is the **sole owner** of `usr/joey/joey.c`, which is **9771
lines and about fifty functions**. What the dossier describes — bringup, spawn,
readiness, attach, pivot, re-graft — is a few hundred of them, and its title says
so honestly.

The remainder is init's other work, and the vault describes none of it: the
long-lived-daemon registry and adopted-orphan reaper; ~850 lines of
identity-daemon bringup and its wire protocol; the boundary-line smoke suite; the
exec, fork and foreign-shell gates; the login and recovery end-to-end runs; the
**session getty loop, which is what init actually spends its life doing**; the
toolchain and GL gates; and seven-plus numbered regression probes. Grepping the
whole vault for the terms naming these returns **zero notes** for the
foreign-shell gate, the identity-daemon bringup, the smoke suite, the toolchain
gate and the reaper. (Two apparent hits for the session loop are the harness and
the banner ABI; three for "orphan reaper" are the compositor's weave reaper — the
same words, a different subsystem.)

This is the batch-35 "ownership is not possession" shape again, but it now has
teeth it did not have then. The ratified [[dec-2026-08-15-cutover]] reads
`quaestor owner`'s exit 0 as *"the vault carries that surface, so the prose
belongs there."* For this file the tool answers OWNED, and for roughly
eight-ninths of it that answer routes a writer to a dossier with nowhere to put
what they know.

**That makes it the first measured weak point in the protocol ratified
yesterday** — which is worth stating plainly, because the protocol's correctness
is exactly as good as the ownership record it consults. Tracked as task #177, the
userspace twin of #119.
