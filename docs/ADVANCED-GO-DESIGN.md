# ADVANCED-GO-DESIGN — debugger maturation + kernel-up software breakpoints

**Status:** signed off 2026-07-22. This is the successor arc to the **Go IDE arc**
(GO-PORT-PLAN Stage 8, `docs/GO-IDE-DESIGN.md`), which is hereby marked **COMPLETE**
(§1). Advanced Go carries the Stage-8 remainder + the headline new work: **proper,
Linux-parity software breakpoints, built from the kernel up.**

Binding. Implementation deviations update this doc first (design-first policy).
The software-breakpoint mechanism (§3) opens with its own focused design + audit
(it is a new privilege + W^X-adjacent kernel surface) and is **spec-first** for the
one real concurrency (§3.8).

---

## 1. The Go IDE arc (Stage 8) is COMPLETE

The foundational Go development environment is built and shipping on-device:

| Stage | What | Status |
|---|---|---|
| 8a | `/proc/<pid>` debug-fs + I-39 gate + arm64 HW breakpoints/watchpoints/single-step | LANDED |
| 8b-1 | settled-thread kernel-stack inspect + the cross-boundary unified stack | LANDED |
| 8c | **Ambush** (the `dlv` `proc_thylacine` backend) + the DAP server | LANDED |
| 8d | **gopls** port (the LSP / editing-intelligence half) | COMPLETE |
| 8e | the Nora plugin architecture + the DAP/LSP clients (`parley`) | LANDED |
| 8f-1/2 | the Kaua debug dashboard (Variables / Call Stack / Goroutines / Console) | LANDED |
| 8f-3a/3b | the cross-boundary `── kernel ──` stack divider (Go frames → kernel frames) | LANDED |

A Go program is edited, type-checked, built, launched, breakpointed, stepped, and
inspected across the kernel boundary — entirely on Thylacine. **The Stage-8
*remainder* (8f-3 polish, 8g superpowers, 8h audit) rolls into this arc** rather
than blocking the milestone.

The defining Stage-8 limitation — **every breakpoint routes to one of the CPU's
~6 hardware debug registers** (I-12 W^X + I-36 REVENANT shared code forbid a
software `BRK` into shared text) — is exactly what Advanced Go removes.

---

## 2. The arc

| Sub-arc | Scope |
|---|---|
| **AG-1** | **single-step-`next`** — the interim F10 unblock (dlv-only; §4). Obsoleted by AG-2 but ships now. |
| **AG-2** | **software breakpoints** — the headline; kernel-up (§3). |
| **AG-3** | 8f-3 remainder — inline gutter values, LSP editor affordances, the Bonfire polish pass. |
| **AG-4** | 8g superpowers — namespace/resource inspector, scheduler view, capability-scoped system-wide attach, kernel-aware post-mortem, Stratum snapshot-debugging. |
| **AG-5** | loose ends — goroutine-accurate `kstack` (the 8c-3 v1.x note), kernel-DWARF finish. |
| **AG-6** | the whole-arc focused audit (I-39 + the AG-2 invariant) + SMP gate + ship-by-default + docs. |

Stage 9 (OS-native time-travel) stays the **separate deferred capstone** — the
natural successor *after* Advanced Go, not folded in (§6).

---

## 3. AG-2 — software breakpoints (the headline)

### 3.1 The problem

arm64 exposes ~6 hardware breakpoint registers (`num_brps`, the architectural max is
16; QEMU-max + Apple M2 implement 6). On Thylacine every breakpoint must use one,
because W^X (I-12) + the shared file-backed REVENANT Image cache (I-36) forbid
writing a `BRK` into text. Delve's `next` (step-over) plants a temporary breakpoint
at *every successor PC of the current line + the return address*; a `range` loop
alone exceeds 6, so the overflow `hwbreak` returns `-1` → EPERM → the step-over
aborts. Conditional breakpoints, many user breakpoints, and breakpoint-on-return all
hit the same wall. **The fix is unlimited software breakpoints — which mainstream
OSes provide *under W^X*, so the constraint is not fundamental (§3.2).**

### 3.2 The SOTA synthesis (the two-pass survey, §7)

Software breakpoints coexist with W^X on every peer, by the **same trick**: the trap
byte is written by a **kernel-mediated path that forces a private copy-on-write copy
of the code page.** The user PTE is never simultaneously writable+executable; the
shared original stays pristine.

- **Linux** — `PTRACE_POKETEXT` → `FOLL_FORCE` write → `do_wp_page` COW → a private
  patched anon page; `copy_to_user_page` does the I-cache maintenance.
- **Fuchsia/Zircon** — `zx_process_write_memory` (RFC-0159) *ignores the page's W^X
  protection and gates only on the process handle's debug rights*; the write forks a
  private COW page. Debugger-first exception delivery (fix-and-continue).
