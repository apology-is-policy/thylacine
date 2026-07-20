# GO-IDE-DESIGN — Thylacine as a self-hosting Go development environment

**Status: CHARTER (ratified 2026-06-23, user-directed). Scripture, no code.**
This document is the binding design charter for extending the Go arc
(`docs/GO-PORT-PLAN.md`) past the on-device toolchain into a complete,
by-default, in-OS Go IDE with a capability-scoped, namespace-native,
cross-boundary visual debugger. Per the spec-to-code suspension this is a
prose-validated arc; per-stage design passes (each new kernel surface gets its
own focused design + audit) refine it. The time-travel / record-replay angle is
explicitly **deferred to a flagged NOVEL-stretch** (section 9), built only after
the core IDE is solid.

---

## 1. The ratified vision

A user opens **Nora** (the native Kaua-based editor), writes a Go program, and —
without leaving the OS — gets a **fully-fledged Go IDE**, shipped **by default**:

- Editing intelligence (completion / go-to-definition / find-references / hover /
  rename / live diagnostics / format-on-save) — **gopls**.
- **Visual debugging** in the Kaua TUI: source pane with breakpoint gutters +
  current-line highlight, a variable-inspection tree, a call-stack pane.
- **Stack-trace inspection including kernel symbols** — a unified user->kernel
  stack (section 4), the headline feature.
- **Stack-trace navigation**: select any frame and inspect that frame's variables.
- **Build + interactive run** from inside Nora: a bottom pane (the "ut pane")
  that is a real interactive console for the running program — stdin works,
  stdout/stderr stream, build output and the debugger REPL are addressable.

To this ratified core the charter adds the dimensions that make it singular to
Thylacine (sections 5-8) — and one moonshot held for later (section 9).

---

## 2. Why this is a *Thylacine* flex (not "an IDE on a hobby OS")

The thesis is precise: **we own the entire stack, and everything is a file.** A
debugger on Linux hits the syscall boundary and shows "[in kernel]" — a wall. On
Thylacine the kernel, the scheduler, the namespace, the filesystem, and the
symbol tables are all ours and all addressable, so the debugger crosses
boundaries that are walls everywhere else. That single fact is the source of
every superpower below.

It is also *tractable* — which is why Go is the first toolchain (GO-PORT-PLAN
5.0.1): Go is self-contained (pure-Go `compile`/`asm`/`link`, no LLVM, no C dep
with CGO off), `dlv` (Delve, the standard Go debugger) and `gopls` are both Go
programs that port like the toolchain, and the runtime is already proven in-VM.
So the IDE is mostly *assembly*; the genuinely new build is the kernel debug
surface.

---

## 3. Architecture — three layers

### 3.1 Kernel: the Plan 9 `/proc` debug filesystem + arm64 hardware debug

Debugging the Plan 9 way is *files*. We already have `/proc/<pid>/ctl` (kill /
killgrp, gated by the **I-26** two-axis owner-or-`CAP_KILL` check) and
`/proc/<pid>/ns` (66b). The debug surface *extends an existing one*:

- `/proc/<pid>/mem` — read/write the (stopped) target's user memory (via the
  target's page tables; the kernel already maps cross-Proc for exec).
- `/proc/<pid>/regs`, `/fpregs` — read/write the saved register frame (the
  `exception_context` we already keep per thread).
- `/proc/<pid>/ctl` — debug verbs: `stop` / `start` / `step` / `waitstop` /
  `hwbreak <addr>` / `hwwatch <addr>`.
- `/proc/<pid>/wait` — block until the target stops at a trap.

**Hardware breakpoints, not software.** arm64 has dedicated breakpoint /
watchpoint debug registers (`DBGBVR`/`DBGBCR`, `DBGWVR`/`DBGWCR`) and an
`MDSCR_EL1.SS` single-step bit. We use them, *not* a software `BRK` patched into
text, because a software breakpoint would fight two invariants: **I-12 (W^X)** —
no writing into RO+X text — and the **REVENANT shared Image cache (I-36)** — a
breakpoint written into one debugee's text page would corrupt the page shared
with every other process running that binary. Hardware breakpoints touch neither.
(arm64 has 6 breakpoints + 4 watchpoints minimum; a debugger session multiplexes
them.)

