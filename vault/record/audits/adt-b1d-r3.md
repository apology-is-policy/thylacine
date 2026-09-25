---
id: adt-b1d-r3
type: adt
title: "B-1d round 3: the fixes -- a control one variable away on the wrong axis, and an ABI rule read the convenient way"
date: 2026-09-25
scope: [sub-pouch-mem, sub-kernel-joey, sub-stratum-boot, sub-kernel-territory, sub-substrate-remote-host, sub-coreutils-filters, sub-kernel-devproc]
reviewer: opus
model-start: "claude-opus-5-5"
model-end: "claude-opus-5-5"
verdict: clean
counts: {p0: 0, p1: 0, p2: 1, p3: 6}
findings: [fnd-b1d-r3-f1, fnd-b1d-r3-s2, fnd-b1d-r3-s3]
round-of: chg-2026-09-25-b1d-round3-close
prior-round: adt-b1d-r2
created: 2026-09-25
---
## Scope

The fixes: 0119d242 (WIP 8, built and booted) and a157d47c (WIP 9, unbuilt at
the round). `walk_open_in_bin`, the rewritten `devproc.read_cwd`,
coreutil-smoke's `realpath` and `env --help` legs, the confined child's chdir
and pre-pivot control, build.sh's stray-root guard, joey's post-pivot getcwd
check, the kernel strings and the prose. Opus 5.5 at max, the fallback tier.

## Convergence

0 P0 / 0 P1 / 0 P2 / 5 P3 from the round; with the main session's parallel
self-audit, 0 / 0 / 1 / 6. The self-audit found the round's F1 independently
([[fnd-b1d-r3-f1]]): round 2's fix made the confined leg walk its `..`, but no
control noticed the chdir going away. It also found S2 ([[fnd-b1d-r3-s2]]):
WIP 8 and 9 had reworded five EXTINCTION bodies on the permissive reading of
two binding documents that disagree. F2 and F3 were prose the fixes left wrong
or unswept, fixed, with joey's `/sbin/ptyfs` pulled forward in the same class.
F4 (joey's SYS_WSTAT bad-argument legs cannot fail, and the "reserved" 0x8 is
`T_WSTAT_SIZE`) is pre-existing and enqueued. F5 ([[abi-boot-banner]]
undercounted the fault gate's matched bodies) is fixed: eight variants, six
bodies, after a live `tools/test-fault.sh` passed all eight. Writing the spec
rows at the close, the main session found S3 ([[fnd-b1d-r3-s3]]):
CoveredIsItsPoint was the one covered-member invariant with no buggy
configuration, and it has one now. Clean by count and by shape. Verified
sound: `walk_open_in_bin`'s handle discipline and the O_PATH second hop, #81's
read denial, the `read_cwd` test's construction and both of its
discriminations, the two `setdot` callers, `realpath`, the `env --help` leg
against the old collision, the confined child reaching the floor with no musl
cache taint, the stray guard's placement and coverage, the getcwd contract,
and that no tool consumes the renamed strings.