- **macOS/Apple-Silicon** (hardened-runtime W^X) — `mach_vm_protect(VM_PROT_COPY)`
  (a Mach shadow-object COW) + write, gated on the debugger entitlement.
- **Plan 9 (our heritage)** — `acid` writes the trap into `/proc/n/mem`; text
  segments are shared read-only (`SG_RONLY`), so the mem-write path privatizes the
  page; the *restore* byte is read from the pristine `text` file, never `mem`.
- **seL4** — HW-debug-registers ONLY (zero code patching); the lesson is that a
  code-mutation debug path lives *outside* the verified core, so gate it tightly.
- **Genode** — copies the whole ROM dataspace into RAM up front (the heavy
  whole-segment alternative we reject).

**We already own every primitive:** COW (lazy-anon + file-backed demand paging), the
transient **RW-not-X** patch alias (`arch/arm64/mmu.c::mmu_patch_text` + the W1.5 LSE
patcher — never a W+X PTE, even momentarily), and `arch_icache_sync_range`.

### 3.3 Authority — I-39, unchanged

Every capability peer converges on **"debug = a capability that names the target"**:
seL4 (a TCB cap), Zircon (a task handle + job scope), Mach/Hurd (the task port),
Plan 9 (`/proc` file permission + host-owner, local-only). That is **exactly our
I-39 two-axis gate** (owner OR `CAP_DEBUG`/`CAP_HOSTOWNER`; kproc + `NOTRACE`
refused). No new authority model. We add only Plan 9's **"write to a *stopped*
process only"** rule — which the debug-fs already enforces (the fully-stopped gate).

### 3.4 The write path — COW-break-and-patch (fork 2: the Plan 9 mem idiom)

**Decision (fork 2): a write to a code VA through the debug-fs `mem` surface
COW-breaks and patches** — the Plan 9 / Zircon idiom — rather than a dedicated
`swbreak` verb. This is minimal on the Ambush side (dlv's *generic* software-
breakpoint path is a `mem` write, exactly like `PTRACE_POKETEXT`), and it is the
heritage shape. Mechanism, for a write landing on a shared file-backed Image page in
a fully-stopped, I-39-authorized target:

1. **COW-break just that one page** into a per-Proc private anon page (copy the
   pristine bytes; the shared Image page + every other Proc on that binary stay
   untouched — the load-bearing correctness point unique to us).
2. **Patch through the transient RW-not-X alias** (`mmu_patch_text` shape): map the
   private page RW+PXN+UXN at a scratch VA, write the bytes, unmap. The target's own
   PTE stays RO+X throughout — **no W+X window, I-12 preserved.**
3. **I-cache maintenance** (`arch_icache_sync_range`: `dc cvau` on the alias / `ic
   ivau` on the canonical VA / `dsb ish` / `isb`) — mandatory, or the core fetches
   stale bytes and the breakpoint fires intermittently.
4. **Remap** the target's code VA to the private page RO+X.

**Restore is dlv-side, from the pristine source** (the Plan 9 discipline): dlv saves
the original instruction bytes before patching and writes them back to remove a
breakpoint. The kernel patches whatever dlv writes; it need not track originals.

The `BRK` trap (**EC 0x3C** from EL0) routes to the existing debug-stop delivery,
mirroring the EC-0x30 HW-breakpoint path.

### 3.5 The step-over — in-place, all-stop-safe (fork 3; the de-risking)

The research's biggest finding: **we do NOT need displaced (out-of-line) stepping or
the arm64 instruction-simulation tables.** That machinery (gdb displaced stepping /
Linux kprobes XOL + the PC-relative `ADR`/`ADRP`/`B`/`CBZ`/`LDR`-literal simulators)
exists to solve the step-over race in **non-stop** mode — a peer thread running past
a momentarily-removed breakpoint. **Two facts make it unnecessary for us:**

- Our debugger is **all-stop** (whole-Proc-stop; every thread parked off-cpu at the
  EL0-return checkpoint). During a step-over, no peer of the target runs.
- The COW-break **isolates the breakpoint to the debugged Proc** — other Procs on the
  same binary execute the pristine shared Image page, so they never trap.

So dlv's **generic in-place remove-step-reinsert** (restore the original byte →
single-step with our existing `step` verb → re-insert the `BRK`, all before the
general resume) is safe, and in-place stepping is **PC-correct** (the instruction
runs at its real address → no simulation). **Decision (fork 3): in-place for v1;
displaced stepping + the sim tables are a documented v-next, needed only if we ever
add non-stop mode.**

**Edge case (shared by both approaches):** a `BRK` cannot sit on a load/store
**exclusive** (`LDXR`/`STXR`) — any step/trap between the pair clears the local
monitor, so the `STXR` never succeeds. The SW-breakpoint path **rejects** a code
write that would land a trap on an exclusive (fall back to a HW breakpoint there, or
the next statement boundary). Exception-generating / `MSR`/`MRS` system-register /
`HINT`-space instructions are the same class.

