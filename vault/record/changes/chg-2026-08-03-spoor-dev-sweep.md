---
id: chg-2026-08-03-spoor-dev-sweep
type: chg
title: "the Spoor/Dev substrate -- a clone inherits five flags and the safety of four is accidental"
date: 2026-08-03
arc: arc-vault
commits: []
touched:
  - sub-kernel-spoor
  - sub-kernel-dev
  - moc-kernel-namespace
  - inv-i33
  - sub-kernel-path
established:
  - sub-kernel-spoor
  - sub-kernel-dev
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-03
---
Batch 33, the fifth sweep off the census: the Spoor/Dev substrate --
`kernel/spoor.c` + `spoor.h` (582 lines, the Plan 9 `Chan`),
`kernel/dev.c` + `dev.h` (588, the vtable and the bestiary), and the
trivial Devs (`devnone`, `null`, `zero`, `full`). Main had moved to
`a6b982cf` (#128); merged before starting. L-1 absent on the
TWENTY-FIRST check. Two dossiers, no lock note -- the only lock on this
surface is dead (F4).

Both dossiers went under [[moc-kernel-namespace]] rather than a new area.
[[sub-kernel-path]] was already a child, and `Path` is a FIELD of
`Spoor`; a substrate cannot live in a different area from its own field.

**THE HEADLINE IS ONE LINE OF CODE.** `spoor_clone` copies the flag word
through a mask with exactly one exclusion:

```c
nc->flag = c->flag & ~CWALKONLY;   // #81: never inherit the nav-only marker
```

The comment argues from `CWALKONLY`'s own semantics -- it is a
per-final-handle marker, so a child created from an `O_PATH` base must
not inherit it and reject its own legitimate I/O. That reasoning is
correct and specific. It is also the ONLY reasoning present: five other
flags inherit, and nothing anywhere says why that is safe.

Asking the question the mask invites -- *what else does it not exclude?*
-- finds that `COPEN` inherits, so **`COPEN` does not mean "this Spoor
was opened"**; it means "this Spoor, or an ancestor it was cloned from,
was opened". Its two consumers land on opposite sides of that.

**F1 -- A FAILED WALK DISARMS THE LIVE CONSOLE TAP.** `devdev_close`
gates the console-drain disarm on `qid.path == DEV_KIND_CONSDRAIN &&
(flag & COPEN)`, and its comment calls the COPEN check load-bearing:
*"only the Spoor that actually minted through `devdev_open` carries
COPEN, and there is exactly one at a time"*. False. Every step of the
chain is in-tree: `SYS_WALK_OPEN(consdrain_fd, "x")` clones (COPEN and
the consdrain qid come along), `devdev_walk` takes the reuse-`nc` branch
and re-stamps the qid, `walk_one` misses from a leaf and returns a
NON-NULL Walkqid with `nqid == 0`, the handler's `nqid != 1` exit calls
`spoor_clunk(nc)`, and the close hook fires `cons_drain_close()` on a tap
the renderer still holds an fd for -- the framebuffer console stops
receiving output with no error anywhere.

Latent and self-inflicted (only the bound renderer can hold a consdrain
fd; aurora does not walk from it), but the guard's stated rationale is
false as written, on the I-27-adjacent console surface. Task #74.

The other COPEN consumer is the counterexample that makes the point:
dev9p's dir-fid park gates on `COPEN == 0`, so the same spurious
inheritance only declines to park a parkable fid. **One inherited flag,
two consumers, opposite outcomes, neither reasoned about at the clone.**

And the remaining three flags ARE safe -- for reasons belonging to
entirely different mechanisms. `CDEBUGOWNER` survives because the debug
release walk matches `debug_owner` on POINTER IDENTITY, and a clone is a
different pointer; the comment justifying that choice cites pid reuse and
post-reap staleness, never clones. `CCONSWINSZONLY` survives because it
is restrictive, so inheriting it fails safe. `CSRVCLIENT` survives
because `devsrv_walk` refuses a non-registry source outright. **Five
flags inherit; the safety of four is accidental relative to the rule that
produces it.**

**F2 -- THREE FAILURE PATHS, TWO OF THEM EXPLAINED.**
`sys_walk_open_handler` has three post-clone exits. Two detach `nc->aux`
before releasing, with the reason written down: *"Calling `dev->close` on
`nc` would clunk src's fid through the shared aux -- wrong."* The third,
the partial-walk exit, calls `spoor_clunk(nc)` with the same shared
`aux` and says nothing.

It is safe today, and I checked every Dev that can reach it rather than
assuming: six use `aux` nowhere at all, devctl's close only clears
`COPEN`, dev9p returns NULL rather than a partial, and devsrv both
refuses a non-registry source and normalizes `nc->aux = NULL` on entry --
with its rationale written down, which is the one place this hazard is
actually documented. So the third path depends on an unwritten,
untested constraint on all future Devs, and devsrv is the standing proof
that a Dev CAN hold a refcounted connection in `aux`. Task #75.

**F3 -- THE HEADER TEACHES THE SHAPE THAT CAUSED THE BUG.** `dev.h` says
*"trivial leaf Devs (devcons / devnull / devzero / devnotes) leave the
`.stat_native` slot NULL"*. devcons implements it (#55); devnotes
implements it (#97). What lifts this above a stale comment is why both
changed: a Dev without `stat_native` fails `SYS_FSTAT`, and clang treats
a non-EBADF fstat failure on fds 0/1/2 as fatal -- which silently killed
every concurrent `make -j4` job. The paragraph still teaches the
bug-producing shape by worked example, to the one audience positioned to
repeat it: the next author of a leaf Dev. Task #19 (`/dev/winsize`, same
family) is open, which is evidence the guidance is being followed in the
wrong direction. Task #76.

**F4 -- A RESERVATION THAT OUTLIVED ITS DECISION.** `struct Spoor` has a
`spin_lock_t lock` reserved "so the SMP-safe refcount upgrade (Phase 5+)
doesn't need a struct change". The upgrade shipped as atomics and never
used it: `spin_lock_init` is the only reference in the tree, executed on
every allocation and clone on the hot walk path, acquired nowhere. The
header still calls the Spoor "Single-CPU at v1.0" and lists the atomic
refcount as future work, while `spoor.c` documents it as done five
times. Task #77. (Same family as #60, three headers calling `vma_lock`
future work when it has existed since #713.)

**THE COUNTERWEIGHT, AND IT IS THE BEST STRUCTURAL IDEA THE SWEEP HAS
FOUND.** `dev_register` refuses to boot a Dev that exposes
`.wstat_native` without `.perm_enforced`. Since #47 made `SYS_WSTAT`'s fd
gate kind-only, `perm_wstat_check` -- which runs only when
`perm_enforced` is set -- is the SOLE write-authority check on the
metadata path, so that combination would let any handle holder rewrite
mode/uid/gid with no identity check. Rather than write it down as a rule
for Dev authors, registration makes it un-shippable. **A vtable-shape
constraint caught at registration instead of at the call site that would
misuse it** -- exactly the inversion F1 and F2 are missing.

Two smaller ones worth keeping. `devnone`'s documented role is an audit
guard and nothing checks for it; it does not need checking, because every
op returns NULL or -1, so a Spoor that reached it by mistake fails
everything rather than doing something plausible with the wrong backing
-- **enforcement by being uniformly useless**, the right shape for a
sentinel. And `dev.vtable_slot_coverage` asserts all 16 mandatory slots
on every registered Dev, which is what makes the nine "NULL-permitted"
comments trustworthy.

**A SMALLER OBSERVATION.** That same test ends with `devs_checked >= 8`,
its comment enumerating the eight Devs of P4-A..P4-E. Eighteen are
registered now. Strong per-Dev assertions sitting above a count
assertion that would pass with ten Devs missing -- an assertion
satisfiable by a substantially broken system, in the same function as
one of the better structural tests in the tree.

**PATTERN, TEN BATCHES.** b24 assertions pin values not their
description; b25 models pin mechanisms not their own scope; b26 each copy
pinned to itself not to the others; b27 the guard travelled but not its
reason; b28 the ledger pins the areas not the areas to the tree; b29 the
enforcement list names a guard that cannot fire; b30 plus a justification
whose stated and real reasons diverged; b31 the documents are wrong about
which code runs; b32 the guard is right about the case it was written for
and silently wrong about the case nobody asked it; **b33 the exclusion
list has one element and one reason, and the five unexcluded cases are
safe by mechanisms that were built for other questions.**

b32's through-line was a mechanism argued and tested against the case
that motivated it, with the adjacent case inheriting the mechanism but
not the argument. b33 sharpens it one turn: here the adjacent cases are
mostly CORRECT, and the reasoning for why is absent. That is the state
that decays silently -- nothing fails, so nothing prompts anyone to
write the reason down, and the day a mechanism changes for its own
reasons the unrelated thing depending on it breaks without warning.
`CDEBUGOWNER` is one refactor of the debug release walk away from being
F1's twin.

LEDGER. Corpus 816 -> **819**. Coverage 150 -> **158 owned of 421
(37%)**; `kernel` 31 unowned -> 23. [[inv-i33]] gains
[[sub-kernel-spoor]] as a guard -- its Enforcement section already named
`kernel/spoor.c` and three of its hooks, so the invariant had been
describing a file no dossier owned.
