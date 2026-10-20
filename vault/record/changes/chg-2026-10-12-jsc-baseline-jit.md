---
id: chg-2026-10-12-jsc-baseline-jit
type: chg
title: "JavaScriptCore's Baseline and regexp JITs run on one code region, written only through a sealed thunk"
date: 2026-10-12
arc: arc-boosty
commits: ["372594867", "9e0ff1a6e", "540527f33", "1c4ddf8e1", "ccb6498e6", "c011c9e3c", "33aa25af1", "09465bc12", "5597a17b0"]
touched:
  - sub-webkit
  - sub-substrate-build
  - sub-dosbox
  - sub-sdl-port
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-10-12
---
Operator votes: 2026-09-28, the tier order (Baseline + YARR, then DFG, then
FTL/BBQ/OMG); 2026-10-08 16:25Z, "gate tiers at run time"
([[dec-2026-10-08-jsc-tiers-run-time]]), after upstream WebKit proved unable to
build JIT-on with the DFG and FTL off on ARM64.

**What** ([[sub-webkit]]). Patch 0002 selects JSC's separated-WX heap for
`OS(THYLACINE)` with no jump islands and a 64 MiB pool, so one near branch
reaches the whole pool and a `static_assert` proves it. The pool is the exec
alias of one `SYS_JIT_CREATE` region, adopted by a new
`PageReservation::adoptExecutable`; commit and decommit are no-ops there,
because the region is demand-zero and charged on first touch (B-2a). The
writer address lives only as immediates in a 33-word thunk, hand-encoded by
`usr/lib/thylajit/thyla_jit_thunk.h` and sealed execute-only by
`SYS_JIT_CREATE_SEALED` (B-2b); the returned region struct and the word array
are scrubbed with volatile stores. The thunk traps (`brk`, the Proc dies of
`snare:brk`) on any write outside the region, does an 8-byte write at an
8-aligned destination as one store, and zeroes the register that held the
writer address before it returns. `jsc` takes `CAP_JIT` by the SELF form before
`JSC::initialize`; `--thylacine-no-cap-jit` skips it, and the refused create
leaves the pool empty, so JSC runs its interpreters. Every tier is built; DFG,
FTL, BBQ and OMG are forced off in `Options::notifyOptionsChanged`, after which
the options are frozen.

**Witnesses** (`tools/interactive/ls-jsc.exp`, added to the boot banner's mirror
set without a change to the banner, [[abi-boot-banner]]). Each JIT leg is paired with its denied twin one flag away: `jit`
(a hot function runs Baseline code), `rx` (a pattern through the YARR JIT),
`loop`, `sunspider` (SunSpider 1.0.2, 23 of its 26 tests self-checking), `dfg`
(a hot function never reaches DFG code with every tier asked for by flag),
`tiers`, and `thunk`/`thunk-oob`: `pouch-hello-jitthunk` runs the same header's
encoding against the kernel, writes the region's edges and reads them back
through the exec alias, and makes four out-of-bounds calls, each of which must
die of `snare:brk`.

**Build and the JIT headers** ([[sub-substrate-build]], [[sub-dosbox]],
[[sub-sdl-port]]): `build_jsc` builds every tier with the Baseline and YARR
JITs on and `usr/lib/thylajit` on the include path (the C++ toolchain file's
extra-includes hook). `thyla_jit.h` includes `thyla_capjit.h` unless the
consumer defines `THYLA_JIT_NO_CAPJIT` (JSC's allocator does; DOSBox takes
both), and `THYLA_CAPJIT_SELF_ONLY` compiles the bearer form and its demo
password out, with every report on stderr so `jsc`'s stdout stays its output
(SDL keeps both forms). `tools/forage.sh` widens an existing sparse checkout
with `sparse-checkout add`, so a hand-added cone survives, and reports a full
checkout at the path instead of converting it (`test-forage.sh` C5).

**Verification.** Fable 5.1 round 1 (start == end) 0 P0 / 0 P1 / 0 P2 / 7 P3
and round 2 0/0/0/4, both clean; dispositions in the closed list. F3, F4 and F5
of round 1 are carried as owned obligations (OPEN-BUGS 2026-10-08 16:41Z): the
content process runs without `/proc`; a kernel-emitted thunk and a `CAP_JIT`
drop after B-2e; the Wasm cross-PE publish at B-2e.
ls-jsc PASS on the rebased tip 5597a17b0 (3 builds: 15:09Z, 15:34Z, 15:55Z); SunSpider 1.0.2 112 ms with the JIT vs 352 ms denied, the int32 loop 442 vs 931 ms; six REDs each red at its predicted first leg (thunk check 1 -> thunk-oob 8@-8 snare:segv; check 2 -> thunk-oob 1@end snare:segv; no DFG gate -> dfg JSCP-TRUE-TRUE; only the DFG gate -> tiers JSCO-0-1-1-1-1-1; CAP_JIT despite the flag -> jit-deny; CAP_JIT after JSC::initialize -> jit JSCG-FALSE); suite 1984/1984 on the default image; test-fault 8/8; ci-smp-gate N=10 50/50 (default smp1/4/8, ubsan smp4/8), 0 corruption; check-production --all PASS (the lean image builds, boots, and both dev accounts log in).
Found during the run and owned (OPEN-BUGS 2026-10-12, operator votes the same day): JSC's
collector suspends a mutator that has released heap access, and `Thread::suspend` is fatal
here, so jsc exits 127 (a B-0 defect; the cooperative scan is the next chunk), and Pouch's
`abort()` is a silent `_Exit(127)` (to become a signalled death).

