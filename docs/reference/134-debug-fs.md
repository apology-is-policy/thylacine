# 134 — /proc debug-fs: the kernel debug surface (I-39) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-debug-fs-doc-absorb`).
The `/proc/<pid>` debug control surface — the entire kernel side of the Go-IDE
debug arc: stop/resume/step, register and memory inspection of a stopped target,
and hardware breakpoints/watchpoints, all under **I-39** (debug authority is
namespace-plus-two-axis, stopped-only, never stranding the quarry). Audit-bearing.
Its content lives, code-verified and current, in:

- the **whole debug-fs control surface** — the I-39 two-axis gate
  (`devproc_debug_authorized`; owner OR `CAP_DEBUG`/`CAP_HOSTOWNER`, and
  `CAP_DAC_OVERRIDE` deliberately *not* an axis), the stop checkpoint and the two
  stop owners / one park (`debug_stop_req` read alone so a Ctrl-Z'd process is not
  debugger-readable), the fully-stopped conjunction (death wins) and the
  three-conjunct park-predicate fix (the thread-on-its-way-out stale-registration
  race), the SPSR-never-written register guard, the bare-pointer attach slot + the
  atomic `CDEBUGOWNER` release, the **die-with-launcher exitkill release** (folded
  at this absorption), the kstack raw/symbolic KASLR split (I-16), and the
  cross-Proc re-resolve-by-pid lifetime discipline:

      vault/system/kernel/introspection/sub-kernel-devproc.md   (guarded-by inv-i39/i26)

- the **hardware-debug tier** (8a-2) — the per-Proc DBGBVR/DBGWVR breakpoint and
  watchpoint install, `MDSCR.SS` single-step, the step-over-breakpoint dance, and
  the **SA-1 stale-fire-vs-detach** gate (a hardware fire racing a detach delivers
  only while owned):

      vault/system/kernel/introspection/sub-kernel-hwdebug.md

- the **debug exception dispatch** — the EC 0x30/0x32/0x34 arms that offer a
  breakpoint / single-step / watchpoint to the debug layer:

      vault/system/kernel/entry/sub-kernel-exception.md

- the **cross-Proc memory** read/write (`mmu_cross_proc_read`/`write`, clamped one
  page per call):

      vault/system/kernel/memory/sub-kernel-mmu.md

- the **settled-thread unified kernel stack** (`halls_walk_kernel_frames`, the 8b
  cross-boundary stitch — a thread blocked deep inside a syscall):

      vault/system/kernel/entry/sub-kernel-halls.md

- the **formal models** — `specs/debug_stop.tla` (the stop/continue machine +
  `EventuallyLaunchedDies`) + `specs/debug_step.tla` (the step machine); the
  PTY-stop composition is `spec-pty-stop`.

The in-process DAP round-trip (8c-4b) and the Ambush client (8c-4c) are userspace,
owned by the shell-TUI debugger surface (`sub-parley`).

**What this file got WRONG or MISSED by the time it was absorbed:**

- **The die-with-launcher exitkill release was in no dossier body — folded at
  absorption.** `sub-kernel-devproc` documented the `debug_exitkill` field and the
  resume-on-release (NoStrand) but not its complement: `devproc_debug_release_cb`
  (`devproc.c:940`) `proc_group_terminate`s a debugger-LAUNCHED `exitkill`-marked
  ALIVE target instead of resuming it (closing the launched-orphan leak; the #811
  cascade wakes debug-parked threads by rendez, not `debug_stop_req`, so death
  wins; the release disarms breakpoints/watchpoints). The audit-F1 trigger nuance
  (the release-cb runs on the target and cannot observe the debugger's liveness,
  so it fires on any ctl-fd close of a marked ALIVE target without a prior
  `detach`; the load-bearing case is death) and the spec's `EventuallyLaunchedDies`
  / `BUGGY_EXITKILL_IGNORED` are now folded into `sub-kernel-devproc`.
- **The content is distributed** — the control surface to `sub-kernel-devproc`,
  the HW tier + SA-1 to `sub-kernel-hwdebug`, the exception dispatch to
  `sub-kernel-exception`, cross-Proc memory to `sub-kernel-mmu`, the unified stack
  to `sub-kernel-halls`, the DAP/Ambush client to `sub-parley`.
