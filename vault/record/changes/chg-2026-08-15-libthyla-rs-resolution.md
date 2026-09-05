---
id: chg-2026-08-15-libthyla-rs-resolution
type: chg
title: "libthyla-rs re-swept: the kernel resolves now, and the primitive that got it wrong is still public"
date: 2026-08-15
arc: arc-vault
commits: ["1de67850"]
touched: [sub-libthyla-rs]
established: []
closed: []
opened: []
mirrors-checked: [usr/lib/libthyla-rs/src/fs/file.rs, usr/lib/libthyla-rs/src/fs/mod.rs, usr/lib/libthyla-rs/src/fs/path.rs, usr/lib/libthyla-rs/src/env.rs]
depth: rich
created: 2026-08-15
---
Two changes of substance, and they pull in opposite directions on the same
question — how much a library should do before handing a path or an
environment to the kernel.

## The principle: the kernel resolves; userspace only splits

The crate used to join a relative path onto the working directory and
**lexically clean** it — popping `..` without proving the popped component
existed, dropping `.` and trailing separators — so the resolver's gates never
saw the components they exist to judge.

Six consequences were **measured on the pre-fix library**, not argued:
removing `f/` deleted the very file the slash asserts is a directory; removing
`nope/../x` removed `x` through a directory that does not exist; `/a/f/.`
acted on `f`; renaming `d/.` renamed the directory; opening `/a/../f` failed
as an invalid argument; opening `f/` opened `f`.

The fix **deleted the cleaning** rather than correcting it. A plain open now
hands the whole path to the kernel in one call — join, dot resolution under
containment, the trailing-slash and not-a-directory gates, per-component
search permission, the real errno, all kernel-side, and an N-syscall
per-component walk retired along the way. The create and mutation paths split
**lexically at the last component only**, no cleaning, and send the parent
prefix back through the same resolver. That is what yields Linux's
parent-walk-first ordering.

**The same defect existed at three layers independently** — the kernel's join,
the ported libc's splitter, and this — each wrong differently. That is the
argument for the principle rather than for three fixes: when N layers each
normalize, they each normalize wrong.

## The finding: removing a defect's callers does not disarm the primitive

The commit's claim — "nothing in libthyla-rs pops a `..` or drops a dot
anymore" — is **true**, and I checked it rather than taking it: no
`components()` walk survives in the fs operations.

But `components()` does. It is `pub`, it still skips empty segments —
consecutive slashes, trailing slashes — and still classifies `.` and `..`,
which is precisely the mechanism behind two of the six measured defects. Three
things turn that from a shrug into a task:

- **The lossy function is public and the safe one is not.** The sanctioned
  splitter the rewrite added is `pub(crate)`; an out-of-crate caller can reach
  only the lossy iterator.
- **Its rustdoc advertises the loss as correctness.** The worked example is
  `///foo//bar/` — a trailing-slash input, shown having its slash dropped, as
  an illustration of the iterator working properly. A reader deciding whether
  it is suitable for "does this path escape my root" reads that and concludes
  yes.
- **Nothing connects the file to the rewrite.** Zero references to it, or to
  the resolver, anywhere in the file that holds the hazard.

The asymmetry with the kernel is the durable part. There, the same lesson
landed as gates **inside** the resolver, so a new kernel caller inherits them.
Here it landed as **deletions in the callers**, so a new userspace caller
inherits nothing and rebuilds the defect from scratch — with the API
documentation telling them it is fine. Task #185.

## Two surfaces for the environment, and this one is documented right

The environment is now two objects that are both correct and can disagree.
`/env` is authoritative and mutable, inherited through the kernel's clone. The
startup frame's `envp` is a **snapshot** projected at exec — a later `/env`
write does not appear in it — and exists because every C runtime expects
`environ` on the stack, and because a Linux guest's `execve` passes an
environment with nowhere else to land.

They diverge exactly there: an `execve` from a phenotyped guest puts its
`envp` on the new frame without rewriting `/env`, so the enumerator reports
what the caller asked for while the single-variable reader reports what the
process inherited.

**Recorded as a positive**, because the standard it meets is the one
`components()` misses next door: the divergence is in the module header *and*
repeated on the enumerator's own rustdoc, which names which surface a
Thylacine program usually wants and the two cases where the frame is the right
answer. A reader at the call site is warned. That is the whole difference
between the two findings in this sweep — same crate, same kind of hazard, one
documented where the reader stands and one documented as a feature.

## A seam the sweep surfaced

There is **no `set_var`**. The environment is mutable — writing `/env/NAME` is
the mechanism — but the crate exposes only the read side, so a program that
wants to set a variable opens the file itself. That is the library-side half
of the shell's inability to implement `export`: it links the module that
documents the mechanism and finds no method for it.

## A smaller note on shape

`Path` is now the second type in this crate that is a **vocabulary rather than
a validator** — the rights bitflag being the first. It arrived there the
harder way: it *was* a resolver, its resolution was proven wrong six ways, and
the resolution was deleted rather than repaired. Both are the right end state;
only one was designed that way.
