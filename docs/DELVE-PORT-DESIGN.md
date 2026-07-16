# DELVE-PORT-DESIGN — porting Delve (`dlv`) to Thylacine over the `/proc` debug-fs

**Status: DESIGN / SCOPING (Stage 8c focused design pass — scripture, no code).**
This is the binding design charter for **Stage 8c** of the Go IDE arc
(`docs/GO-IDE-DESIGN.md §8`): a `proc_thylacine` backend for **Delve** (`dlv`,
the standard Go debugger) driven entirely over the as-built kernel debug-fs
(`docs/reference/134-debug-fs.md`, Stage 8a + 8b). The resulting Thylacine
debugger is named **Ambush** (§11, user-voted 2026-07-16) — a Delve port
(binary `ambush`, its CLI mirroring `dlv`). Per the spec-to-code
suspension this is a prose-validated arc; 8c is a **userspace port** (a Go
program cross-built `GOOS=thylacine`, execution-verified, NOT a new kernel
surface), a different — lighter-audit — risk profile than the 8a/8b kernel work.
The kernel debug surface it binds to is DONE and closed (8a-1 software
checkpoint + 8a-2 arm64 HW debug + 8b-1 settled-thread inspect).

The design pass follows the arc pattern (8a opened with `DEBUG-FS-DESIGN.md`):
research the port target's backend contract, map it onto our primitives, name
the load-bearing decisions, and lay out the sub-chunks — before any code.

---

## 1. Scope — what 8c lands, and what it does not

**In scope (8c):** `dlv` runs on-device and drives a Go program through the
debug-fs — **attach, inspect (goroutines / stacks / variables / registers /
memory), breakpoints, single-step, continue, watch** — CLI first (the `dlv`
REPL + `dlv dap` over stdio), no UI. The genuinely new build is a small set of
build-tagged Go files in Delve's `pkg/proc/native` package (the OS backend) plus
the build-tag surgery to select them for `GOOS=thylacine`. Everything above the
backend — `service/debugger`, `service/dap`, `service/rpc2`, the DWARF/variable
engine, the goroutine/scheduler model — is portable Go that already cross-builds
on the Go fork.

**Out of scope (later stages):**
- **gopls** (editing intelligence) — Stage 8d.
- The **Nora plugin** (DAP + LSP clients) — Stage 8e.
- The **Kaua debug UI** (source / vars / stack / goroutines panes + run-pane) —
  Stage 8f.
- The **superpowers** (namespace/resource inspector, scheduler view,
  system-wide attach, crash post-mortem, Stratum snapshot-debugging) — Stage 8g.
- **Kernel DWARF** (source-line mapping of kernel frames) — 8b deferred it to
  8c *as a dependency of dlv's DWARF reader arriving*, but it is a **kernel-frame
  enrichment**, not a dlv-backend requirement: 8c ships `func+offset` kernel
  symbolization (already on-target via HX-2 + the `/proc/<pid>/kstack` file), and
  kernel source lines are a 8c-late / 8g refinement once a real consumer exists.
- **Reverse execution / time-travel** — the deferred Stage-9 NOVEL.

---

## 2. Why Delve ports cleanly (the three facts)

