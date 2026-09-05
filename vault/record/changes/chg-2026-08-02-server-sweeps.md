---
id: chg-2026-08-02-server-sweeps
type: chg
title: "the two server sweeps -- a template copied without its fixes, and a conservation clause that covers one of two arms"
date: 2026-08-02
arc: arc-vault
commits: []
touched:
  - sub-ptyfs
  - sub-tapestryd
  - spec-pty
  - spec-tapestry-present
  - inv-i20
  - inv-i40
  - moc-userspace
established:
  - sub-ptyfs
  - sub-tapestryd
  - spec-pty
  - spec-tapestry-present
  - inv-i20
  - inv-i40
closed: []
opened: []
depth: skeletal
created: 2026-08-02
---
Batch 27, the last two subsystem sweeps: `usr/ptyfs` (2,168 lines) and
`usr/tapestryd` (6,746). Both were owed since batch 25, which could not
write their spec notes — a spec note whose action-site map points at
unread code is a hollow record. Main had moved to `#102` during batch 26;
synced first. L-1 checked for the FIFTEENTH time and still absent.

Two dossiers, two spec notes, and the two invariants they realize —
[[inv-i20]] (pty atomicity) and [[inv-i40]] (the shared-pixel-page
lifetime) — neither of which existed, because the invariant notes are
minted by the sweep that reads their enforcement.

**F1 -- A TEMPLATE COPIED WITHOUT ITS FIXES.** tapestryd's header says
its "Conn/fid table, frame extractor, dispatch, deferred replies, and
the 4-site cancel discipline are the audited ptyfs shapes." The frame
extractor is verbatim. The fid table is not: `h_walk` carries **neither**
the `newfid == P9_NOFID` reject nor the `newfid already in use` reject.

Both siblings have both. netd has them because they *are* its `net-4d`
F2 fix; ptyfs inherited them. tapestryd's `h_attach` carries the NOFID
half — so the fix travelled to one handler and not the other.

The consequence here is small: tapestryd's fids hold no refcount, so a
silently-clobbered binding leaks nothing, and a NOFID binding wastes one
of 32 slots the client may then address as fid `0xFFFFFFFF`. P3, exactly
what netd's F2 got. Task #47.

It is worth fixing anyway because of what those same two lines do next
door. In ptyfs, "reject a walk to an in-use newfid" is **link 3 of a
four-link chain that makes `HupAtMostOnce` true**: masters are mint-only,
no walk resolves a master path, 9P forbids walking from an opened fid,
and a walk to an in-use newfid is rejected — therefore at most one master
fd per pts can ever exist and the carrier-loss edge fires at most once.
A guard that reads like protocol hygiene is load-bearing for a safety
property one server over, and it is exactly the guard the descendant
dropped.

Same handler, one screen down: tapestryd's `h_version` sets
`version_done` unconditionally and answers `9P2000.L` to *any* proposed
version, where ptyfs answers `unknown` for an unsupported one. Inert —
the only client proposes `9P2000.L` — and the same shape.

**F2 -- THE CONSERVATION CLAUSE COVERS ONE OF TWO ARMS.** `pty.tla`'s
`CookData` is guarded `m2s < CAP`: in the model, a cook onto a full ring
does not happen. That is back-pressure, and it is precisely what
`master_write`'s **raw** arm does — `ring_push` returning 0 breaks the
loop without consuming, so the short `Rwrite` makes the writer retry.

The **cooked** arm does the opposite, deliberately: a byte past
`LINE_MAX` is consumed and dropped un-echoed, and a line flush into a
full `m2s` discards the tail (`let _ = ring_push(...)`, the result
ignored). `echo()` drops on a full `s2m` unconditionally, where the model
guards `ECHO => s2m < CAP`.

Nothing is wrong in the code. Those are the classic tty-overrun
semantics, the kernel console reference drops the same way, and the
docstring of the very function says so in two sentences.

What is wrong is the claim above it. `SPEC-TO-CODE.md:807` has
`CookData` pinning *"`RingConserved`: every consumed non-signal byte is
ring data (assembled-then-flushed or raw)"* — false in both cases, with
the parenthetical naming the assembly whose overflow breaks it. The
module header calls leg (1) *"no byte lost/torn/duplicated across the
cook."* Task #48.

**One spec action, one function, two branches of opposite overflow
behavior** — and a one-row-per-action map has no way to say which branch
it modelled. Which is batch 25's shape (the models pin the mechanisms,
nothing pins the model's own scope) sharpened: this is not a mechanism
the model omitted, it is a behaviour the model's invariant *forbids* and
the shipping code deliberately performs.

