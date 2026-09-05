---
id: sub-ptyhold
type: sub
title: "ptyhold -- the shared PTY master-hold, and the fd-ownership contract it encodes"
parent: moc-userspace-runtime
code:
  - usr/lib/ptyhold/src/lib.rs
  - usr/lib/ptyhold/Cargo.toml
audit: light
guarded-by: []
validated-by: [prose]
locks: []
hazards: []
abis: []
design: ["docs/PTY-DESIGN.md section 5", "docs/PTY-DESIGN.md section 10"]
created: 2026-09-05
updated: 2026-09-05
---
## Purpose

The mechanism every pts host performs, factored out: mint a pts (the returned
fd *is* the master), optionally seed the slave's winsize, and spawn a program
on the slave as its fd 0/1/2. Extracted verbatim from `/bin/ptyhost` (PTY-4)
so the kaua-term (the Halcyon per-tile terminal, KT-1) reuses the identical
master-hold rather than growing a second copy of a delicate sequence.

What differs between hosts is the relay policy over the master -- ptyhost
pumps raw bytes to an outer terminal, the kaua-term parses master bytes into a
cell stream -- and that stays in each host. This crate holds only the shared
mint / seed / spawn, and its whole value is that the fd-lifetime discipline of
that sequence is written down once and correct.

## Contract

`Master::mint()` opens the clone file and validates it, returning
`Master { mfd, n }` or a `HoldError`. `Master::seed_winsize(cols, rows)` and
the free `set_winsize(n, cols, rows)` set the slave size best-effort.
`Master::spawn_on_slave(argv)` opens the slave three times and spawns `argv`
on it as fd 0/1/2.

**The fd-ownership contract is the point of the crate, and it is exact.**
After `mint` succeeds the caller owns `mfd` and must `t_close` it when the
session ends (or on any later error); neither `seed_winsize` nor
`spawn_on_slave` closes it. `mint`'s own failure paths close the just-opened
fd before returning -- so a caller closes `mfd` only after a successful mint,
never after an `Err` from mint.

## Mechanism

**`mint` proves the fd is a master before handing it over.** It opens
`/dev/pts/ptmx` (the returned fd is the master), `fstat`s it, and checks the
qid against the ptyfs endpoint-qid contract (`PTS_FLAG | N<<8 | filekind`,
filekind 1 = master, PTY-DESIGN 5), extracting the pts index `n`. On the fstat
failure or a non-master qid it closes the fd before returning the error, so a
mis-minted fd never leaks.

**`HoldError` encodes the master-fd's state at each failure**, and that is the
caller's cleanup contract in the type: `Mint` -- no fd was opened; `Fstat` /
`NotMaster` -- the fd was opened then closed by mint; `OpenSlave` / `Spawn` --
the master fd is still open and the caller owns it. A caller that treats these
uniformly either double-closes or leaks; the variant tells it exactly which
state it is in.

**`spawn_on_slave` arms drain-then-EOF by construction.** It opens the slave
three times (one `File` per stdio slot) and moves all three into the child's
fd 0/1/2; each spawn slot consumes its `File`, and the parent's copies close
inside spawn. So the only remaining slave-opens belong to the child, and the
child's exit is what drops the slave-open count to zero -- which is what arms
the kernel's drain-then-EOF on the master (the I-20 teardown the master's host
relies on). An empty `argv` is guarded (it would index `argv[0]` and abort
under panic=abort); on any error the master is left open for the caller and
the slaves opened so far are closed by `File`'s `Drop`.

**`set_winsize` is standalone on purpose.** A party that holds only the pts
index -- the kaua-term's input thread reacting to a Resize record, not the
`Master` owner -- can set the size without a `Master`, via `/dev/pts/<n>ctl`.
It is best-effort, and a winsize change raises the kernel's TTY_SIG_WINCH ->
SIGWINCH to the foreground process group.

## Data structures

`Master { mfd, n }` -- the owning master fd + the pts index. `HoldError` (the
five failure states above). The qid consts `PTS_FLAG` and `PTS_FK_MASTER`.

## Concurrency

None inside the crate. It is written, though, to be called from two parties:
the `Master` owner (mint/seed/spawn) and a separate winsize-setter (the free
`set_winsize`, which needs only the index) -- so a host's input thread can
resize without touching the owner's `Master`.

## Invariants enforced

None of the numbered system invariants -- no capability, and it drives only
descriptors and a spawn. Its own rules:

- **The master-fd ownership contract**: mint closes on its own failure;
  success transfers `mfd` to the caller; seed and spawn never close it.
- **`spawn_on_slave` retains no parent slave copy**: all three `File`s move
  into the child, so the child's exit alone can drive the slave-open count to
  zero and arm the master's EOF. A retained copy would wedge drain-then-EOF.

## Error paths

Every failure is a `HoldError` documenting the fd state (above); nothing
panics except the deliberately-guarded empty-argv case, which returns
`HoldError::Spawn` instead. `seed_winsize` / `set_winsize` swallow a
ctl-open-or-write failure (the ptyfs default size is the fallback).

## Performance

Irrelevant -- once per session (mint + spawn) and once per resize.

## Prosecution

- **`mint` must close the fd on every internal-failure path.** The fstat and
  non-master arms both do; a new failure arm that forgets leaks the master.
- **The `HoldError` variant must keep reporting the true fd state.** A caller
  double-closes (on an opened-then-closed variant) or leaks (on a
  still-open variant) if the mapping drifts from the code.
- **`spawn_on_slave` must move all three slave `File`s into the child.**
  Keeping a parent copy holds the slave-open count above zero, and the master
  never sees EOF -- the host hangs waiting for a drain that cannot complete.
- **The empty-argv guard must stay.** Under panic=abort, `argv[0]` on an empty
  slice is a silent process death, not a returned error.

## Seams

- The relay policy over the master (raw pump vs cell-parse) is each host's;
  only mint/seed/spawn are shared here.
- The thematic name `den` is a HELD proposal (PTY-DESIGN 10); `ptyhold` is the
  working name until it is surfaced for signoff.

## Caveats

- **Not an AUDIT-TRIGGERS surface, but a delicate one.** The PTY audit-trigger
  rows cover the kernel seam, ptyfs, and the pouch boundary-line; this
  userspace master-hold is not among them. Its risk is the fd-lifetime
  contract, which is real (a leak or double-close class) and is why the crate
  is `light` rather than `none` -- a careful read is warranted even though no
  capability crosses it.
- **Depends on [[sub-libthyla-rs]]**, unlike the pure crates it was batched
  with -- it drives real syscalls (open/fstat/close/write) and the runtime's
  `Command`/`Child`, so it is not host-testable the way vt/beacon/cartoon are.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
