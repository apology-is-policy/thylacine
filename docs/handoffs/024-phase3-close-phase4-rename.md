# Handoff 024 — Phase 3 close (P3-G + P3-H) + Phase 4 entry rename

**Tip**: `0d1917a` (Phase 4 entry: thematic renames). Predecessor: `6446715` (P3-H hash fixup).

## TL;DR

This handoff covers two coherent work units:

1. **Phase 3 closure** (P3-G + P3-H, `7a61651` → `6446715`):
   - **P3-G** (`7a61651` substantive + `cccd3ec` hash fixup): WFI-aware work-stealing IPIs. Closes R5-H **F77** + **F78**. ready/wakeup IPI an idle peer (per-CPU `idle_in_wfi` flag, `sched_notify_idle_peer`); try_steal contention sentinel + sched retry; test-mode toggle (off during in-kernel tests, on for production /init). Spec extension: `scheduler.tla` adds `wfi` variable + `EnterWFI` + `NotifyWFIPeer` actions; `IPI_Deliver` clears `wfi[dst]`; `Resume`/`Steal` preconditions tightened with `~wfi`. New configs: `scheduler_liveness_wfi.cfg` (5760 states) + `scheduler_buggy_wfi.cfg` (LatencyBound counterexample).
   - **P3-H** (`c2d7886` substantive + `6446715` hash fixup): Phase 3 closing audit R7 + fixes — **Phase 3 CLOSED**. R7 surfaced 11 findings F127–F137 (1 P1 + 2 P2 + 6 P3 actionable + 2 withdrawn). Trip-hazard #157 reproduced + bisected; root cause beyond TLB/cache/recycle (suspected QEMU-virt sim state); deferred with forward-looking P0 (Phase 4 blocker) + F136 one-call guard hardening. **All P0/P1/P2 closed**: F127 (SYS_PUTS user-VA bound check), F128 (per_cpu_main IRQ-mask discipline), F129 (mmu_install_user_pte TLB flush). **P3 closed**: F134 (D/I-cache maintenance for executable segments), F135 (userland.S SUBTLE comment rewrite), F136 (init_run one-call guard). **P3 deferred (forward-looking)**: F130 (W^X defense-in-depth), F132 (proc_free TLB-flush ordering), F137 (proc_alloc rollback symmetry). Audit memory: `audit_r7_closed_list.md`. **ROADMAP §6 exit criteria 8/8 MET**.

2. **Phase 4 entry — thematic renames + scaffold** (`0d1917a`):
   - User-driven thematic naming pass before Phase 4's device-model implementation lands. Six renames atomically applied across 91 files (kernel + arch + specs + scripture):
     - `Chan` → `Spoor` (Plan 9 file/resource handle; spoor = animal track, follows naturally with the walk verb).
     - `Pgrp` + `namespace` → `Territory` (Plan 9 process namespace; thylacine = territorial apex predator).
     - `Vmo` → `Burrow` (memory pages live in a burrow; on-brand for marsupial OS).
     - `devtab` → `bestiary` (kernel device registry as medieval fauna catalog).
     - `_hang` → `_torpor` (WFI halt loop; torpor = marsupial deep-sleep state).
     - `/init` → `/joey` (joey = baby marsupial; first userspace process).
   - File renames + spec module renames + audit-list aware identifier renames. Kept Plan 9 names for portability (`rfork`, `exits`, `wait_pid`, `sleep`, `wakeup`, `Rendez`, `bind`, `mount`, `unmount`, `RFNAMEG`, `sched`, `ready`).
   - **96/96 tests** PASS × default + UBSan; 4/4 fault; 5/5 KASLR; 4 specs + 14 cfgs all clean (correct configs PASS, buggy configs produce expected counterexamples).
   - Boot output now reads: `joey: rforking child for /joey (9-instr hello blob)` → `hello` → `joey: /joey pid=N exited cleanly (status=0)` → `Thylacine boot OK`.
   - Phase 4 status doc scaffolded at `docs/phase4-status.md` with 15 sub-chunk plan (P4-A through P4-Z).

## What landed in this session window

In addition to P3-G + P3-H + Phase-4-entry rename:

- **R7 closing audit** ran as a background agent; spawned, completed, surfaced 11 findings; triage + fix landed in P3-H.
- **Trip-hazard #157 reproducer** authored at `kernel/test/test_userspace2.c` (orphaned in tree, not registered in build). Reproduces the second-userspace-iteration hang. Pre-eret state bit-identical between iter 1 and iter 2. Hypotheses ruled out: TLB negative-cache, walker cache, ASID recycle, pgtable PA recycle, D/I-cache. Deferred with forward-looking P0 classification.

## Current state (post-rename, 2026-05-07)

- **Phase 0 complete.** Scripture binding.
- **Phase 1 CLOSED** at `ceecb26`.
- **Phase 2 CLOSED** at `5914230`.
- **Phase 3 CLOSED** at `c2d7886` (P3-H substantive); tip `6446715` (P3-H hash fixup).
- **Phase 4 OPEN** at `0d1917a` (Phase 4 entry: thematic renames + scaffold). No P4 sub-chunks landed yet.
- **Tip is `0d1917a`**.

**Tests**: 96/96 PASS × default + UBSan. ~338 ms boot (production); ~344 ms UBSan; fault matrix 4/4; KASLR 5/5 distinct.

**Specs**: 4 specs (scheduler, territory, burrow, handles) + 14 cfg variants. All correct configs PASS:
- scheduler.cfg — 25416 states
- scheduler_liveness.cfg — 23 states
- scheduler_liveness_wfi.cfg — 5760 states
- territory.cfg — 625 states
- burrow.cfg — 100 states
- handles.cfg — 6.05M states

All 8 buggy configs produce expected counterexamples (4 scheduler_buggy_*, 1 territory_buggy, 3 burrow_buggy_*, 4 handles_buggy_*).

**Open audit findings**: 0 P0/P1/P2. Cumulative deferrals (forward-looking, all Phase 5+): F108/F109/F110 (R6-A), F113/F115/F116/F119 (R6-B), F130/F132/F137 (R7), plus older Phase 1+2 deferrals.