### 3.6 Ambush simplifies

Ambush's Stage-8 `proc_thylacine` backend **forces every breakpoint to the HW path**
(`threads_thylacine_arm64.go`). AG-2 **removes that override**: implement
`WriteBreakpoint`/`ClearBreakpoint` as `mem` writes (dlv's generic software
breakpoints), and reserve the HW registers for **watchpoints** (data breakpoints,
which need no code patching and stay the clean choice, per every peer). dlv's native
`next`/conditional-breakpoint/return-breakpoint logic then "just works," and the F10
register-exhaustion vanishes. Net Ambush change: *less* code.

### 3.7 The invariant (reserved **I-41**)

AG-2 introduces one invariant, registered here and formalized in ARCH §28 at SB-1:

> **I-41 (software-breakpoint isolation).** A debug write to a target's code
> COW-breaks the shared file-backed Image page into a **per-Proc private** copy: the
> patched page is R+X with the trap byte, the target's PTE is never W+X (I-12
> preserved), and the shared REVENANT Image page — and therefore every *other* Proc
> running the same binary, and the persistent on-disk image — is left **pristine**
> (I-36 preserved). The write primitive is gated on I-39 debug authority + a
> fully-stopped target; it can COW-or-refuse but never mutate shared/executable state
> in place. Composes I-12 / I-36 / I-39; adds no ambient authority.

### 3.8 The concurrency to spec/audit

Because the step-over race is *absent* (all-stop, §3.5), the only real concurrency is
the **COW-break vs. the Image-cache lifecycle**: the break reads + copies the shared
Image page and remaps the target's PTE while (a) the target is fully-stopped (its own
threads cannot fault — like the HW-breakpoint arm's quiescence requirement) and (b)
another Proc / the reclaim pass could touch the Image entry. The target's
`mapping_count` ref pins the shared page across the copy; the break runs under the
target's `vma_lock`; lock order `vma_lock → v->lock → buddy`. **Spec-first is
RE-ENABLED for this surface** (the deep-review precedent): a focused
`specs/swbreak_cow.tla` models the COW-break vs. eviction/remap (no lost page, no
UAF, the shared page stays pristine, exactly-once private copy). No step-over spec is
needed (in-place, all-stop).

### 3.9 Sub-chunks

- **SB-1** — the focused design/scripture pass: the ARCH §28 I-41 row, `swbreak_cow.tla`
  (model-first), the reference-doc skeleton. (Design-only.)
- **SB-2** — the kernel substrate: COW-break-and-patch on a debug code write + the
  EC-0x3C `BRK` route + the exclusive-instruction reject. Validated against the spec.
- **SB-3** — Ambush: drop the HW-only override → dlv generic software breakpoints;
  keep HW watchpoints. The E2E: a Go target with >6 breakpoints + `next` over a
  `range` loop both work.
- **SB-4** — the focused audit (the reviewer + a self-audit; the I-41 isolation + the
  COW-vs-eviction concurrency + the I-cache discipline) + the SMP gate + the re-measure.

### 3.10 Forks resolved

1. **Write surface** → the Plan 9 `mem`-write-COWs-code idiom (minimal Ambush change,
   heritage shape), gated on I-39 + stopped. (Not a dedicated `swbreak` verb.)
2. **Step-over** → **in-place** remove-step-reinsert (all-stop-safe); displaced
   stepping + arm64 sim tables are the documented v-next (non-stop only).
3. **Image page** → **per-page COW** (Zircon model), not Genode's whole-segment copy.

---

## 4. AG-1 — single-step-`next` (the interim)

