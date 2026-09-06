---
id: chg-2026-09-06-debug-fs-doc-absorb
type: chg
title: "absorb docs/reference/134-debug-fs (I-39 debug surface): fold the die-with-launcher exitkill release into sub-kernel-devproc"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: [sub-kernel-devproc]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---

# docs/reference/134-debug-fs.md -> ABSORBED (I-39 audit-trigger surface)

Absorbed the 1115-line debug-fs reference doc into a multi-redirect stub. The
owning dossier `sub-kernel-devproc` (446 lines, guarded-by inv-i39/i26, updated
2026-09-06) is deep and current -- verified atom-by-atom, it covers the I-39
two-axis gate, the two-stop-owners park (debug_stop_req read alone), the
fully-stopped conjunction (death wins) + the three-conjunct park-predicate fix,
the SPSR guard, the bare-pointer attach slot + atomic CDEBUGOWNER, the kstack
KASLR raw/symbolic split (I-16), and the re-resolve-by-pid lifetime discipline.
The HW tier (8a-2) + SA-1 -> sub-kernel-hwdebug (verified: line 83/181, "a fire
racing a detach"); the EC dispatch -> sub-kernel-exception; cross-Proc mem ->
sub-kernel-mmu; the unified stack -> sub-kernel-halls.

ONE genuine residue, code-grounded and folded:

- **The die-with-launcher exitkill release was in no dossier body.**
  sub-kernel-devproc named the debug_exitkill field + the resume-on-release
  (NoStrand) but not the terminate-on-release complement. Code-grounded
  (kernel/devproc.c:940-975, devproc_debug_release_cb): `if (exitkill && p->state
  == PROC_STATE_ALIVE) proc_group_terminate(p, "debugger exited")` -- a
  debugger-LAUNCHED marked target dies with its launcher (else NoStrand-resume
  orphans it to init to run forever). The #811 cascade wakes debug-parked threads
  by rendez blocked-on (NOT debug_stop_req) so death wins; the release
  hwdebug_*_clear_all disarms breakpoints/watchpoints (else the orphan re-traps
  forever). Audit-F1 trigger nuance: the release-cb runs on the TARGET, cannot
  observe the debugger's liveness, so it fires on any ctl-fd close of a marked
  ALIVE target without a prior explicit detach (the load-bearing case is death,
  the #68 close-at-exit leak). Spec: debug_stop.tla EventuallyLaunchedDies
  (BUGGY_EXITKILL_IGNORED counterexample). Folded into sub-kernel-devproc as a
  Die-with-launcher Mechanism subsection + an I-39 amendment (resume-OR-terminate-
  on-release) + a prosecution bullet.

90 -> 91 absorbed of 157. lint 0-fail.