**Open trip-hazards**:
- **#157 — second-userspace-iteration hang. Forward-looking P0 (Phase 4 blocker).** Reproducer at `kernel/test/test_userspace2.c` (orphaned). v1.0 mitigation: F136 one-call guard converts silent hang to explicit extinction. Surgical-fix candidates from R7 audit: `-cpu cortex-a76` MTE check; `dc civac` on freed pgtable pages; `TCR_EL1.TCMA0=1`; `qemu -d in_asm,exec,int,mmu` instruction trace.
- **Phase numbering offset** (#180, P4-entry): local Phase 4 = ROADMAP §6 (titled "Phase 3"). The local impl split ROADMAP Phase 2's deferred address-space deliverables across local Phase 2 + Phase 3, shifting numbering by one. Status docs use local; ROADMAP refs use scripture with cross-reference.

## Verify on session pickup

```bash
git log --oneline -10
# Expect:
#   0d1917a Phase 4 entry: thematic renames Chan/namespace+Pgrp/Vmo/devtab/_hang/init
#   6446715 P3-H: hash fixup
#   c2d7886 P3-H: Phase 3 closing audit (R7) + fixes — Phase 3 CLOSED
#   cccd3ec P3-G: hash fixup
#   7a61651 P3-G: WFI-aware work-stealing IPIs — closes R5-H F77 + F78
#   0e336e8 P3-F: hash fixup
#   00527db P3-F: minimal /init in production boot path
#   43a6115 P3-Ed: handoff 023 — Phase 3 D + E sub-chunks; userspace runs

git status
# Expect: clean (only untracked: kernel/test/test_userspace2.c if not committed; docs/estimate.md, loc.sh as before).

tools/build.sh kernel
tools/test.sh
# Expect: 96/96 PASS, ~338 ms boot. Boot output:
#   joey: rforking child for /joey (9-instr hello blob)
#   hello
#   joey: /joey pid=N exited cleanly (status=0)
#   Thylacine boot OK

tools/test.sh --sanitize=undefined
# Expect: 96/96 PASS.

tools/test-fault.sh
# Expect: 4/4 PASS.

tools/verify-kaslr.sh -n 5
# Expect: 5/5 distinct.

# Specs (note: redownload tla2tools.jar if /tmp was cleared):
[ -f /tmp/tla2tools.jar ] || curl -sL -o /tmp/tla2tools.jar \
  https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
cd specs
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config scheduler.cfg              scheduler.tla
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config scheduler_liveness.cfg     scheduler.tla
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config scheduler_liveness_wfi.cfg scheduler.tla
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config territory.cfg               territory.tla
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config burrow.cfg                  burrow.tla
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config handles.cfg                 handles.tla
```

## What's NEXT — Phase 4 sub-chunks

Per `docs/phase4-status.md`:

1. **P4-A: Dev vtable + Spoor infrastructure**. NEXT. New `kernel/include/thylacine/dev.h` + `kernel/include/thylacine/spoor.h` + `kernel/dev.c` + `kernel/spoor.c`. Plan 9 17-op vtable (verbatim per ARCH §9.2). Spoor refcount + per-Spoor lock + spoor_alloc/free/clone/walk/clunk. bestiary[] sentinel-terminated registry + dev_register + dev_lookup_by_dc/by_name. devnone stub Dev (all ops return `-ENOSYS`) anchors unconfigured Spoors. Boot wire-up via `dev_init()` in main.c.
2. **P4-B**: kernel-internal trivial Devs (cons, null, zero, random).
3. **P4-C**: dev/proc — `/proc/<pid>/`.
4. **P4-D**: dev/ctl — `/ctl/`.
5. **P4-E**: dev/ramfs — cpio-loaded.
6. **P4-F**: VirtIO core (kernel/virtio.c) + virtqueue + MMIO transport.
7. **P4-G**: kernel/irqfwd.c — IRQ forwarding to KObj_IRQ blocker.
8. **P4-H**: kernel/virtio_pci.c — minimal PCIe enumeration.
9. **P4-I**: userspace virtio-blk driver (Rust). **First chunk that triggers trip-hazard #157 — must root-cause #157 before this lands.**
10. P4-J/K/L: virtio-net / virtio-input / virtio-gpu drivers.
11. **P4-M**: driver supervision.
12. **P4-N**: VMO finalize (now BURROW; `specs/burrow.tla` reconciliation + impl audit).
13. **P4-Z**: Phase 4 closing audit.

## Important commitments (cumulative)

- C99 kernel; Rust-port-friendly discipline.
- Userspace drivers from Phase 4 — no in-kernel virtio shortcuts.
- 9P2000.L + Stratum extensions as the universal protocol (Phase 5 = ROADMAP §7).
- Spec-first BINDING from Phase 2 onward; held through Phase 3 close.
- SOTA hardening from Phase 1 — KASLR, W^X, canaries, PAC, BTI, LSE, MTE-aware all live.
- Halcyon held to Phase 8 — risk isolation; v1.0-rc.1 from Phase 7 is the shippable fallback.
- Stratum dependency: Stratum is feature-complete on Phases 1-7; Phase 9 (9P server + extensions) is Thylacine Phase 5's integration target.

## Naming conventions (post-rename)

| Concept | Plan 9 / generic | Thylacine |
|---|---|---|
| File/resource handle | Chan | Spoor |
| Process namespace | Pgrp + namespace | Territory |
| Memory object | Vmo | Burrow |
| Device registry | devtab | bestiary |
| WFI halt loop | _hang | _torpor |
| First userspace process | /init | /joey |
| Kernel panic | panic | extinction |

**Kept (Plan 9 portability)**: `Proc`, `Thread`, `Rendez`, `sleep`, `wakeup`, `rfork`, `exits`, `wait_pid`, `bind`, `mount`, `unmount`, `RFNAMEG`, `sched`, `ready`, `Dev` vtable, `Walkqid`, `qid`.

**Kept (Stratum continuity)**: audit-prosecutor agent name.

## Reference maintenance discipline

Per CLAUDE.md: maintain BOTH technical reference (`docs/REFERENCE.md` + `docs/reference/NN-*.md`) AND user reference (`docs/USER-MANUAL.md` + `docs/manual/`) per-chunk. Phase 3 reference docs span 14-process-model through 29-joey. Phase 4 will add 30-dev-spoor (P4-A), 31-cons (P4-B), 32-virtio (P4-F), 33-irqfwd (P4-G), etc.

## Open follow-ups

- **Trip-hazard #157**: Phase 4 blocker. Investigation required before P4-I (userspace virtio-blk driver, first chunk that exec's a userspace process again).
- **Phase numbering**: scripture vs local — propose ROADMAP renumbering OR keep offset and document. User signoff required per CLAUDE.md scripture-binding.
- **Memory closed lists**: audit_r*.md files reference VMO/Chan/namespace as historical names. Future cumulative audits will use Burrow/Spoor/Territory; preamble logic still works (the literal IDs F127 etc. are stable).
