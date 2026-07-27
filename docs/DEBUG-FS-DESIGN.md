# DEBUG-FS-DESIGN — the kernel debug surface (Go IDE Stage 8a-8b)

**Status: 8a AS-BUILT (8a-1 + 8a-2 landed + audited, 2026-07-15). §5b (Stage 8b)
DESIGN ratified 2026-07-16, user-voted — scripture, no code yet.** This is the
Stage 8a-8b focused design pass mandated by `docs/GO-IDE-DESIGN.md §8` ("each
kernel sub-stage opens with its own focused design pass + audit — it is a new
privilege surface"). It designs the kernel half of the cross-boundary debugger:
the `/proc/<pid>` debug filesystem, the stop/continue/single-step state machine,
cross-Proc memory + register access, the unified user->kernel stack walk (§4.6 +
the Stage-8b settled-thread inspect §5b), arm64 hardware breakpoints/watchpoints,
and the debug-authority invariant **I-39**. It is prose- and spec-validated (per the
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
wants a blocked thread to stop *now* — was deferred as a v1.x refinement here;
8c-1 ground-truthed it as the blocker for every multi-threaded Go target, so it
is now **designed as 8c-2 in §5c** — a nested park inside `sleep()` that best
satisfies this section's "preserve the in-progress syscall" principle.)

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

**As-built (8a-1b-gamma-3):** the walker is the ADDITIVE `halls_walk_kernel_frames`
(not a refactor of `halls_backtrace` — the dying-machine dump path stays
byte-for-byte unchanged), taking explicit `[lo,hi)` bounds + `fp+16<=hi`, fed by
`th->ctx.lr`/`th->ctx.fp`. **Stage 8b (§5b)** makes this walk available on a
*settled but not debug-stopped* thread (the blocked-in-syscall case the §4
headline needs) and defers source-line kernel DWARF to 8c.

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

## 5b. Stage 8b — the cross-boundary unified stack (settled-thread inspect)

Stage 8b makes §4.6's headline first-class and closes the one gap ground truth
surfaced. It delivers two things and defers a third (all three user-voted
2026-07-16):

1. **The stitch** — one `user -> kernel` call stack. The kernel already exposes
   both halves: the user frames via `regs` + `mem` (a stopped target), the kernel
   frames via `kstack` (the additive `halls_walk_kernel_frames` fp-chain,
   symbolized `func+offset` via the HX-2 symtab). The debugger concatenates them
   at the SVC boundary. This is consumer-side (dlv's `proc_thylacine` backend at
   8c does the full user fp-walk + DWARF); 8b proves the kernel *provides* both
   halves and they classify + stitch (user = TTBR0 VAs; kernel = named TTBR1).

2. **The settled-thread kernel-stack inspect** — the new kernel work, and the
   answer to the gap. The 8a debug-stop parks a target **only at the EL0-return
   tail** (a syscall *complete*, §4.2), so a debug-stopped thread's `kstack` shows
   the return-tail path, **not** the deep `blocked-in-channel-recv -> sched.c::sleep
   -> the exact rendez` stack the §4 headline promises. Worse, a thread genuinely
   *hung* in a syscall never reaches the tail, so a debug-stop never takes — you
   cannot freeze a hung thread to look at it. The fix is the **Linux
   `/proc/<pid>/stack` + Plan 9 model**: reading a *settled* (`on_cpu==false`)
   thread's kernel stack is a **read-only inspection**, distinct from and strictly
   weaker than a debug-stop — no execution control, no user-memory access. So
   `/proc/<pid>/kstack` reads the head thread's kernel fp-chain whenever it is
   settled (parked on **any** rendez — the pipe rendez, the torpor rendez, the
   debug rendez), **without requiring `debug_stop_req`**. "Why is this thread
   hung" becomes a `cat /proc/<pid>/kstack` — the single most useful debug
   diagnostic, available even without attaching.

3. **Kernel DWARF — DEFERRED to 8c.** 8b ships `func+offset` kernel symbolization
   (HX-2, already on-target: reloc-free, KASLR-independent). Source-line mapping +
   kernel-frame locals need the kernel ELF's DWARF (a few MB) *and* an on-device
   DWARF reader — neither exists yet, and dlv (8c) brings Go's `debug/dwarf`
   reader (the consumer) with it. Shipping MB of `.debug_*` + a bespoke reader with
   no consumer in 8b is premature; it lands at 8c where the reader + the consumer
   arrive together. (Chunk-completeness deferral, user-signed-off 2026-07-16.)

### 5b.1 The safety of reading a settled-but-not-stopped thread

Relaxing the `kstack` gate from `fully_stopped` to `settled` (`on_cpu==false`)
must not weaken memory-safety. It does not:

- **Lifetime** — the read holds `g_proc_table_lock` (via `proc_for_each`), which
  pins the target *reachable*; `wait_pid` UNLINKS the ZOMBIE from the table under
  `g_proc_table_lock` (`proc_unlink_child`) BEFORE the lock-free `thread_free`, so
  no reachable target's Thread can be freed under the read — ALIVE or
  ZOMBIE-unreaped alike (a reachable ZOMBIE's head is `THREAD_EXITING`, atomically
  under the lock, so `devproc_format_kstack` returns 0). The load-bearing pin is
  the unlink-before-free ordering, not "ALIVE" per se. (v1.0 reads the head thread;
  per-tid — which would walk a non-head thread — is v1.x and MUST preserve this
  ordering. Holotype F2.)
- **Bounded walk** — `halls_walk_kernel_frames` gates every deref to the thread's
  own usable kstack `[base+guard, base+size)` + `fp+16 <= hi`. A thread that
  *wakes and runs* mid-walk (a wake takes the rendez/rq locks, NOT
  `g_proc_table_lock`, so it is not excluded) can mutate its own kstack and
  `th->ctx` under the read — a **torn `u64`** fp/lr or a garbage frame link — but
  every deref stays inside the thread's own kstack and an out-of-range link stops
  the walk. So the walk is memory-safe **regardless** of the target's concurrent
  execution.
- **Consistency = best-effort** (the Linux `/proc/stack` contract). A genuinely
  *sleeping* thread (blocked on a rendez) has a stable kstack until it is woken,
  so the common case — inspect a hung/blocked thread — is coherent. A running or
  waking thread yields a possibly-inconsistent (but bounded, never unsafe) frame
  list. `on_cpu==true` is reported as `running` (no meaningful saved frame — the
  live registers are in hardware, not `th->ctx`), not walked.
- **No new race with the stop machinery** — the inspect reads `th->ctx` +
  kstack memory; it touches neither `debug_stop_req` nor the park/wake protocol
  nor the death path, so `specs/debug_stop.tla` is **unchanged** (the read is a
  gate condition, not a state-machine transition — like the 8a `mem`/`regs`
  reads). The gate change is prose-validated (this section + the focused audit),
  not a spec extension.

### 5b.2 The I-39 refinement

I-39's "register/memory access is only permitted on a stopped target" is refined
(not weakened) to name the diagnostic-inspect tier explicitly:

> A read-only inspection of a **settled** (`on_cpu==false`) thread's **kernel**
> stack (`/proc/<pid>/kstack`) is I-39-*authorized* (the same owner-or-`CAP_DEBUG`/
> `CAP_HOSTOWNER` gate) but does **not** require a debug-stop — the Linux
> `/proc/<pid>/stack` diagnostic tier. It is memory-safe (bounded to the thread's
> own kstack) and best-effort-consistent. **User memory, user registers, and all
> execution control (stop/step/write) remain stopped-only.** The inspect confers
> no authority beyond the gate and controls no execution — it is strictly weaker
> than a debug-stop.

**The raw-address gate (I-16; holotype F1).** A kstack line's raw slid `addr` +
unslid `link` reveal the KASLR slide (`koff = addr − link`), an I-16 secret
`/ctl/kernel-base` gates behind `CAP_HOSTOWNER` (#57a). So `devproc_format_kstack`
emits them ONLY to a `CAP_DEBUG`/`CAP_HOSTOWNER` caller (the debugger tier — it
reads `/ctl/kernel-base` anyway + needs raw addrs to correlate with the kernel
DWARF at 8c); the **owner axis** gets the KASLR-INDEPENDENT symbolic form
(`#N  name+0xsoff` — link-relative, no slide), which IS the "why is it hung"
diagnostic. Without this, an unprivileged owner reads its own `koff` off a settled
head thread — 8b widened the pre-existing 8a owner-`attach`-and-`stop`-another-
owned-Proc path (which reached the same raw kstack) to a no-attach self-read.

### 5b.3 v1.x seams

- **Per-tid inspect** (`/proc/<pid>/thread/<tid>/kstack`) — 8b reads the head
  thread; the specific blocked M of a multi-thread Go proc is v1.x.
- **The user frames of a *hung* thread** — the full `user + kernel` unified stack
  of a thread blocked mid-syscall needs the user `regs`/`mem` of a non-stopped
  thread, a larger (and genuinely racier) relaxation the vote deliberately did not
  take. 8b delivers the *kernel* half for a blocked thread (the "where is it
  stuck" answer) and the full stitch for a *stopped* thread; joining them is v1.x.
- **Source-line kernel DWARF** — 8c (above).

---

## 5c. Stage 8c-2 — explicit stop-of-a-sleeper (the multi-thread-stop refinement)

**Status: AS-BUILT (2026-07-17). Signed off; kernel impl landed; the E2E is live
+ boot-fatal (DELVE-PORT-DESIGN §18).** Owed pre-close: the SMP gate + a focused
Fable holotype + the ARCH/CLAUDE I-39 as-built-row updates. This is the v1.x
refinement §4.2 named ("an explicit-stop-of-a-sleeping-thread … is a v1.x
refinement"), pulled forward because 8c-1 ground-truthed it as the blocker for
**every real Go target**.

### 5c.1 The seam (ground-truthed at 8c-1, DELVE-PORT-DESIGN §17)

§4.4 assumed "a thread sleeping in a syscall … reaches the tail when its
syscall/handler returns." That holds for a thread whose syscall is *about to
return* — but **not** for one blocked in an *indefinite* wait. A multi-threaded
Go target always parks idle M threads in an indefinite `futex`/torpor wait; they
sit on the torpor rendez, never on `debug_rendez`, and never return to the tail
on their own. So `devproc_all_threads_parked` (every non-EXITING thread on its
*own* `debug_rendez`, `on_cpu==false`) is never satisfied → the target never
becomes fully-stopped → the debugger's blocking `stop` (the `procstopwait` shape)
hangs forever. `debug-child` (one Rust yield loop) stops fine; Ambush attaching
to any Go program hangs. This is the Linux `ptrace` group-stop (a `SIGSTOP`
interrupts the futex wait *in-kernel*) that Thylacine deferred.

### 5c.2 The mechanism — a nested park **inside** `sleep()` (recommended)

§4.2's governing principle is *"a stop must **preserve** the in-progress syscall
state."* §4.2 framed the choice as tail-park (preserve) vs sleep-unwind (destroy)
and picked tail-park — but it did not consider a **third** option that best
satisfies its own principle for a sleeper: **park the sleeper in place, on
`debug_rendez`, and re-block on resume.** The syscall is never unwound; it stays
on the stack and resumes exactly where it blocked.

`sleep()`/`tsleep()` (the two primitives every #811-death-interruptible site
calls) gain a **stop-detour**, checked at the same wake point as the #811
death-check, in the death-wins order:

1. On wake, **die-check first** (existing #811): if `group_exit_msg` is set →
   `SLEEP_INTR` → unwind → `el0_return_die_check` → the thread dies. *Death always
   wins over a stop*, exactly as at the tail.
2. Else if the per-Proc **stop flag** is set → **re-park on `debug_rendez`** via a
   register-then-observe under `wait_lock` (the I-9 shape): set
   `rendez_blocked_on = &debug_rendez`, re-check the stop flag, and sleep on
   `debug_rendez` if it is still set (else fall through — the debugger already
   resumed). The debugger's confirm-walk takes the *same* `wait_lock`, so it
   confirms only a sleeper that has genuinely re-parked — no lost stop.
3. On resume (`start` clears the stop flag + wakes `debug_rendez`), the sleeper
   re-registers on its **original** rendez (the existing `sleep()`
   register-then-observe), re-checks its original condition, and re-blocks or
   returns. A condition that became true during the stop (e.g. data arrived via an
   IRQ while every peer was frozen) is seen by the re-check — **no lost data, no
   spurious `EINTR`, no restart machinery.**

The delivery side: **`proc_debug_stop_deliver` gains a wake of the target's
sleepers** — it walks `p->threads` and wakes each parked one (each under its
`wait_lock`), the *exact* cascade `proc_group_terminate` already runs for the
#811 death-wake, but arming the stop-detour instead of the die. A running peer is
still IPI'd to the tail (§4.4, unchanged). So the whole change reuses the shipped
#811 wait/wake substrate; the only genuinely new code is the stop arm of the
`sleep()` detour.

**Lock discipline (why parking in `sleep()` is safe):** `sleep()` atomically
releases the caller's condition lock before sleeping, and by the existing
sleeper-holds-no-locks discipline (a sleeper holding an unrelated lock would
already deadlock its peers) the thread holds *no* locks at the park point — so
the nested `debug_rendez` park strands nothing, unlike a die-in-place. This is
precisely why §4.2 could not park a *dying* thread in `sleep()` (death must
unwind to release the syscall's frame) but a *stopped* thread can (it resumes and
continues).

### 5c.3 The rejected alternative — unwind-to-tail + syscall restart (Linux ERESTARTSYS)

The other option: on a stop-wake, propagate `SLEEP_INTR` up the syscall like a
death, park at the *tail* (reusing the audited tail handshake), and on resume
**restart** the syscall from its `SVC` (rewind `ELR_EL1`). Rejected: it needs the
full ERESTARTSYS machinery — every blocking syscall handler must distinguish
"unwound for stop" from "unwound for death" and re-execute rather than return
`EINTR`, and every such syscall must be idempotent-from-`SVC`. That is a change
across *every* #811 site + handler, versus 5c.2's change to the *two* `sleep`
primitives, and it can surface a spurious `EINTR` if any handler mis-implements
restart. 5c.2 preserves the syscall literally; ERESTARTSYS destroys-and-recreates
it. (It is recorded here so the signoff weighs both; the recommendation is 5c.2.)

### 5c.4 Invariants + the spec

No new §28 invariant number — **I-9 generalized** (the sleeper's nested park is a
register-then-observe leg; no stop-wake is lost between a sleeper re-parking and
the debugger confirming) and I-39 unchanged (authority/stopped-only gates are
untouched; a stopped sleeper is as inspectable as a tail-parked thread). **Safety
is untouched** — a sleeping thread is `off-cpu` and never confirmable until it
re-parks, so `NoEL0AfterStopped` stays vacuously safe; the seam is purely a
**liveness** bug (the halt never completes).

`specs/debug_stop.tla` is extended model-first (spec-first RE-ENABLED for this
surface): a new `"sleep"` PC, `EnterSleep` (a thread blocks), and
`StopWakesSleeper` (the stop/death drives a sleeper to its park), plus the
liveness property **`EventuallyStopSettles`** (once a stop is owned, the target
eventually settles — all parked/dead, or the stop cleared) and the buggy knob
**`BUGGY_STOP_SKIPS_SLEEPER`** (the v1.0 skip → the executable counterexample).
The model routes the woken sleeper through the register-then-observe handshake to
its park; that handshake is the load-bearing part and is identical whether the
impl places it inside `sleep()` (5c.2) or at the tail (5c.3), so the spec's
wake-completeness + death-wins + I-9 + liveness properties bind either mechanism.
Verified: the clean cfg is TLC-green (Safety + `EventuallyAllDead` +
`EventuallyResumed` + `EventuallyStopSettles`; 2952 distinct states, depth 29) and
`debug_stop_buggy_stop_skips_sleeper.cfg` violates `EventuallyStopSettles`.

### 5c.5 Audit obligations + impl plan (held for signoff)

An SMP wait/wake change on the most bug-prone lineage (#788/#806/#860/#809/#811/#68)
→ a focused Fable holotype + the SMP gate. Prosecute: the sleeper's re-park I-9
(no stop-wake lost between the wake and the `debug_rendez` register-then-observe);
**death-wins** in the detour (the die-check precedes the stop-check on wake,
exactly as at the tail — a stop racing a group-terminate never re-parks a dying
sleeper); no lost/double wake across `start`/`detach`/close/death (the
`ExactlyOnceResume` latch extends to the nested park); the **cond-changed-during-
stop** re-check (the resume re-registers on the original rendez + re-checks — no
lost wakeup, no stale return); the lock discipline (a sleeper holds no locks at
the park point — audit every `sleep()`/`tsleep()` caller for a held lock across
the wait); and the interaction with 8a-2 single-step / the fault-stop
(`proc_debug_fault_stop`) delivery. Impl surface (est.): `kernel/sched.c`
(`sleep`/`tsleep` stop-detour), `kernel/proc.c` (`proc_debug_stop_deliver`
sleeper-wake walk), `kernel/devproc.c` (unchanged — `devproc_all_threads_parked`
already accepts a `debug_rendez` park wherever it happens). The `ambush-probe`
stage B flips from `ATTACH_ENABLED=false` to a gated assertion (goroutines / bt /
print against the now-stoppable Go target), the runtime witness.

### 5c.6 As-built — the #88 trapframe regression + the #89 reader-role seam

The 5c.2 detour landed (`kernel/sched.c` `sleep`/`tsleep` stop-detour +
`kernel/proc.c` `proc_debug_stop_sleeper_park` + the `proc_debug_stop_deliver`
sleeper-wake walk). Two follow-ups surfaced at the close.

**#88 (fixed in 8c-2).** §5c.5's "devproc.c unchanged" assumption was WRONG in one
respect: `devproc_all_threads_parked` does accept a `debug_rendez` park wherever it
happens, but `devproc_build_regs` (`/proc/<pid>/regs`) reads the HEAD thread's
`debug_trapframe`, which **only** `el0_return_stop_check` (the EL0-return TAIL park)
sets — from its vector `ctx`. A **detour-parked** thread never reaches that tail
while stopped, so its `debug_trapframe` stayed NULL, and `bt`/`print` on a multi-M
Go target whose head thread was sleeping-in-a-syscall at stop-time (Go's sysmon in
`nanosleep`→`tsleep`, or the elected 9P reader) intermittently EPERM'd (NULL frame
→ build_regs returns 0 → `-1`). The signature that ground-truthed it: `goroutines`
(a `/proc/mem` read, needs no trapframe) ALWAYS succeeds; `bt`+`print` (both
`/proc/regs`) fail. **Fix:** capture the EL0-entry frame at the single EL0-sync
choke point — `arch/arm64/exception.c::exception_sync_lower_el_impl` sets
`current_thread()->debug_trapframe = ctx` at entry, so a syscall/fault-blocked
detour-parked thread reports the correct "ptrace at the syscall boundary" EL0
register state (the outermost EL0→EL1 frame, valid on the kstack for the whole
syscall). Read-gated on fully-stopped (a parked thread always has a fresh frame —
tail-park overrides with its return ctx; detour-park is mid-syscall), so the
between-syscalls stale value is never observed and a non-debugged Proc never reads
its own. No new field. Verified 16/16 smp8 boots.

Corollary seam (pre-existing, unchanged by #88): a `step` on a detour-parked
(syscall-blocked) HEAD thread now reads a valid PC but arms `SPSR.SS` in a frame the
detour-resume does not `eret` from, so the single-step is ineffective — the head is
in a syscall, not at an EL0 instruction. This was already broken (NULL trapframe)
before #88; the per-thread `/proc/<pid>/thread/<tid>/` step is its v1.x home.

**#89 (fixed in 8c-3, as-built).** The elected 9P-reader role (the #841
`reader_active`) was HELD across the detour-park: a debugged Proc whose thread
holds the **system** Stratum client's reader role at stop-time froze system-FS
(`/bin`, `/lib`) for every survivor Proc sharing that client, until the debugger
continued/detached. Per-user home clients (A-5b `--single-session`) are isolated,
so the surface was the system client only; the freeze was bounded to the
debug-stop duration (developer-controlled). The fix mirrors the death path's
role-release, composed of three mechanisms:

The fix is **FRAME-ATOMIC** (the 8c-3 holotype F1 correction — see below): the
elected reader is interruptible-by-a-stop ONLY at a frame boundary and blocks a
mid-frame stop through. It is composed of a fourth flag plus three role-release
mechanisms:

0. **`Thread.stop_no_park`** (a 2nd per-Thread bool in the same padding). The
   `reader_recv_frame` wrapper holds it for the WHOLE recv tenure. A stop hitting
   the recv MID-FRAME (bytes already consumed, `stop_unwinds` false) must NOT
   unwind (would discard the partial frame → desync) NOR park in place (holds
   `reader_active` + pins the partial frame): the `sleep()`/`tsleep()` detour, when
   `stop_no_park` is set and `stop_unwinds` is clear, FALLS THROUGH to the normal
   register+sched so the reader finishes the frame (bounded by the trusted server's
   chunked delivery), then unwinds at the next boundary. Both flags are cleared on
   the wrapper's exit so a following role-free park PARKS.
1. **`Thread.stop_unwinds`** (a per-Thread bool, in the `debug_ss_armed` padding
   — no struct-size change). `reader_recv_frame` sets it `= (got == 0)` PER-CHUNK,
   so a stop unwinds ONLY at a clean frame boundary (no bytes consumed). The
   `sleep()`/`tsleep()` detour, when it is set, RETURNS `SLEEP_INTR`/`TSLEEP_INTR`
   — reusing the death-interrupt propagation the transport recv already tolerates
   (it leaves the transport reusable, no `ERROR` latch) — INSTEAD of parking in
   place. So the recv unwinds to `client_wait`, which detects the stop
   (`debug_stop_req` set, not dying), releases +
   `client_handoff_reader_locked`s the role, parks role-free
   (`proc_debug_stop_sleeper_park`, with `stop_unwinds` cleared so it parks), and
   re-elects on resume (the re-loop's `rpc->done` may already be true — a survivor
   demuxed the reply while stopped). Every OTHER sleep (both flags clear —
   Go's sysmon `nanosleep`, a futex wait) parks in place (the 8c-2 default,
   preserving the syscall).
2. **A top-of-loop stop guard** in `client_wait`: a debug-stopped thread reaching
   `client_wait` fresh (a mid-syscall stop) parks PROMPTLY (never elects); it
   re-hands-off first if it was handed `be_reader` (the handed-then-stopped race).
3. **The handoff skips debug-stopped owners** (`p9_rpc.owner` = the submitting
   Proc): the released role must land on a SURVIVOR, not bounce to another stopped
   sibling — a stopped sibling's thread has parked, so its `rpc->rendez` has no
   waiter and the `be_reader` wakeup would be a no-op (a lost role). If no survivor
   op is pending, the handoff finds nothing → the role is dropped (`reader_active`
   is already false) → a future survivor op self-elects.

4. **F2 — the role-release covers ALL FOUR `reader_active` sites.** Centralizing
   the flags in `reader_recv_frame` gives the block-through to every caller, and
   each handles its stop-return without killing the shared session or spinning:
   the `client_pump_or_park_locked` self-pump skips `client_mark_dead_locked` on a
   `client_stop_pending` unwind; `client_send_flow` (spilling `out_buf` FIRST, the
   #375 discipline, since `client_debug_stop_park` drops `c->lock`) +
   `client_drain_until_free_tag` PARK a stopped sender at their loop top — WITHOUT
   this a stop-unwound self-pump drains nothing, so `c2s` never frees, every send
   EAGAINs, the Proc never fully-stops, and the debugger's stop HANGS; and
   `p9_client_reader_pump_once` skips mark_dead on a stop. Only
   `p9_client_reader_pump_once_deadline` has no guard, justified kproc-only (its
   sole callers — `dev9p_poll.c` + `loom.c` SQPOLL — have `t->proc == NULL`, so
   the detour is immune).

**Why frame-atomic (the 8c-3 holotype F1 [P1]).** The FIRST fix (set `stop_unwinds`
for the whole recv, a plain unwind) was WRONG: delivery is CHUNKED, so a stop can
unwind mid-frame and desync. `srvconn_client_recv` tsleeps when the ring is EMPTY
(`count == 0`), but under pipelining depth ≥ 2 + ring pressure a frame is SPLIT
across the ring in time (`srvconn` `chan_ring` short-read/write + a wrendez park
when the frame doesn't fit), so the reader consumes a partial frame and its next
recv (for the tail) tsleeps MID-FRAME (`got > 0`). A stop-unwind there discards the
consumed bytes → a survivor's fresh `reader_recv_frame` reads the frame TAIL as a
header → shared SYSTEM-Stratum session death / task-#50 silent misframe. The
frame-atomic design (mechanisms 0 + 1) closes it for the STOP path. The IDENTICAL
death-path mid-frame unwind — the `#811` die-check below the detour is UNCHANGED,
so a death still unwinds mid-frame — is a PRE-EXISTING #841/#811 latent (grep-
confirmed: the #841/#845 audits never analyzed it) that #89 does not introduce but
would WIDEN from Proc-death (rare, terminal) to a debug stop (normal, repeatable);
it is **owned + tracked as task #90** (fixing it narrows #811 universal
death-interruptible sleep for this one recv → a §28 refinement → user-deferred to
its own spec-first sub-chunk).

`DeathWinsOverStop` holds at every branch (the dying check precedes the stop check
in the recv loop, the top-of-loop, and the role-free park; a death still unwinds a
mid-frame block-through via the die-check below the detour). NO new §28 invariant
and NO spec change (the reader-role is below the `9p_client.tla` +
`debug_stop.tla` models, both re-verified GREEN; validated by the focused 8c-3
Fable holotype + the `9p_client.handoff_skips_debug_stopped_owner` regression +
the SMP gate; the block-through decision lives in the sched detour, kproc-immune,
so it has no deterministic kproc regression — the SMP gate + reasoning are its
durable coverage).

### 5c.7 As-built — the #95 focus-thread (multi-M breakpoint inspect)

8c-1 did attach + inspect only; 8c-2 (5c.2) made a multi-M target
*fully-stoppable*. But the FIRST `continue` past a `break` on a real Go target
exposed a deeper gap the whole 8a/8b/8c-2 design carried silently: the debug-fs
(`devproc_build_regs`/`apply_regs` + the step verb + the 8b `kstack`) read only
the **head** thread (`target->threads`, TID==PID). A hardware breakpoint on a
multi-M Go target fires on whichever M runs the migrated goroutine — **not** the
head — so `/proc/regs` reported the head M's PC (not the bp), the debugger
(Ambush/Delve) could not attribute the stop (`FindBreakpoint(pc)` missed →
`CurrentBreakpoint` nil → pkg/proc's `Continue` fell to its `just repeat`
default), and `continue` auto-resumed forever (ground truth: 8199 `start`+fire
pairs, 0 step-over, kernel + Go-side instrumented, on `ambush exec
/ambush-child`). The `/debug-child` (8a-2b) is single-threaded native, so it never
exercised this — its bp fires on its only thread, which *is* the head.

**The fix (Option A, user-voted): the debug-fs FOCUSES the stop-triggering
thread.** `struct Proc.debug_focus_thread` (proc.h, @328) is the M whose frame the
debug-fs reports; `NULL` = head (the manual-stop / attach default). Every `bp` /
`step` / `wp` EC fire routes through **one** path — `proc_debug_fault_stop`, from
`hwdebug_{breakpoint,singlestep,watchpoint}_from_el0` — so setting it there to
`current_thread()` (under `g_proc_table_lock`) focuses the firing M uniformly, and
a `step` re-focuses on its own EC 0x32 (which re-enters `fault_stop`).
`proc_debug_resume` clears it. `devproc_focus_thread(target)` validates the
pointer is still a thread of `target` (else head) under the lock; the four read
sites use it, so regs + kregs (`tpidr_el0` → Delve's goroutine recovery) + kstack
+ step report the firing M **coherently**. A single-threaded target has
`current_thread()` == head, so 8a-2b + attach + the 8b settled-thread kstack are
byte-behaviour-unchanged.

**Soundness.** No UAF — the load-bearing reason is the **`g_proc_table_lock`
pin**, not the fully-stopped gate (#95-audit F1): reap does `proc_unlink_child`
under GPTL *before* the lock-free `thread_free` loop, and `proc_for_each` walks
the kproc-rooted children tree, so a still-reachable target has not been unlinked
→ none of its threads is freed; `devproc_focus_thread`'s in-list validation then
rejects any stale/foreign pointer (that validation is the safety net — the 8b
settled kstack drops the fully-stopped gate, so it is not the net). The
fully-stopped gate is an *additional* property of the mem/regs/kregs/step readers.
A set focus is always `current_thread()` of the debugged Proc — guarded at the
store (`ct->proc == p`, #95-audit F2); any stale/mismatched pointer falls back to
head. The set (`fault_stop`, under the lock) / clear (`resume`, `__atomic`) / read
(the 4 walk_cbs, under the lock) are consistent. **No new §28 invariant** — I-39 holds
(it only refines *which* stopped thread's frame crosses, gated identically);
`debug_stop.tla` + `debug_step.tla` are frame-reporting-agnostic and unchanged
(re-verified GREEN). Validated by the focused 8c-2-#95 Fable holotype + the
`/ambush-probe` stage-C E2E (the durable multi-M regression — break + continue +
bt/print at a HW bp on a multi-M Go target; it loops 8199× without the fix) +
1138/1138 + boot OK + the SMP gate. This unblocks 8c-4. As-built:
`docs/reference/134-debug-fs.md` (the "8c-2 #95" section); closed list
`memory/audit_8c2_95_closed_list.md`.

---

## 5d. EXITKILL — die-with-launcher (the launched-target death-release)

Designed 2026-07-23 (user-voted "Kernel EXITKILL on gfx-4, full treatment"). The
I-39 NoStrand liveness (§3, §5c) resumes a target when its debugger detaches or
dies — *provably never strands a stopped quarry*. That is correct for an
**attached** target: it existed before the debugger, so on the debugger's death it
should keep running. It is **wrong for a debugger-LAUNCHED target**: a program the
debugger `exec`'d has no independent existence — it is the debugger's child — and
on the debugger's death the Plan 9 resume orphans it to init, where it runs
forever. This is the `PTRACE_O_EXITKILL` distinction: a tracer-launched tracee dies
with its tracer; a tracer-attached one is released.

**The concrete leak.** `/ambush-probe` stages C (`ambush exec /ambush-child`) and D
(`dap-selftest`) launch a Go debuggee. Both stages issue the graceful `kill` on
exit — stage C via the terminal's `handleExit` → `Detach(true)` → `ctlWrite("kill")`,
stage D via `DisconnectRequestWithKillOption(true)` (both **stock Delve**, traced
end-to-end). But when the debugger dies **without** completing that graceful kill —
a blocked `cmd.Wait()` on a slow kill, the probe's own 18s-timeout `amb.kill()`, a
crash — the ctl fd closes with the target still parked, and
`devproc_debug_release_cb` `proc_debug_resume`s it: the child is resumed and
orphaned (the HVF-idle ~6.6% debuggee leak). The userspace debugger *cannot* close
this: the resume fires on the fd-close, i.e. on **any** debugger-death mode. Only
the kernel — which owns the ctl-fd-close release — can guarantee die-with-launcher.

**The mechanism.**

1. **The mark.** A per-Proc `Proc.debug_exitkill` (a bool in a pad slot near
   `debug_stop_req`; no struct growth) records "this target was launched by its
   debugger — kill it if the debugger dies." Set once, cleared with the slot.
2. **Setting it.** A new ctl verb, `exitkill`, sets `debug_exitkill = true`,
   owner-gated exactly like `stop`/`start`/`step` (`target->debug_owner == c`, under
   `g_proc_table_lock`). The debugger sends it right after attaching a child it
   spawned (`native.Launch`: `attach` → `stop` → `exitkill`). An attached target is
   never marked. Best-effort on the ambush side: a failed mark loses only the
   death-safety-net (the graceful `kill` still fires) — it never fails the launch.
3. **Honoring it.** `devproc_debug_release_cb` (the ctl-fd-close release, reached on
   debugger death via `devproc_close` + the #68 last-thread-out handle close)
   branches: an `exitkill`-marked **ALIVE** target is `proc_group_terminate`d
   ("debugger exited") instead of `proc_debug_resume`d. The `#811` death cascade
   wakes the debug-parked threads; each hits the EL0-return die-check (death wins
   over the stop) and terminates; the last out becomes ZOMBIE and is reaped by init.
   `proc_group_terminate` is safe under `g_proc_table_lock` (the `devproc_kill_walk_cb`
   precedent: it takes only torpor / rendez / cs locks, all below it), and the ALIVE
   guard mirrors the kill verb — a dying/ZOMBIE target falls to the (magic-guarded,
   dying-safe) `proc_debug_resume`.
4. **The explicit-detach carve-out.** Only the *implicit* death-release honors the
   mark. The explicit `detach` verb clears `debug_exitkill` and resumes — a debugger
   that types `detach` is deliberately choosing to leave the target running (it
   would send `kill` to terminate instead). `attach` also clears it (a fresh slot
   starts unmarked). Both the `exitkill` set and the release run under
   `g_proc_table_lock`, so there is no set-vs-release race.

**Invariants.** NO new §28 invariant — this refines I-39. `NoStrand`/`EventuallyResumed`
is unchanged for an attached target (it still resumes). `StopImpliesOwned` holds: the
exitkill branch explicitly clears `debug_stop_req` + `debug_focus_thread` **after** the
terminate (self-audit SA-1 — `proc_group_terminate` does NOT itself clear the stop
flag; ordered after so `gflag` is the wake the parked threads act on, no resume-window;
matches the spec's exitkill `ReleaseSlot` `sflag'=FALSE`). `DeathWinsOverStop` is
exactly the mechanism the terminate relies on. The new obligation is
`EventuallyLaunchedDies` (§6): a launched (`exitkill`) target whose debugger dies
without an explicit detach eventually dies. The buggy cfg `exitkill_ignored` (the
pre-fix always-resume) is its executable counterexample.

**The precise trigger (audit F1).** The release-cb runs on the *target* and cannot
observe the debugger's liveness, so it terminates a marked ALIVE target on **any
ctl-fd close without a prior `detach` verb** — debugger *death* (the load-bearing #68
close-at-exit leak scenario) OR a *live* debugger's bare `SYS_CLOSE` of the fd. Both
have the identical outcome (terminate a marked launched child), the live-bare-close is
unexercised (ambush always sends `kill`/`detach` first) and sound (within the
debugger's slot authority — it can already `kill` the target), and `debug_stop.tla`
abstracts it as the `~dbg_live` case (the same-outcome documented modeling boundary at
`ReleaseSlot`). An explicit `detach` clears the mark, so a launched target survives a
release **only** via an explicit detach.

**Why not the ambush side.** The ambush debugger already kills on exit (both stages,
stock Delve). Adding a second kill there is redundant, and no userspace change can
cover the abrupt-death modes (the resume is the kernel's ctl-fd-close reaction).
The correct fix is the kernel refinement; the ambush half is only the one-line
`exitkill` mark so the kernel knows *which* targets are launched.

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
- **EventuallyLaunchedDies** (5d, the EXITKILL refinement) — a debugger-LAUNCHED
  (`exitkill`-marked) target whose debugger dies without an explicit detach
  eventually DIES (die-with-launcher): the release-cb `proc_group_terminate`s it
  rather than resuming-and-orphaning it. The buggy cfg `exitkill_ignored` (the
  pre-fix always-resume) is the counterexample. Refines NoStrand — for a launched
  target "not stranded" means dead, not resumed (and `EventuallyResumed` still holds:
  dead is not stopped).
- **StopImpliesOwned** (8a-2 SA-1) — the per-Proc stop flag is set only while a
  debugger owns the slot; the EC-path fire (`proc_debug_fault_stop`) delivers
  under `g_proc_table_lock` gated on `debug_owner != NULL`.
- **EventuallyStopSettles** (8c-2, 5c above) — once a stop is owned, the target
  eventually settles (all parked/dead, or the stop cleared); the stop-of-a-sleeper
  guarantees the halt completes even when a thread is blocked in an indefinite
  sleep.

Buggy cfgs (each a minimal counterexample on its named invariant): `park_before_die`
(stop-check ordered before the die-check -> DeathWinsOverStop), `lost_stop`
(observe-before-register at the tail -> NoLostStop), `double_wake` (resume without
the single-waiter discipline -> ExactlyOnceResume), `strand_on_debugger_death`
(the slot not released at ctl-fd close -> NoStrand), `fault_stop_ungated` (the EC
fire sets the flag without the owner gate -> StopImpliesOwned), and `stop_skips_
sleeper` (the v1.0 Plan 9 non-preemptive stop leaves a sleeper asleep ->
EventuallyStopSettles, the 8c-2 multi-thread-stop seam).

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
- **8b-1a** — scripture (this pass): the DEBUG-FS §5b design (the settled-thread
  kstack inspect + the I-39 refinement §5b.2 + the DWARF-defer-to-8c) + the ARCH
  §28 I-39 text. No code.
- **8b-1b** — relax `/proc/<pid>/kstack` from `fully_stopped` to the settled-thread
  gate (`on_cpu==false` + non-EXITING; a `running` marker on `on_cpu==true`), I-39
  authorization + kproc-refuse unchanged; the kernel test (settled non-stopped ->
  walks; running -> marker); `debug_stop.tla` re-verified UNCHANGED (the read is a
  gate condition, not an SM transition). The §25.4 prosecution row + the CLAUDE.md
  mirror join here (the reserved-surface pattern).
- **8b-1c** — the in-guest proof (`usr/stack-probe`, boot-fatal): (B) a child
  blocked in `torpor_wait`-forever, its `/proc/<pid>/kstack` read owner-authorized
  WITHOUT a debug-stop -> a named blocked kernel frame; (A) the cross-boundary
  stitch on a debug-stopped child (user `regs.pc` [TTBR0] ++ `kstack` [named
  TTBR1] -> both halves present + classified). Kernel-byte-unchanged (consumer).
- **8b-1d** — the focused 8b audit (the settled-thread read's memory-safety +
  the `g_proc_table_lock` lifetime pin + the best-effort-consistency contract +
  no-new-stop-race) + the SMP gate + `docs/reference/134` §8b.
- **8c+** — `dlv`'s `proc_thylacine` backend (which ships kernel DWARF — the
  consumer + the reader arrive together), `gopls`, the Nora plugin, the Kaua UI,
  the superpowers, the whole-arc audit.

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