### 3.2 Userspace: the Go debugger + language server (ports)

- **`dlv` (Delve)** with a new `proc_thylacine` backend over the `/proc`
  debug-fs (Delve's native backend is OS-specific; thylacine has none upstream).
  Delve is Go-aware: goroutines, the scheduler, the calling convention, defers.
- **`gopls`** — the editing intelligence. Pure Go, ports like the toolchain.
- Both speak standard protocols: Delve speaks **DAP** (Debug Adapter Protocol);
  gopls speaks **LSP**. Using the standards means the Nora side is reusable for
  any future language and we invent no protocol.

### 3.3 The IDE: Nora plugin architecture + the Kaua debug UI

- A **Nora plugin architecture** (a real extension API); the **Go plugin** is its
  first and reference implementation, shipped by default.
- The plugin is a **DAP client** + an **LSP client**, driving `dlv` and `gopls`.
- The **Kaua debug UI**: source / variable-tree / call-stack / goroutines / watch
  panes + the bottom **run-pane** (a real interactive console over the
  cons/consctl PTY infra from LS-8; rebuild-on-save is instant because the
  toolchain is local).

The **focused UX design is `docs/NORA-IDE-UX.md`** (Stage 8e/8f). Ratified there
(user-voted 2026-07-20): the layout is the **IDE dashboard** (editor center +
right sidebar [Variables/Call-Stack/Goroutines] + a bottom Console, collapsible
when not debugging); the interaction extends Nora's modal + `[Space]`-menu + `:`
idiom; the load-bearing decision is the **async client architecture** (persistent
gopls/Ambush over a `PollSet`-multiplexed loop with JSON-RPC framing — the
one-shot-filter subprocess model would deadlock a persistent server); and the
debug UI **grows the Kaua widget registry** (`Tree`/`Table`/`Tabs`/`Scrollbar`).

---

## 4. The headline feature — cross-boundary unified stacks

The ratified "kernel symbols in stack traces" is made first-class: a single
**user->kernel call stack**. A goroutine blocked in a channel recv shows its Go
frames -> the SVC trampoline -> `sched.c::sleep` -> the exact rendez it is parked
on, every frame symbolized via the **Halls of Extinction symtab (HX-2)** that
already does `func+offset`. Select a kernel frame: see *why* it is blocked, in
kernel terms; where DWARF is shipped, its locals. No production OS gives this in
an ordinary debugger (you need kgdb + a second debugger + manual correlation). We
get it natively because the per-thread saved kernel frame and the kernel symtab
are already present, and the Halls fp-chain backtrace logic (built for the
dying-machine dump) adapts to a live stopped thread.

---

## 5. The superpowers — what only Thylacine can do (the NOVEL surface)

1. **Namespace / resource inspector.** Debug the process's whole *world*, not
   just its memory: its namespace (`/proc/ns`), its fds and *what each points to*
   (`/proc/fd`, #66c), mounts, caps, hardware allowance, Loom rings, Weft flows.
   "Goroutine 7 blocked on fd 5" expands to "fd 5 = `/net/tcp/3/data` = a Weft
   flow to 10.0.2.2:443, 0 bytes buffered." You debug the *resource*, traced
   through the namespace. gdb fundamentally cannot show this.
2. **Scheduler / concurrency view.** All goroutines x their Ms x the CPUs those
   Ms occupy x the kernel run-queues, correlated across the boundary because we
   own EEVDF. A deadlock becomes a graph. Goroutine leaks, M-starvation, priority
   inversion — visible.
3. **System-wide, capability-scoped attach.** Debugging is mounting
   `/proc/<pid>`, so the IDE attaches to *any process you have rights to* — netd,
   stratumd, corvus, a stuck shell — not only what it launched. Secure by
   construction (section 6).
4. **Kernel-aware crash post-mortem.** On a `snare:segv`, capture the core + the
   FS state + the unified user/kernel stack and open the *dead* process like a
   live session — variables, frames, the kernel context at the fault. Wires
   straight into the Halls of Extinction infra, extended down to userspace.
5. **Snapshot-debugging via Stratum.** Copy-on-write snapshots give a **debug
   checkpoint**: at a breakpoint, snapshot the whole FS + process state — branch
   the session, or diff "the FS at the bug" vs "now." Reproducible, shareable
   debugging states, exploiting a filesystem feature no debugger normally reaches.

---

## 6. The invariant this introduces (debug authority is bounded)

Reading/writing another process's memory and registers, and controlling its
execution, is a **privilege surface** — it gets a load-bearing invariant, now
**I-39** in ARCH §28 (finalized 2026-07-14 in `docs/DEBUG-FS-DESIGN.md §3`;
provisional text below):

> **Debug authority is exactly namespace- + capability-bounded, and never
> bypasses memory-safety.** A Proc may debug a target iff it can *name*
> `/proc/<pid>` in its namespace AND passes the I-26-class two-axis gate (owner
> identity OR a debug capability, e.g. `CAP_DEBUG`); the debug-fs confers no
> authority beyond what that gate grants. No debug operation writes executable
> text (I-12 holds — breakpoints are hardware), corrupts the shared Image cache
> (I-36 holds), or reads/writes memory outside the target's own address space.
> Register/memory access is only permitted on a *stopped* target. The gate is
> the *same* security model that already governs `/proc/<pid>/ctl` kill — debug
> is one more verb on a surface whose authority is already capability-scoped.

This is the crux of "secure by construction": the namespace + capabilities that
bound everything else on Thylacine bound the debugger too. You can debug exactly
what your rights permit — and nothing else — with no special-case debugger
privilege.

---

## 7. Load-bearing decisions (locked)

- **Hardware breakpoints / watchpoints / single-step**, never software `BRK` —
  preserves I-12 (W^X) and I-36 (the shared Image cache). (Section 3.1.)
- **Standard protocols** — DAP (debug) + LSP (editing). No invented protocol; the
  Nora side generalizes to future languages.
- **Capability-scoped via the I-26 model** — debug is a verb on the existing
  capability-gated `/proc/<pid>` surface (section 6).
- **The Plan 9 `/proc` debug-fs idiom** — debugging-as-files, namespace-native,
  the lineage-correct shape (Plan 9's `acid` debugs exactly this way).
- **gopls + dlv as ports**, driven over their standard protocols — not
  reimplemented.

---

## 8. The staged plan (GO-PORT-PLAN Stage 8 — the IDE/debugger arc)

Sequenced AFTER the toolchain (Stages 4-6) lands. Each kernel sub-stage opens
with its own focused design pass + audit (it is a new privilege surface).

- **8a** — kernel `/proc/<pid>/{mem,regs,ctl-debug-verbs,wait}` debug-fs +
  arm64 hardware breakpoints / watchpoints / `MDSCR_EL1.SS` single-step + the
  I-26-class debug gate (the new invariant, section 6). **DESIGN LANDED
  2026-07-14 (user-voted) — the focused pass is `docs/DEBUG-FS-DESIGN.md`.** Four
  ratified decisions: `CAP_DEBUG` is clearance-grantable elevation-only (bit
  `1<<10`, corvus-gated legate, `rfork`-stripped, zero new devcap.c); the stop is
  handle-lifetime-tied (the open debug ctl fd owns it — close/detach/debugger-death
  resumes, strand-freedom from #68 close-at-exit); **spec-first is RE-ENABLED**
  (`specs/debug_stop.tla`, model-first); and 8a splits into **8a-1** (the
  software-checkpoint tier — debug-fs + I-39 gate + stop-at-the-EL0-return-tail +
  cross-Proc mem/regs + the unified stack walk, ZERO debug registers) then
  **8a-2** (arm64 HW debug + EC routing + single-step + per-thread register
  install, gated behind the audited 8a-1 and the empirical HVF verify —
  confirmed feasible: guest self-hosted EL0 debug works under HVF QEMU>=8.2, TCG
  fallback).
- **8b** — cross-boundary unified stack walk + ship kernel DWARF (section 4).
- **8c** — **Ambush** (§10), a `dlv` `proc_thylacine` backend; drives a Go
  program on-device (CLI first — breakpoints, step, inspect — before any UI).
  **DESIGN LANDED 2026-07-16 — the focused pass is `docs/DELVE-PORT-DESIGN.md`.** A userspace
  port (Delve cross-builds `GOOS=thylacine`, no new kernel surface): the backend
  is ~4-5 build-tagged Go files that wrap the debug-fs (`ctl`/`mem`/`regs`/
  `fpregs`/`kregs`/`kstack`/`wait`), reusing Delve's `linutil.ARM64Registers`
  verbatim because `t_user_regs` *is* Linux `user_pt_regs`. The load-bearing
  decision: **all breakpoints route to the arm64 HW path** (I-12 W^X + I-36
  forbid a software `BRK` into shared text), giving a hard ceiling of the debug
  registers (4 code + 4 data at the v1.0 clamp) — the defining ergonomic
  difference from `dlv` on Linux. Sub-chunks 8c-1 (attach + inspect) → 8c-2
  (HW breakpoints + step + continue) → 8c-3 (goroutines + the unified user→
  kernel stack via `kstack`) → 8c-4 (launch + the DAP server over stdio).
- **8d** — `gopls` port (the editing-intelligence half). **COMPLETE 2026-07-18 —
  the focused pass is `docs/GOPLS-PORT-DESIGN.md`; as-built reference is
  `docs/reference/137-gopls.md`.** A userspace port that needs **no kernel
  surface** (unlike 8c): gopls is pure Go over file I/O + the on-device `go`
  toolchain (`go/packages`) + LSP over stdio (no `/net`). The cross-build
  (`GOOS=thylacine`) is GREEN with just **two** vendored build-fallback shims
  (telemetry-mmap `io.ReadAll` + robustio `getFileID` via the 9P qid.path) —
  thylacine is not in Go's `unix` build tag. Fork base = gopls v0.21.1 (the
  newest whose go.mod `go` directive is `1.25`). Sub-chunks 8d-1 (cross-build +
  vendor + `build_gopls`) → 8d-2 (the on-device engine E2E via the offline CLI
  subcommands `gopls check`/`definition` over `/goroot`) → 8d-3 (arc close: the
  env-requirement chain [`CAP_CSPRNG_READ` for crypto/rand + `PATH` for the `go`
  lookup + os.Executable — all login-provided], the telemetry-disable fork fix
  [private-OS posture: no phone-home; retires the os.Executable-dependent
  sidecar], the deterministic transport-free joey boot probe `joey: go8d OK`
  [gopls check + definition, boot-fatal], + the lean go8d.exp console leg). The
  Nora LSP client is 8e; the Kaua editing UI is 8f.
- **8e** — the Nora plugin architecture + the Go plugin (DAP + LSP clients).
  **UX design LANDED 2026-07-20 — `docs/NORA-IDE-UX.md`.** The load-bearing
  sub-chunk is **8e-1** (the async client substrate: persistent gopls/Ambush
  children + the `PollSet`-multiplexed loop generalizing `PollSource` + a
  JSON-RPC codec + dirty-on-event), then **8e-2** (the LSP client, inline in the
  editor) and **8e-3** (the DAP client, headless from `:`). The `MENU`/`COMMANDS`
  const tables become an extensible registry (the plugin architecture).
- **8f** — the Kaua debug UI (source / vars / stack / goroutines / watch) + the
  interactive run-pane. Per `docs/NORA-IDE-UX.md`: **8f-1** the Kaua widget
  registry additions (`Tree`/`Table`/`Tabs`/`Scrollbar`, pure + unit-tested) →
  **8f-2** the dashboard (layout + collapse + focus + the sidebar tiles + the
  Console `Tabs` [Program pts / Debug REPL]) wired to the DAP client state →
  **8f-3** polish (the cross-boundary `── kernel ──` stack, inline values, LSP
  affordances, the Bonfire pass).
- **8g** — the superpowers (section 5): namespace/resource inspector, scheduler
  view, capability-scoped system-wide attach, kernel-aware post-mortem, Stratum
  snapshot-debugging.
- **8h** — whole-arc focused audit (the debug-authority invariant is the
  centerpiece) + SMP gate + by-default shipping + docs.

The lighter debugging tier (panic traces, `go tool trace`, offline `pprof`)
lands *for free* with the toolchain (Stages 4-6) — no new kernel surface.

---

## 9. Deferred moonshot — OS-native time-travel (flagged NOVEL-stretch)

Delve has reverse-step via Mozilla `rr` (perf-counters + ptrace; fragile). We own
the scheduler, the syscall ABI, the CSPRNG, and the clock — *every* source of
nondeterminism — so we could record those and **replay deterministically**,
giving the Go debugger a true reverse-step that is OS-native, not bolted-on.
Stepping *backwards* through a Go program on your own OS is the single biggest
flex. It is a research-grade kernel arc of its own and is **deferred** (user-
voted 2026-06-23): the core IDE (sections 1-8) is built first; time-travel is
earned afterward, as **Stage 9** + a NOVEL entry of its own.

---

## 10. Naming — the debugger is **Ambush** (signed off 2026-07-16)

The debugger is the apex predator's instinct made literal: it **tracks** a
running program by the **spoor** the OS already leaves (Plan 9 file handles are
already named `Spoor`), and a breakpoint is an **ambush** — it lies in wait and
takes its quarry at the chosen instant, even across the kernel boundary.

**Ambush** is the debugger (the whole dlv-backed capability + the 8f Nora/Kaua
debug UI), signed off at Stage 8c (`docs/DELVE-PORT-DESIGN.md §11`). The on-disk
binary is `ambush` — a **Delve (`dlv`) port** whose CLI mirrors dlv's
(`ambush attach`/`exec`/`debug`/`dap`), so Delve knowledge transfers directly
and the DAP protocol is standard. The kernel debug surface (8a/8b) keeps its
descriptive Plan 9 verbs (`stop`/`start`/`step`/`attach`/`detach`,
`mem`/`regs`/`wait`); Ambush is the userspace debugger that drives them. (Alt
candidate **Vigil** — keeping watch over the quarry — not chosen.)

The IDE itself needs no new name — it is *Nora's Go plugin* (the first plugin of
the Nora extension architecture).

---

## 11. NOVEL framing

This is NOVEL angle **#13** (`docs/NOVEL.md`): a **capability-scoped,
namespace-native, cross-boundary visual debugger** — one that inspects a
program's memory *and its OS reality* (fds -> files -> flows, the scheduler, the
namespace), across the user/kernel wall, bounded by the same namespace +
capabilities that bound everything else, with the editing intelligence (gopls)
and visual UI (Nora/Kaua) to drive it. No other OS offers this natively, because
no other OS owns its whole stack the way Thylacine does. The time-travel angle
(section 9) is a separate, deferred NOVEL.

---

## 12. References

- `docs/GO-PORT-PLAN.md` (the toolchain arc; 5.0.1 the locked decisions; Stage 8
  = this arc).
- `docs/KAUA.md` (the TUI substrate the debug UI is built on).
- `docs/reference/101-halls.md` (the symtab + fp-chain backtrace reused for
  cross-boundary stacks).
- ARCH §9.4 (`/proc`), §28 I-26 (cross-Proc control two-axis), I-12 (W^X), I-36
  (REVENANT Image cache).
- `docs/NOVEL.md` angle #13.
