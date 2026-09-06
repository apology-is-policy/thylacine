---
id: chg-2026-09-06-corvus-syscalls-doc-absorb
type: chg
title: "absorb docs/reference/58-corvus-syscalls (5 hardening syscalls, P5 scaffold): multi-redirect, two stale framings named (CSPRNG + NOTRACE superseded)"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
Five P5 hardening syscalls (SYS_MLOCKALL=16, SET_DUMPABLE=17, SET_TRACEABLE=18,
EXPLICIT_BZERO=19, GETRANDOM=20) + caps + one-way PROC_FLAG_*. Forward-compat
scaffolding, mostly superseded; verified atom-by-atom.

WHERE EACH ATOM LIVES (verified, not assumed):
- The CSPRNG behind SYS_GETRANDOM (kern_random_bytes, ChaCha20 stir, RNDR/FEAT_RNG,
  DTB-seed + CNTPCT-jitter mixing) -> sub-kernel-content (owns chacha20.c +
  random.c).
- PROC_FLAG_NOTRACE enforcement (the I-39 debug gate refuses NOTRACE) ->
  sub-kernel-devproc (:395 "kproc and NOTRACE are refused before the authority
  axes").
- The one-way proc_flags mechanism (proc_mark_*/proc_is_*, monotonic, fail-closed
  readers) -> sub-kernel-proc (:42).
- The caps (CAP_LOCK_PAGES/CAP_CSPRNG_READ) -> abi-caps; corvus's mlockall'ed/
  undumpable/untraceable posture -> sub-corvus (:33).
- explicit_bzero secret-wipe -> sub-corvus-crypto.

TWO STALE FRAMINGS NAMED (both superseded by landed work):
1. "GETRANDOM RNDR-absent is permanent, no software-CSPRNG mixing" -- REFUTED: the
   Lazarus W3 ChaCha20 stir landed; an RNDR-less target (Apple/HVF, A72) seeds from
   the DTB boot seed + CNTPCT jitter (sub-kernel-content).
2. "Flags are not enforced at v1.0" -- STALE for NOTRACE: the I-39 gate enforces it.

Zero-fold: NODUMP/MLOCKED still await their enforcing subsystems (no core-dump/swap
subsystem in the tree, verified: 0 vault hits for coredump); the escalate-then-
crash-dump one-way refusal is the generic monotonic proc_flags property. Multi-
redirect stub.
