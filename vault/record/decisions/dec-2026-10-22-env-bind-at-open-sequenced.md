---
id: dec-2026-10-22-env-bind-at-open-sequenced
type: dec
title: "An /env handle acts on its opener's environment, built on b3 right after B-3g"
date: 2026-10-22
status: standing
decided-by: user-vote
affects: [sub-kernel-content, sub-kernel-syscall-dispatch]
created: 2026-10-22
---
## Fork

devenv resolves the calling Proc on every operation, and a spawned child
receives a copy of its parent's environment. So `echo 1 > /env/V` from ut
empties ut's `V` (ut opens the file) and writes `1` into echo's copy,
which dies with echo. Measured 2026-09-16 (a later child saw `BEACON`
empty) and again on 2026-10-22, when a gate leg set `FC_DEBUG` that way
and fontconfig saw it empty.

## Research

- **Plan 9 (heritage).** devenv also resolves `up->egrp` per operation,
  but fork shares the environment group unless `RFENVG` asks for a copy,
  so a descriptor passed to a child writes the shared group and the idiom
  works. A Chan for `#e` carries its group.
- **The tree.** `struct Env` already has an atomic reference count; the
  Spoor can hold one. ARCH 9.7 calls copy-at-spawn "the Plan 9
  default-copy-on-rfork", which is not Plan 9's default.

## Options

1. **Bind at open**: open and create take a reference on the opener's
   Env into the Spoor; every later operation through it acts there.
2. Share the environment group across spawn, as Plan 9's fork does.
3. Leave it; give commands variables only through `env`.

## The call

Option 1 (operator, 2026-09-16). On 2026-10-22, after native `env` made
the idiom unnecessary for B-3, the operator placed the work on `b3`, as
its own sub-chunk right after the B-3g close and before B-3h, rather than
on main after B-3 lands or never (the recommended option). Order:
scripture (ARCH 9.7 per opener, the misstatement corrected; the `/env`
audit-trigger row), the kernel change (walks and directory reads on the
root still act on the caller; a lazily absent Env is created at open),
a kernel test and an in-guest leg each run both ways, a Fable round.

## Rationale

The handle is the authority in this system: whoever holds a descriptor
acts with what it was opened on. Resolving per operation made a passed
descriptor act on a different environment from the one its opener
emptied.
