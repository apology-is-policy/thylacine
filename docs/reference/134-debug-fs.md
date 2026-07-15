# 134 — the kernel debug surface (`/proc/<pid>` debug-fs)

**Status**: 8a-1 (the software-checkpoint tier) as-built. Landed across
`3a3d3489` (alpha: `CAP_DEBUG` + the I-39 gate + the attach slot) →
`cb5c8d60` (widen: `CAP_HOSTOWNER` is a debug axis) → `811924e6` (beta: the
stop/resume state machine) → `4307a6f0` (gamma-1: cross-Proc memory) →
`aed98779` (gamma-2: regs + fpregs + the SPSR guard) → `e7ae09ea` (gamma-3:
kregs + kstack + wait) → `698034db` (8a-1c: the in-guest E2E + the two
defects it surfaced) → `7be95961` (the UBSan test-copy fix). Binding design:
`docs/DEBUG-FS-DESIGN.md`. Invariant: **I-39** (ARCH §28 + the §25.4
audit-trigger row, the authoritative prosecution copy). Spec:
`specs/debug_stop.tla` (the stop/continue/step state machine — clean cfg +
4 buggy cfgs, TLC-green; model-first). The 8a-2 hardware-debug tier (real
breakpoints/watchpoints/single-step via the arm64 debug registers) has its
empirical delivery verify landed at **8a-2a** (§ below); the full per-thread
install + single-step + watchpoints are **8a-2b**.

## Purpose

A cross-Proc inspector realized as files, the Plan 9 devproc model on the
Thylacine kernel: a debugger **stops** a target EL0 thread at a checkpoint,
**reads/writes its registers and memory**, **walks its unified user→kernel
stack**, and **resumes** it — with no debug-register code (software
checkpoint only). It is the kernel half of the Go IDE Stage 8 cross-boundary
debugger (`docs/GO-IDE-DESIGN.md`); the `dlv` backend (8c) drives these files.

The surface is added to the existing `devproc` Dev (`dc='p'`), NOT a new Dev —
it reuses devproc's pid-keyed qid encoding (`qid.path = (pid<<32)|subkind`,
the subkind in the low 32 bits) and its two-level walk, so the debug files
are flat additions (new `PQS_*` enum values + `g_proc_pid_files[]` rows +
read/write dispatch cases), no walk-state-machine surgery.

## The debug files

