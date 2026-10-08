---
id: dec-2026-10-06-chdir-physical
type: dec
title: "chdir stores the name of where the walk landed"
date: 2026-10-06
status: standing
decided-by: user-vote
affects: [sub-kernel-stalk, sub-kernel-territory, sub-kernel-syscall-dispatch]
created: 2026-10-06
---
## Fork

`SYS_CHDIR` validated one directory and stored the name of another. It joined
the argument to the cwd verbatim and resolved the join through the resolver,
which follows links and pops `..` off its trail. It then stored a lexically
cleaned copy of the join. The cleaner's own comment explained why the two
agreed: with no symlinks, the lexical pop and the trail pop consume the same
components. Symlinks then landed, and the argument stopped holding.
- `cd link/..` stored the link's lexical parent, while the resolver's `..` had
  climbed out of the link's target. The stored cwd named a different directory
  from the one validated, so `cd link/..` and `ls link/..` disagreed.
- `getcwd` returned link components, which POSIX forbids.
- A retargeted link moved every cwd entered through it, because the cwd is a
  name that is resolved again at every use.

Three options were researched:
- **Physical.** Store the name of where the resolver landed (POSIX `getcwd`,
  Linux `d_path`, and the resolver's own physical `..`). The resolver has to
  export its trail's names.
- **Logical.** Clean first, validate the cleaned path and store it (Plan 9's
  `cleanname` view of names as used). To keep #83's refusal of `cd file/..` it
  has to validate both the verbatim path and the cleaned one.
- **Keep the behaviour and document it.**

## Decision

The operator voted on 2026-10-06: **physical**.

Decided under the operator's "your guts" grant, as the vote's own scope:
- The resolver builds the name as it builds the trail (`stalk_landed`).
  Nothing in the resolver reads it, and it is never taken from a Spoor's
  `Path`, because I-33 makes the Path non-load-bearing and a Path is wrong
  under a chroot.
- A name that outgrows the buffer fails the change-directory.
- The lexical canonicalizer had no other production caller and is deleted.
- The shell's `cd` stays logical: ut cleans its argument before the call, as
  bash's `cd -L` does, so its builtin `pwd` keeps the spelling the user typed,
  while `/bin/pwd` asks the kernel.

## Rationale

Physical makes what is stored the same as what was validated, by
construction. It agrees with the resolver's `..`, which has been physical since
symlinks landed. With logical, the stored path and the walked path would stay
two computations that must be kept in agreement. That is the shape #83 was a
bug in, and the shape this fork exposed again. The spawn cwd field
(`SYS_SPAWN_FULL_ARGV`) inherits the rule.

One node has no physical name: a served link resolved from a union member past
the first can land on a node an earlier member shadows. The name is therefore
walked once more and must land on the same node, or the change-directory fails
(found in the self-review before the gate).