**F3 -- THE GUARD THAT CANNOT FAIL LOUDLY.**
`tapestry_present.tla`'s `ServerRelease` is gated on `intransfer = 0`.
In the implementation that is discharged **by construction**: the virtio
command engine is synchronous, so a present's transfer window opens and
closes inside one 9P dispatch and the in-flight set is provably empty at
every retire decision point. There is no drain because nothing can be in
flight.

So the model's most important guard is the one with no corresponding
code. A pipelined controlq — the obvious performance lift — would not
make `ServerRelease` *false*; it would make it **unimplemented**,
silently, with the spec still green, because the spec is not the thing
that changed. Both source headers warn about it; [[spec-tapestry-present]]
and [[inv-i40]] now carry it as the gate condition, which is "did the
construction survive," not "did the model change."

**F4 -- TWO SMALL DIVERGENCES IN THE SHARED PARSERS.** ptyfs's
`parse_dec` rejects leading zeros with the comment *"one canonical name
per pts."* tapestryd's accepts them, so `surface/007` and `surface/7`
name one surface while readdir emits only the canonical form. And
readdir ordinals in both are positions in a live table, so a mint or free
between two paginated calls shifts them — the netd precedent
dispositions that as a benign listing artifact. Recorded as caveats, not
tasks.

**COUNTERWEIGHT.** Both servers are unusually good, and the honest
things in them are worth naming as loudly as the findings.

ptyfs's `FILE_RW` constant carries an eleven-line comment stating that
its mode is `0666` SYSTEM-owned, that the pts registry gates only the
controlling-terminal syscalls and never slave I/O, that consequently any
Proc which can name a live pts can read and inject into it, that this is
inert at single-session v1.0 and live under concurrent multi-user, and
what the fix needs. That is a security gap documented against its own
author's interest, at the constant that causes it.

And the `#31` fix in `gpu.rs` records not just the change but the
*inference* that failed: an interrupt is a hint, the used ring is the
authority, and reading `used.idx` once on the first wake turned a benign
timing event into a permanent engine desync — `seq` diverging from the
device's avail consumption, every later command reading its own zeroed
response buffer.

**THE PATTERN ACROSS FOUR BATCHES.** Batch 24: the assertions pin the
values, nothing pins their description. Batch 25: the models pin the
mechanisms, nothing pins the model's own scope. Batch 26: each copy is
pinned to itself, nothing pins the copies to each other. Batch 27: **the
guard travelled with the template; the reason for the guard did not.**

All four are one shape — a guard whose subject is narrower than its
apparent claim — and F1 adds the transmission mechanism. A fix lands in
the file that was audited. The *reasoning* is general, the fix is local,
and the next thing derived from that file inherits the structure without
the correction. That is batch 26's F4 (the `offset_of!` defence applied
to the audited struct, not the class) at the granularity of a whole
server.

PROBE. On the vault's own record, since F1 is about a guard not
travelling: **does the linter check that a `models:` target exists?**
Point a spec note's `models:` at a nonexistent dossier id and it does:

```
FAIL vault/specs/spec-pty.md: models -> unknown id 'sub-does-not-exist-at-all'
```

That is exactly the check batch 26's R6 mirror probe wanted and did not
have, in a different field. `mirrors:` is free text naming paths in the
build tree, which the linter cannot see; `models:` names ids inside the
corpus, which it can. The rule is not that the vault is careless about
one and careful about the other — it is that **a reference the checker
can resolve gets checked, and one it cannot does not.** F1's shape
again: the guard exists where it was cheap.

The same run made the point twice, unplanned. Both dossiers were
written freeform and **failed** on the required-section schema, then
**warned** on section *order* — a structural check the linter enforces
hard, because structure is something it can see. It has no opinion
whatever on whether the prose under `## Concurrency` describes
concurrency. The vault checks what is checkable and trusts the rest, and
the honest version of that sentence is the one worth keeping: the
linter is a shape checker, and every claim in this batch rests on
reading, not on its greenness.

LEDGER. Corpus 789 -> **796**.

**The subsystem sweep is COMPLETE** — every kernel subsystem and every
`usr/` service now has a dossier, ptyfs and tapestryd being the last
two.

**Spec coverage is COMPLETE** — `view-spec-coverage` goes 31 dossiered /
2 missing to **33 dossiered / 0 missing across 33 modules**. `pty.tla`
and `tapestry_present.tla` were the two, and both were blocked on
exactly this batch, because their action-site maps point into these two
servers.

Invariant notes gain [[inv-i20]] and [[inv-i40]] — minted here rather
than earlier because an invariant note is written from its enforcement,
and the enforcement was unread until now.

Absorption unchanged at 46/101/147. `135-pty-kernel.md`,
`136-ptyfs.md` and `139-tapestryd.md` now have somewhere to be absorbed
*to*, which is the next pass — and with the sweep and the registries
both done, absorption is no longer blocked on anything.
