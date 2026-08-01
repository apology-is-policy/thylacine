---
id: adt-ls4-r1
type: adt
title: "LS-4 (per-Proc cwd) focused round"
date: 2026-06-09
scope: [sub-kernel-territory, sub-kernel-stalk]
reviewer: opus
model-start: "claude-opus"
model-end: "claude-opus"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 3}
findings: [fnd-ls4-r1-f1, fnd-ls4-r1-f2, fnd-ls4-r1-f3]
round-of: chg-2026-06-09-ls4-cwd
created: 2026-08-01
---
## Scope

The cwd substrate — `dot_path` / `dot_lock` / the resolver quartet /
`SYS_CHDIR` / `SYS_GETCWD` / the `SYS_OPEN` relative join / the struct
layout asserts. One Opus prosecutor (which built and ran the suite) plus
a concurrent self-audit.

## Convergence

CLEAN, and the interesting part is the size of the prosecuted-and-
unbroken set relative to the three cosmetic P3s.

[[inv-i28]] escape: NONE, and the argument is that two independent
clamps COMPOSE rather than one carrying the load. Lexically, `..` pops
with the offset provably non-negative so excess nets to `"/"`.
Structurally, `stalk` treats a depth-0 `..` as a no-op contained at the
base. And `territory_setdot`'s only caller feeds it the lexical
resolver's output, so `dot_path` is ALWAYS cleaned — but even a
hypothetical uncleaned string entering it would be re-clamped by stalk.
That is the shape a security-critical addition should have: not a new
wall, but a redundant one.

`territory_setdot` UAF/double-free: none — readers hold `dot_lock`
across the entire dereference, the swap happens under it, the old string
is freed outside, and two concurrent setdots capture distinct olds.
`kmalloc` under `dot_lock` in `territory_clone`: sound, no reverse edge
to SLUB, process-context only. The chdir X-search gate matches stalk's
AND supplies the final-directory check `STALK_WALK` deliberately omits.
`cwd_lexical_resolve` is memory-safe under every input, including the
`outcap < 2` guard that makes the empty-to-`"/"` branch safe.

The round's own record of the FROM_ROOT read is worth preserving as a
point-in-time truth: it noted chdir copying `sys_open`'s lockless
`root_spoor` read verbatim and correctly classified it as the
pre-existing #848 race rather than a new instance. RW-4 closed that a
day later with `territory_root_ref`, which chdir now uses.