Before AG-2 lands, F10/step-over is unblocked in Ambush (user-to-push) by teaching
dlv's `next` to **fall back to single-stepping** when it would exceed the HW
breakpoint budget: single-step the line (dlv already has `StepInstruction`; the
kernel `step` verb is `/debug-probe`-proven), stepping over a nested call with one
temporary HW breakpoint at the return. Needs ≤1 register; leaves `continue` at full
speed; no kernel change. **Obsoleted by AG-2** (unlimited software breakpoints make
dlv's native breakpoint-based `next` fit), but it ships first because it is small and
the debugger is otherwise unusable for step-over on loops. Cross-ref
`memory/project_ambush_singlestep_next.md`.

---

## 5. AG-3..AG-6 (the Stage-8 remainder + the arc close)

- **AG-3 (8f-3 remainder):** inline gutter values (the current line's variable values
  rendered in the editor gutter), the LSP editor affordances (rename, code actions,
  signature help wired through `parley::lsp`), the Bonfire polish pass (the visual
  identity of the debug session). Pure userspace (Nora/Kaua).
- **AG-4 (8g superpowers):** the Thylacine-only debug surface (GO-IDE-DESIGN §5) — the
  `/proc/<pid>/ns` namespace + resource inspector, the scheduler view, capability-
  scoped system-wide attach, kernel-aware post-mortem (a Halls dump → a debug
  session), Stratum snapshot-debugging. Each superpower opens with its own scope note.
- **AG-5 (loose ends):** goroutine-accurate `kstack` (walk the parked M's stack per
  goroutine, not just the head thread — the 8c-3 v1.x note); finish shipping kernel
  DWARF so kernel frames symbolize with source (8b deferred).
- **AG-6 (arc audit + ship):** the whole-arc focused audit (I-39 + I-41 the
  centerpieces) + the SMP gate + debug-by-default + the reference-doc pass.

---

## 6. Stage 9 — OS-native time-travel (the deferred capstone)

Kept **separate** (not folded into Advanced Go). Deterministic record-replay
reverse-step is uniquely ours — we own every source of nondeterminism (the
scheduler, the syscall ABI, the CSPRNG, the clock) — so we can record those and
replay deterministically, giving a true OS-native reverse-step (not `rr`'s fragile
perf-counter bolt-on). It is a research-grade kernel arc + a NOVEL entry of its own,
earned *after* Advanced Go. Foldable into Advanced Go later if sequencing warrants;
for now it remains Stage 9 (GO-IDE-DESIGN §9).

---

## 7. SOTA / prior-art survey (the two-pass digest)

Two focused research passes (2026-07-22) grounded the design. Headline conclusions:

**Mechanism (Linux/macOS/kprobes/gdb):**
- Software breakpoints under W^X = a kernel-mediated write that forces a **private COW
  copy** of the code page (Linux `FOLL_FORCE`+`do_wp_page`; macOS `VM_PROT_COPY`
  shadow objects). Confirmed the shared original is never mutated.
- The multi-thread step-over race is solved by **displaced (out-of-line) stepping**
  (gdb `displaced_step_copy_insn`/`fixup`; Linux kprobes XOL) — but **only matters in
  non-stop mode**; an all-stop debugger with per-Proc-isolated breakpoints does
  in-place stepping safely.
- If displaced stepping were needed: the arm64 correctness work is simulating
  PC-relative instructions (`ADR`/`ADRP`/`B`/`BL`/`B.cond`/`BR`/`BLR`/`RET`/`CBZ`/
  `CBNZ`/`TBZ`/`TBNZ`/`LDR`-literal) against a register copy and **rejecting**
  `LDXR`/`STXR` exclusives — the `arch/arm64/kernel/probes` reference. **Not needed
  for us (§3.5).**
- Delve's `next` plants breakpoints at all successor lines + the return (never
  single-steps) — the direct cause of our HW-slot exhaustion.

**OS architecture (capability microkernels + Plan 9):**
- "Debug = a cap on the target" is universal (seL4 TCB cap, Zircon task handle, Mach
  port, Plan 9 `/proc` permission) → our I-39 is the right, already-built model.
- Zircon's `zx_process_write_memory` *ignores page W^X, gates on debug rights, COWs*
  (RFC-0159) is the exact kernel-mediated-write model to borrow.
- Plan 9's discipline: write the trap into `mem`, restore from the pristine `text`
  file, writes only to a stopped process — all adopted.
- seL4's caution: keep the code-mutation primitive tightly gated (authority + stopped
  + COW-or-refuse) so it is not a soundness/W^X escape hatch — encoded in I-41.

**Sources:** ptrace(2) + LWN UBP/XOL + the arm64-kprobes simulation series; gdb
displaced-stepping.{h,c} + aarch64-tdep; Zircon RFC-0159 + exceptions + arm64
debugger.cc; seL4 debugging-userspace + the verification FAQ; Genode GDB monitor;
Plan 9 acid paper + proc(3) + segattach(2) + auth.md; QNX DCMD_PROC_BREAK. (Full URLs
in the session research digest.)

---

## 8. References

- `docs/GO-IDE-DESIGN.md` — the predecessor Stage-8 arc (now complete).
- `docs/DEBUG-FS-DESIGN.md` — the I-39 debug-fs + HW-debug design (8a).
- `docs/DELVE-PORT-DESIGN.md` — the Ambush backend (8c); the HW-only-routing note this
  arc removes.
- `docs/reference/126-revenant.md` + ARCH §6.5 (I-36) — the shared Image cache the
  COW-break must leave pristine.
- `arch/arm64/mmu.c::mmu_patch_text` + PORTABILITY.md §4.5 (W1.5) — the transient
  RW-not-X patch primitive AG-2 reuses.
- `memory/project_ambush_singlestep_next.md` — AG-1.
- ARCH §28 — I-39 (debug authority); I-41 (reserved here, formalized at SB-1).
