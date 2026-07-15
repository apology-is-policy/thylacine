# DEBUG-FS-DESIGN — the kernel debug surface (Go IDE Stage 8a)

**Status: DESIGN (focused pass, ratified 2026-07-14, user-voted). Scripture,
no code.** This is the Stage 8a focused design pass mandated by
`docs/GO-IDE-DESIGN.md §8` ("each kernel sub-stage opens with its own focused
design pass + audit — it is a new privilege surface"). It designs the kernel
half of the cross-boundary debugger: the `/proc/<pid>` debug filesystem, the
stop/continue/single-step state machine, cross-Proc memory + register access,
the unified user->kernel stack walk, arm64 hardware breakpoints/watchpoints, and
the debug-authority invariant **I-39**. It is prose- and spec-validated (per the
2026-05-23 suspension, with spec-first RE-ENABLED for the stop/step state
machine — a race-bearing wait/wake surface on the death-path lineage).

The userspace half (the `dlv` `proc_thylacine` backend, `gopls`, the Nora plugin
architecture, the Kaua debug UI) is Stages 8c-8f and is NOT designed here; this
doc defines exactly the kernel primitives those layers drive.

---

## 1. The four ratified decisions (2026-07-14 user vote)

1. **`CAP_DEBUG` is a clearance-grantable, elevation-only capability.** Bit
   `1<<10`, added to `CAP_ELEVATION_ONLY` (so `rfork` strips it from every child)
   and `CAP_GRANTABLE_CLEARANCE` (so it flows through the existing 32-byte
   corvus-gated clearance grant -> legate machinery, scope- and time-bounded).
   THREE header edits, ZERO new `devcap.c` code (§7.1).
2. **The stop is handle-lifetime-tied.** "Attached/stopped" is owned by the open
   debug ctl fd; its close — including debugger death, which closes handles at
   exit via #68/#926 — resumes and detaches the target. One debugger per target
   (the Plan 9 `Einuse` shape, realized on our handle table). Strand-freedom is
   structural, not a bolted-on reaper (§7.2).
3. **Spec-first is RE-ENABLED for the stop/continue/step state machine.**
   `specs/debug_stop.tla` is written model-first, TLC-green with buggy cfgs,
   BEFORE the 8a-1 impl (§6). The precedent is `death_wake` / `loom` / `asid` /
   `allowance` — each re-enabled because the tests are structurally blind to the
   SMP wait/wake race the model catches. This surface sits on the most bug-prone
   lineage in the tree (#788/#806/#860/#809/#811/#68).
4. **8a splits into two tiers.** **8a-1** (software checkpoint): the debug-fs +
   the I-39 gate + stop-at-checkpoint + cross-Proc mem/regs + the unified stack
   walk — a working inspector with ZERO debug-register code, landed and audited
   first. **8a-2** (arm64 HW debug): breakpoints/watchpoints/single-step + the
   EC-routing + the step-over-breakpoint dance + per-thread register install,
   gated behind the audited checkpoint tier and the empirical HVF verify.

---

## 2. Feasibility — the ground-truth verdict

Four parallel ground-truth passes (devproc+caps; thread-lifecycle+stop; symtab+EC
routing; arm64-recipe+QEMU/HVF) establish the surface is buildable on this dev
loop with no blockers:

- **The dev loop exercises the whole thing.** Guest self-hosted EL0 debugging
  WORKS under macOS HVF (QEMU >= 8.2): QEMU applies the *guest's*
  `DBGBVR/DBGBCR` to real hardware and calls
  `hv_vcpu_set_trap_debug_exceptions(false)` whenever no external gdb is attached
  to QEMU's own gdbstub — which is exactly the Thylacine dev loop. TCG fully
  emulates guest-programmed bp/wp/single-step (EC 0x30/0x32/0x34 to guest EL1)
  and is the mature, host-independent fallback. The single constraint (guest
  HW-debug XOR external-gdb-on-QEMU-using-HW-breakpoints — they contend for one
  physical register set) never bites our workflow. **8a-2 must still verify HVF
  empirically on the dev host before shipping** (HVF debug is young; QEMU >= 8.2
  required).
- **The arm64 model needs no exotic state to debug EL0 from EL1.** `KDE` is NOT
  needed (it enables only same-EL debug exceptions); `PSTATE.D` masks only
  same-EL debug exceptions, so debugging EL0 requires ZERO `PSTATE.D`
  manipulation. Requirements: `MDSCR_EL1.MDE=1` (HW bp/wp) and the SS machine
  (step); clear the OS Lock at boot. (`KDE`+`PSTATE.D=0` matter only for the
  EL1-self-debug ambition, a deferred stretch — §9.)
- **Stage 8b is mostly refactor-and-expose.** The Halls fp-chain walker is pure
  given a `ctx` + kstack bounds (it reads kernel VAs through TTBR1, so any
  thread's kstack is walkable from any CPU); it needs its `static` primitives
  exposed, explicit `[lo,hi)` bounds instead of the `fp+512KiB` dying-machine
  heuristic, and a bypass of the `halls_frame_is_live` gate (purpose-built to
  reject other-stack frames). Kernel DWARF already ships in the host
  `build/kernel/thylacine.elf` (the on-target flat `.bin` carries only the
  2828-symbol HX-2 symtab); userspace Rust/C binaries already ship
  `.debug_info + .symtab` in the ramfs; Go keeps arm64 frame pointers, so
  goroutine stacks walk with the same logic.
- **The kernel stop is one clean insertion.** A stopped thread parks at the
  EL0-return tail — the only zero-lock, `preempt_count==0`, clean-saved-frame
  point — and prior scaffolding already anticipated this (`SYS_SET_TRACEABLE` +
  `PROC_FLAG_NOTRACE`; the `EC_BRK` handler's "Debuggers attaching at EL0 are a
  Phase 5+ concern" note).

---

## 3. Invariant I-39 — debug authority bounded

> **I-39 (debug authority bounded, never bypasses memory-safety).** A Proc may
> debug a target iff it can *name* `/proc/<pid>` in its namespace AND passes the
> two-axis gate (**owner** — same `principal_id` on the `0600` ctl — **OR** the
> capability axis `CAP_HOSTOWNER`/`CAP_DEBUG`), the I-26-analog (owner OR the host
> owner OR the domain cap, exactly as the kill gate is owner OR
> `CAP_HOSTOWNER`/`CAP_KILL`). `CAP_HOSTOWNER` is a debug axis (the Plan 9 "eve"
> super-user shape; the host owner already kills/chowns/DAC-overrides any target,
> and debug is strictly less invasive than kill — user-voted 2026-07-15);
> `CAP_DEBUG` is the clearance-grantable cross-identity axis for a non-hostowner
> debugger; `CAP_DAC_OVERRIDE` is deliberately NOT a debug axis (fs-admin stays
> orthogonal to debug, as it is to kill). The debug surface confers no authority
> beyond that gate. Register/memory reads AND writes are permitted only on a **stopped**
> target (a running target's mem/regs are refused). No debug operation (a) writes
> executable text (I-12 W^X holds — breakpoints are hardware, never a software
> `BRK` patched into text), (b) corrupts the shared REVENANT Image cache (I-36
> holds — the same reason), or (c) reads/writes memory outside the target's own
> address space (the cross-Proc walk resolves strictly through the *target's*
> `pgtable_root`, bounded by the target's VMA list). A stop is bounded by an
> explicit revocable ownership (the open debug ctl fd): a debugger's death or
> detach provably resumes the target — no debugger can strand its quarry.
> `kproc` (pid 0) is never debuggable; a Proc's own session/console-owner and
> `PROC_FLAG_NOTRACE` policy seams are honored.

Enforcement: the two-axis gate at each debug read/write site (the
`devctl_kernel_base_readable` pattern — atomic-load `caller->caps`, check owner
OR `CAP_HOSTOWNER`/`CAP_DEBUG`, NULL->deny); the stopped-only guard on mem/reg access; HW
breakpoints (never software `BRK`) preserving I-12/I-36; the cross-Proc VA->PA
walk confined to the target `pgtable_root` under the target `vma_lock`; the
handle-lifetime-tied stop ownership. Validation: `specs/debug_stop.tla` (the
stop/continue/step SM) + prose in this doc + ARCH §28 + the focused 8a audits +
the SMP gate.

I-39 is the I-26 (cross-Proc kill) analog for the read/write/control axis, and
composes — adds no bypass to — I-1 (namespace containment), I-12 (W^X), I-13
(kernel/user isolation), I-22 (no ambient super-authority; `CAP_DEBUG` is
elevation-only, never an identity), I-36 (the Image cache), and the #811/#68
death-path invariants (§8).

---

## 4. 8a-1 — the software-checkpoint tier

The first landing: a working cross-Proc inspector — stop a target at a checkpoint,
read/write its registers and memory, walk its unified user->kernel stack — with
NO debug-register code. This is the Plan 9 devproc model (stop at a syscall/note
boundary, not preemptively) realized on the Thylacine kernel.

### 4.1 The debug files (added to `devproc`, not a new Dev)

Ground truth: `devproc` (`dc='p'`) encodes qids as `(pid<<32)|subkind` with the
subkind occupying the full low 32 bits (0..5 used today — ample room). Per-pid
files today: `status`, `cmdline`, `ctl`, `ns`. The debug surface adds flat files
(no subdir — the 2-level walk stays intact):

| File | Mode | Semantics |
|---|---|---|
| `ctl` (extend) | `0600` | debug verbs (§4.3), per-verb I-39 gate, ADDED to the existing kill/killgrp parser |
| `mem` | `0600` | RW the target's user memory via its `pgtable_root`; **stopped-only** for both R and W (I-39); a non-resident VA (unfaulted lazy-anon / REVENANT FILE) reads as "not resident" |
| `regs` | `0600` | RW the saved EL0 GPR frame (`x0..x30`, `SP_EL0`, `ELR_EL1`, `SPSR_EL1`); writes stopped-only |
| `fpregs` | `0600` | RW the saved FP/SIMD frame; stopped-only writes |
| `wait` | `0400` | read blocks until the target stops at a trap/checkpoint (the debugger's stop-notification channel) |
| `kregs` | `0400` | RO the kernel-side saved frame (`t->ctx` callee-saved + the kstack PC) — feeds the unified stack (§4.6) |

Every readable debug file gates at the READ site on the I-39 two-axis check
(modes are advisory — `devproc.perm_enforced==false`), following the
`devctl_kernel_base_readable` template. `ctl` open stays the bare
`dev_simple_open` shared with kill users; authority is per-verb at the write site
(§4.3). Adding these is: new `PQS_*` enum values, new `g_proc_pid_files[]` rows,
and new read/write dispatch cases — no walk-state-machine surgery.

### 4.2 The stop checkpoint (where a stopped thread parks)

The single structurally-safe park is the **EL0-return tail** (`vectors.S:328`
sync + the `0x480` IRQ tail): zero locks held, `preempt_count==0` guaranteed, the
clean saved EL0 frame at `kstack_base + KSTACK_TOTAL - 288`, and the DAIF-masked
`eret` still ahead. A stop becomes a **new fourth leg** there, ordered AFTER
`el0_return_die_check` (so death always wins over a stop) and beside
`notes_deliver_at_el0_return`:

```
tail: preempt_check_irq -> el0_return_die_check -> [notes] -> STOP-CHECK -> eret
```

On observing a pending stop, the thread transitions to `THREAD_SLEEPING` under
its `wait_lock` (the `sleep()` discipline) and sleeps on a per-Proc *debugger
rendez* — it does NOT proceed to `.Lexception_return`. Nothing parks at the tail
today (all legs are check-and-die / check-and-deliver), so a stop is the first
blocking tail path; it must park cleanly (sleep on a rendez, re-run the tail on
resume), never spin.

**The sharp line: a stop is NEVER observed inside `sleep()`/`tsleep()`.** That
slot is the #811 death-interruptible check, whose job is to *unwind* the syscall
(`SLEEP_INTR`). Death unwinds; a stop parks-and-reparks and MUST preserve the
in-progress syscall state. So a thread blocked deep in a syscall sleep is stopped
at the *next* checkpoint it reaches (Plan 9's `startsyscall` semantics), not by
interrupting the sleep. (An explicit-stop-of-a-sleeping-thread — a debugger that
wants a blocked thread to stop *now* — is a v1.x refinement; v1.0 stops at
checkpoints, matching Plan 9's non-preemptive stop.)

### 4.3 Run-control verbs (on `ctl`, the Plan 9 idiom)

The `ctl` write parser (today: `kill`/`killgrp`) gains debug verbs, each
authorized by the I-39 two-axis gate (distinct from the kill verbs' I-26 gate —
per-verb branching before dispatch):

- `attach` — claim the one-debugger slot (`Einuse` if taken); the claim is owned
  by this open ctl fd (§7.2).
- `stop` — request the target's threads park at their next checkpoint; blocks the
  writer until stopped (the Plan 9 `procstopwait` shape) or the target exits.
- `start` — resume (requires stopped); clears the stop flag, wakes the debugger
  rendez. Each woken thread re-runs `el0_return_die_check` after unpark, so a
  concurrently-flagged death still wins.
- `step` — resume for exactly one instruction (8a-2 wires this to the SS machine;
  8a-1 may approximate with syscall-granularity `startsyscall`).
- `waitstop` — block the writer until the target is stopped/exits (the
  notification join; mirrors `/proc/<pid>/wait`).
- `hwbreak <addr>` / `hwwatch <rwx> <addr> <len>` — 8a-2 only (arm the arm64
  registers, §5).
- `detach` — release the slot (implicit on ctl-fd close): resume + clear all
  breakpoints/watchpoints installed for this target.

### 4.4 Stop delivery (SMP)

Set a per-Proc stop flag (a new field, read at the EL0-return tail), then force
each of the target's threads to a checkpoint:

- A thread sleeping in a syscall or IRQ-preempted is already in the kernel; it
  reaches the tail when its syscall/handler returns.
- A thread RUNNING at EL0 on another CPU has no saved frame — send it a
  **targeted `gic_send_ipi(cpu, STOP_SGI)`** (SGIs 1/2/3 are reserved-and-free;
  the receive handler bumps a counter, the wake traps the CPU into `0x480` ->
  `preempt_check_irq` -> the tail). The 1 kHz tick is the floor if an IPI is
  missed.

The delivery mirrors the `proc_group_terminate` cascade (walk `p->threads`,
per-peer `wait_lock`) but wakes-and-reparks at the tail rather than terminating.
"Stopped" is reached when every non-EXITING thread of the target is parked on the
debugger rendez with `on_cpu==false`.

### 4.5 Cross-Proc memory + register access (new code)

Every `uaccess_*` and every `pgtable_root` reader today operates on the CURRENT
`TTBR0` only. The debug read/write is genuinely new:

- **Registers.** Once the target is stopped and `on_cpu==false` (the #788
  spin-until-off-cpu discipline — never trust `on_cpu==true`), its saved EL0
  frame is the topmost frame on its kstack at `kstack_base + KSTACK_TOTAL - 288`
  (`regs[31]` + `SP_EL0@0xF8` + `ELR_EL1@0x100` + `SPSR_EL1@0x108`). TLS/TTBR0
  are NOT in the trapframe — they are in `t->ctx` (`tpidr_el0@104`, `ttbr0@112`);
  the debugger reads TLS from `t->ctx.tpidr_el0` (load-bearing for Delve's Go `g`
  recovery, §11). Register writes are stopped-only (I-39) and land in the saved
  frame, taking effect on resume.
- **Memory.** A non-growing VA->PA walk cloned from `mmu_install_user_pte`
  (descend L0..L3 via `pa_to_kva` per level, bail on any `!PTE_VALID`, read the
  leaf, `pa_to_kva(leaf_pa)` for direct-map access) against the *target's*
  `pgtable_root`, taken under the target's `vma_lock` for a coherent VMA/PTE
  snapshot. An empty L3 (an unfaulted `BURROW_TYPE_ANON_LAZY` or REVENANT
  `BURROW_TYPE_FILE` page) reads as "not resident" — the debug reader does NOT
  drive `userland_demand_page` on the target (that path assumes `current` is the
  faulting Proc and would fault-in on the debugger's behalf; v1.0 reports
  not-resident, honest to the fault model). Cross-Proc writes obey I-13 (they
  reach only the target's own address space) and stopped-only. The rolling ASID
  (`context_id`) is never assumed stable — the walk keys on `pgtable_root`, not
  an ASID.

### 4.6 The unified user->kernel stack (the headline, 8a-1 half)

The Halls fp-chain walker becomes reusable on a live stopped thread (§2): expose
`halls_backtrace`/`halls_emit_code_addr`/`halls_peek`, pass explicit `[lo,hi)`
kstack bounds (`kstack_base .. kstack_base+kstack_size`) instead of the
`fp+512KiB` heuristic, and bypass `halls_frame_is_live` (which rejects
other-stack frames by design). Given the stopped thread's saved `x29` (from its
trapframe or `t->ctx`), the walker produces the kernel frames symbolized via the
HX-2 symtab (`func+offset`, already reloc-free + KASLR-independent). The user
frames come from the Go/native x29 chain walked over the target's mem (`mem`
file). Stitched at the SVC boundary (the syscall trampoline frame), this is the
single user->kernel call stack — every frame symbolized on-target (kernel via
HX-2, userspace via the ramfs `.symtab`); DWARF locals are the 8b host-`.elf`
plumbing (out of 8a scope, but the walk is here).

---

## 5. 8a-2 — the arm64 hardware-debug tier

Gated behind the audited 8a-1 checkpoint tier and the empirical HVF verify. Adds
the debug registers so breakpoints/watchpoints/single-step work — never a
software `BRK` (I-12 W^X: text is RO+X; I-36: a `BRK` written into one debuggee's
text corrupts the page shared by every process running that binary via the
REVENANT Image cache). This decision is not merely principled — it is FORCED: the
same two invariants make Delve's default software-breakpoint path impossible, so
the `proc_thylacine` backend (8c) routes breakpoints to this HW path.

### 5.1 Boot: unlock + enumerate

- Clear the OS Lock at boot (it is LOCKED at reset): `OSLAR_EL1=0` +
  `OSDLR_EL1=0` + `ISB`. QEMU models this (TCG + HVF).
- Enumerate `hwfeat.c`: read `ID_AA64DFR0_EL1` (`.BRPs`/`.WRPs` = count-1;
  architectural min 2/2, QEMU cortex-a57 = 6/4) into `g_hw_features`. (Today
  `hwfeat.c` reads ISAR0/1 + PFR0/1, not DFR0.)

### 5.2 EL0 breakpoint / watchpoint recipe

- **Breakpoint** `DBGBCR<n>_EL1 = 0x1E5` (`E=1`, `PMC=0b10` EL0-only, `BAS=0xF`,
  `HMC=0`, `SSC=0b00`, unlinked); `DBGBVR<n>_EL1` = 4-byte-aligned user VA with
  correct RESS sign-extension. `ISB` after every `DBGB*` write.
- **Watchpoint** `DBGWCR<n>_EL1` (`E=1`, `PAC=0b10` EL0, `LSC`=load/store,
  8-bit `BAS`, `MASK` for >8-byte power-of-2 ranges); `DBGWVR<n>_EL1` =
  8-byte-aligned VA. FAR on a watchpoint exception may be imprecise -> a
  closest-match heuristic (the Linux `get_distance_from_watchpoint` shape).
- `MDSCR_EL1.MDE=1` to enable HW bp/wp exceptions. No `KDE`, no `PSTATE.D` change
  (EL0 debug from EL1, §2).

### 5.3 Per-thread register install (load-bearing)

The DBGB*/DBGW* registers are the single physical unit set — NOT auto-banked, and
`PMC=EL0` alone does NOT isolate two processes sharing a user VA. So the debugged
thread's breakpoints are installed (write `DBGBVR/DBGBCR` + set `MDE`) on
context-switch-IN and disabled on switch-OUT, keyed to the target thread (the
Linux per-task model). This is a scheduler-adjacent hook (`sched.c` switch path)
— it composes with the on_cpu protocol and must be prosecuted for the SMP race
(a breakpoint installed for thread A must never fire for thread B on another CPU).

### 5.4 EC routing (the exception side)

Route the four debug ECs from EL0 in `exception_sync_lower_el_impl`'s
`switch(ec)` (the clean insertion point at `exception.c:349`), replacing today's
`default -> proc_fault_terminate(SNARE_ILL)` (and the `EC_BRK -> SNARE_BRK`
"Phase 5+ concern" arm):

- `0x30` breakpoint (ELR=PC; no bp-number in base arch -> identify by matching
  ELR against the target's DBGBVRs),
- `0x32` software step (ELR=next PC; `EX`=stepped-was-load/store-exclusive),
- `0x34` watchpoint (FAR=VA, possibly imprecise; `WnR`),

each delivering the stop to the debugger via the `/proc/<pid>/wait` channel and
parking the thread (§4.2) instead of terminating the Proc. The kernel-side
(`exception_sync_curr_el`) debug-EC arms stay `extinction` (kernel self-debug is
§9, deferred).

### 5.5 Single-step + the step-over-breakpoint dance

- **Step**: `MDSCR_EL1.SS=1` + `SPSR_EL1.SS=1` (bit 21) in the resumed EL0 frame
  -> Active-not-pending -> one instruction -> EC `0x32` to EL1 -> re-park at the
  tail. Re-set `SPSR.SS` to keep stepping; clear `MDSCR.SS` to stop. Returning
  with `SPSR.SS=0` while `MDSCR.SS=1` is Active-pending = stuck-PC re-trap — the
  classic bug to avoid.
- **Step-over-breakpoint** (mandatory): a thread resuming at a breakpointed PC
  re-traps forever. On resume, if the PC matches an armed breakpoint, disable
  that bp's `E` bit, single-step one instruction, re-enable it (the Linux
  `try_step_suspended_breakpoints` + `suspended_step` machinery). Watchpoints are
  disabled across the step (the kernel can trigger them via an unprivileged
  access).
- Note the Cortex-A76 erratum 1463225 (step-into-SVC) for real hardware
  (Lazarus); QEMU/HVF is unaffected.

---

## 6. `specs/debug_stop.tla` (spec-first, model-first)

Written and TLC-green BEFORE the 8a-1 impl. Models the stop/continue/step state
machine and its composition with the death path. Target invariants:

- **NoLostStop** — a stop request set before a thread reaches the checkpoint is
  observed there (register-then-observe at the tail; the I-9 shape).
- **DeathWinsOverStop** — a thread with `group_exit_msg` set (or the LS-5c
  terminate latch) DIES rather than parking on the debugger rendez; a
  concurrently-flagged death during a stop-resume still terminates the thread
  (the tail's die-check precedes the stop-check, and resume re-runs it).
- **NoEL0AfterStopped** — a stopped thread executes no EL0 instruction until
  resumed (it parks before the `eret`), and executes exactly one on `step`.
- **ExactlyOnceResume** — a resume wakes each parked thread exactly once; a
  double `start` / a `start` racing a `detach` / a `start` racing the target's
  death does not double-wake, lose a wake, or resume a dead thread.
- **NoStrand** — the stop is bounded by the ctl-fd-owned slot: releasing the slot
  (explicit `detach` OR ctl-fd close OR debugger death via #68 close-at-exit)
  provably resumes the target (a liveness witness: `EventuallyResumed`).

Buggy cfgs (each a minimal counterexample on its named invariant): `park_before_die`
(stop-check ordered before the die-check -> DeathWinsOverStop), `lost_stop`
(observe-before-register at the tail -> NoLostStop), `double_wake` (resume without
the single-waiter discipline -> ExactlyOnceResume), `strand_on_debugger_death`
(the slot not released at ctl-fd close -> NoStrand).

The model pins the impl sites (the tail stop-leg, the delivery cascade, the
ctl-fd-close resume); `specs/SPEC-TO-CODE.md` records the action<->site mapping.

---

## 7. Capability + ownership realizations

### 7.1 `CAP_DEBUG` (three header edits, zero devcap.c)

1. `caps.h`: `#define CAP_DEBUG (1ull << 10)` (the next free bit).
2. `caps.h`: add `CAP_DEBUG` to `CAP_ELEVATION_ONLY` (rfork strips it; the
   `CAP_ALL & CAP_ELEVATION_ONLY == 0` assert stays green without touching the
   `CAP_ALL` pin, since elevation-only bits are excluded from `CAP_ALL` by
   design).
3. `devcap.h`: add `CAP_DEBUG` to `CAP_GRANTABLE_CLEARANCE` (the `⊆
   CAP_ELEVATION_ONLY` static_assert stays satisfied).

The existing 32-byte clearance grant path
(`cap_register_clearance_grant_for_writer` -> `proc_become_legate`) conveys any
subset of `CAP_GRANTABLE_CLEARANCE` with no new devcap.c — the register-gate and
redeem-subset checks are mask-driven. So a dev session's debugger acquires
`CAP_DEBUG` via a corvus-mediated, scope- and time-bounded legate, exactly as
`CAP_KILL`/`CAP_DAC_OVERRIDE`/`CAP_CHOWN` do today.

### 7.2 Handle-lifetime-tied stop ownership

The one-debugger-per-target slot (`Einuse` on a second `attach`) is owned by the
open debug ctl fd. The resume-on-release is structural, not a reaper:

- Explicit `detach`, or ctl-fd `close`, releases the slot -> resume + clear all
  installed bp/wp for the target.
- **Debugger death** closes its handle table — and since #68/#926, a
  multi-thread or killed debugger closes its handles at EXIT (last-thread-out),
  not only at reap. So a debugger that crashes mid-stop releases the slot at its
  own exit, and the target resumes. Strand-freedom composes with the death-path
  work I just landed — no new backstop needed.

This is the Zircon suspend-token property (close = resume) realized on the Plan 9
`p->pdbg`/ctl-fd shape, on our handle table. (A dedicated `KObj_Debug` token with
refcounted multi-stop is the v1.x generalization; v1.0's one-per-target needs
only the ctl-fd-owned slot.)

---

## 8. Composition with the death path (#811 / #68)

The stop machinery sits ON the most bug-prone lineage in the tree, so the
composition is load-bearing and prosecuted hard:

- **Death always wins.** The tail orders the stop-check AFTER
  `el0_return_die_check`, and a resume re-runs the die-check after unpark, so a
  target that is group-terminated (or LS-5c interrupt-terminated) while stopped
  DIES rather than resuming to EL0 (`DeathWinsOverStop`).
- **A stop never unwinds a syscall.** Unlike death (`SLEEP_INTR`), a stop is
  observed only at the EL0-return tail, never inside `sleep()`/`tsleep()` — so a
  syscall in progress completes (or the thread stops at its next checkpoint),
  preserving in-kernel invariants.
- **The stop-park obeys the on_cpu protocol.** A stopped thread is inspected only
  after `on_cpu==false` (the #788 spin); the debugger never reads a frame/ctx
  being written by an in-flight `cpu_switch_context`.
- **Never park in the #68 close windows.** The `exits()` / `thread_exit_self`
  unlock->close->relock->recount-assert windows require the thread RUNNING and a
  stable live-peer count; the stop-leg lives at the EL0-return tail, outside any
  `g_proc_table_lock` hold, so it cannot trip
  `extinction("... peer appeared during handle close")`.
- **`kproc` is never debuggable** (special-cased before the gate, as kill is);
  the session/console-owner and `PROC_FLAG_NOTRACE` seams (e.g. login forbids
  debug-attach for its session Proc) are honored.

---

## 9. What 8a is NOT (scope fences)

- **NOT the debugger product.** `dlv`'s `proc_thylacine` backend (8c), `gopls`
  (8d), the Nora plugin architecture (8e), the Kaua debug UI + run-pane (8f), and
  the superpowers (8g) are later sub-stages; 8a defines only the kernel
  primitives they drive.
- **NOT kernel self-debug.** Debugging EL1 (breakpoints in kernel code, stepping
  the kernel) needs `KDE=1` + `PSTATE.D=0` windows and must account for the
  #713/alternatives all-DAIF regions — a genuinely harder surface, deferred to a
  post-8 stretch (it is what makes superpower #4, kernel-aware post-mortem,
  fully live; the cross-boundary *stack walk* — 8b — does NOT require it).
- **NOT time-travel.** Record-replay reverse-step is Stage 9 (GO-IDE-DESIGN §9),
  deferred.
- **NOT the DWARF plumbing.** Shipping the host `.elf` kernel DWARF + wiring
  userspace DWARF to the backend is 8b; 8a builds the on-target-symtab stack walk
  and the mem/reg primitives DWARF resolution reads.

---

## 10. Sub-chunk plan + cadence

- **8a-1a** — `specs/debug_stop.tla` (clean + buggy cfgs, TLC-green, model-first)
  + the SPEC-TO-CODE reservation.
- **8a-1b** — the debug-fs files + the I-39 gate + `CAP_DEBUG` (the 3 header
  edits) + the ctl verbs (`attach`/`stop`/`start`/`waitstop`/`detach`) + the
  handle-lifetime-tied slot + the stop-at-checkpoint SM (the tail leg + delivery)
  + cross-Proc mem/regs + the exposed unified stack walk. Validated against the
  spec.
- **8a-1c** — the focused 8a-1 audit (I-39 the centerpiece; the death-path
  composition; the cross-Proc walk lifetime; the SMP stop delivery) + the SMP
  gate + the kernel tests + docs (`docs/reference/NN-debug-fs.md`).
- **8a-2a** — the empirical HVF-debug verify on the dev host (a boot probe that
  arms a `DBGBCR` and confirms the EC delivers to guest EL1). If HVF fails,
  8a-2 tests run TCG-only (design unchanged).
- **8a-2b** — `hwfeat.c` DFR0 enumeration + OS-Lock unlock + the DBGBCR/BVR/WCR/WVR
  programming + `MDSCR.MDE` + the EC routing + single-step (SS machine) + the
  step-over-breakpoint dance + per-thread register install (the scheduler hook).
- **8a-2c** — the focused 8a-2 audit (the per-thread install SMP race; the EC
  routing; W^X/I-36 preservation; the step-over dance correctness) + the SMP gate
  + the HVF+TCG boot proofs + docs.

Each audit-bearing sub-chunk gets a focused Fable-5-max `holotype-reviewer` round
+ a concurrent self-audit + the SMP gate, per the standard cadence. The audit
row for this surface is CLAUDE.md + ARCH §25.4 (§11 below).

---

## 11. Prior-art grounding

- **Plan 9 devproc** (the lineage-correct shape): debugging-as-files —
  `/proc/<pid>/{ctl,mem,regs,fpregs,kregs,wait,text}`, run-control verbs
  (`stop`/`start`/`startsyscall`/`waitstop`/`hang`), stop-at-checkpoint (not
  preemptive), one-debugger-per-target (`p->pdbg` `Einuse`), and the load-bearing
  asymmetry we adopt: mem/reg WRITES require Stopped; reads are
  capability/ownership-gated. `acid` drives four orthogonal files (symbols from
  `text`, bytes from `mem`, control into `ctl`, stop-notify from `wait`) — no
  bespoke syscall.
- **Zircon** (the capability SOTA): the suspend-token / exception-channel
  property — "attached/stopped" is an explicit closeable handle, so a dead
  debugger provably cannot strand its target. We realize this as the
  ctl-fd-owned slot (close=resume), composing with #68 close-at-exit.
- **Why not ptrace**: it conflates run-control, signal delivery, and
  parent-child ownership into one pid-keyed, signal-mediated API with no explicit
  handle — hence the per-thread-not-per-process fan-out, the ATTACH/SIGSTOP race
  (`PTRACE_SEIZE`), run-control-on-signal-delivery, and tracer/tracee
  re-parenting. We design each as an explicit closeable capability over an
  explicit checkpoint.
- **Delve** (the 8c consumer, informs 8a): wants a native ptrace-equivalent
  backend (mem R/W + reg R/W incl. TLS for the Go `g` via `TPIDR_EL0` +
  attach/detach/continue/step + thread-enum + wait-with-stop-reason). Software
  breakpoints are free over mem R/W in general — but FORBIDDEN here (I-12/I-36),
  so the backend routes breakpoints to the 8a-2 HW path. A GDB-stub server is not
  cheaper (needs the lldb dialect + Delve patching). `dlv dap` gives Nora a
  native DAP frontend; DWARF is required (Go default-on).

---

## 12. References

- `docs/GO-IDE-DESIGN.md` (the ratified charter; §8a = this focused pass; §3.1
  the kernel debug-fs sketch; §6 the I-39 provisional text this doc finalizes).
- `docs/GO-PORT-PLAN.md` §5 Stage 8 (the arc; Stages 4-6 the toolchain landed).
- `docs/ARCHITECTURE.md` §28 I-39 (the invariant) + §25.4 (the audit-trigger
  row) + §9.4 (`/proc`/devproc) + I-12 (W^X) + I-36 (REVENANT Image cache) +
  I-26 (cross-Proc kill, the gate analog).
- `docs/reference/101-halls.md` (the HX-2 symtab + fp-chain walker reused for the
  unified stack).
- `specs/debug_stop.tla` (the stop/continue/step SM; landed at 8a-1a) +
  `specs/SPEC-TO-CODE.md`.
- `kernel/devproc.c` (the host Dev), `kernel/include/thylacine/caps.h` +
  `kernel/devcap.c` (`CAP_DEBUG`), `arch/arm64/exception.c` (EC routing),
  `arch/arm64/vectors.S` (the EL0-return tail), `arch/arm64/halls.c` (the walker),
  `arch/arm64/hwfeat.c` (DFR0), `kernel/sched.c`/`proc.c` (the stop SM +
  delivery + the death-path composition).

---

## 13. Naming (held for signoff at the debugger product, 8c)

Per the CLAUDE.md naming discipline and GO-IDE-DESIGN §10, the debugger *product*
name (lead candidate **Ambush** — the breakpoint as the apex predator's trap,
lying in wait to take its quarry at the chosen instant, even across the kernel
boundary; alt **Vigil**) is held for the user's signoff when the `dlv`-backed
debugger lands (8c). The 8a *kernel surface* keeps the lineage-correct Plan 9
verb names (`stop`/`start`/`step`/`waitstop`/`attach`/`detach`) and descriptive
file names (`mem`/`regs`/`wait`) — the names readers expect from devproc; no
thematic rename is forced onto the kernel primitives.