1. **Delve is a Go program.** `dlv` cross-builds `GOOS=thylacine GOARCH=arm64`
   like the toolchain itself (Stages 4-6) — no C, no CGO on the thylacine target
   (the darwin backend's cgo/mach files are build-tagged out). It is a **port,
   not pouch** (per the native-vs-ported split, `CLAUDE.md`).
2. **The backend surface is small and OS-factored.** Delve isolates all
   OS-specific work into build-tagged files in one package (`pkg/proc/native`):
   `proc_linux.go` / `threads_linux.go` / `ptrace_linux.go` /
   `registers_linux_arm64.go` / `threads_linux_arm64.go`. A `proc_thylacine.go`
   set replaces exactly those; the rest of `native` (and all of `proc`,
   `service`, `terminal`) is shared portable Go.
3. **Our ABIs are already the Linux ABIs.** The debug-fs `regs` file
   (`struct t_user_regs`, 272 B) *is* the Linux `user_pt_regs` layout
   (`regs[31]` @0, `sp` @248, `pc` @256, `pstate` @264) — deliberately, since
   8a-1 gamma-2. So Delve's `linutil.ARM64PtraceRegs` / `linutil.ARM64Registers`
   (its Linux-arm64 register decode + `proc.Registers` implementation) are
   **reused verbatim** — the largest single structural win in the port. Same for
   `fpregs` (`t_user_fpregs`, 520 B = `vregs[512]` + `fpsr` + `fpcr`).

The consequence: 8c is **mostly thin file-I/O wrappers** (`pread`/`pwrite`/ctl
writes) over the debug-fs, wearing the register decode Delve already has.

---

## 3. The backend contract (Delve v1.25.2)

Delve's `pkg/proc` defines the minimal interfaces a backend implements. The
`native` backend's `nativeProcess` / `nativeThread` satisfy them; a Thylacine
backend fills the same shape.

**`proc.ProcessInternal`** (the backend surface — `proc/interface.go`):
`BinInfo` / `EntryPoint` / `FindThread` / `ThreadList` / `Breakpoints` /
`Memory` (the `Process` half) + `Valid` / `RequestManualStop` /
`WriteBreakpoint` / `EraseBreakpoint` / `SupportsBPF` / `SetUProbe` /
`GetBufferedTracepoints` / `DumpProcessNotes` / `MemoryMap` /
`StartCallInjection` / `FollowExec`.

**`proc.Thread`** (`proc/threads.go`): `Breakpoint` / `ThreadID` / `Registers` /
`RestoreRegisters` / `BinInfo` / `ProcessMemory` / `SetCurrentBreakpoint` /
`SoftExc` / `Common` / **`SetReg`** ("a minimal implementation of this interface
can support just setting the PC register").

**`proc.Registers`** (`proc/registers.go`): `PC` / `SP` / `BP` / `LR` / `TLS` /
`GAddr` / `Slice` / `Copy` — all satisfied by `linutil.ARM64Registers` once fed
our `regs`/`fpregs`/`tpidr_el0` bytes.

**`proc.ProcessGroup`** (`ContinueOnce` / `StepInstruction` / `Detach` /
`Close`) — the resume/step/detach driver, implemented on the backend's
`processGroup`.

The OS-specific *functions* the `native` backend factors out (the exact
`proc_thylacine` fill-set):

- `proc_*.go`: `Launch`, `Attach`, `initialize`, `kill`, `requestManualStop`,
  `addThread`, `updateThreadList`, `trapWait`, `wait`, `resume` (the group
  ContinueOnce), `stop` (post-trap stop-all), `detach`, `EntryPoint`,
  `SupportsBPF`/`SetUProbe`/`FollowExec` (unsupported stubs).
- `threads_*.go`: `WriteMemory`, `ReadMemory`, `singleStep`, per-thread `stop`/
  `resume`/`Stopped`, `SoftExc`.
- `ptrace_*.go`: the raw `ptraceAttach/Detach/Cont/SingleStep` — the layer we
  **replace with ctl writes**; there is no ptrace on Thylacine.
- `registers_*_arm64.go`: `ptraceGetGRegs`/`SetGRegs`/`GetFpRegset`/
  `GetTpidr_el0`, `setPC`, `SetReg`, `registers(thread)`.
- `threads_*_arm64.go`: `writeHardwareBreakpoint`/`clearHardwareBreakpoint`/
  `findHardwareBreakpoint`, `getWatchpoints`/`setWatchpoints`.

---

## 4. The mapping — Delve backend fn → debug-fs primitive

| Delve native fn | Thylacine debug-fs realization |
|---|---|
| `Attach(pid)` | open `/proc/<pid>/ctl` (holds the attach slot for the session) + write `attach` (the I-39 gate; `Einuse` if taken) |
| `Launch(cmd, …)` | `SYS_SPAWN` the ELF (stdio piped) → `attach` → `stop` → `hwbreak <entry>` → `start` → `wait` (stop-at-entry; §7) |
| `requestManualStop` | write `stop` to ctl (Delve's Halt / `^C`) |
| `resume()` (ContinueOnce) | write `start` to ctl, then read `wait` (block until the next stop/exit) |
| `singleStep(thread)` | write `step` to ctl (arms `MDSCR.SS` + the step-over dance kernel-side), then read `wait` |
| `trapWait` / `trapWaitInternal` | read `/proc/<pid>/wait` — blocks until the target stops at a checkpoint / exits / the caller is death-interrupted; the returned event drives Delve's stop-reason logic |
| `wait(pid)` (reap) | `SYS_WAIT_PID` (already wired in the Go fork's `syscall`) |
| `detach(kill)` | write `detach` to ctl (or close the ctl fd — the #68/#926 close-at-exit resume); `kill` → `kill` verb (I-26) |
| `kill` | write `kill` / `killgrp` to ctl (the I-26 two-axis gate) |
| `ReadMemory(data, addr)` | `pread(/proc/<pid>/mem, data, addr)` — the target VA is the file offset; a non-resident VA reads 0 bytes moved |
| `WriteMemory(addr, data)` | `pwrite(/proc/<pid>/mem, data, addr)` — refuses an RO leaf (I-12/I-36: a debugger writes DATA; breakpoints are HW) |
| `ptraceGetGRegs(regs)` | `pread(/proc/<pid>/regs)` → `t_user_regs` == `linutil.ARM64PtraceRegs` (byte-identical) |
| `ptraceSetGRegs` / `setPC` / `SetReg` | `pwrite(/proc/<pid>/regs)` — **the SPSR is silently ignored** (the privilege guard); Delve only sets PC/SP/GPRs, never pstate, so this is a no-op divergence |
| `ptraceGetFpRegset` | `pread(/proc/<pid>/fpregs)` → `t_user_fpregs` |
| `ptraceGetTpidr_el0` (the Go `g`) | `pread(/proc/<pid>/kregs)` → `t_kernel_regs.tpidr_el0` @104 |
| `writeHardwareBreakpoint(addr,W,idx)` | write `hwbreak <hexva>` (code) or `hwwatch <rwx> <hexva> <declen>` (data) to ctl — **the kernel owns the DBGBCR/DBGWCR encoding**; the backend hands a plain VA |
| `clearHardwareBreakpoint(addr,W,idx)` | write `hwrmbreak <hexva>` / `hwrmwatch <hexva>` |
| `findHardwareBreakpoint()` | after a stop, read `regs.pc`; match against the armed bp set (an arm64 HW bp fires with ELR = the not-yet-executed bp'd instruction) |
| `SupportsBPF` / `SetUProbe` / `GetBufferedTracepoints` | `false` / unsupported / empty — no eBPF/uprobe surface |
| `FollowExec(v)` | unsupported (a debugged Proc's spawns are not followed at v1.0) |
| `DumpProcessNotes` / `MemoryMap` / `StartCallInjection` | optional — omit at 8c (core-dump, mem-map, and call-injection are 8g/v1.x) |
| kernel half of the unified stack | read `/proc/<pid>/kstack` (8b symbolized fp-chain) |

`GetManualStop` / the stop-reason disambiguation ride the `wait`-file event, not
a `waitpid` status word — the one place the Thylacine `trapWait` diverges
structurally from the Linux `trapWaitInternal` (which decodes `WSTOPSIG` etc.).
Thylacine's `wait` file returns a *stopped/exited/interrupted* verdict; the
backend maps it to Delve's `StopReason`.

---

## 5. The register-layout reuse (the big win, in detail)

Delve's Linux-arm64 register path:

```
ptraceGetGRegs(pid, &linutil.ARM64PtraceRegs{...})   // PTRACE_GETREGSET NT_PRSTATUS
registers(thread) -> linutil.NewARM64Registers(regs, fpregs, tpidr, …)  // proc.Registers
```

Thylacine substitutes the *source* of the bytes and keeps the decode:

```
pread(/proc/<pid>/regs)   -> t_user_regs   (== ARM64PtraceRegs, 272 B, verbatim)
pread(/proc/<pid>/fpregs) -> t_user_fpregs (V0..V31 + fpsr + fpcr, 520 B)
pread(/proc/<pid>/kregs)  -> tpidr_el0 @104
registers(thread) -> linutil.NewARM64Registers(...)   // UNCHANGED
```

So `proc.Registers` (`PC`/`SP`/`BP`/`LR`/`TLS`/`GAddr`/`Slice`) and the DWARF
register mapping come for free. `GAddr` (the Go `g` pointer) resolves from `TLS`
(= `tpidr_el0`) exactly as on Linux-arm64. The only backend-specific decode is
the `fpregs` slice offset if `t_user_fpregs` differs from Linux
`user_fpsimd_state` in trailing reserved bytes (Linux has `__reserved[2]`, ours
ends at `fpcr`) — a 4-line adjustment in `fpRegisters()`, not a rewrite.

---

## 6. The hardware-breakpoint-only constraint (the headline decision)

**This is the single most consequential 8c design fact.** Delve's DEFAULT code
breakpoint is a **software** breakpoint: `WriteBreakpoint(bp)` writes a `BRK`
(arm64) trap instruction into the target's text via `WriteMemory`, and
`EraseBreakpoint` restores the original bytes. On Thylacine that is **forbidden**:

- **I-12 (W^X)** — text is RO+X; you cannot write a `BRK` into it.
- **I-36 (REVENANT shared Image cache)** — text pages are shared across every
  process running the same binary; a `BRK` written into one debuggee's page
  would corrupt every other process's text.

The kernel already enforces this: `mmu_cross_proc_write` (the `mem` file's write
path) **refuses an RO leaf**. So a Delve software-breakpoint write to text
*fails at the kernel*.

**The resolution (locked by I-12/I-36):** the `proc_thylacine` backend routes
**every** breakpoint — ordinary line breakpoints included — to the **hardware**
path (`ctl hwbreak <va>`), never a memory write. Concretely, `WriteBreakpoint`
arms a HW breakpoint and `EraseBreakpoint` clears it; the backend reports
`SoftExc()==false` (there is no software-exception path). Watchpoints already go
to `hwwatch` on every backend.

**The user-visible consequence — a hard breakpoint ceiling.** arm64 guarantees
≥ 6 breakpoint registers + 4 watchpoint registers (the debug-fs enumerates the
real counts into `g_hw_features.num_brps`/`num_wrps`; clamped to
`DEBUG_HWBP_SLOTS=4` / `DEBUG_HWWP_SLOTS=4` at v1.0). So a Delve session on
Thylacine can hold **at most 4 code breakpoints + 4 watchpoints simultaneously**
(the kernel's v1.0 clamp; raising the clamp toward the architectural 6/4 is a
one-line kernel change weighed at 8c-2). Setting the 5th breakpoint returns a
clear "out of hardware breakpoint slots" error — NOT a silent failure. This is
the DEBUG-FS-DESIGN §3.1 "a debugger session multiplexes them" note made
concrete, and it is the defining ergonomic difference from `dlv` on Linux.

Delve *has* a hardware-breakpoint path (its watchpoints, and `-hardware` bps on
backends that support them), so `WriteBreakpoint`-routes-to-HW is a supported
backend shape, not a fork of the `proc` layer — but the slot-scarcity handling
(clear errors, and later a slot-multiplexing / most-recently-used policy) is the
real 8c-2 design work. A **software-breakpoint-via-single-step** fallback
(step-and-compare-PC, no text write — the classic HW-less debugger trick) is a
possible v1.x lift to lift the ceiling; deferred.

---

## 7. Launch, attach, and stop-at-entry

Two entry workflows, in order of tractability:

**`dlv attach <pid>` (8c-1 milestone — no new kernel surface).** Open the
running Go process's `/proc/<pid>/ctl`, `attach`, `stop`, enumerate its state.
This is the clean first target: it exercises attach + the full inspect surface
(regs/mem/kregs/kstack/goroutines/stacks) with zero launch complexity, and it is
already a superpower (§5.3 of the charter — attach to *any* process you have
rights to: netd, stratumd, a stuck shell).

**`dlv exec <bin>` / `dlv debug` (launch — 8c-1/8c-4).** Delve must gain control
*before* the Go runtime runs, to set breakpoints before `main.main`. Thylacine
has no "stop at exec/entry" primitive today. Two paths:

- **(a) attach-first + breakpoint-at-entry (the DECIDED 8c path, user-voted
  2026-07-16).** Spawn the child (`SYS_SPAWN`, stdio piped) → `attach`
  immediately (the pid is known from spawn) → `stop` (the child parks at its
  *first* EL0-return tail — the first syscall the runtime makes at startup,
  microseconds in, far before `main.main`) → `hwbreak <entry>` (or the DWARF
  entry) → `start` → the child runs to the entry breakpoint. This needs **no new
  kernel surface** and matches how Delve launches on backends without a
  stop-at-exec primitive. The only gap: a *trivial* program that reaches
  `main.main` between spawn and the stop landing (a genuine but narrow race;
  real programs do substantial runtime init first).
- **(b) a `SYS_SPAWN` "debug-stopped-at-birth" flag (the v-next closure).** A
  spawn variant that creates the child already debug-stopped before its first
  instruction, with the debugger's attach slot pre-claimed. This closes the
  race unconditionally but adds a **new kernel surface** (I-39-adjacent,
  audit-bearing). **Recorded, NOT in the 8c arc** — a small, well-scoped kernel
  follow-up if the entry race proves to bite in practice.

Path (a) keeps 8c a pure userspace port and delivers both `ambush attach` and
`ambush exec`.

`EntryPoint()` reads the ELF `e_entry`. Go's default build is **non-PIE**
(position-dependent) — the toolchain links at a fixed base and REVENANT maps
segments at their ELF virtual addresses, so there is no load slide to add
(unlike a PIE where Delve adds the ASLR base). **Assumption to verify at 8c-1:**
that Thylacine execs Go binaries non-relocated (no per-exec slide on the text
VA); if a slide is ever introduced, `EntryPoint`/`BinInfo` must add it, exactly
as Delve's Linux backend does for PIE.

---

## 8. The unified user→kernel stack (the §4 headline, via 8c)

Delve already walks the **user** stack (the Go `g`/frame model + the arm64 `x29`
fp-chain via `regs` + `mem` reads). 8c stitches the **kernel** half:

- A stopped thread's kernel frames come from `/proc/<pid>/kstack` (8b: the
  symbolized `func+offset` fp-chain — `sched → sleep → tsleep → …` down to the
  SVC entry vector).
- A thread *blocked deep in a syscall* (not debug-stoppable — it never reaches
  the EL0-return tail) is readable via the **8b settled-thread inspect**: the
  same `kstack` file, I-39-authorized, no debug-stop, showing exactly *why* the
  goroutine is hung in kernel terms.
- The stitch point is the SVC boundary: the user stack's outermost frame is the
  syscall trampoline; the kernel stack begins at `exception_sync_lower_el`. The
  Nora plugin (8e) renders them as one call stack; 8c exposes the kernel half to
  the DAP `stackTrace` response as synthetic frames (a `[kernel] func+off`
  frame with no source until kernel DWARF lands).

This is the feature no other production debugger gives natively (§4 of the
charter); 8c is where the dlv side first reads `kstack` and appends the kernel
frames.

---

## 9. Build + transport mechanics

- **Build tags.** Add `thylacine` to the native-backend build-tag matrix: new
  `proc_thylacine.go` / `threads_thylacine.go` / `threads_thylacine_arm64.go` /
  `registers_thylacine_arm64.go` (`//go:build thylacine`); ensure the existing
  `linux`/`darwin`/`windows` files exclude thylacine (they already tag on their
  own OS); ensure `nonative_*` does NOT match thylacine (so the native backend is
  selected). `cmd/dlv` wires the native backend for supported OSes — add
  thylacine.
- **Cross-build.** `GOOS=thylacine GOARCH=arm64 go build ./cmd/dlv` with the Go
  fork (`~/projects/go-thylacine`), exactly like the toolchain and the coreutils
  ports. Delve's imports (`os`, `os/exec`, `syscall`, `debug/dwarf`,
  `debug/elf`, `encoding/json`, networking) are all covered by the fork's
  `GOOS=thylacine` support (Stages 4-6).
- **Vendoring — an OUTSIDE-repo Delve fork, pool-baked (the Go-toolchain
  pattern; refined from the initial `usr/ambush/` guess by the 8c-1 probe, §14).**
  Delve is a large multi-package program whose binary is tens of MB — it cannot
  ride a ramfs blob, and its full tree would bloat the OS repo. So Ambush
  patterns with the **Go toolchain fork**, not the small in-`usr/` probes: the
  Delve fork lives OUTSIDE the thylacine repo (e.g. `~/projects/ambush`, like
  `~/projects/go-thylacine`), `tools/build.sh` cross-builds it with an
  `AMBUSHFORK` env override (mirroring `GOFORK`), and the `ambush` binary is
  **baked onto the Stratum pool** alongside the trimmed GOROOT (the
  `THYLACINE_BAKE_GOROOT` machinery). The fork's patch set is small: the sentinel
  build-tag exclusion + the `proc_thylacine` backend files + stripping the
  `x/telemetry` integration (§14).
- **DAP transport = stdio first.** `dlv dap` speaks DAP over stdin/stdout (no
  listener needed) — the clean transport for the Nora plugin (8e) and for
  in-guest E2E. A TCP listener over `/net` (`dlv dap --listen`) is available for
  remote debugging later (the network stack is live), but stdio is the v1.0
  path. The CLI REPL (`dlv attach`/`dlv exec` interactive) is the 8c-1..3 driver
  before any DAP client exists.
- **DWARF from the build.** Delve reads the debuggee's own `.debug_*` sections
  (Go emits DWARF by default; the toolchain builds it on-device). No new plumbing
  — the binary in the ramfs / on Stratum carries its DWARF, and Delve's
  `debug/dwarf` reader (portable Go) consumes it. This is why goroutine names,
  local variables, and source lines work for the *user* program at 8c with no
  extra kernel work; only *kernel*-frame source lines wait on kernel DWARF (§1).

---

## 10. Sub-chunk plan

Each sub-chunk is execution-verified in-guest (a `dlv` invocation against a real
Go program, the ground-truth-over-theory discipline the whole arc has used);
8c is a port, so the rigor floor is the in-guest E2E + a focused holotype at the
arc close (not per-sub-chunk kernel audits — the kernel is byte-unchanged).

- **8c-1 — scaffold + attach + inspect.** Vendor/patch Delve for
  `GOOS=thylacine`; the `proc_thylacine` backend skeleton (`Attach`, `Valid`,
  `Memory` via `mem`, `regs`/`fpregs`/`kregs` via `linutil`, `ThreadList` =
  head thread, `trapWait` via `wait`, `detach`). Milestone: `ambush attach
  <pid>` to a running Go program on-device → `goroutines`, `stack`, `regs`,
  `print <var>`, `bt` all work (read-only inspect). Verify the non-PIE entry
  assumption. In-guest E2E: a `usr/ambush-probe` that spawns a known Go child and
  `ambush attach`es it, asserting a known variable/frame.
- **8c-2 — breakpoints + continue + step (the HW-routing).**
  `WriteBreakpoint`/`EraseBreakpoint` → `hwbreak`/`hwrmbreak` (the §6
  HW-only routing + the slot-ceiling error handling); `ContinueOnce` → `start`
  + `wait`; `StepInstruction` → `step`; `findHardwareBreakpoint` (post-stop
  PC-match). Watchpoints → `hwwatch`/`hwrmwatch`. Milestone: `break main.main;
  continue; step; print; continue` drives a Go program to a breakpoint and
  single-steps it. Weigh the `DEBUG_HWBP_SLOTS` 4→6 kernel clamp lift here.
- **8c-3 — the goroutine / scheduler model + the unified stack.** Confirm
  Delve's goroutine walk works over the debug-fs (it reads runtime structures
  from `mem` + the `g` from `tpidr_el0` — should be free once 8c-1 lands
  `TLS`/`GAddr`); append the kernel frames from `/proc/<pid>/kstack` to the
  stack-trace (§8), including the 8b settled-thread inspect for a
  blocked-in-kernel goroutine. Milestone: `goroutines -with running`, `goroutine
  <n> bt` shows the Go frames → the SVC boundary → the kernel frames.
- **8c-4 — launch + the DAP server.** `ambush exec`/`ambush debug` (the §7(a)
  attach-first + bp-at-entry launch); `ambush dap` over stdio drives the same
  backend through the DAP protocol (the Nora-plugin transport). Milestone: a DAP
  `launch` → `setBreakpoints` → `configurationDone` → `stackTrace` →
  `variables` → `continue` round-trip against a Go program, over stdio. The
  arc-close focused holotype (the backend's debug-fs usage: fd lifetimes, the
  slot ceiling, attach/detach composition with target death, no cross-Proc leak)
  + the in-guest E2E + docs/reference.

The v1.x seams (per-thread `/proc/<pid>/thread/<tid>/`, the spawn-stopped
primitive §7(b), the software-bp-via-single-step ceiling lift, core-dump /
`DumpProcessNotes`, call injection, remote DAP over `/net`) are recorded, not
built, at 8c.

---

## 11. Naming — **Ambush** (user-voted 2026-07-16)

The debugger is named **Ambush** — the breakpoint-as-trap: it lies in wait and
takes its quarry at the chosen instant, even across the kernel boundary (the
apex-predator instinct made literal, `GO-IDE-DESIGN §10`). The name applies to
the whole debugger capability (the `proc_thylacine`-backed dlv port + the 8f
Nora/Kaua debug UI), not only the UI.

Realization: the on-disk binary is **`ambush`** — Thylacine's Go debugger, a
**Delve (`dlv`) port** under the hood. Its CLI **mirrors dlv's verbatim**
(`ambush attach <pid>`, `ambush exec <bin>`, `ambush debug`, `ambush dap`,
`ambush trace`) so a user who knows Delve transfers directly, and the DAP/JSON
protocol is byte-identical (the Nora plugin at 8e is a standard DAP client). The
`proc_thylacine` backend is the only genuinely new code; everything above it is
the stock Delve port. The kernel debug surface (8a/8b) keeps its Plan 9 verbs
(`stop`/`start`/`step`/`attach`/`detach`, `mem`/`regs`/`wait`) — descriptive,
lineage-correct, unchanged; Ambush is the userspace debugger that drives them.

Vendored as an outside-repo Delve fork, cross-built `GOOS=thylacine
GOARCH=arm64` and pool-baked like the Go toolchain (§9, refined by the 8c-1
probe §14 — Delve is too large for the in-`usr/` module pattern).

---

## 12. Signoff items — RESOLVED

1. **Naming (§11)** — RESOLVED: the debugger is **Ambush** (user-voted
   2026-07-16), a Delve port; binary `ambush`, CLI mirrors `dlv`.
2. **Launch strategy (§7)** — RESOLVED: **attach-first + bp-at-entry**
   (user-voted 2026-07-16); 8c stays a pure userspace port. The `SYS_SPAWN`
   debug-stopped-at-birth flag is the recorded v-next closure if the entry race
   proves to bite in practice (a small, well-scoped kernel follow-up, not in
   the 8c arc).

Reported-at-implementation calls (mine to make + surface, not block on):

- **Delve vendoring mechanics** — the `usr/ambush/` fork layout + the patch set
  scope (a 8c-1 mechanics call).
- **HW breakpoint slot ceiling (§6)** — ship at the v1.0 clamp (4 code / 4
  data) with clear "out of slots" errors; weigh the 4→6 kernel-clamp lift + a
  slot-mux policy at 8c-2 (an ergonomics call, reported at 8c-2).

---

## 13. References

- `docs/GO-IDE-DESIGN.md` (the charter; §8 the staged plan; §3.2 dlv-as-port;
  §4 the unified stack; §10 naming).
- `docs/DEBUG-FS-DESIGN.md` + `docs/reference/134-debug-fs.md` (the as-built
  kernel debug-fs — the primitives 8c binds to; 8a-1 + 8a-2 + 8b).
- ARCH §28 **I-39** (debug authority bounded — the invariant 8c consumes, adds
  none), §25.4 debug-fs row.
- `docs/GO-PORT-PLAN.md` (the toolchain arc — the cross-build mechanics 8c
  reuses).
- Delve v1.25.2 `pkg/proc/{interface,threads,registers}.go` +
  `pkg/proc/native/{proc,threads,ptrace,registers}_linux*.go` (the backend
  contract + the OS-factored surface `proc_thylacine` fills).
- `docs/NOVEL.md` angle #13 (the capability-scoped cross-boundary debugger).

---

## 14. 8c-1 feasibility probe (ground truth, 2026-07-16)

A cross-build of stock Delve v1.25.2 for `GOOS=thylacine GOARCH=arm64
CGO_ENABLED=0` with the Go fork (`~/projects/go-thylacine`, go1.25.3),
de-risking the port before writing the backend. Results:

- **The mechanism is confirmed.** Delve's `pkg/proc/native` has a build sentinel
  (`support_sentinel.go`, `//go:build !linux && !darwin && !windows && !freebsd`
  → `package your_operating_system_is_not_supported_by_delve`) that collides with
  `package native` for any unlisted OS. Adding `&& !thylacine` to that tag
  resolves the collision — the single line the port needs there. The `_linux*.go`
  backend files are Linux-only by **filename convention** (no explicit tags), so
  the port adds parallel `*_thylacine*.go` files. The shared files
  `proc.go` / `proc_unix.go` / `threads.go` / `followexec_other.go` already
  select on thylacine — they port for free.
- **Two dependency gaps sit above the backend, both tractable:**
  - **`os/user` (the FORK) — FIXED + committed** (`go-thylacine` `7242ba7`). The
    `!cgo` `lookup_stubs.go` path thylacine takes provides `currentUID`/
    `currentGID` (so `user.Current()` works from the kernel uid/gid), but the
    `lookupUser`/`lookupUserId`/`lookupGroup`/`lookupGroupId`/`listGroups` family
    lived only in `lookup_unix.go` (thylacine isn't in the `unix` tag set) → a new
    `lookup_thylacine.go` stubs the five (no `/etc/passwd`, the Plan 9 shape).
    `os/user` now builds green for thylacine.
  - **`x/telemetry/internal/mmap` (a dep) — strip in the Ambush fork.** Delve
    imports `golang.org/x/telemetry` in exactly two places (`cmd/dlv/main.go`
    `telemetry.Start(...)` + `pkg/logflags` `counter.NewStack("delve/bug", 16)`);
    the dep's `internal/mmap` has no thylacine file. A debugger does not need Go
    usage analytics — the fork strips both sites (trivial), removing the dep
    entirely.
- **The backend contract is the designed set.** Behind those deps, the
  `_thylacine*.go` fill-list is the `_linux*.go` surface: the `osProcessDetails`/
  thread-`osSpecificDetails` types + `Launch`/`Attach`/`initialize`/`kill`/
  `requestManualStop`/`addThread`/`updateThreadList`/`trapWait`/`wait`/`resume`/
  `stop`/`detach`/`EntryPoint` + `ReadMemory`/`WriteMemory`/`singleStep` +
  `registers`/`setPC`/`SetReg`/`fpRegisters` + the `writeHardwareBreakpoint`/
  `clearHardwareBreakpoint`/`findHardwareBreakpoint`/`getWatchpoints`/
  `setWatchpoints` family + the unsupported `SetUProbe`/`FollowExec`/dump stubs —
  each a thin `pread`/`pwrite`/ctl-write over the debug-fs, wearing the reused
  `linutil.ARM64Registers` decode.
- **Vendoring confirmed (§9):** Delve's binary is tens of MB → outside-repo fork
  + pool-bake (the Go-toolchain pattern), not `usr/ambush/`.

Remaining 8c-1 work: set up the outside-repo Ambush fork (Delve v1.25.2 + the
sentinel tag + the telemetry strip + the `proc_thylacine` backend), the
`tools/build.sh` cross-build + pool-bake integration, and the on-device
`ambush attach <pid>` E2E (`usr/ambush-probe`).
