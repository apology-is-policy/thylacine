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
4 buggy cfgs, TLC-green; model-first) + `specs/debug_step.tla` (the single-step
machine; the sibling, clean + 2 buggy cfgs). The 8a-2 hardware-debug tier (real
breakpoints/watchpoints/single-step via the arm64 debug registers) is landed
across **8a-2a** (the empirical delivery verify) + **8a-2b-1** (breakpoints) +
**8a-2b-2** (single-step) + **8a-2b-3** (watchpoints); the consolidated holotype +
the ARCH §25.4 row are **8a-2c** (§§ below).

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

**8c-2 (#88) — the detour-park trapframe.** The 5c stop-of-a-sleeper adds a
SECOND park path, `proc_debug_stop_sleeper_park` — a nested `sleep(&debug_rendez)`
reached when a `sleep()`/`tsleep()` DETOURS because a debug stop is pending on a
syscall-blocked thread. That park does NOT go through `el0_return_stop_check`, so
it never records the trapframe from a vector `ctx`. Without the fix, a
detour-parked HEAD thread (Go's sysmon in `nanosleep`→`tsleep`, or the elected 9P
reader) had `debug_trapframe == NULL` → `build_regs` returned 0 →
`/proc/<pid>/regs` EPERM'd INTERMITTENTLY (only `bt`/`print`, which read regs;
`goroutines` via `/proc/mem` needs no frame and always worked — the ground-truth
signature). Fix: record the EL0-entry frame at the single EL0-sync choke point —
`arch/arm64/exception.c::exception_sync_lower_el_impl` sets
`current_thread()->debug_trapframe = ctx` at entry, so a syscall/fault-blocked
detour-parked thread reports the correct "ptrace at the syscall boundary" EL0
register state (the outermost EL0→EL1 frame, valid on the kstack for the whole
syscall — not overwritten by the thread's own kernel activity, which runs BELOW
it). Read-safety is preserved: `debug_trapframe` is READ only when fully-stopped,
and a fully-stopped thread is always parked (tail-park overrides the entry value
with its return `ctx`; detour-park is mid-syscall so the entry frame is the
current outermost EL0 frame), so the between-syscalls stale value (set at a
syscall entry, not cleared at the syscall exit) is never observed and a
non-debugged Proc never reads its own. No new field. `debug_stop.tla` is
unaffected (an impl-level frame-pointer store, below the model). Verified 16/16
smp8. Corollary seam: a `step` on a detour-parked (syscall-blocked) head now reads
a valid PC but arms `SPSR.SS` in a frame the detour-resume does not `eret` from,
so the single-step is ineffective — pre-existing (NULL trapframe before #88),
unchanged; the per-thread `/proc/<pid>/thread/<tid>/` step is its v1.x home.

**8c-3 (#89) — releasing the elected-9P-reader role across the park.** The 5c
detour parks a syscall-blocked sleeper IN PLACE. When the sleeper is the #841
**elected 9P reader** (blocked in `reader_recv_frame`'s transport recv on the
SYSTEM Stratum client), parking in place holds `reader_active`, freezing every
SURVIVOR Proc sharing the client (`/bin`, `/lib`) for the debug-stop's duration.
(Per-user home clients — A-5b `--single-session` — are isolated, so the surface
is the system client only.) The fix is **FRAME-ATOMIC** (the holotype F1
correction — a mid-frame stop must block through, never unwind) and composed of a
fourth flag plus the three role-release mechanisms:

- **`Thread.stop_no_park`** (`kernel/include/thylacine/thread.h`, the 2nd bool in
  the same padding). The `reader_recv_frame` wrapper holds it for the WHOLE recv
  tenure; `do_reader_recv_frame` sets `stop_unwinds = (got == 0)` before each recv.
  A stop hitting the recv MID-FRAME (`got > 0`, `stop_unwinds` false) must NOT
  unwind (discards the partial frame → the survivor reads the tail as a header →
  desync) NOR park in place (holds `reader_active`): the `sleep()`/`tsleep()`
  detour, when `stop_no_park` is set + `stop_unwinds` clear, FALLS THROUGH to the
  normal register+sched so the reader finishes the frame (bounded by the trusted
  server's chunked delivery), then unwinds at the next boundary. Both flags are
  cleared on the wrapper's exit so a following role-free park PARKS.
- **`Thread.stop_unwinds`** (`kernel/include/thylacine/thread.h`, in the
  `debug_ss_armed` padding — no struct-size change). `reader_recv_frame` sets it
  `= (got == 0)` PER-CHUNK — a stop unwinds ONLY at a clean frame boundary. The
  `sleep()`/`tsleep()` detour (`kernel/sched.c`), when it is
  set, RETURNS `SLEEP_INTR`/`TSLEEP_INTR` — reusing the death-interrupt
  propagation the transport recv already tolerates (leaves the transport reusable,
  no `ERROR` latch) — instead of parking in place. The recv unwinds to
  `client_wait`, which detects the stop (`client_stop_pending`: `debug_stop_req`
  set, not dying), clears `reader_active`, `client_handoff_reader_locked`s the
  role, then parks role-free (`client_debug_stop_park` →
  `proc_debug_stop_sleeper_park`, with `stop_unwinds` cleared so it PARKS), and
  re-elects on resume (the re-loop's `rpc->done` may already be true — a survivor
  demuxed the reply while stopped). Every other sleep (`stop_unwinds` clear) parks
  in place (the 8c-2 default).
- **The top-of-loop stop guard** in `client_wait`: a debug-stopped thread reaching
  `client_wait` fresh (a mid-syscall stop) parks promptly (never elects), after
  re-handing-off if it holds `be_reader` (the handed-then-stopped race).
- **The handoff skips debug-stopped owners** (`p9_rpc.owner` = the submitting
  Proc; `client_handoff_reader_locked` skips `owner->debug_stop_req != 0`): the
  released role must land on a runnable SURVIVOR, not bounce to a stopped sibling
  (whose thread has parked, so its `rpc->rendez` has no waiter and the `be_reader`
  wakeup is a no-op — a lost role). No survivor pending → the role is dropped
  (`reader_active` already false) → a future survivor op self-elects. `owner` is
  alive while its rpc is inflight (as safe as the existing `r->done` deref; the
  only two non-NULL `inflight[]` installs — `client_run`, `submit_async` — set it);
  the death path is unchanged (dying owners carry no `debug_stop_req`, so the F6
  bounce still handles them).

**F2 — the role-release covers ALL FOUR `reader_active` sites.** Centralizing the
flags in `reader_recv_frame` gives the block-through to every caller; each handles
its stop-return without killing the shared session or spinning: the
`client_pump_or_park_locked` self-pump skips `client_mark_dead_locked` on a
`client_stop_pending` unwind; `client_send_flow` (spilling `out_buf` FIRST — the
#375 discipline, since `client_debug_stop_park` drops `c->lock`) +
`client_drain_until_free_tag` PARK a stopped sender at their loop top (else a
stop-unwound self-pump drains nothing → `c2s` never frees → every send EAGAINs →
the Proc never fully-stops → the debugger's stop HANGS); `p9_client_reader_pump_once`
skips mark_dead on a stop. `p9_client_reader_pump_once_deadline` alone has no guard,
kproc-only (its sole callers, `dev9p_poll.c` + `loom.c` SQPOLL, have
`t->proc == NULL`, detour-immune).

**Why frame-atomic (the holotype F1 [P1]).** The first fix (plain unwind for the
whole recv) desynced mid-frame: delivery is CHUNKED — `srvconn_client_recv` tsleeps
on an EMPTY ring, but under pipelining depth ≥ 2 + ring pressure a frame is split
across the ring in time (`srvconn` `chan_ring` short-read/write), so the reader
consumes a partial frame and its next recv tsleeps MID-FRAME (`got > 0`); a
stop-unwind there discards the consumed bytes → the survivor reads the frame TAIL
as a header → shared-session death / task-#50 misframe. Mechanisms 0+1 (frame-
atomic) close it for the STOP path. The IDENTICAL death-path mid-frame unwind (the
`#811` die-check below the detour is UNCHANGED) is a PRE-EXISTING #841/#811 latent,
**owned + tracked as task #90** (its fix narrows #811 → a §28 refinement → deferred
to its own spec-first sub-chunk). `DeathWinsOverStop` holds at every branch (a death
still unwinds a mid-frame block-through via the die-check). NO new §28 invariant and
NO spec change (the reader-role is below the `9p_client.tla` + `debug_stop.tla`
models, both re-verified GREEN). Validated by the 8c-3 (dirty-close) Fable holotype +
`9p_client.handoff_skips_debug_stopped_owner` (revert-probed: pre-fix 1137/1138 FAIL)
+ the SMP gate (the block-through decision is in the sched detour, kproc-immune, so
it has no deterministic kproc regression — the SMP gate + reasoning are its durable
coverage).

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

## 8a-2b-1 — real per-Proc hardware breakpoints

The first arm from the 8a-2b HW-debug tier: a debugger arms a real hardware
breakpoint at a user VA in a stopped target, and the target stops when a thread
executes that VA. No software `BRK` (I-12 W^X + I-36 REVENANT Image cache) — the
break is a debug register.

**The per-Proc table** (`struct debug_hw`, `arch/arm64/hwdebug.h`): a lazily
`kzalloc`'d array of up to `DEBUG_HWBP_SLOTS` (4, clamped to the CPU's `num_brps`)
breakpoint VAs, hung off `Proc.debug_hw`. Allocated on the first `hwbreak`, freed
ONLY at `proc_free` (never at detach) — so the ctx-switch reader never derefs
freed memory (every thread is reaped + on_cpu-spun before `proc_free`, so no CPU
is in a switch-in for the Proc). The table is mutated only when the target is
fully-stopped (`hwbreak`/`hwrmbreak` are stopped-only), so the ctx-switch reader
is quiescent; `bp_count` is `__atomic` so a detach-while-running (count -> 0)
races cleanly.

**The context-switch install** (`hwdebug_switch_in`, called from `sched.c` after
`sched_install_asid_ttbr0`, IRQs masked + CPU stable): loads `next`'s Proc
breakpoints into THIS CPU's `DBGBVR`/`DBGBCR` + sets `MDSCR.MDE`, or clears them
if `next` is not a debugged Proc and this CPU had them loaded (a per-CPU
`g_cpu_debug_loaded` flag keeps the common non-debugged switch MSR-free). This
is the load-bearing per-CPU-MDE isolation: a breakpoint at VA X fires ONLY while
the debugged Proc runs — `PMC=EL0` alone does NOT isolate two Procs sharing X, so
the install is keyed to "a thread of the debugged Proc is scheduled here."
`hwdebug_init_cpu` clears every IMPLEMENTED bp/wp control register at boot (a
reset-garbage `E`-bit would fire the first time `MDE` is set), on every CPU.

**The EC 0x30 route** (`hwdebug_breakpoint_from_el0`, `exception.c`): matches the
trap's `ELR` against the current Proc's armed table. A HIT calls
`proc_debug_stop_deliver` (the whole-Proc stop, Delve all-stop) — the thread
returns to the EL0-return tail and parks via the 8a-1 checkpoint machinery
(death still wins: the tail's die-check precedes the stop-check), and the
debugger learns via `/proc/<pid>/wait` and reads `regs.pc == the bp VA` (a HW bp
fires with `ELR` = the not-yet-executed bp'd instruction). A no-match on a Proc
that HAS a `debug_hw` is a benign STALE bp (a detach cleared the table before
this CPU reloaded) — this CPU's debug regs are disabled and the instruction
resumes (only the kernel arms a bp, so an unmatched fire is never an attack). A
no-match on a never-debugged Proc is a defensive `snare:ill` (impossible in
practice).

**The verbs** (`ctl`, DEBUG-FS-DESIGN section 4.3): `hwbreak <hexva>` /
`hwrmbreak <hexva>`, gated on slot-ownership + the I-39 two-axis re-check +
stopped-only (the quiescent-table discipline; Delve arms on a stopped process).
`detach` and the ctl-fd-close release both clear the table (`bp_count=0`) so an
orphaned target never re-traps on a dead debugger's breakpoint.

Data structure: `struct debug_hw { u32 bp_count; u64 bp_va[4]; }`;
`Proc.debug_hw` (`struct Proc` 320 -> 328, the pointer @320). Tests:
`hwdebug.bp_table` (the table logic — dedup, capacity, remove, reuse, clear);
the ctx-switch INSTALL + the EC route need real EL0 execution and are the
`/debug-probe` E2E's job (a bp armed at the child's `bp_landmark` VA fires
cross-Proc, `regs.pc == landmark`, then `hwrmbreak` + resume runs it clean) --
the kernel test is structurally blind to them, exactly as it is to the verify
DELIVERY. Also `#73`: `mmu_cross_proc_read`/`write` gained an explicit
`vaddr+len` u64-wrap guard (HF3 defense-in-depth).

## Status

8a-1 complete (the software-checkpoint tier: audited, SMP-gated, `debug_stop.tla`
green). 8a-2a complete (the HW-debug delivery verify: PASS on HVF + TCG). 8a-2b-1
complete (real per-Proc hardware breakpoints: the table + ctx-switch install +
the EC 0x30 route + `hwbreak`/`hwrmbreak` + `#73`). 8a-2b-2 complete (the arm64
single-step machine + the step-over-breakpoint dance + the `step` verb; §
below). 8a-2b-3 complete (hardware watchpoints via `DBGWCR`/`DBGWVR` + the EC 0x34
route + `hwwatch`/`hwrmwatch`; watchpoints-disabled-during-step; § below).
**8a-2c complete — THE 8a-2 ARC IS CLOSED** (the arc-close section below): the
consolidated Fable-5-max holotype over the whole 8a-2 surface + the SA-1 fix
(`proc_debug_fault_stop`, the attach-gated hardware-fire stop delivery) +
`StopImpliesOwned` in `debug_stop.tla` + the F1/F2/F3 fixes; default 1136/1136 +
boot OK + `/debug-probe` (bp/step/wp) + `/hwbp-verify` on HVF + the SMP gate + the
ARCH §25.4 authoritative row + the CLAUDE.md mirror + this doc.

## 8a-2b-2 — single-step + the step-over-breakpoint dance

A `step` resumes a stopped thread for exactly ONE EL0 instruction, then re-parks
(model-first against `specs/debug_step.tla`, the sibling of `debug_stop.tla`;
single-step is the only genuine protocol growth in 8a-2 — a bp-fire is
trigger-agnostic).

**The SS machine is per-thread** (`Thread.debug_ss_armed` + `debug_stepover_va`,
struct 1168 → 1184) so `MDSCR.SS` follows the thread across an IRQ-preempt
migration mid-step — the Linux per-task model (SS is per-PE; a migrated step would
run free otherwise). `hwdebug_switch_in` loads `MDSCR.SS` from `debug_ss_armed`
(alongside `MDE`/bps) and SKIPS `debug_stepover_va` when loading the bp table (the
step-over: a thread stopped AT a bp would re-trap on the resume instead of
stepping, so that bp is loaded disabled for its step). `el0_return_stop_check`
arms `SPSR.SS` (bit 21) in the resume frame the `eret` restores (active-not-
pending → one instruction), IRQ-masked from the arm to the `eret`.

`hwdebug_singlestep_from_el0` (the EC 0x32 route): an armed step completed →
disarm SS (the per-thread flags + this CPU's `MDSCR.SS`) + re-stop the whole Proc
(`proc_debug_stop_deliver`) so the thread re-parks at the tail (death wins there)
and the debugger reads the advanced `regs.pc`; a spurious step EC → benign clear +
resume (only the kernel arms `MDSCR.SS`).

The `step` ctl verb arms the head thread's SS + the step-over VA (if the PC is at
an armed bp) + resumes, then **blocks via `devproc_wait_block`** (which polls
`devproc_target_fully_stopped` — it checks `debug_stop_req`, re-set by the EC
0x32) — NOT the `all_threads_parked` wait `stop` uses. That is the load-bearing
fix the in-guest E2E caught: `proc_debug_resume` clears `debug_stop_req` + wakes
the head, but the head's `rendez_blocked_on` stays `&debug_rendez` (+ `on_cpu ==
false`) until it resumes from `sleep()`, so `all_threads_parked` reads a STALE
"still parked" state and the wait returns before the step even runs (then the
regs read hits `fully_stopped`'s `debug_stop_req == 0` gate and fails).

v1.0: `step` resumes the WHOLE Proc, so a multi-thread target's peers briefly run
during the head's step — the per-thread step is a v1.x refinement (with the
per-thread `/proc/<pid>/thread/<tid>/` layer). Single-threaded targets are clean.
Tests: `hwdebug.singlestep_benign` (the spurious-step path — the armed path needs
real EL0 execution) + the `/debug-probe` step phase (steps 3× FROM the
breakpointed entry, verifying `pc` advances by exactly 4 bytes/instruction — the
SS machine AND the step-over dance).

## 8a-2b-3 — hardware watchpoints

A watchpoint traps a data ACCESS to a user address (the debugger's `watch var`).
It is the twin of the breakpoint path — same per-Proc table, same ctx-switch
install, same stop delivery — so it adds **no new spec** (a wp-fire routes through
`proc_debug_stop_deliver`, covered by `debug_stop.tla`; the step machine is
`debug_step.tla`).

**The table** (`struct debug_hw`, `arch/arm64/hwdebug.h`) gains a parallel
watchpoint array: `wp_count` + `wp_va[]` (the watched VA) + `wp_len[]` (1..8) +
`wp_flags[]` (`DEBUG_WP_R`/`DEBUG_WP_W`), `DEBUG_HWWP_SLOTS = 4` clamped to
`ID_AA64DFR0_EL1.WRPs` at enumerate (`g_debug_max_wp`). Same lifetime + quiescence
discipline as the bp table: lazily allocated, freed at `proc_free`, mutated
stopped-only, `wp_count` `__atomic` (published RELEASE-last, after the slots) for
the detach-while-running race.

**The encoding** (`hwdebug_wp_encode` → `DBGWVR`/`DBGWCR`): `DBGWVR` = the VA
aligned down to the 8-byte doubleword; `DBGWCR` = E=1, PAC=`0b10` (EL0-only), LSC
from the rwx flags (`0b01` load / `0b10` store / `0b11` both), BAS = the byte mask
covering `[va, va+len)` within the doubleword, MASK=0. v1.0 supports **1..8 bytes
within ONE doubleword** (`(va&7)+len ≤ 8`) — a naturally-aligned scalar (the
realistic `watch` target); a cross-doubleword or `>8`-byte region is rejected at
`hwdebug_wp_add` (MASK-based larger regions are a v1.x lift). The `hwdebug.wp_encode`
unit test pins the bit math (aligned/8/W → `0x1FF5`, off4/4/R → `0x1E0D`,
off1/2/RW → `0xDD`); the E2E proves the delivery.

**Watchpoints are DISABLED during a single-step.** `hwdebug_load_debug` loads
`wpc = ss ? 0 : wp_count` — the single stepped instruction's own data access must
not trap a watchpoint mid-step and derail `StepExactlyOne` (the Linux model). A
watchpoint survives a step (re-armed on the post-step resume, `ss=false`); it is
inactive only for the one stepped instruction. `MDE` is set when any bp OR wp is
armed (it gates both).

**The EC 0x34 route** (`hwdebug_watchpoint_from_el0`): an armed watchpoint fired
(`wp_count > 0`) → deliver the whole-Proc stop (the thread parks at the tail; the
debugger reads `regs.pc` == the accessing instruction). arm64 reports `FAR`
**imprecise-within-the-block**, so delivery is gated on `wp_count > 0`, NOT an
exact `FAR` match (gating on `FAR` would risk a MISSED stop; the hardware only
traps accesses matching a programmed `DBGWCR`, so a wp EC on a debugged Proc with
armed wps IS a hit). `wp_count == 0` → a benign STALE wp (a detach/`hwrmwatch`
cleared the table before this CPU reloaded) → disable this CPU's debug regs +
resume (symmetric with the bp STALE arm).

**The verbs** (`ctl`): `hwwatch <rwx> <hexva> <declen>` / `hwrmwatch <hexva>`,
gated identically to `hwbreak` (slot-owner + the I-39 re-check + stopped-only + the
lazy-table pre-alloc-outside-the-lock). `<rwx>` is `r`/`w` chars (at least one);
`<declen>` is 1..8. Removal keys on the exact VA. Both cleared at detach +
close-release (`hwdebug_wp_clear_all`, alongside the bp clear) so a dead debugger's
watchpoints do not leave the orphaned target re-trapping on the watched access
forever.

**A `start` (continue) directly over a still-armed watchpoint re-traps** — a wp
fires with `ELR` = the accessing instruction, which re-executes on `eret`. The
debugger `step`s past it (watchpoints are OFF for the step) or `hwrmwatch`es it
first; this is the raw arm64 debug-interface semantic (Delve/GDB wrap it). The
auto-step-over-on-continue is a v1.x refinement.

Tests: `hwdebug.wp_table` (add/remove/dedup/capacity + the input-validation
rejects: bad len, cross-doubleword, empty flags) + `hwdebug.wp_encode` (the
register math) + the `/debug-probe` watch phase (arm a WRITE watchpoint on a
cross-Proc stack word, resume, the child's store traps → verify the EL0t stop →
`hwrmwatch` + resume → exit 0; the wp is the ONLY reason the store stops).

## 8a-2c — the consolidated close (SA-1 + the arc audit)

8a-2c is the arc close: a focused Fable-5-max holotype over the WHOLE 8a-2
hardware-debug surface (the bp/step/wp EC handlers + the ctx-switch install + the
detach/stop/resume composition with the death path) + the SMP gate + the HVF/TCG
boot proofs + this doc + the ARCH §25.4 authoritative prosecution row + the
CLAUDE.md mirror.

### SA-1 — the stale-fire-vs-detach strand (self-found; fixed)

The hazard (found by the 8a-2b-3 self-audit, pre-existing since the b-1 bp path): a
hardware fire (a bp/wp hit or a single-step completion) delivered the whole-Proc
stop by calling `proc_debug_stop_deliver` **directly from the EC handler** — in the
target's own exception context, holding NO lock and with NO owner check. Meanwhile
a `detach` / ctl-fd close cleared `debug_owner` + the hw table + called
`proc_debug_resume`, all **under `g_proc_table_lock`**. Those two are unserialized,
so a fire whose `debug_stop_req` store lands AFTER the detach's `proc_debug_resume`
cleared it parks the target at its EL0-return tail with **no debugger left to
resume it** — the strand (violates `debug_stop.tla` NoStrand). Narrow (a genuine
race window), debuggee-only (the rest of the system is unaffected), I-39-gated (only
a Proc a debugger attached to). The ctl `stop` verb was never exposed — it delivers
under `g_proc_table_lock` (`proc_for_each`) with `debug_owner` already verified; the
EC path was the asymmetric, ungated setter.

The fix: `proc_debug_fault_stop(p)` (`kernel/proc.c`) — the EC-path counterpart of
`proc_debug_stop_deliver`. It takes `g_proc_table_lock` (so it serializes with a
concurrent detach) and delivers the stop **only while `p->debug_owner != NULL`**; it
returns whether it delivered. The three EC handlers
(`hwdebug_breakpoint_from_el0` / `hwdebug_watchpoint_from_el0` /
`hwdebug_singlestep_from_el0`) now call it: on a live owner it delivers (the thread
parks as before); on a NULL owner (detached in the race window) it no-ops and the
bp/wp handler falls back to the benign STALE-arm path (disable this CPU's debug regs
+ resume the instruction), while the step handler simply lets the thread run free
(SS is already disarmed) — exactly right for a detached target. The EC handler holds
no kernel lock when it fires (it was at EL0), so taking `g_proc_table_lock` (the
outermost lock) is deadlock-free vs the detach's `GPTL → wait_lock` walk; and
`smp_resched_others()` under the lock is the same shape the ctl `stop` verb already
uses. The deliver-before-detach sub-case (a fire that delivers, then the thread
parks after the detach's resume walk) is covered by the existing I-9
register-then-observe in `proc_debug_resume` (clear-before-walk under the per-Thread
`wait_lock`): a thread registering after the walk re-observes the cleared flag and
does not park; one registered before is found + woken.

**The model** gained the invariant `StopImpliesOwned == sflag => attached` (the
per-Proc stop flag is set only while a debugger owns the slot) + a `FaultStop`
action (gated on `attached` in the clean model; ungated under the new
`BUGGY_FAULT_STOP_UNGATED` knob) + the `debug_stop_buggy_fault_stop_ungated.cfg`
counterexample. `RequestStop` was always attach-gated, so `StopImpliesOwned` is an
invariant of the correct model; SA-1 was the impl violating it, and
`proc_debug_fault_stop`'s `debug_owner`-under-GPTL check aligns the impl to the
model. Clean stays TLC-green (2264 distinct, unperturbed — `FaultStop`-clean is
dominated by `RequestStop` + `DbgDie`); the buggy cfg violates `StopImpliesOwned`.
Regression: `devproc.debug_stop_start_resume` gains the SA-1 leg (fault-stop with no
owner does NOT deliver + sets no flag; with a live owner it delivers) — revert-probed
(the ungated gate → 1134/1135 FAIL at exactly that leg).

### The holotype + the fixes (F1/F2/F3)

The consolidated Fable-5-max holotype (MODEL(end): Fable 5) + a concurrent
self-audit over the whole 8a-2 surface closed **0 P0 / 1 P1 / 0 P2 / 2 P3, NOT
dirty** (all localized fixes — a flag-clear, a boot-window gate, a symmetric disable;
no wait/wake-protocol restructure). Both prosecutors converged on the SA-1 fix being
sound + the verified-sound set (gate completeness, no deadlock, no torn `(count,
slots)` read, death wins, the SPSR guard, the I-39 gate, the wp-encode math).

**F1 [P1] — a stale `debug_ss_armed` leaks a spurious single-step (the multi-thread
trap; FIXED).** `debug_ss_armed` is set by the `step` verb and was cleared in exactly
one place — the SS EC handler (`hwdebug_singlestep_from_el0`). So a `step` on the head
H that is SUPERSEDED by a peer P's bp/wp fire (a whole-Proc stop, reachable on the
always-multi-threaded Go workload) before H reaches its own EC 0x32 leaves
`debug_ss_armed` set; H parks for P's stop, and on the next `continue`
`el0_return_stop_check` re-arms `SPSR.SS` off the stale flag → H executes ONE
instruction and stops again — a phantom stop after a `continue`. Invisible to the
single-threaded `debug-child` E2E and below `debug_step.tla`'s single-thread step
abstraction (no cross-thread stop flag). FIX: a whole-Proc stop supersedes any
in-flight step — `proc_debug_stop_deliver` clears `debug_ss_armed` +
`debug_stepover_va` for every thread of `p` (idempotent with the normal step
completion, which clears the flag at its own EC 0x32 first; under
`g_proc_table_lock`, so `p->threads` is stable). Regression
`devproc.debug_step_cancel_on_stop` — revert-probed (the clear disabled → 1135/1136
FAIL). The cross-thread step-vs-stop supersede is a multi-thread interaction below the
single-thread `debug_step.tla` abstraction; modeling it would require merging the step
+ stop multi-thread machinery, disproportionate for a bounded functional (phantom-stop,
not soundness) bug — the fix + the mechanical regression + this prose are the rigor
(the below-the-model pattern, cf. net-4d / #294).

**F2 [P3] — the global `hwverify` slot could swallow another Proc's real breakpoint
(FIXED).** The 8a-2a self-scoped verify uses a GLOBAL one-at-a-time slot whose
EC-swallow is keyed on an ELR match, NOT the arming Proc, and any unprivileged Proc
could arm a verify on itself post-boot and leave it armed. A debugged Proc D with a
real bp at the same VA would have its stop swallowed + its MDE transiently cleared —
bounded (self-healing, no authority gain) but a real cross-Proc integrity effect on an
authorized session. FIX: the verify is a BOOT-ONLY diagnostic (the boot probe
`usr/hwbp-verify` runs before `SYS_BOOT_COMPLETE`; post-boot the real HW-debug path is
the 8a-2b per-Proc install), so `devproc.c` refuses `hwverify` once `boot_is_complete()`
(exposed from `kernel/main.c`) — no post-boot arm → the global slot stays idle → the
whole surface is gone. The boot probe still passes (`/hwbp-verify PASS`).

**F3 [P3] — the detached-step leg left MDE + bps loaded (only SS cleared; FIXED).** A
step loads MDE + the bp table too (`hwdebug_load_debug` with `ss=true`), so on the
detached-step leg (`proc_debug_fault_stop` returns false) clearing only SS left a wider
stale window than the bp/wp detached arms (which call `hwdebug_disable_this_cpu`). FIX:
symmetric `hwdebug_disable_this_cpu()` on the detached-step leg + the corrected comment.

**Gates**: default **1136/1136** + boot OK + 0 EXTINCTION + `/debug-probe` (bp/step/wp)
PASS + `/hwbp-verify` PASS on HVF (`-cpu host`, GICv2) — the 8a-2c HVF proof;
`debug_stop.tla` (clean 2264 + 5 buggy) + `debug_step.tla` (clean + 2 buggy) TLC-green;
the SMP gate default+UBSan × smp4/smp8 N=10 = **40/40 PASS, 0 corruption**. Closed list:
`memory/audit_8a2_closed_list.md`.

## 8b — the cross-boundary unified stack (the settled-thread inspect)

Stage 8b makes the §4 headline — the unified user→kernel stack of a thread
blocked deep in a syscall — first-class, and closes the one gap ground truth
surfaced. Design: `docs/DEBUG-FS-DESIGN.md §5b` (user-voted 2026-07-16).

**The gap.** The 8a debug-stop parks a target ONLY at the EL0-return tail (a
syscall *complete*), and every debug read is gated `fully_stopped`. So a
debug-stopped thread's `kstack` shows the return-tail path, NOT the
`blocked-in-channel-recv → sched.c::sleep → the rendez` stack the §4 headline
promises — and a thread genuinely *hung* in a syscall never reaches the tail, so
a debug-stop never takes: you cannot freeze a hung thread to look at it.

**The fix — the Linux `/proc/<pid>/stack` model.** Reading a *settled*
(`on_cpu==false`) thread's kernel stack is a **read-only inspection**, distinct
from and strictly weaker than a debug-stop (no execution control, no user
memory). So `/proc/<pid>/kstack` (`devproc_kstack_walk_cb`) DROPS the
`fully_stopped` gate — it reads the head thread's kernel fp-chain whenever the
head is settled (parked on ANY rendez: the pipe/torpor/debug rendez), with **I-39
authorization ONLY** (`devproc_debug_authorized` + kproc-refuse — unchanged), NO
debug-stop. `devproc_format_kstack` gates the head on `on_cpu==false` (a running
head reports `<running>` — its live registers are in hardware, not `th->ctx`).
**`mem`/`regs`/`kregs`/`wait` KEEP the `fully_stopped` gate**: user memory, user
registers, and all execution control stay stopped-only (the vote relaxed ONLY the
read-only kernel-stack inspect).

**Memory-safety (§5b.1).** The relaxation is memory-safe regardless of the
target's concurrent execution:

- **Lifetime** — the read holds `g_proc_table_lock` (via `proc_for_each`), which
  pins the target *reachable*; `wait_pid` UNLINKS the ZOMBIE from the table under
  GPTL (`proc_unlink_child`) BEFORE the lock-free `thread_free`, so no reachable
  target's Thread can be freed under the read — ALIVE or ZOMBIE-unreaped alike (the
  load-bearing pin is the unlink-before-free ordering, not "ALIVE"; a reachable
  ZOMBIE's head is `THREAD_EXITING` → `devproc_format_kstack` returns 0). [F2]
- **Bounded walk** — `halls_walk_kernel_frames` gates every deref to the thread's
  own usable kstack `[base+guard, base+size)` + `fp+16 <= hi`. A thread that wakes
  and RUNS mid-walk (a wake takes the rendez/rq locks, NOT GPTL, so it is not
  excluded) can mutate its own kstack + `th->ctx` under the read — a torn `u64`
  fp/lr, a garbage frame link — but every deref stays inside the thread's own
  kstack, and an out-of-range/misaligned link stops the walk (`halls_fp_is_sane`).
  `base`/`kstack_size` are set at thread creation and never change, so the bounds
  are stable even if `th->ctx` is torn.
- **Consistency = best-effort** (the Linux `/proc/stack` contract) — a genuinely
  *sleeping* thread has a stable kstack until woken, so the common case (inspect a
  hung/blocked thread) is coherent; a running/waking thread yields a
  possibly-inconsistent but bounded (never unsafe) frame list.
- **No new stop-machinery race** — the inspect reads `th->ctx` + kstack memory; it
  touches neither `debug_stop_req` nor the park/wake protocol nor the death path,
  so `specs/debug_stop.tla` is **UNCHANGED** (the read is a gate condition, not a
  state-machine transition — the 8a `mem`/`regs`-read precedent). Re-verified
  clean 2264 distinct GREEN.

**The I-39 refinement (§5b.2).** I-39's "register/memory access only on a stopped
target" is refined (not weakened): a read-only inspection of a settled thread's
KERNEL stack is I-39-*authorized* but does not require a debug-stop; user memory,
user registers, and all execution control remain stopped-only. The inspect
confers no authority beyond the gate (a non-owner without `CAP_DEBUG` is refused)
and controls no execution. **The raw-address gate (I-16; holotype F1):** a kstack
line's raw slid `addr` + unslid `link` reveal the KASLR slide (`koff = addr −
link`), an I-16 secret `/ctl/kernel-base` gates behind `CAP_HOSTOWNER` (#57a). So
the raw columns are emitted ONLY to a `CAP_DEBUG`/`CAP_HOSTOWNER` caller (the
debugger tier — it reads `/ctl/kernel-base` anyway + needs raw addrs to correlate
with the kernel DWARF at 8c); the **owner axis** gets the KASLR-independent
symbolic form (`#N  name+0xsoff`), which IS the "why is it hung" diagnostic.
Without this an unprivileged owner reads its own `koff` off a settled head thread
(8b widened the pre-existing 8a owner-`attach`-and-`stop` path to a self-read).

**Kernel DWARF — DEFERRED to 8c.** 8b ships `func+offset` kernel symbolization
(HX-2, already on-target). Source-line mapping + kernel-frame locals need the
kernel ELF's DWARF (a few MB) + an on-device DWARF reader — dlv (8c) brings Go's
`debug/dwarf` reader (the consumer) with it; shipping MB of `.debug_*` + a bespoke
reader with no consumer in 8b is premature.

**Proof.** `usr/stack-probe` (boot-fatal) spawns `usr/stack-child` (which blocks
forever in `torpor_wait` → `sleep()` on the torpor rendez — settled, never at the
EL0-return tail), hands it CPU time via a short never-woken timed wait, then reads
`/proc/<pid>/kstack` **owner-authorized, NO attach/stop** until the child is
blocked in `sleep()`. The ground truth it prints — the deep-in-syscall kernel
stack, no debug-stop:

```
#0 sched   #1 sleep   #2 tsleep   #3 sys_torpor_wait_for_proc
#4 syscall_dispatch   #5 exception_sync_lower_el   #6 _exception_vectors
```

That is "why is this thread hung" all the way down to the EL0→EL1 SVC entry
vector. The kernel unit test `devproc.debug_kstack_settled` covers the mechanism
synthetically (settled-not-stopped → walks; `on_cpu==true` → `<running>`;
non-owner → I-39 refused) and is revert-probed: restoring the `fully_stopped` gate
→ 1136/1137 FAIL at "settled kstack walks".

**A hunt recorded (ground-truth over theory).** The first probe caught the child
in its NEVER-RUN state — a single frame `thread_trampoline+0x0` (`th->ctx.lr` was
still the initial value `cpu_switch_context` had not yet overwritten, because the
child had not been dispatched). Reading `cpu_switch_context` (it saves `ctx.lr =
x30`, the return into `sched`) proved the single frame meant "not yet run", not
"blocked" — the poll accepted it too early. Fix: hand the child real CPU time (a
timed block, not `t_yield`) + wait for the `sleep` frame, not merely "settled".

**v1.x seams**: per-tid `/proc/<pid>/thread/<tid>/kstack` (8b reads the head
thread); the *user* frames of a hung thread (the full user+kernel stitch of a
thread blocked mid-syscall needs the user `regs`/`mem` of a non-stopped thread, a
larger relaxation the vote deliberately did not take — 8b delivers the kernel half
for a blocked thread and the full stitch for a *stopped* thread); source-line
kernel DWARF (8c).

**Gates**: default **1137/1137** + boot OK + 0 EXTINCTION + `/stack-probe` PASS
(the `sched→sleep→tsleep→torpor→dispatch→exception-entry` chain) +
`debug_stop.tla` clean 2264 GREEN (unchanged) + the SMP gate. Closed list:
`memory/audit_8b_closed_list.md`.

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
  checkpoint it reaches, not by interrupting the sleep. **8b's settled-thread
  `kstack` inspect READS a blocked thread's kernel stack without stopping it (the
  Linux `/proc/<pid>/stack` model — see §8b);** an explicit *stop*-a-sleeping-
  thread-now (execution control on a blocked thread) remains a v1.x refinement.
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