| File | Mode | Semantics |
|---|---|---|
| `ctl` (extend) | `0600` | debug verbs (below), each per-verb I-39-gated, added to the existing kill/killgrp parser |
| `mem` | `0600` | RW the target's user memory via its `pgtable_root`; **stopped-only**; `off` = the target VA; a non-resident VA (unfaulted lazy-anon / REVENANT FILE) reads/writes as "not resident" (0 bytes moved) |
| `regs` | `0600` | RW the saved EL0 GPR frame (`x0..x30`, `SP_EL0`, `ELR_EL1`, `SPSR_EL1`); writes stopped-only; **the SPSR is READ-ONLY on write** (the privilege guard) |
| `fpregs` | `0600` | RW the saved FP/SIMD frame (`V0..V31` + FPSR + FPCR); stopped-only writes |
| `wait` | `0400` | a read blocks until the target stops at a checkpoint / exits / the caller is death-interrupted (the debugger's stop-notification channel) |
| `kregs` | `0400` | RO the kernel-side saved callee-saved frame (`x19..x28` + fp/lr/sp + `tpidr_el0`) — the input to the unified stack walk; `tpidr_el0` is the EL0 TLS base `dlv` reads to recover the Go `g`; `ttbr0` is deliberately omitted (a kernel-pgtable PA — an info-leak with no debug value) |
| `kstack` | `0400` | RO the target head thread's symbolized kernel backtrace (the 8a-1 half of the unified user→kernel stack) |

`devproc` is **`.seekable = true`** — the debug files are positioned I/O
(`mem` VA-addressed; `regs`/`kregs`/`kstack`/`wait` struct/content-offset), so
`SYS_PREAD`/`PWRITE`/`LSEEK` must reach them. Without the flag the #37 ESPIPE
gate (`sys_pread`/`sys_pwrite`, `kernel/syscall.c`) rejects positioned I/O on
a non-seekable Dev *before* `devproc_read` runs. This was the 8a-1c F1 defect:
the debug files were unusable via `t_pread`/`t_pwrite` even though `devproc_read`
handled `off` correctly (Plan 9's `/proc/<pid>/mem` is seekable — you seek to
the address). The pre-debug files (`status`/`cmdline`/`ns`) already slice at
`off`; `ctl` is a control channel whose verb parse ignores `off` (positioned
writes are the same kill/attach/stop authority — no I-26/I-39 change).

## Run-control verbs (`ctl`)

Each is authorized by the I-39 two-axis gate (distinct from the kill verbs'
I-26 gate — per-verb branching before dispatch):

- `attach` — claim the one-debugger slot (`Einuse` if taken); the claim is
  owned by *this* open ctl fd (`Proc.debug_owner`, an identity token, never
  dereferenced, `g_proc_table_lock`-guarded). The `CDEBUGOWNER` Spoor flag
  gates the close-hook release.
- `stop` — request the target's threads park at their next checkpoint; blocks
  the writer (outside the lock, a bounded re-poll) until stopped / the target
  exits / the slot is released / the caller is death-interrupted.
- `start` — resume (requires the slot owner); clears the stop flag, wakes the
  debugger rendez. Each woken thread re-runs `el0_return_die_check` after
  unpark, so a concurrently-flagged death still wins.
- `waitstop` — block the writer until the target is stopped/exits.
- `detach` — release the slot; **resumes** the target (implicit on ctl-fd
  close, since handles close at exit per #68/#926 — a dead debugger provably
  resumes its quarry).

## Implementation

### The I-39 gate (`devproc_debug_authorized`)

`devproc.c`. The two-axis check at each debug read/write site (modes are
advisory — `devproc.perm_enforced == false` — so the gate is at the SITE, the
`devctl_kernel_base_readable` template): **owner** (`caller->principal_id ==
target->principal_id`) **OR** `CAP_HOSTOWNER` **OR** `CAP_DEBUG`. `kproc`
(pid 0) and `PROC_FLAG_NOTRACE` targets are refused. `CAP_DAC_OVERRIDE` is
deliberately NOT a debug axis (fs-admin stays orthogonal to debug). `attach`
gates on the same predicate; the run-control verbs (`stop`/`start`) gate on
the STRICTER slot-ownership (`debug_owner == ctl`), so a stranger who could
attach but hasn't cannot drive another debugger's target.

### The stop checkpoint (`el0_return_stop_check`, `kernel/proc.c`)

The single structurally-safe park is the **EL0-return tail** (`vectors.S`:
the `0x480` IRQ tail + `.Lel0_sync_return`): zero locks held,
`preempt_count == 0`, and the DAIF-masked `eret` still ahead. The stop is a
new leg ordered **after** `el0_return_die_check` (death always wins) and
beside `notes_deliver_at_el0_return`:

```
tail: preempt_check_irq → el0_return_die_check → [notes] → STOP-CHECK → eret
```

On observing a pending `debug_stop_req`, the thread parks via
`sleep(&t->debug_rendez, ...)` — the audited register-then-observe under
`wait_lock` (I-9), on its OWN per-Thread rendez (single-waiter; a multi-thread
target parks each thread on its own rendez). The loop re-checks
`group_exit_msg` on every wake and calls `thread_exit_self()` (noreturn) if a
group termination raced the stop (`DeathWinsOverStop`). **A stop is NEVER
observed inside `sleep()`/`tsleep()`** — that slot is the #811
death-interruptible check, whose job is to *unwind* the syscall; death
unwinds, a stop parks-and-reparks and preserves the in-progress syscall state.

Delivery (`proc_debug_stop_deliver`): set the per-Proc flag (RELEASE) then
`smp_resched_others` (a broadcast reschedule — the proven death-delivery
vehicle, not a targeted SGI at 8a-1). "Fully stopped" =
`devproc_target_fully_stopped`: the target is ALIVE, `debug_stop_req` is set,
and every non-EXITING thread is parked on its own `debug_rendez` with
`on_cpu == false` (the #788 spin-until-off-cpu discipline — a thread mid
`cpu_switch_context` still reads `on_cpu == true`, so a coherent mem/reg read
must wait for it to fully deschedule).

Resume (`proc_debug_resume`): clear `debug_stop_req` (RELEASE, ordered before
the per-peer wake — the I-9 close) then wake each thread parked on its own
`debug_rendez`.

### The trapframe pointer (the 8a-1c F2 fix)

The EL0-entry trapframe (`struct exception_context`, 288 B) is **NOT at a
fixed kstack offset**. `KERNEL_ENTRY` builds it at `SP_EL1 − 288` where
`SP_EL1` is the kernel SP at the exception; that equals `kstack_top − 288`
only for a thread's very first entry. For a *running* thread the outermost
EL0 frame sits lower (ground truth from the E2E: a looping child's
`x20`-holding frame was ~0x260 below `kstack_top − 288`, and the fixed-offset
read returned KP_ZERO'd stack). So `el0_return_stop_check(struct
exception_context *ctx)` takes the vector-supplied `ctx` (== the current SP ==
the trapframe base — both EL0-return tails do `mov x0, sp` before the `bl`;
the sync tail's `x0` was clobbered by `notes_deliver`) and records it into a
new `Thread.debug_trapframe` at the park. `devproc_build_regs` /
`devproc_apply_regs` read/write THAT pointer (validated within
`[kstack_base + GUARD, kstack_base + kstack_size − EXCEPTION_CTX_SIZE]`), NOT
the fixed offset. It is set on the park and cleared on the resume return, so
it is non-NULL and fresh exactly while the thread is fully-stopped — the only
window `build_regs` is reachable. The stop/resume state machine is unchanged,
so `debug_stop.tla` is unaffected.

`fpregs` (from `t->ctx.fp_v` + fpsr + fpcr), `kregs` (from `t->ctx`), and
`kstack` (from `t->ctx.lr`/`fp`) all read the SAVED kernel context, not the
trapframe, so they were never affected by F2 — only `regs` was.

### The SPSR privilege guard (`devproc_apply_regs`)

A `regs` write applies `x0..x30` + `SP_EL0` + `ELR_EL1` but **never**
`SPSR_EL1`: an arbitrary SPSR could `eret` the target to EL1 (privilege
escalation). `pc`/`sp`/GPR writes are EL0-safe because SPSR stays EL0t — a bad
`pc` just faults the target at EL0.

### Cross-Proc memory (`mmu_cross_proc_read`/`write`, `arch/arm64/mmu.c`)

A non-growing VA→PA walk cloned from `mmu_install_user_pte`: descend L0..L3 via
`pa_to_kva` per level against the *target's* `pgtable_root`, bail on any
`!PTE_VALID` (an empty L3 = "not resident" — the debug reader does NOT drive
`userland_demand_page` on the target), read the leaf, `pa_to_kva(leaf_pa)` for
direct-map access. I-13: `vaddr >> 47 != 0` rejects any kernel VA;
`PTE_OA_MASK` strips the leaf's UXN/PXN. The WRITE refuses an RO leaf (I-12/
I-36 — a debugger writes DATA; breakpoints are the 8a-2 HW path, never a
software `BRK` into shared REVENANT text). Taken under the target's `vma_lock`
(a leaf below `g_proc_table_lock`, which pins the target ALIVE across the copy
— no reap-UAF of `pgtable_root`). The rolling ASID (`context_id`) is never
assumed stable — the walk keys on `pgtable_root`.

### The unified stack (`halls_walk_kernel_frames`, `arch/arm64/halls.c`)

An ADDITIVE fp-chain primitive (NOT a refactor of `halls_backtrace`) — the
dying-machine Halls dump path stays byte-for-byte unchanged. Given the stopped
thread's `t->ctx.lr`/`fp`, it walks the kernel frames with explicit `[lo,hi)`
usable-kstack bounds (guard excluded) + a per-frame `fp+16 <= hi` gate (no
HX-I1 guard on this LIVE path — a wild `fp` can neither spin nor read
unboundedly), symbolized via the pure `halls_link_addr` + `halls_symbolize`
(the reloc-free HX-2 symtab). The kernel frames are the `kstack` file; the user
frames are the debugger's job (walk the EL0 `x29` chain via `regs` + `mem`);
stitched at the SVC boundary this is the single user→kernel call stack.

## Data structures

- `struct t_user_regs` (272 B, syscall.h) — the `regs` ABI (Linux
  `user_pt_regs`): `regs[31]` (x0..x30) @ 0, `sp` @ 248, `pc` @ 256,
  `pstate` @ 264. `_Static_assert`-pinned.
- `struct t_user_fpregs` (520 B) — the `fpregs` ABI: `vregs[512]` + `fpsr` +
  `fpcr`.
- `struct t_kernel_regs` (112 B) — the `kregs` RO ABI: `x[10]` (x19..x28) @ 0,
  `fp` @ 80, `lr` @ 88, `sp` @ 96, `tpidr_el0` @ 104. Field offsets mirror
  `struct Context`'s GP region so the build is a verbatim copy.
- `Thread.debug_trapframe` (`struct exception_context *`) — the recorded EL0
  trapframe (8a-1c F2). `struct Thread` 1152 → 1168 (8-byte field, 16-aligned
  pad).
- `Proc.debug_owner` (`struct Spoor *`) — the attach slot token.
- `Proc.debug_stop_req` (`_Atomic u32`) — the stop flag.
- `Thread.debug_rendez` (`struct Rendez`) — the per-Thread park rendez.

## Spec cross-reference

`specs/debug_stop.tla` (model-first, spec-first RE-ENABLED for this surface):
Safety = `NoLostStop` + `DeathWinsOverStop` (`EventuallyAllDead`) +
`NoEL0AfterStopped` + `ExactlyOnceResume`; Liveness = `EventuallyResumed`
(`NoStrand`). Clean cfg TLC-green (2264 distinct, depth 27). 4 buggy cfgs each
violate their named property: `park_before_die` → `EventuallyAllDead`,
`lost_stop` → `NoLostStop`, `double_wake` → `ExactlyOnceResume`,
`strand_on_debugger_death` → `EventuallyResumed`. The model mirrors
`death_wake.tla` (register-then-observe under the per-Thread `wait_lock`). The
8a-1c F1/F2 fixes do not touch the modeled state machine (F1 is a Dev flag; F2
records a pointer), so the spec is unchanged and stays green. Action↔site map:
`specs/SPEC-TO-CODE.md::debug_stop.tla`.

## Tests

- Kernel (`kernel/test/test_devproc.c`): `debug_authorized_predicate`,
  `debug_attach_detach_lifecycle`, `debug_stop_start_resume`, `debug_mem`,
  `debug_regs` (the SPSR guard + `debug_trapframe`), `debug_kregs_kstack_wait`.
  These exercise the read/write logic against a synthetic thread-less /
  hand-parked target — they call `devproc_read`/`build_regs` directly (so they
  cannot exercise the `SYS_PREAD` seekable gate — the F1 blind spot) and place
  the synthetic frame where the code reads it (so they cannot exercise the F2
  location bug).
- In-guest E2E (`usr/debug-probe` + `usr/debug-child`, joey-spawned pre-pivot,
  boot-fatal): the ONLY coverage of a genuinely-parked EL0 thread —
  attach → stop → regs/mem/kregs/kstack/wait → cross-Proc mem write → resume →
  reap 0. This is the regression witness for F1 and F2 (the kernel tests were
  structurally blind to both).

## Error paths

- A read/write of a debug file returns `-1` on: not-found (pid mismatch),
  `kproc`, not-authorized (I-39), or not-fully-stopped. `mem` returns the bytes
  moved (0 at a hole). `regs` returns 0 if `debug_trapframe` is NULL / out of
  bounds. A `t_pread`/`t_pwrite` on a debug file returns `-1` up front if
  `devproc` were ever made non-seekable again (the F1 regression surface).
- `attach` → `Einuse` (`-1`) if the slot is taken. Run-control verbs → `-1`
  for a non-owner. `wait`/`stop` → `-1` if the caller is death-interrupted.

## 8a-2a — the empirical hardware-debug verify

The 8a-2 HW-debug tier depends on one empirical fact the design (DEBUG-FS-DESIGN
§2, §10) required PROVEN on the dev host before building on it: does a
GUEST-programmed EL0 hardware breakpoint deliver its debug exception (`EC 0x30`)
to guest EL1? TCG fully emulates guest debug; macOS HVF on Apple Silicon is the
young path (it applies the guest's `DBGBVR/DBGBCR` to real hardware and calls
`hv_vcpu_set_trap_debug_exceptions(false)` when no external gdb is attached to
QEMU — the Thylacine dev loop). 8a-2a answers it end to end.

**The reusable 8a-2b prerequisites** (`arch/arm64/hwdebug.c`):

- `hwdebug_init_cpu()` — clears the OS Lock (`OSLAR_EL1 = 0` + `OSDLR_EL1 = 0`,
  LOCKED at reset — it suppresses debug exceptions) + idles `MDSCR`/`DBGBCR0`.
  Banked per-PE → run on every CPU (`kernel/main.c` boot CPU; `kernel/smp.c`
  `per_cpu_main` each secondary).
- `hwdebug_enumerate()` — reads `ID_AA64DFR0_EL1` into
  `g_hw_features.num_brps`/`num_wrps` (the `.BRPs`/`.WRPs` field + 1;
  architectural min 2/2). Called from `hw_features_detect`.

**The verify** (the self-scoped `hwverify` ctl verb + `usr/hwbp-verify`): a Proc
writes `hwverify <hexva>` to its OWN `/proc/<pid>/ctl` (SELF-ONLY — the target
pid must equal the caller, which satisfies I-39 by construction since a Proc
owns itself); the kernel arms `DBGBVR0 = va`, `DBGBCR0 = 0x1E5` (`E=1`,
`PMC=0b10` EL0-only, `BAS=0xF`), `MDSCR.MDE = 1`. The probe then executes the
armed VA (calls `hwbp_target`); the `EC 0x30` handler
(`exception_sync_lower_el_impl`) calls `hwdebug_verify_on_ec`, which records the
delivery + disarms (so the resumed EL0 instruction proceeds) and returns true —
the empirical proof. The probe reads the verdict back from the same ctl file
(`hwverify fired=<0|1> elr=0x..`, gated so only the arming Proc sees its own
result — no cross-Proc leak).

**The verdict is not a boot gate.** `fired=1` → PASS (8a-2 HW-debug enabled).
`fired=0` (never trapped) is a LEGITIMATE verdict — "HW debug does not deliver
under this accel, so the 8a-2 tests run TCG-only" — logged, boot continues. Only
a malfunction (the probe SNARE-terminated on a stray EC, or the ctl surface
misbehaving) fails the boot. **Measured 2026-07-15: PASS on BOTH HVF (`-cpu
host`, Apple M2, GICv2) AND TCG (`-cpu max`, GICv3), attempt 1** — the research
verdict empirically confirmed, so 8a-2b builds on the HW path, not TCG-only.

Tests: the EL1 half is `hwdebug.dfr0_enumerate` (>= 2 bp/wp) +
`hwdebug.arm_disarm_roundtrip` (the `DBGBVR0`/`DBGBCR0`/`MDSCR.MDE` writes
stick + a concurrent arm is refused + disarm clears them). The DELIVERY half
needs real EL0 execution and is the in-guest `/hwbp-verify` probe (a kernel test
runs at EL1 with no EL0-scheduling loop).

## Status

8a-1 complete (the software-checkpoint tier: audited, SMP-gated, `debug_stop.tla`
green). 8a-2a complete (the HW-debug delivery verify: PASS on HVF + TCG). 8a-2b
(the full arm64 HW-debug tier: the `DBGBCR`/`BVR`/`WCR`/`WVR` + `MDSCR.MDE` +
the EC routing to `/proc/<pid>/wait` + single-step + the step-over-breakpoint
dance + the per-thread register install on ctx-switch) + 8a-2c (its focused
Fable-5-max holotype + the SMP gate) are next.

## Known caveats / footguns

- **The trapframe is not at `kstack_top − 288`.** Any future consumer of a
  stopped thread's EL0 frame MUST read `Thread.debug_trapframe` (set by
  `el0_return_stop_check` at the park), never a fixed kstack offset. The fixed
  offset is correct only for a thread's first entry.
- `debug_trapframe` is valid ONLY while the target is fully-stopped (parked in
  `el0_return_stop_check`). Reading it otherwise (a running thread) is
  meaningless — the gate (`devproc_target_fully_stopped`) enforces this.
- `regs`/`fpregs`/`kregs` read the HEAD thread (`p->threads`). A per-thread
  `/proc/<pid>/thread/<tid>/` layer is v1.x.
- The stop is a checkpoint stop (Plan 9 `startsyscall` semantics), not
  preemptive: a thread blocked deep in a syscall sleep stops at the *next*
  checkpoint it reaches, not by interrupting the sleep. An explicit
  stop-a-sleeping-thread-now is a v1.x refinement.
- Software breakpoints are FORBIDDEN (I-12 W^X + I-36 REVENANT Image cache) —
  the `dlv` backend routes breakpoints to the 8a-2 HW path.
- The 8a-2a `hwverify` breakpoint is GLOBAL (one at a time) and armed on the CPU
  where the ctl-write ran; the `EC 0x30` handler disarms the CPU that fired
  (= the arm CPU). The only unpaired case is a not-fired verify whose thread
  MIGRATED off the arm CPU in the single-instruction arm→trap window — leaving
  `DBGBCR0`/`MDE` set on the migrated-away CPU. In the boot context this is
  unreachable (a single serial verify, no competing runnable thread to migrate
  to, and it fires on attempt 1 on both HVF and TCG — so the not-fired path is
  never taken). The proper closure is 8a-2b's per-thread install (arm on
  ctx-switch-IN to the debugged thread on its own CPU, disarm on switch-OUT),
  which makes arm-CPU == run-CPU == disarm-CPU by construction. `hwverify` is a
  self-scoped verify affordance that 8a-2b's cross-Proc `hwbreak <va>`
  supersedes.
