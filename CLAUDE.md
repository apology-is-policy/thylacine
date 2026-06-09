# CLAUDE.md

Operating notes for Claude Code instances working on Thylacine OS.

This document is binding scripture for every implementation session. It encodes the operational framework, the discipline expected, the build commands, the audit-trigger surfaces, and the invariants that must hold. Read this before doing anything else; refer back to it often.

---

## Mission

Thylacine is a Plan 9-heritage operating system targeting ARM64, designed to be a real OS — not a toy, not a research prototype. It is built on three convictions: Plan 9's ideas were correct; the shell is sufficient as a UI; the filesystem is the OS. The fourth, methodological conviction — the one that binds the project at every level — is that **complexity is permitted only where it is verified**: maximum implementation rigor, formal specifications for every load-bearing invariant, adversarial audit before every invariant-bearing merge, and no shortcut implementations even when "we'll fix it later" would save weeks.

See `docs/VISION.md` for the full mission statement.

---

## Whole-system stewardship — there is no "my chunk"

The system is OURS, not yours. Every instance inherits the entire tree — not just the sub-chunk it was spawned to land. **Care about the code you did NOT touch exactly as much as the code you did.** A bug, instability, or unsoundness anywhere in Thylacine (or in Stratum, which is in-scope) is your problem the moment you see it.

**The forbidden disownment phrases.** "Not mine," "not my chunk," "not my code," "pre-existing," "already broken," "unrelated," "out of scope," "someone else's subsystem," "in-flight elsewhere," "known bug," "known flake," "tracked already," "they're investigating it," "v1.x," "deferred" — **the instant you write or think ANY of these about a live defect, STOP. The phrase is not a disposition; it is a TRIGGER.** It means a real bug just crossed your field of view and the convenience-seeking part of you is reaching for a reason to walk past it. These are the precise rationalizations that let real defects rot across session boundaries, each instance tending its own plot while the commons decays.

**Attribution is not ownership — and never changes priority.** It is correct and often necessary to establish that a defect is pre-existing, inherited, cross-tree, or causally independent of your change (e.g. by stashing your work and reproducing on the base — that is good ground-truth triage, exactly what the bug skill demands). But that finding changes only the *attribution* (who introduced it, where it lives). It changes NOTHING about the *ownership* (it is ours) or the *priority* (a soundness threat is a soundness threat). The moment your investigation lands on "pre-existing," the next action is **enqueue it as real work + fix-or-properly-escalate** — NEVER "so I can move on." Using a true attribution finding to disown, deprioritize, or close-around a defect is the violation, and it is worse than skipping the investigation, because it launders a dodge as diligence. (Worked failure, this project: a coordinator chunk's full test suite surfaced a reproducing `STM_ECORRUPT` under concurrent reflink; the instance correctly proved it pre-existing by stashing + rebuilding on the base, then wrote "in-flight Stratum bug, not mine" and moved on — the proof was right, the conclusion was a stewardship breach. Correct move: same proof, then enqueue it as next + own it.)

**Why this is binding, not sentiment:** a chunk's value is entirely *derivative* of the system's soundness. A perfectly-implemented, audited, green sub-chunk landed into a system that is buggy, unstable, or unsound is worth **nothing** — the achievement evaporates the moment the system it lives in falls over. Local correctness is necessary but never sufficient; the only deliverable that counts is a sound *system*. So caring about your chunk *requires* caring about the whole — they are not separable concerns.

Concrete obligations:

- **A soundness threat outranks chunk completion — anywhere it lives.** When you discover or inherit an instability (a corruption-class symptom, an SMP race, a deferred-forever hazard, a "flake"), it is not a footnote beside your chunk's win. Surface it with at least the weight you give your own deliverable, and treat resolving-or-properly-escalating it as part of the job — even when it sits in a subsystem you never opened.
- **Never verify *around* an instability.** If your chunk only passes because you dodged the configuration that exercises a known hazard (e.g. verifying at `-smp 1` to avoid an SMP overflow, skipping a sanitizer, narrowing a stress test), your chunk is **NOT verified** — the dodge is itself the bug, and it blocks the close. Verify in the configuration that exercises the hazard, or fix the hazard. A green result obtained by avoidance is a *misleading* result, which is worse than a red one.
- **Inherited defects are now yours.** When you pick up the tree, its open soundness debt — the deferred `handle_get` TOCTOU, the P5-hostowner I-2 capability hole, a recurred "resolved" bug, an unlanded multi-thread `_Exit` hazard — is your debt to weigh, not "the prior session's problem." Don't let a chain of sessions each punt it as "adjacent." (This is the system-soundness twin of the depth-first-dependencies rule: pull the latent hazard forward, don't seam-and-defer it indefinitely.)
- **Encounter a bug → enqueue a bug. Always, immediately, before you do anything else with it.** The instant a real defect crosses your field of view — yours, inherited, pre-existing, cross-tree, doesn't matter — its FIRST disposition is a tracked work item (a `TaskCreate` entry + a memory/status note), created the moment you see it, BEFORE you decide whether to fix it now or sequence it later. A bug that is only mentioned in prose (a commit body, a chat reply, a "caveat") is a bug being walked past in slow motion. "Surfaced it to the user" is NOT enqueuing. The queue is the proof you own it; prose is the proof you noticed it and hoped someone else would.
- **Report the system, not just the chunk.** End-of-iteration summaries lead with system soundness — does the whole thing still boot, stay up, hold its §28 invariants under the *real* configuration? — *then* the chunk. A green chunk reported without its system-level caveats reads as "all is well" when it may not be. And a caveat is not a disposition: every soundness caveat in a summary MUST point at the queue item that owns it.

This is the stewardship companion to the flake-dismissal discipline (`DEBUGGING-PLAYBOOK.md` §6.11) and the "distrust hollow AUDITED CLEAN closes" rule (§"When in doubt"): the SAME convenience-seeking instinct wants to wave a bug away as "just a flake" (it isn't a real bug) AND as "not my chunk" (it's a real bug but not my problem). They are two faces of one dodge. The `elusive-bug-hunt` skill now trips on BOTH families — the flake-dismissal vocabulary AND the disownment vocabulary above — and routes the disownment case here. Resist both. **It is all ours.**

---

## The scripture

These documents are binding. Implementation deviations either update scripture first or get reverted.

| Document | Purpose |
|---|---|
| `docs/VISION.md` | What we're building and why. Properties ranking. Latency budget. Invariants (first pass). Non-goals. |
| `docs/COMPARISON.md` | Where we sit vs comparable systems. Feature matrix. Positioning. |
| `docs/NOVEL.md` | The 9 lead positions. Per-angle scope, done definition, dependencies, complexity, risk. |
| `docs/ARCHITECTURE.md` | How we're building it. Foundational decisions with rationale. 20 enumerated invariants. Audit-trigger surface table. |
| `docs/ROADMAP.md` | In what order. 8 phases with deliverables, exit criteria, risks, dependencies. Risk register. |
| `docs/TOOLING.md` | Development tooling and agentic loop. QEMU + 9P host share + agent protocol. |
| `docs/REFERENCE.md` + `docs/reference/NN-*.md` | As-built technical reference. Per-subsystem; deep; binding. Updated per chunk. |
| `docs/USER-MANUAL.md` + `docs/manual/NN-*.md` | User-facing reference. Per-topic; deep; binding. Updated per user-visible change. |
| `docs/ERRORS.md` | Error-code system. Errno registry (Thylacine-wide, POSIX-aligned values), `snare:*` fault-note family (thematic; replaces EL0-unhandled-fault extinction with per-Proc termination), exit-status semantics, boundary-line translation policy. ABI-bearing; updates require user signoff. |
| `CLAUDE.md` (this) | Operational framework for Claude Code sessions. |
| `docs/DEBUGGING-PLAYBOOK.md` | **Mandatory reading when an elusive bug appears** (corruption-class symptom, inconsistent repro, cross-layer, or a recurred "resolved" bug). The AEGIS-corruption-triplet case study + the ground-truth-first method. The `elusive-bug-hunt` skill auto-surfaces the condensed method; this doc is the full journal. |

Read first, in this order: VISION → ARCHITECTURE → ROADMAP → CLAUDE.md → the relevant phase status doc.

---

## Design-first policy (Phase 0 was)

Phase 0 produced the scripture above. Implementation is now permitted, *bound by* the scripture. If implementation surfaces a need that the scripture doesn't cover, **update the relevant scripture document first**, get user signoff for any binding change, then implement. Never silently deviate.

Active phase status doc per `ROADMAP.md` is `docs/phaseN-status.md` (e.g. `docs/phase1-status.md`). Update per chunk.

---

## Spec-first policy (applies to every invariant-bearing feature)

**If a feature touches a load-bearing invariant — concurrency, commit ordering, namespace operations, handle transfer, VMO lifecycle, 9P pipelining, scheduler IPI, futex atomicity, poll wait/wake, note delivery, PTY semantics, capability checks, anything in the §28 Invariants list in ARCHITECTURE.md — the TLA+ model comes BEFORE the implementation.** Write the spec, let TLC chew on it, let invariant violations surface at the spec level where they cost minutes, not at runtime where they cost commits.

Concrete pattern:

1. Propose the feature in prose (problem + shape).
2. Model the mechanism in TLA+ — state, actions, invariants. TLC with small bounds.
3. Iterate until TLC is green under the invariants the implementation must uphold. If a bug shows up, fix the DESIGN before writing code.
4. Where a spec captures a specific bug, also write a `{spec}_buggy.cfg` that fails the invariant under the buggy assumption. Executable documentation of "this is the bug, this is the fix."
5. Implement against the model. Cross-reference each impl step to the corresponding spec action in comments. Keep `specs/SPEC-TO-CODE.md` current.
6. When the impl surfaces a new mechanism the spec didn't cover, extend the spec FIRST, then update the impl.

The nine specs gate-tied to phases (per `ARCHITECTURE.md §25.2`):

| # | Spec | Phase | Invariants |
|---|---|---|---|
| 1 | `specs/scheduler.tla` | 2 | EEVDF correctness, IPI ordering, wakeup atomicity, work-stealing fairness |
| 2 | `specs/namespace.tla` | 2 | bind/mount semantics, cycle-freedom, isolation between processes |
| 3 | `specs/handles.tla` | 2 | Rights monotonicity, transfer-via-9P invariant, hardware-handle non-transferability |
| 4 | `specs/vmo.tla` | 3 | Refcount + mapping lifecycle, no-use-after-free |
| 5 | `specs/9p_client.tla` | 4 | Tag uniqueness per session, fid lifecycle, out-of-order completion correctness, flow control |
| 6 | `specs/poll.tla` | 5 | Wait/wake state machine, missed-wakeup-freedom across N fds |
| 7 | `specs/futex.tla` | 5 | FUTEX_WAIT / FUTEX_WAKE atomicity (no wakeup lost between value check and sleep) |
| 8 | `specs/notes.tla` | 5 | Note delivery ordering, signal mask correctness, async safety |
| 9 | `specs/pty.tla` | 5 | Master/slave atomicity, termios state transitions |

Features that clearly benefit: scheduler IPI, namespace bind/mount, handle transfer, VMO lifecycle, 9P pipelining, poll wait/wake, futex wait/wake, note delivery, PTY master/slave atomicity.

Features that usually don't (pure computation, test helpers, config parsing, CLI glue): skip the spec; just write + test. Use judgment.

**If you cannot articulate the invariant formally, you don't understand it well enough to implement it.**

### Spec-to-code FULLY suspended until further notice (user-authorized, broadened 2026-05-23)

**This supersedes the 2026-05-21 clean-cfg-only suspension.** The spec-first policy is now **fully suspended** for new sub-chunks: no `specs/*.tla` module is written for an invariant-bearing feature; the invariant is validated by **careful prose reasoning** in the impl's file header + commit message + reference doc, and rigor is provided by the audit round + the runtime test suite. Per the user's 2026-05-23 direction: "let's suspend spec-to-code until further notice, just validate the model by thinking."

The 2026-05-21 record (clean-cfg-only suspension; spec-first design still binding) is preserved as the predecessor; the broadening was triggered at sub-chunk 8 (`pouch-wait-addr`) — the `torpor` wait-on-address primitive — where the I-9-specialized no-lost-wakeup invariant is validated by walking the WAIT/WAKE interleavings with lock-acquire as the serializing event, not by a TLA+ module.

Why broaden: spec-first design served as a thinking aid — the discipline of articulating the invariant in formal syntax. The user has signalled trust that we can validate models by careful prose reasoning. The corvus precedent (a CSPRNG-token verification chunk whose spec wasn't load-bearing in retrospect) was the 2026-05-21 narrow lift; sub-chunk 8 is the explicit broadening.

What stays binding:
- **Buggy-cfg counterexamples on EXISTING specs**: any impl change that touches a mechanism modelled in `specs/` must re-run the relevant buggy cfgs (`scheduler.tla`, `namespace.tla`, `handles.tla`, `vmo.tla`, `9p_client.tla`, `pipe.tla`, `poll.tla`, `corvus.tla`, `burrow.tla`, `tsleep.tla`, ...). They terminate fast and remain pre-commit gates for invariant-detection regressions on already-spec'd subsystems.
- **Audit-trigger surfaces** (CLAUDE.md §"Audit-triggering changes") are unchanged; the formal-audit discipline is now the load-bearing rigor pass for new invariant-bearing work — it does not get suspended.
- **The 21 enumerated invariants** in `ARCHITECTURE.md §28` remain proof obligations; the suspension affects how we verify them, not whether they must hold. Whatever invariant a new sub-chunk introduces must be articulated (in prose) and audited.
- **The audit round + runtime test suite are the rigor floor** for new sub-chunks.

What gets deferred:
- TLA+ modules for new features. Sub-chunk 8 is the worked example — no `specs/futex.tla` written; the no-lost-wakeup model validated by reasoning in `kernel/torpor.c` + `kernel/include/thylacine/torpor.h` + the audit.
- Clean-cfg TLC runs (suspended since 2026-05-21).
- Coverage claims of the form "spec re-verified clean GREEN" per chunk.

When to re-enable: at user direction. The natural re-enabling points: (a) an invariant-bearing feature that genuinely benefits from machine-checked exploration; (b) when wall-clock budgets allow returning the spec-first DESIGN discipline as a thinking aid.

**RE-ENABLED for the SMP scheduler/thread-lifecycle (user-directed, 2026-06-05; deep-smp-review).** This is re-enabling point (a) realized: the deep SMP review found the original `specs/scheduler.tla` was structurally blind to the bug class (it models `Steal` atomically with no `on_cpu`), which is why the on_cpu protocol + boot-CPU idle dispatch accreted point-patches (#788/#806/#860). The SMP redesign (ARCH §8.4) is **model-first**: `specs/sched_oncpu.tla` (the diagnostic — reproduces #860) + `specs/sched_alpha.tla` (the gating model for the target design — TLC-green) are written + checked BEFORE the impl, and every redesign sub-chunk is validated against `sched_alpha.tla`. The HMP placement *logic* is additionally logic-verified by a synthetic-asymmetric-DTB unit test; the empirical EAS tuning is deferred to real heterogeneous hardware (ARCH §8.4.4). Re-enabled for THIS surface only; the broader suspension stands elsewhere.

Cross-link: `memory/feedback_spec_to_code_suspended.md` (project-wide policy record; updated 2026-05-23 to reflect the broadening).

### Research prior art before surfacing a design fork

Before you take a design fork to the user (the pattern below), do the homework that makes the fork legible -- and often dissolves it. A fork surfaced cold ("A or B?") makes the user do the research you should have done. In order:

1. **How does the heritage system solve it?** Thylacine is Plan 9-lineage: how do Plan 9 and its relevant daemons (e.g. factotum + secstore, devmnt's shared-mount, the per-process namespace) do this exact thing? We inherit its model, so its answer is usually load-bearing.
2. **What is the modern SOTA?** Look at the closest peers. For OS-level questions that is the capability microkernels -- Fuchsia, Genode, seL4, Hurd -- NOT Linux/macOS, whose global-VFS / ambient-authority answers frequently don't map onto Thylacine's per-Proc, capability-scoped model. Name the mechanism each uses, not just the product.
3. **How well does each fit Thylacine?** Ground the fit in VERIFIED facts about the tree -- which syscalls/mechanisms already exist (run the greps), what the section-28 invariants demand, what the lineage idiom is. Don't assume.
4. **Improvement / novel angle?** The best Thylacine answer is frequently a fusion of the Plan 9 idiom and the capability-microkernel SOTA. If the synthesis is genuinely new, it's a NOVEL.md candidate -- record it even when v1.0 defers building it.

Then surface the fork WITH the research attached: each option annotated by precedent, fit, and cost. Often the research collapses four options to one obvious choice -- make the call and report the reasoning instead of asking. Escalate only the residue the research genuinely can't resolve (a value/scope tradeoff that is the user's to weigh). Worked example: the A-1b "where does corvus's persistent storage live" fork -- Plan 9 factotum/secstore + devmnt shared-mount, the Fuchsia/Genode per-component-session SOTA, the verified facts (`SYS_MOUNT` exists; the 9P client serializes every RPC under one lock; spawn can pass a Spoor handle), and a novel "storage-as-a-spawn-capability" angle -- all gathered BEFORE re-posing the choice.

### Design conversation -> scripture commit (mid-project pattern)

When an implementation chunk surfaces a non-trivial design question -- a new mechanism, a load-bearing decision, an invariant not yet in scripture -- the workflow is:

1. **Stop the implementation.** Don't try to design-while-coding. Stop, surface to the user.
2. **Surface as a structured option set.** Not a yes/no; lay out 2-4 options with their consequences. Auto-mode bias is "make the call" -- but scripture-altering decisions are explicitly outside auto-mode and warrant the user's vote.
3. **Have the conversation in-session.** Iterate to user signoff in one round-trip where possible.
4. **Land the design as a SCRIPTURE COMMIT FIRST -- no code.** The commit updates `ARCHITECTURE.md` / `NOVEL.md` / phase-design docs / `CLAUDE.md` / `ROADMAP.md` as needed, and adds a memory-file index entry. The commit message names the design decision, the rationale, the alternatives considered, and the open questions resolved.
5. **THEN implement** in a subsequent commit that references the scripture commit's SHA in its message.
6. **THEN audit** (the standard pattern for audit-bearing implementations).

The pattern is "scripture before code, every time the code would otherwise determine the scripture." Examples that drove this pattern in Thylacine:
- P6-pouch-mem-design (`2fd9797`): the two-tier native memory interface, surfaced mid-implementation of `pouch-mem`; landed as scripture commit before the kernel-side syscalls.
- P6-pouch-compiler-rt-design (`bc97630`): the compiler-rt + `pouch-ld` requirement, surfaced by `pouch-hello-smoke`; landed as scripture before the rt build wiring.
- P6-pouch-signals-design (`237f096`): the fd-first notes substrate (novel angle), surfaced before the kernel notes implementation; landed as scripture + NOVEL.md update before the kernel-side code.

The pattern produces audit-traceable design history and makes the implementation auditable against a fixed reference. The scripture commit is short, focused, and reversible if implementation surfaces a flaw in the design.

### TLA+ setup

Install OpenJDK (`/opt/homebrew/opt/openjdk/bin` on macOS; `apt-get install default-jdk` on Linux).

Download TLA+ tools:

```bash
curl -sL -o /tmp/tla2tools.jar \
  https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar
```

Run every spec in `specs/`:

```bash
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
cd specs
for s in $(ls *.tla | sed 's/\.tla$//'); do
    echo "== $s =="
    java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock \
        -config "$s.cfg" "$s.tla" 2>&1 | tail -3
done
```

Pre-commit for invariant-bearing features: spec clean + buggy-config counterexample confirmed + all tests pass.

---

## Audit-triggering changes

Any change to the surfaces below MUST spawn a focused adversarial soundness audit before merge. Not as ceremony — each round has historically surfaced bugs the test suite didn't catch, and the pattern is that regressions in these areas are not caught by tests.

**Trigger list** (refresh after each ARCH change; mirrors `ARCHITECTURE.md §25.4`):

| Surface | Representative files | Why |
|---|---|---|
| Exception entry + EL0-entry trampolines | `arch/arm64/start.S`, `arch/arm64/exception.c`, `arch/arm64/vectors.S`, `arch/arm64/userland.S` (`userland_enter`), `arch/arm64/context.S` (`thread_user_trampoline`) | Every syscall / IRQ / fault path. Privilege boundary. **#713 eret-window invariant**: any hand-rolled `eret`-to-EL0 path that sets `ELR_EL1` MUST mask DAIF (`msr daifset, #0xf`) across the ELR-set..`eret` window -- else an IRQ in the window overwrites `ELR_EL1` with the interrupted kernel PC and the `eret` lands EL0 at a kernel VA (rare, IRQ-timing-dependent, the #713 root cause). The `eret`'s `SPSR_EL1=0` re-enables IRQs at EL0 atomically. The kernel `thread_trampoline` (EL1, no `eret`) is exempt -- it legitimately unmasks. `KERNEL_EXIT` is exempt -- always reached IRQ-masked. See `docs/reference/08-exception.md` "eret-window IRQ race (P6 #713)". |
| Halls of Extinction crash dump | `arch/arm64/halls.c`, `arch/arm64/halls.h`, `arch/arm64/exception.c` (the four entry wrappers `exception_sync_curr_el` / `exception_irq_curr_el` / `exception_sync_lower_el` / `exception_unexpected` -> static `*_impl`, bracketed by `halls_enter_frame`/`halls_leave_frame`), `kernel/extinction.c` (`halls_dump((void*)0)` call sites) | HX-1 Tier-1 crash dump on the fatal path -- runs on a dying machine, so it must be SOUND under unknown state. **HX-I1**: a fault DURING the dump trips the per-CPU `g_halls_in_dump` guard (set BEFORE any faulting read) and bails to `_torpor` -- never loops/recurses. **HX-I2**: the fp-chain backtrace is depth-capped (32) + sanity-gated (`halls_fp_is_sane`: 16-aligned, strictly increasing, in `[lo,hi)`); a wild x29 cannot spin or read unboundedly. **HX-I3**: the `EXTINCTION: ` ABI line (TOOLING.md section 10) stays first + unchanged; the dump follows under `HALLS:`. The per-CPU live-frame slot is set/restored by the entry wrappers (handlers do not migrate CPUs mid-execution at v1.0); an extinction (noreturn) inside a handler keeps the slot pointed at the dying frame. Return addrs are PAC-stripped (`xpaci`) before KASLR link-translation. **HX-2 (Tier-2 symbolization)** adds `arch/arm64/halls_symtab.h` + `halls_symtab.stub.c`, the PER-BUILD-DIR generated `<build>/generated/halls_symtab.c` (`tools/gen-halls-symtab.py` + the two-pass `tools/regen-halls-symtab.sh`, wired via `kernel/CMakeLists.txt` configure-seed + `tools/build.sh` + inherited by `tools/test-fault.sh`), and `halls_symbolize`/`halls_symbolize_table` in `halls.c` feeding `func+0xN` into `halls_emit_code_addr`. **HX-2 dump-safety**: `halls_symbolize` reads ONLY the `.rodata` table (no faulting stack reads), is bounded by `log2(count)`, takes no locks + allocates nothing, so it composes with HX-I1/HX-I2; the offset subtraction cannot underflow (search invariant `tab[lo].off <= q`). **Reloc-free + KASLR-independent**: the table stores u32 link-relative offsets, NOT absolute VAs -> 0 `R_AARCH64_RELATIVE` relocs (an absolute VA would draw one per symbol that the boot stub then SLIDES; verified 0 relocs land in the table region) -> the stored values are never slid. Generated PER-BUILD-DIR (default 1727 vs UBSan 1760 symbols -> a source-tree copy would clobber across configs) + BEST-EFFORT (stub `count=0` -> `halls_symbolize` returns NULL -> graceful HX-1 raw-only fallback when nm/python3 absent; symbolization is ergonomics, never a build gate). **No new spec** per the 2026-05-23 broadening -- prose validation in `docs/reference/101-halls.md` (Tier-2 section) + `docs/HALLS-OF-EXTINCTION.md` + this row + the audit + the 8 unit tests (`halls.*` incl. `symbolize_table`) + the `tools/test-fault.sh` E2E. |
| Page fault + COW + W^X | `arch/arm64/fault.c` (`userland_demand_page` holds `p->vma_lock`), `kernel/syscall.c` (`SYS_MMIO_MAP`/`SYS_DMA_MAP` `burrow_map` under `vma_lock`), `mm/vm.c`, `mm/wxe.c` | Lifetime, demand-page, COW, W^X invariant (I-12). **#713 vma_lock invariant**: EVERY `p->vmas` mutator AND the demand-page reader hold `p->vma_lock` (multi-thread-Proc SMP safety; stratumd is the first heavily-threaded Proc). Lock order `vma_lock -> buddy zone->lock`. `exec_setup`/`vma_drain` exempt (single-threaded by construction). |
| Boot-time LSE alternatives-patcher (Lazarus W1.5) | `arch/arm64/alternatives.{c,h}` (the `ALTERNATIVE()` macro emitting `.altinstructions` + `.altinstr_replacement`; `apply_alternatives()` the boot pass + the `g_alt_total`/`g_alt_applied` counters), `arch/arm64/atomic_lse.h` (the LL/SC-default patchable RMW primitives `t_atomic_xchg_acq_u32` / `t_atomic_fetch_{add,sub}_*` via the `T_ATOMIC_XCHG` / `T_ATOMIC_FETCH_OP` macros + the `fetch_sub == fetch_add(-v)` C wrappers), `arch/arm64/mmu.c::mmu_patch_text` + static `patch_kva_to_pa` (`at s1e1r`+PAR_EL1) / `patch_map` / `patch_unmap` / `patch_sync_icache`, `arch/arm64/kernel.ld` (`.altinstructions` + `.altinstr_replacement` in `.rodata` with `_alt_instructions_start/_end`), `kernel/main.c` (`apply_alternatives()` after `hw_features_detect` + allocator, strictly before `smp_init`), the routed sites (`kernel/include/thylacine/spinlock.h` `spin_lock`/`spin_trylock` xchg, `kernel/sched.c` `g_try_steal_rotate`, `kernel/spoor.c` + `kernel/srvconn.c` int refcounts) | Lazarus W1.5 (PORTABILITY.md section 4.5; user-voted 2026-06-06 over outline-atomics). **Self-modifies `.text` -- the central invariant is I-12 (W^X).** Rewrites the LL/SC atomic baseline (the v8.0 floor from W1) to single-instruction LSE on FEAT_LSE cores at boot, with zero steady-state branch -- the Linux arm64 `apply_alternatives` model. **I-12 holds because the write goes through a TRANSIENT RW-not-X alias** (a scratch VA mapped `PTE_KERN_RW` = RW+PXN+UXN); the canonical `.text` stays RO+X and the P3-Bb direct-map alias RO+XN throughout -- no page is ever writable AND executable at PTE granularity, not even momentarily. Runs **single-CPU, full-DAIF-masked (`msr daifset, #0xf`), strictly before `smp_init`** -- no peer executes a site mid-patch; secondaries start later with cold I-caches and fetch the patched bytes. (Masking F/A as well as I also closes the `at s1e1r`->`PAR_EL1` window in `patch_kva_to_pa`; W1.5-audit F1.) I-cache maintenance per ARM ARM B2.4 (`dc cvau` on the scratch alias / `ic ivau` on the canonical VA / `dsb ish` / `isb`; CTR_EL0 line stride; per-page loop for cross-page spans). The `.altinstructions` table is **reloc-free + KASLR-independent** (PC-relative `.word` offsets slide with their targets, like the Halls symtab). LL/SC is the always-correct fallback: an unpatched site (A72, or any skipped entry) is correct by construction, so a patcher bug fails **safe** (slower, never wrong). `fetch_sub == fetch_add(-v)` preserves the signed-PRE value the Spoor/SrvConn `pre <= 0` UAF check reads. **No new spec** per the 2026-05-23 broadening -- prose validation in PORTABILITY.md section 4.5 + this row + the ARCH section 25.4 row + the focused audit (`memory/audit_w15_closed_list.md`) + the `alternatives.*` unit tests (`patch_applied`: `g_alt_applied == g_alt_total` -- every FEAT_LSE site -- on `-cpu max`; `atomics_correct`: the patched LSE forms compute correctly) + default 716/716 + the SMP gate (default+UBSan x smp4/smp8) on `-cpu max` (LSE active). |
| GICv2 driver + EL1 virtual timer (Lazarus W2) | `arch/arm64/gic.c` (the new GICv2 path: `gic_init_v2` + `dist_init_v2` + `gic_cpu_config_v2` + `cpu_iface_init_v2`; the GICC MMIO CPU interface `gic_acknowledge`/`gic_eoi` with the per-CPU `g_v2_eoi_token` CPUID-preserving EOI; `gic_send_ipi` GICD_SGIR; `gic_enable_irq`/`gic_disable_irq` banked-distributor; `gic_init_secondary` v2; the `v2_w32`/`v2_r32`/`v2_w8` `dsb`-after-each HVF-MMIO accessors), `arch/arm64/gic.h`, `kernel/mmio_handle.c` (reserve `arm,gic-400` + `arm,cortex-a15-gic` GICD+GICC -- I-5), `kernel/main.c` (v2-aware banner); AND the virtual-timer switch (#889): `arch/arm64/timer.c` + `arch/arm64/timer.h` (CNTV_CTL/TVAL/CNTVCT + `TIMER_INTID_EL1_VIRT`=27 + EL0VCTEN), `kernel/irqfwd.c` + `kernel/main.c` + `kernel/smp.c` (the INTID-27 consumers), `usr/irq-bench/src/main.rs` (EL0 CNTVCT) | Lazarus W2 (PORTABILITY.md section 5). **Audit-bearing: scheduler/IPI surface -- I-18 (IPIs from CPU A to B processed in send order). Also I-15 (DTB-driven GIC discovery), I-5 (the GICC CPU-interface MMIO must be kernel-reserved so a CAP_HW_CREATE driver cannot scribble on live IRQ ack/EOI state).** The v2 MMIO CPU interface (GICC) is the HVF-on-Apple enabler -- it sidesteps the GICv3-distributor `isv` assertion; the `dsb`-between-GIC-MMIO mitigation is **empirically validated under HVF on M-series** (gic_init + IRQ delivery + the full kernel boot run clean under `-accel hvf -machine gic-version=2`). The GICv2 SGI EOI subtlety: GICC_IAR carries the source CPUID in bits[12:10] for SGIs and GICC_EOIR must echo it back, so the raw IAR is saved per-CPU between ack + EOI (single in-flight IAR per CPU -- handlers run IRQ-masked, no nesting). **The virtual timer (#889) is pulled in** because the HVF dev loop -- W2's purpose -- also requires it: under a hypervisor the EL1 physical timer (CNTP_*) is reserved and a guest CNTP_TVAL_EL0 write reflects as an EC=0 undef; the virtual timer (CNTV_*) is the EL1-guest timer on every substrate (TCG/HVF/bare-metal where CNTVOFF=0), keeping ONE timebase. Prosecute: GICv2 SGI in-order delivery + the GICD_SGIR target bounding; the EOI-token lifetime (no cross-CPU race; spurious leaves a harmless stale token); the banked SGI/PPI distributor semantics (per-CPU bring-up on the secondary); the GICC reservation completeness (both v2 compats); the virtual-timer consistency (timer + counter + EL0 all CNTV; tsleep deadlines). **No new spec** per the 2026-05-23 broadening -- prose validation in PORTABILITY.md section 5 + this row + `docs/reference/10-gic.md` (GICv2 section) + `docs/reference/11-timer.md` (virtual-timer) + the focused audit + the version-aware `gic.init_smoke` + `mmio_handle.kernel_reserved_rejected` tests + default 716/716 + GICv2-TCG 716/716 + the SMP gate (v3) + the HVF+GICv2 empirical boot. **Owed follow-up #890 -- CLOSED @0124a8f (W3.5)**: the userspace virtio-mmio drivers tripped the same `isv` assert under HVF (a different layer; the kernel GIC + timer were already HVF-proven) -- fixed by routing every userspace device-MMIO access through an ISV-safe single-instruction `ldr`/`str` primitive in `libthyla_rs::hardware`. HVF boot now reaches `Thylacine boot OK` (721/721, 0 isv); the repo launchers default to HVF (W3.5; `tools/run-vm.sh` auto-detect). |
| Kernel CSPRNG (software-RNG, Lazarus W3) | `kernel/chacha20.{c,h}` (the pure ChaCha20 keystream primitive), `kernel/random.c` (the forward-secure CSPRNG: state machine + seed sources + the kernel virtio-rng driver `random_virtio_pull` + `kern_random_bytes`/`kern_random_seeded`/`random_seed_from_virtio` + the devrandom Dev), `kernel/include/thylacine/random.h`, `kernel/main.c` (the boot virtio reseed after `virtio_init`), `kernel/mmio_handle.c` (the I-5 RNG-slot posture doc) | Lazarus W3 (PORTABILITY.md section 6). **Audit-bearing: CSPRNG quality + the seed sources + the reseed cadence.** Replaces the RNDR-only baseline with a ChaCha20 forward-secure CSPRNG (the OpenBSD arc4random construction) so the SAME path runs on RNDR-less targets (HVF / A72) -- the entropy break W2's HVF dev loop exposed. Seed = DTB kaslr-seed/rng-seed + CNTPCT jitter + RNDR-when-present (stir only) + the kernel virtio-rng pull (the strong source); boot seeds early (DTB+cntpct) then strong-reseeds from virtio-rng after `virtio_init`; ~1 MiB strong-reseed cadence + rekey-on-buffer-drain (fast key erasure). Prosecute: predictable-output / weak-seed (the BSS-zero first rekey -- secrecy must come from the seed, not the public zero-state keystream; the DTB-seed/KASLR entropy-sharing in the pre-virtio window); no rekey material ever served (`g_rng_buf[0..40)` zeroed + skipped, served region `[40..1024)`); fail-closed `g_rng_seeded` gate (monotonic; `kern_random_bytes`->-1 + `SYS_GETRANDOM` refuses while unseeded); SMP races on the chacha state / counter / seeded flag (`g_random_lock`) + the device (`g_rng_dev_lock`, released before the chacha lock -- no nesting/deadlock; lock-order vs the buddy allocator); the virtio driver memory safety (UAF / double-free of the entropy page + vq; the device fully stopped [QUEUE_READY=0 + reset] before the page is freed; descriptor/avail/used bounds; `used.len` clamped to `want`; the bounded `RNG_VIRTIO_POLL_MAX` poll; every error path resets+unlocks+no-leak); the **all-zero guard** (a non-coherent DMA / dead device must fail SAFE to the DTB seed, never silently to zero entropy); secret hygiene (the seed / vbuf / cbuf / the entropy page / served bytes scrubbed on every path incl. errors); I-5 (the RNG slot is kernel-driven but page-share-infeasible-to-reserve -> inherits the documented v1.0 virtio-mmio kproc-trust posture, the same residual virtio-blk carries -- the Phase-5+ per-device-kobj revisit closes both). **No new spec** per the 2026-05-23 broadening -- prose validation in PORTABILITY.md section 6 + `docs/reference/106-random.md` + this row + the focused audit + the `chacha20.block_vector` (RFC 8439 vector) / `chacha20.keystream_continuity` / `kern_random.{two_reads_differ,large_read_nonzero,virtio_reseed}` tests + default 721/721 + the SMP gate (default+UBSan x smp4/smp8). |
| Allocator | `mm/buddy.c`, `mm/slub.c`, `mm/magazines.c` | Allocation correctness, lock-free invariants |
| Scheduler (SMP redesign, deep-smp-review 2026-06-05) | `kernel/sched.c` (the `on_cpu` protocol + the multi-step switch + `try_steal`/`pick_next` + the new `select_target_cpu` placement hook + the `balance()` abstraction + the per-CPU **pinned in-tree idle**), `kernel/thread.c` (`cpu_pinned`, the `thread_free` on_cpu gate), `arch/arm64/context.S` (`cpu_switch_context` + the trampolines), `kernel/smp.c` (per-CPU idle bringup + the dedicated cpu0-idle BSS stack + IPI logic), `kernel/main.c` (the production transition) | EEVDF correctness, SMP, wakeup atomicity, the **`on_cpu`/migration protocol (I-21: a thread runs on <=1 CPU; a ctx/kstack is never written by two CPUs concurrently)**, the **per-CPU pinned in-tree idle (retires `g_bootcpu_idle` + the deadlock path + the off-tree state; closes #860 by construction)**, the `idle_in_wfi` fix + the steal-invariant assert, and the HMP placement-hook + `balance()` + per-CPU capacity (I-8, I-9, I-17, I-18, I-21). **Model-gated**: `specs/sched_oncpu.tla` (diagnostic; reproduces #860) + `specs/sched_alpha.tla` (the target-design gate; TLC-green). Spec-first is RE-ENABLED for this surface. |
| Territory | `kernel/territory.c` | Cycle-freedom (I-3), isolation (I-1), mount-refcount consistency (§9.6.6). P6-pouch-stratumd-boot 16c adds `territory_pivot_root` (atomic root_spoor swap with displaced-ref tear-down) for `SYS_PIVOT_ROOT` -- see the dedicated row. |
| Handle table | `kernel/handle.c` | Rights monotonicity (I-2, I-6), transfer-via-9P (I-4), hardware-handle non-transferability (I-5). **#844 handle-lifetime pass** (`c1c2948` + the F1/F2 audit close): the per-Proc `HandleTable.lock` (the Plan 9 Fgrp lock) serializes alloc / close / get / dup; `handle_get` returns a BY-VALUE snapshot with the obj refcount HELD (`handle_acquire_obj` under the lock) paired with `handle_put` that drops it OUTSIDE the lock (release may sleep -- spoor_clunk's Dev close hook, SrvConn teardown) -- closing the multi-thread-Proc TOCTOU where a sibling `handle_close` freed a slot's obj + zeroed the slot under a live `struct Handle *` (widest across the blocking dev->read/write / accept / 9P handshake). The KOBJ_SPOOR-only helpers (`sys_lookup_spoor` / `sys_lookup_rw_handle` / `sys_resolve_dir_wr`) TRANSFER the ref (the caller `spoor_clunk`s); direct sites snapshot + `handle_put` on every exit. Lock order: table lock -> `v->lock` (a `burrow_ref` in get/dup); never the reverse. Audit R1 (Opus) closed **F1[P1]** (`handle_alloc` -- the primary fd-creating path -- was left UNLOCKED while get/close/dup were locked -> peer-thread slot collision -> double-owned fd / leak / UAF; fixed by routing it through the same locked `handle_install_locked`) + **F2[P2]** (`sys_srv_peer` read the raw `cn = sp->aux` AFTER `spoor_clunk(sp)` -> UAF window; fixed by hoisting the value-captured reads before the clunk); **F3[P3]** the pivot-vs-walk `root_spoor` race tracked (#848, dormant). **No new spec** -- prose in this row + `memory/audit_844_closed_list.md` + the matrix (smp4 + smp8 + UBSan 704/704 + login E2E + 0 EXTINCTION). |
| VMO / BURROW | `kernel/burrow.c`, `kernel/include/thylacine/burrow.h`, `mm/vmo_pages.c` | Refcount, mapping lifecycle (I-7). **#847** (`8acdcce`, the #844 precursor): the dual refcount (`handle_count` + `mapping_count`) AND the `handle_count==0 && mapping_count==0` free decision -- duplicated in `burrow_unref` + `burrow_release_mapping` -- now run under a per-Burrow `spin_lock` (was a plain non-atomic `++/--` + a lockless dual-check -> a latent SMP UAF/double-free reachable from a multi-thread Proc: torn count, or two paths racing the free decision). `burrow_free_internal` runs OUTSIDE `v->lock` (leaf discipline; it takes the buddy / mmio / dma locks). Needed because the #844 `handle_put` drops the Burrow ref OUTSIDE the handle-table lock, so the burrow refcount HAD to be intrinsically SMP-safe. spoor / srvconn / mmio / irq / dma refcounts were already atomic ACQ_REL. **No new spec** -- prose in this row + `memory/audit_844_closed_list.md`. |
| MMU user-PTE clear + TLBI | `arch/arm64/mmu.c::mmu_uninstall_user_pte / mmu_uninstall_user_range`, `kernel/burrow.c::burrow_unmap` (call site) | P6 hardening #2 / audit F1 fix: symmetric counterpart to `mmu_install_user_pte`. Walks L0..L3, clears the leaf PTE, broadcasts `tlbi vaae1is + dsb ish + isb` for the inner-shareable domain. Called from `burrow_unmap` BEFORE the underlying pages are freed back to the buddy. Without it, stale PTEs + TLB entries persist after `SYS_BURROW_DETACH` -> content-sensitive silent corruption when buddy LIFO returns a different PA on re-attach (suspected AEGIS-256 / mallocng root cause). Invariants: idempotent on already-cleared PTEs and on never-faulted-in pages; `pgtable_root == 0` rejected with -1 (early-boot helper paths); VA bounds match `mmu_install_user_pte`'s (`vaddr >> 47 != 0` reject). Per-page cost dominated by `dsb ish` wait (microseconds); batched-TLBI optimization deferred. **No new spec** — prose validation in this row + the audit (see `memory/audit_p6_memory_model_hazards.md` F1) + the runtime test suite (599/599 PASS at landing). |
| Errno ABI surface + `snare:*` fault-note family | `kernel/include/thylacine/errno.h` (T_E_* registry; ABI-pinned by `_Static_assert`s to POSIX errno values), `kernel/include/thylacine/notes.h` (NOTE_NAME_SNARE_* family), `docs/ERRORS.md` (binding design + rollout) | P6 hardening #3a scripture (user-authorized 2026-05-26). Two coordinated registries: (a) Thylacine-wide errno header pins POSIX-aligned values (`T_E_INVAL == 22 == EINVAL`, etc.) so the pouch boundary-line patch's `[-4095,-2]->errno` passthrough works without translation; (b) `snare:*` thematic note-name family (`snare:segv`/`snare:bus`/`snare:align`/`snare:bti`/`snare:brk`/`snare:ill`/`snare:fpe`) for kernel-synthetic posts on EL0 unhandled fault, replacing the v1.0 FAULT_UNHANDLED_USER kernel extinction with per-Proc termination via `proc_fault_terminate`. **ABI commitments**: errno values are append-only; `snare:` prefix is reserved for kernel-synthetic posters (userspace SYS_POSTNOTE with a `snare:`-prefixed name MUST be rejected at `notes_post`). Adding/removing/renumbering any errno value or `snare:*` name is audit-bearing -- re-validate every syscall surface that emits the changed code + every fault-classification site. **No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in `docs/ERRORS.md` + per-header `_Static_assert`s + the audit + the runtime test suite. Rollout staged: scripture LANDED first (no code); #3a impl uses the new constants; v1.x extends to the structured 64-bit exit_status + per-touch syscall errno upgrade. |
| Memory-model defense-in-depth (F3 + F4 + F5) | `mm/phys.c::phys_init` (RAM cap), `mm/phys.c::alloc_pages` (KP_ZERO barrier), `arch/arm64/mmu.c::clear_page_then_free` + `l3_walk_and_free` / `l2_walk_and_free` / `l1_walk_and_free` / `proc_pgtable_destroy`, `kernel/proc.c::proc_free` (asid_free / pgtable_destroy ordering) | P6 hardening #2 follow-up: closes the P2/P3 findings the Opus auditor surfaced alongside the F1 P1 fix. **F3** (P2) caps the buddy zone at `mem_base + 8 GiB` so `alloc_pages` never hands out a PA the kernel direct map (`l1_directmap[1..8]`) cannot reach -- otherwise the KP_ZERO loop dereferences `pa_to_kva(pa)` past the direct-map cliff and takes an unhandled EL1 translation fault. A uart diagnostic fires only when DTB-reported RAM exceeds the cap (QEMU `-m 2048` default is dormant). To raise the cap, extend `l1_directmap` first. **F4** (P2) zeroes every Proc-pgtable page (L0/L1/L2/L3) before returning it to the buddy free-list, removing the speculative-prefetch info-leak vector of L3-leaf PA-of-user-page bit-patterns surviving into recycled buddy pages; and reverses `proc_free`'s ordering so `asid_free` (inner-shareable TLB flush) runs before `proc_pgtable_destroy`, narrowing the window in which a sibling-CPU speculative walker (carrying stale ASID TLB hints) could reach a recently-recycled sub-table page. Both orderings are correct -- the F4 order is the defense-in-depth choice. **F5** (P3) adds `dsb ish` after the KP_ZERO clearing loop so a second CPU mapping the same PA via a different VA sees zeroes (single-CPU v1.0 dormant; Phase 5+ SMP load-bearing). **No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in this row + the audit (`memory/audit_p6_memory_model_hazards.md` F3 + F4 + F5) + the runtime test suite (599/599 PASS at landing, default + UBSan). |
| 9P client (pipeline restoration, #841) | `kernel/9p_client.c`, `kernel/9p_session.c`, `kernel/9p_transport.c`, `kernel/9p_attach.c`, SrvConn transport boundary (`kernel/srvconn.c` client send/recv + `kernel/9p_srvconn_transport.c`) | Wire protocol, fid lifecycle (I-11), tag uniqueness (I-10), no-lost-wakeup (I-9 per-rpc rendez), flow control, out-of-order completion. **#841 restores committed ARCH §21/§21.10 pipelining (elected-reader, Plan 9 `mountio`) from the R15-c F230 serial regression** -- the single per-client spinlock held ACROSS the blocking `recv` + the 30s per-op deadline that desynced the shared 9P byte stream (root cause of the stalk-3c-d "UBSan flake": corvus's post-pivot X-search `Tgetattr` over the joey-shared Stratum client busy-spins a CPU stratumd needs -> 30s `TSLEEP_TIMEDOUT` -> stream desync + transport ERROR-latch -> corvus exits -> kproc `wait_pid` wrong-pid extinction). As-built: lock NEVER held across `recv`; multi-in-flight tag-demux; per-rpc reply buffer; no per-op timeout (block until reply or ring-EOF; death-interruptible via #811). Public `p9_client_*` API unchanged (dev9p + consumers untouched). **No new spec** per the 2026-05-23 broadening -- but `specs/9p_client.tla` (clean + the 4 buggy cfgs: tag_collision / ooo_match / fid_after_clunk / unbounded) is the pre-commit gate; prose validation in ARCH §21.10 + this row + the audit + the multi-in-flight runtime tests + the #841 UBSan/`forkstorm`/`capbare` repro GREEN across N boots under host load. Preempts the stalk-3c-d close. **As-built close**: the boot-hang root cause was a SEPARATE latent bug the removed 30s deadline had masked -- `kernel/devsrv.c::devsrv_close` honored `kernel_attached` on the SERVER endpoint, suppressing the ring EOF that wakes the no-timeout client (corvus's post-BadFormat Q11 teardown -> joey's Tclunk hung forever); FIXED by skipping teardown ONLY for the kernel-attached CLIENT endpoint (`(c->flag & CSRVCLIENT) && kernel_attached`) -- the SERVER endpoint always tears down (regression `devsrv.kernel_attached_server_close_eofs`, non-vacuous). Focused audit closed 1P1+1P2+3P3: F1[P1] reply-buffer UAF (read/readdir/readlink alias the per-op buffer `client_run` freed before the caller copies out) FIXED via `c->done_reply_buf` deferred-free; F2[P2] DIED-leaked outstanding slot **CLOSED by #845 (Tflush-on-abandon, see the addendum at the end of this row)**; F3/F5 reconciled in §21.10 (send is all-or-nothing-fail not block-on-room; demux-malformation also deaths the session); F4 accepted. Verified: default 704/704 + UBSan 3/3 + smp8 2/2 + capbare 12/12 under load + spec gate (clean + 4 buggy cfgs). **DIRTY-CLOSE RECURSION (the restructure changed a wait/wake protocol): round-2 prosecuted the death/hand-off SMP path round-1 only reasoned about + found F6[P1] (reader-role-loss strands a survivor when a hand-off target's Proc dies before assuming the role -> re-hand-off on the DIED path, be_reader-gated) + F7[P2] (be_reader never cleared on election-race-loss -> busy-spin -> clear before sleeping) + F8[P3] (destroy free now under c->lock), all FIXED; round-3 prosecuted the round-2 fixes + hand-off completeness -> CLEAN 0/0/0/0. CONVERGED clean over 3 rounds. Closed list: `memory/audit_841_closed_list.md`. Owed: a deterministic multi-in-flight/cross-Proc-death unit test (the SMP window the synchronous harness can't produce) -- lands with the A-5b multi-user workload. **#845 Tflush-on-abandon addendum** (`c31e7fd` + close): closes the F2 DIED-leaked slot via 9P Tflush -- the protocol's request-cancellation (Plan 9 mount driver + Linux v9fs). On CLIENT_WAIT_DIED the client sends `Tflush(oldtag=T)` under `c->lock`; T stays reserved (`outstanding[].awaiting_flush`) and is freed ONLY by its `Rflush`, NEVER by a late original reply (the I-10 reuse-race guard -- per 9P oldtag is not reusable until Rflush, so no stray/duplicate reply is mis-attributed to a reused tag). The flush is ownerless (no `inflight[F]`); a survivor's elected reader drains the Rflush via `demux_frame_locked`'s ownerless dispatch (`p9_session_dispatch_rmsg` routes the late-original = consume-without-clear, AND the Rflush = TFLUSH branch frees oldtag + the tail frees the flush tag). Fallback to the pre-#845 path if the flush can't be built/sent (no regression); a failed transport write latches the session dead. New wire `p9_build_tflush`/`p9_parse_rflush`; `struct p9_outstanding += awaiting_flush + flush_oldtag`. stratumd already Rflushes (lp9 + p9 servers) -> Thylacine-only. Audit (Opus R1) **0 P0 + 0 P1 + 1 P2 + 3 P3**: F1[P2] dup-Rflush-on-a-REUSED-flush-tag = the generic "server sends <=1 reply per tag" wire trust assumption (no per-tag wire generation in 9P; v1.0-UNREACHABLE with trusted stratumd/dev9p; the existing `awaiting_flush` guard is already maximal) -> CLOSED-with-justification + v1.x untrusted-server seam; F2[P3] test-strengthen (end-to-end reclaim-then-reuse) FIXED; F3[P3] late-Rwalk-skips-fid_bind divergence DOC; F4[P3] dead `out->kind` FIXED. Tests 704->707 (`9p_wire.tflush_round_trip`, `9p_session.flush_reclaims_both`, `9p_session.late_reply_does_not_free_awaiting_flush`). Matrix: default smp4 + smp8 + UBSan 707/707 + 0 EXTINCTION + login E2E + spec gate (clean + 4 buggy cfgs). Owed (shared with #841): the deterministic multi-in-flight harness exercising the live DIED->Tflush->survivor-reader path -- lands with A-5b. Closed list: `memory/audit_845_closed_list.md`. |
| Pipe wait/wake | `kernel/pipe.c` | Missed-wakeup-freedom (I-9 specialized to pipe two-direction state machine); per-pipe lock + rendez discipline. Modeled in `specs/pipe.tla`. |
| poll | `kernel/poll.c` | Missed-wakeup-freedom across N fds (I-9); register-then-observe + poll-hook list lifetime. Modeled in `specs/poll.tla`. |
| Notes / signals | `kernel/notes.c`, `kernel/devnotes.c`, `kernel/include/thylacine/notes.h`, `kernel/proc.c` (synthetic `child_exit` post in `exits`), `kernel/pipe.c` (synthetic `pipe` post on write-to-closed), `arch/arm64/exception.c` (EL0-return-tail delivery) | Delivery ordering (I-19); async-safety (delivery only at zero-lock EL0-return tail); N-2 consumed-exactly-once across handler + fd-read paths; N-3 in_handler re-entrancy guard; N-4 `kill` non-catchable (multi-thread Proc: `kill` cascades via `proc_group_terminate` -- SYS_EXIT_GROUP, ARCH §7.9.1 / I-24, task #809). **No `specs/notes.tla`** per the 2026-05-23 spec-to-code broadening — prose validation in `notes.h` + the audit + the runtime test suite are the rigor. See ARCH §7.6.1-§7.6.8 for the design + invariants. |
| Capability checks | All syscall entry points | Privilege correctness |
| KASLR / ASLR | `arch/arm64/start.S`, `arch/arm64/kaslr.c` | Entropy quality, layout correctness (I-16) |
| ELF loader | `kernel/elf.c` | RWX rejection, relocation correctness |
| `burrow_attach` / `burrow_detach` | `kernel/syscall.c` handlers, `kernel/burrow.c`, `kernel/vma.c` | Anonymous-memory syscalls (§6.5 Tier 1) — VMA + Burrow refcount lifecycle, VA placement, per-Proc lock, W^X (RW-only) |
| `torpor_wait` / `torpor_wake` | `kernel/torpor.c`, `kernel/syscall.c` handlers, `arch/arm64/uaccess.S` (new `uaccess_load_u32`) | Wait-on-address — no-lost-wakeup (I-9 specialized; register-then-observe under `torpor_lock`), stack-waiter lifetime, uaccess fault routing. **No `specs/futex.tla`** per the 2026-05-23 spec-to-code broadening — prose validation in `torpor.h` + this audit-trigger row is the rigor. |
| `thread_spawn` / `thread_exit` / multi-thread exit | `kernel/thread.c::thread_create_user`, `arch/arm64/context.S::thread_user_trampoline`, `kernel/syscall.c` handlers (`sys_thread_spawn_handler` / `sys_thread_exit_handler` / extended `sys_set_tid_address_handler`), `kernel/proc.c::exits / thread_exit_self / wait_pid`, `arch/arm64/uaccess.S::uaccess_store_u32` | Multi-thread Procs (P6-pouch-threads sub-chunk 9a) — peer Thread spawn shares pgtable_root + ASID + handle table + Territory; eret-to-EL0 trampoline with x0=arg + TPIDR_EL0=tls; clear-child-tid handoff (atomic store + torpor wake) at SYS_THREAD_EXIT + exits(); peer-EXITING gate in exits() (cross-thread shootdown = `SYS_EXIT_GROUP`, ARCH §7.9.1 / I-24, task #809); multi-Thread reap in wait_pid (walks p->threads, on_cpu-spin each, thread_free each). **No `specs/pthread.tla`** per the 2026-05-23 spec-to-code broadening — invariants validated by prose in `docs/reference/81-sys-thread.md` + the audit + the 4 new tests + the `/thread-probe` E2E. |
| pouch pthread boundary-line | `usr/lib/pouch/patches/0004-pouch-pthread.patch` (the 8-file boundary-line patch against vendored musl), `usr/pouch-hello/pouch-hello-threads.c` (the proving binary), `usr/joey/joey.c` (smoke wiring), `tools/build.sh` (build wiring) | The pouch-side pthread layer (P6-pouch-threads sub-chunk 9b) — the boundary-line that sits between musl's portable upper half and the kernel substrate from 9a/8/7a. Eight files: `bits/syscall.h.in` adds Thylacine extension numbers 39/40/41/42; `pthread_impl.h::__wake`/`__futexwait` + `__timedwait.c::__futex4_cp` + `__wait.c` + `pthread_barrier_wait.c` route through torpor; `pthread_cond_timedwait.c::unlock_requeue` drops FUTEX_REQUEUE; `pthread_create.c` retargets `__clone`→SYS_THREAD_SPAWN + `start()` registers `&__thread_list_lock` as clear_child_tid + the for(;;)SYS_exit loops become SYS_THREAD_EXIT; `aarch64/__unmapself.s` does SYS_BURROW_DETACH+SYS_THREAD_EXIT. The shared `&__thread_list_lock` clear_child_tid target (Linux CLONE_CHILD_CLEARTID equivalent on a userspace spinlock). SP at SYS_THREAD_SPAWN is 16-aligned-DOWN. **No `specs/pthread.tla`** per the 2026-05-23 spec-to-code broadening — invariants validated by prose in `docs/reference/82-pouch-pthread.md` + the audit + `/pouch-hello-threads` (5 workers × 1000 mutex-protected increments, joined). |
| argv pass-through to spawn | `kernel/syscall.c` (new SYS_SPAWN_* with argv buffer OR extended SYS_SPAWN_WITH_PERMS), `usr/lib/libt/include/thyla/syscall.h` (libt wrapper), `usr/joey/joey.c` (richer argv construction), pouch arm if applicable | P6-pouch-stratumd-boot sub-chunk 16b-α — the first kernel surface to expose argv to spawned children (existing SYS_SPAWN family inherits only `argv[0] = name`). Invariants: argv buffer copy bounded by SYS_SPAWN_ARGV_MAX (new constant); per-element NUL-termination enforced; argv buffer lifetime ends at the SYS_SPAWN_ARGV thunk's `exec_setup` (transient kernel-side; never visible to a third Proc); no smuggled handles in argv (strings only). **No new spec** per the 2026-05-23 spec-to-code broadening — prose validation in this row + the audit + the runtime test. Per POUCH-DESIGN §14 row 16b-α. |
| stratumd HW-cap spawn | `usr/joey/joey.c` (calls `t_spawn_with_perms` granting `CAP_HW_CREATE` to stratumd), `kernel/proc.c::rfork_with_caps` exercised with CAP_HW_CREATE | P6-pouch-stratumd-boot sub-chunk 16b-β — the first non-kproc-direct user of CAP_HW_CREATE. Invariants: I-2 capability-monotonic-reduction holds at grant time (CAP_HW_CREATE flows kproc → joey → stratumd; no expansion); I-5 hardware-handle non-transferability persists (stratumd's MMIO/IRQ/DMA handles still cannot move via 9P or any other transfer surface); cap-broadening attack surface bounded by stratumd's known role (the FS server owning its disk per the 16b stratumd-as-driver decision, POUCH-DESIGN §14 row 16b). **No new spec** per the 2026-05-23 spec-to-code broadening — prose validation in `docs/reference/86-pouch-stratumd-boot.md` + this row + the audit + the runtime test. |
| stratumd virtio-blk driver arm | (Stratum branch `thylacine-pouch-arm`) `src/io/bdev_thylacine.c` (~500-800 LOC port of the userspace Rust `usr/virtio-blk-rw` driver into Stratum's stm_bdev abstraction); pouch-side `usr/lib/pouch/patches/*` extension exposing SYS_MMIO_CREATE/SYS_MMIO_MAP/SYS_IRQ_CREATE/SYS_IRQ_WAIT/SYS_DMA_CREATE/SYS_DMA_MAP via `bits/syscall.h.in` | P6-pouch-stratumd-boot sub-chunk 16b-β — block-device I/O ownership transferred to stratumd per the stratumd-as-driver decision (POUCH-DESIGN §14 row 16b). VirtIO 1.2 §3.1.1 init state machine (RESET → ACK → DRIVER → DeviceFeatures → DriverFeatures → FEATURES_OK readback → DRIVER_OK); virtqueue descriptor-chain correctness; CPU-visibility of device-written DMA bytes under Normal-WB cache attribute; queue-0 reservation for virtio-blk; CAP_HW_CREATE held exclusively by stratumd after grant; pool durability across stratumd lifecycles (write-through to QEMU disk.img survives reboots). **No new spec** per the 2026-05-23 spec-to-code broadening — prose validation in `docs/reference/86-pouch-stratumd-boot.md` + this row + the audit + the runtime test suite (the `/pouch-stratumd-boot-e2e` proving binary). |
| native fstat + lseek surface | `kernel/include/thylacine/syscall.h` (SYS_FSTAT=50 + SYS_LSEEK=51 + struct t_stat 72-byte ABI + T_SEEK_*), `kernel/include/thylacine/dev.h` (Dev vtable `.stat_native` slot), `kernel/devramfs.c` (devramfs_stat_native impl + walk reuse-nc shape), `kernel/syscall.c` (sys_fstat_handler + sys_lseek_handler + partial-walk reject in sys_walk_open_handler), `kernel/joey.c` (kproc territory chroot to devramfs root before joey rfork), `usr/lib/libt/include/thyla/syscall.h` (t_fstat + t_lseek + libt struct t_stat mirror), `usr/lib/pouch/patches/0010-pouch-fstat-lseek.patch` (the 3-file boundary-line patch: bits/syscall.h.in + src/stat/fstat.c + src/fcntl/open.c) | P6-pouch-stratumd-boot sub-chunk 16b-γ-syscalls — the POSIX fstat()/lseek() + open() arms required by stratumd's stm_keyfile_load (open + fstat + read + lseek + read sequence). Invariants: SYS_FSTAT rights gate (RIGHT_READ on KOBJ_SPOOR; rejects KOBJ_SRV); struct t_stat layout pinned by `_Static_assert`s on every field offset (so a future kernel field add cannot land without bumping the ABI assertions); SYS_LSEEK overflow check on SEEK_CUR/SEEK_END (INT64_MAX/MIN bounds); SYS_LSEEK rejects new_offset < 0 (POSIX EINVAL); kernel partial-walk reject (`w->nqid != nname`) closes the missed-walk-returns-source-fd vector; devramfs walk now respects sys_walk_open's "return nc" contract (pre-fix it allocated its own spoor and walk_open silently failed). Pouch-side: open() → openat() forwarding closes the SYS_open=0xFFFF bypass; fstat() translates t_stat → musl's struct stat field-by-field. **No new spec** per the 2026-05-23 spec-to-code broadening — prose validation in `docs/reference/87-pouch-fstat-lseek.md` + this row + the audit + joey's `/system.key` probe (walk + fstat + lseek SEEK_END/SEEK_SET round-trip; runs on every boot). |
| pouch abort -> _Exit override | `usr/lib/pouch/patches/0011-pouch-abort.patch` (overrides musl's `src/exit/abort.c` to `_Exit(127)` directly), `usr/joey/joey.c` (retry+drain+reap workaround that depends on the override for clean stratumd exit semantics) | P6-pouch-stratumd-boot sub-chunk 16b-γ-mount-close — the documented v1.x extension landed early because the bdev read-I/O fix unblocks downstream stratumd mount code paths that exercise `assert()` / `abort()`. Upstream musl's `abort()` reaches `a_crash()` (a deliberate NULL deref) as a last-resort kill; at v1.0 the kernel's FAULT_UNHANDLED_USER policy extincts the entire boot rather than terminating the offending Proc. Invariants: WIFEXITED + WEXITSTATUS == 127 replaces Linux's WIFSIGNALED + WTERMSIG == SIGABRT (behavioral delta documented in the patch header + `docs/reference/83-pouch-signals.md`); no raise(SIGABRT) means SIGABRT handlers do NOT see the synthesized note (the override is incompatible with programs that need to run cleanup in a SIGABRT handler — those need v1.x sigaction expansion); `_Exit(127)` is async-signal-safe (the only contract it must hold). **Multi-thread hazard** (audit-bearing v1.x lift per `audit_p6_pouch_stratumd_boot_16b_gamma_mount_close_closed_list.md::F1`): `_Exit` routes through `__NR_exit_group` → kernel `SYS_EXITS`, which extincts the kernel if the Proc has live peer threads. Safe-use envelope: abort() reachable only pre-thread-spawn OR post-thread-join. The fix is SYS_EXIT_GROUP -- now DESIGNED (scripture-first: ARCH §7.9.1 + invariant I-24; task #809, in progress): a new kernel syscall that cascades peer-thread termination + the cross-thread shootdown (flag-and-self-terminate at the EL0-return checkpoint; also closes the documented `kill -> -EIO in multi-thread Proc` from 13b R1-F9). Until the impl lands, the safe-use envelope above holds. Hard prerequisite for 16c (satisfied). **No new spec** per the 2026-05-23 spec-to-code broadening — prose validation in `docs/reference/86-pouch-stratumd-boot.md` 16b-γ-mount-close section + this row + the audit. |
| Stratum bdev_thylacine rights mirror | (Stratum-side `thylacine-pouch-arm` branch) `src/block/bdev_thylacine.c` `T_RIGHT_SIGNAL` constant | P6-pouch-stratumd-boot sub-chunk 16b-γ-mount-close — narrow-scope cap-bits row to flag the rights-mirror discipline. Invariant: every Stratum-side (or pouch-side) reproduction of `kobj-rights` bits MUST match `kernel/include/thylacine/handle.h`'s `RIGHT_*` definitions. The bug fixed in 16b-γ-mount-close was a mirror-drift: `bdev_thylacine.c` defined `T_RIGHT_SIGNAL = (1u << 3)` (collision with the kernel's `RIGHT_TRANSFER`) instead of `(1u << 5)`. Cross-project rights-mirror review should be done whenever the kernel adds / renumbers a RIGHT_* bit. **No new spec** — prose validation in `docs/reference/86-pouch-stratumd-boot.md` 16b-γ-mount-close section + this row + the audit. |
| pouch mallocng assert -> _Exit override | `usr/lib/pouch/patches/0012-pouch-mallocng-crash.patch` (overrides mallocng's `src/malloc/mallocng/glue.h::assert` macro to `_Exit(127)` instead of `a_crash()` = deliberate NULL deref) | P6 hardening #3b (parallels 0011-pouch-abort.patch). Closes the mallocng-internal-assert leg of the userspace-fault-to-kernel-extinction pathway: mallocng's hot-path assertions (`alloc_slot::enframe::assert(!p[-4])`, `get_nominal_size::assert(!end[-5])`, ~10 others) previously routed through `a_crash()` (NULL deref) and triggered FAULT_UNHANDLED_USER -> kernel extinction. After this patch they `_Exit(127)` cleanly, joey reaps with rc=127. Same multi-thread hazard envelope as 0011 (safe in single-thread contexts; the SYS_EXIT_GROUP fix -- ARCH §7.9.1 / I-24, task #809 -- closes for both uniformly). Behavioral delta: a sigaction(SIGABRT) handler doesn't see the assertion (matches 0011's behavior; same v1.x sigaction-surface lift). **No new spec** — prose validation in `docs/reference/86-pouch-stratumd-boot.md` hardening #3b row + the patch header + this row. |
| Thylacine mkfs RNG seed pinning | `tools/build.sh::build_stratum_pool_fixture` (THYLACINE_MKFS_SEED + THYLACINE_MKFS_PRESERVE env vars; auto-generates and logs a hex64 seed every build); paired Stratum-side: `(thylacine-pouch-arm)` `src/cmd/stratum-mkfs/run.c` `--seed HEX64` flag | P6 hardening #4. Pinning point for content-sensitive AEGIS-256 / mallocng corruption inside stratumd's mount path: every build now records its seed in the log; rerunning with that seed (or PRESERVE=1 on saved pool.img + system.key) reproduces. Invariant: --seed pins UUID derivation only; full byte reproduction requires PRESERVE=1 OR pinning the keyfile separately (libsodium's randombytes_buf during format adds per-run entropy). NOT a security-bearing surface (the seed is debug/forensic). |
| 9P-srvconn transport adapter | `kernel/9p_srvconn_transport.{c,h}` (new; parallel to `kernel/9p_spoor_transport.{c,h}`); call site in `sys_attach_9p_srv_handler` | P6-pouch-stratumd-boot 16c -- the second `p9_transport_ops` backend (after `p9_spoor_transport`). Wraps a byte-mode `struct SrvConn`'s `c2s` (kernel-client -> server) + `s2c` (server -> kernel-client) byte rings into the `send` / `recv` / `close` vtable that `p9_transport_round_trip` drives. **Invariants**: the wrapped SrvConn MUST be byte-mode (`__atomic_load_n(&cn->byte_mode, ACQUIRE) == true`) -- 9P-mode SrvConns already have an embedded kernel-owned `p9_client` driving Tread/Twrite on `client_fid`, and a second `p9_client` over the same rings would race. The adapter takes a `srvconn_ref` at init and `srvconn_unref`s at close (mirrors `p9_spoor_transport`'s owns_spoors=true discipline; SrvConn refcount is the lifetime authority). The adapter's `recv` blocks on `s2c` via `srvconn_client_recv` (deadline-bounded by `srvconn_set_client_deadline`); `send` writes `c2s` via `srvconn_client_send` (non-blocking; SRVCONN_RING_CAP is sized to a full msize frame so a synchronous single-frame-in-flight client never blocks). **No new spec** per the 2026-05-23 spec-to-code broadening -- the framing + ordering invariants are inherited from `kernel/9p_session.c` + `kernel/9p_transport.c` (audited Phase 5); prose validation in the new `docs/reference/88-pouch-stratumd-boot-16c.md` + this row + the audit + a new kernel-internal test. |
| `SYS_ATTACH_9P_SRV` syscall | `kernel/syscall.c::sys_attach_9p_srv_handler` (new; parallel to `sys_attach_9p_handler`), `kernel/include/thylacine/syscall.h::SYS_ATTACH_9P_SRV` (new number), `usr/lib/libt/include/thyla/syscall.h::t_attach_9p_srv` | P6-pouch-stratumd-boot 16c -- the bridge from a client-held `KObj_Srv` connection handle to a mountable `KOBJ_SPOOR` dev9p root. Composes: SrvConn handle lookup -> byte-mode gate (-1 if 9P-mode -- the embedded `p9_client` reservation prevents double-driving the rings) -> kmalloc `p9_srvconn_transport` adapter -> `p9_attached_create` (drives Tversion + Tattach) -> `p9_attached_root_spoor` -> handle_alloc as KOBJ_SPOOR with R|W|TRANSFER. **Invariants**: rights gate (`RIGHT_READ` + `RIGHT_WRITE` on the SrvConn handle -- the kernel 9P client writes Twalk/Tread/Twrite and reads Rwalk/Rread/Rwrite); aname bounds (`SYS_ATTACH_ANAME_MAX` same as SYS_ATTACH_9P); n_uname u32 bound check; full failure-path rollback (every kmalloc + every spoor / srvconn ref taken is released on any error). Adopts the same `p9_attached_install_transport` ref-discipline as SYS_ATTACH_9P. **The byte-mode gate matters**: a 9P-mode SrvConn (which corvus mints) is reserved for the kernel-owned p9_client's Tread/Twrite stream -- SYS_ATTACH_9P_SRV rejects it because a second 9P client over the same rings would interleave frames. Pouch sockets (sub-chunk 12) post BYTE-mode via `SYS_POST_SERVICE_BYTE`, so stratumd's `/srv/stratum-fs` listener naturally produces byte-mode SrvConns. **stalk-3b-β update (`46ff378` D + the 3b-β-E close `d6724a0`):** the source endpoint moved `KObj_Srv` -> a `KOBJ_SPOOR` CSRVCLIENT conn Spoor (C1 `42ce2e0`; reached by open=connect on a byte-mode `/srv` service via `kernel/stalk.c` -> `kernel/devsrv.c::devsrv_open_connect`, NOT SYS_SRV_CONNECT). The handler lookup is now KOBJ_SPOOR + CSRVCLIENT flag + `devsrv_conn_of`; the composition is the SHARED `kernel/9p_attach.c::srvconn_attach_dev9p_root` (which also drives `devsrv_open_connect`'s 9p-mode path). The embedded per-SrvConn `p9_client` is RETIRED (D `46ff378`) -- the byte-mode gate no longer reserves "the embedded client" (there is none); it enforces a clean separation: a 9P-mode service is connected via open=connect directly (yielding a dev9p root), so attaching one is meaningless and rejected. **3b-β-E F1 (`devsrv.c`):** `srvconn_attach_dev9p_root` calls `srvconn_set_kernel_attached` once the adapter commits, and `devsrv_read`/`devsrv_write`'s CSRVCLIENT branches MUST honor it (`kernel_attached -> -1`) -- the no-direct-I/O-on-the-rings guard the KOBJ_SRV r/w arms carry, which had to FOLLOW the endpoint from KOBJ_SRV to KOBJ_SPOOR (regression test `devsrv.kernel_attached_io_refused`). **No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in `docs/reference/88-pouch-stratumd-boot-16c.md` + `docs/reference/70-devsrv.md` + `memory/audit_stalk3b_closed_list.md` + this row + the audit + kernel + integration tests. |
| `SYS_PIVOT_ROOT` syscall + `territory_pivot_root` | `kernel/syscall.c::sys_pivot_root_handler` (new), `kernel/territory.c::territory_pivot_root` (new core; companion to existing `territory_chroot`), `kernel/include/thylacine/syscall.h::SYS_PIVOT_ROOT` (new number), `usr/lib/libt/include/thyla/syscall.h::t_pivot_root` | P6-pouch-stratumd-boot 16c -- the long-running-Proc root swap that joey performs LAST in bringup to flip its territory from the bootstrap devramfs root to the disk-backed Stratum FS root. Closes the v1.x note in `usr/joey/joey.c:293-304` ("v1.x adds SYS_UNCHROOT or a proper pivot_root"). **Semantics** (mirrors Linux `pivot_root(2)` minus the put_old argument -- the displaced root is simply unreferenced): under the Proc's territory lock, `spoor_ref(new_root_spoor)` then atomically replace `territory->root_spoor` with the new ref then `spoor_clunk(old_root_spoor)`. Idempotent on same-spoor (returns 0 without bumping). **Why distinct from re-`SYS_CHROOT`**: the existing `territory_chroot` allows the displaced root's ref to drop, but it is documented as "subsequent SYS_CHROOT replaces it"; a NEW syscall + core makes the long-running-Proc usage explicit and audit-trackable, and lets the per-Proc semantics evolve (e.g., post-pivot `/sbin` bind-survivor in v1.x) without re-litigating SYS_CHROOT's contract. **Rights gate**: source fd is `KOBJ_SPOOR` with `RIGHT_READ`. **Invariants**: I-1 (the new root_spoor is a tree the caller already has read rights on -- isolation preserved); I-3 (mount DAG is per-Territory; a pivot replaces one root without introducing cycles); territory-mount-refcount consistency (the displaced root's mounts in the per-Proc mount table are NOT carried over -- mount entries are root-relative and the caller is responsible for re-establishing any binds against the new root; for joey's v1.0 usage that re-establishment is none, since pivot is the last bringup step). **No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in this row + `docs/reference/88-pouch-stratumd-boot-16c.md` + the audit + kernel + boot-path integration tests. |
| `kernel_attached` SrvConn gate (16c-integration) | `kernel/srvconn.{c,h}::srvconn_set_kernel_attached` + `srvconn_is_kernel_attached`; `struct SrvConn::kernel_attached` (new field); `kernel/handle.c::handle_release_obj` KOBJ_SRV branch; `kernel/syscall.c::sys_attach_9p_srv_handler` setter call (at end, after all failure paths); `kernel/9p_srvconn_transport.c::srvconn_transport_close` (now does srvconn_teardown + srvconn_unref) | P6-pouch-stratumd-boot 16c-integration (`457f22d`). Closes the CORVUS-DESIGN.md section 6.2 "close handle = close connection" hazard for kernel-attached SrvConns: when the kernel 9P client wraps a byte-mode SrvConn, the c2s + s2c rings are LOAD-BEARING -- a userspace t_close on the now-redundant KObj_Srv handle MUST NOT EOF the rings. Pre-fix this was the smoking-gun bug: joey's idiomatic `t_close(sd_srv_fd)` post-attach triggered `srvconn_teardown` via handle_close, EOFing c2s before the very first Twalk could land. **Mechanism**: new one-way flag on `struct SrvConn` (false -> true); atomic store-release set by `srvconn_set_kernel_attached(cn)` at the END of `sys_attach_9p_srv_handler` (after all failure paths cleared, before the syscall returns the attach_fd to userspace); atomic load-acquire read by `handle_release_obj`'s KOBJ_SRV branch. If true: SKIP `srvconn_teardown`; only `srvconn_unref`. Teardown migrates to the adapter's `transport.close` (which now does `srvconn_teardown` + `srvconn_unref`); this fires at `p9_attached_destroy` time, when the LAST KOBJ_SPOOR handle referencing the attach session drops. **Invariants**: I-9 (no wakeup is lost; the release-acquire pairing on the atomic flag ensures a cross-CPU userspace close either sees the flag set OR the syscall has not yet returned the attach_fd -- userspace cannot close a handle it has not yet received); I-2 / I-6 (rights monotonicity unaffected -- the flag adds no privilege); teardown idempotency (the non-kernel-attached path is unchanged; `srvconn_teardown` is safe-to-call-twice via the `chan_set_eof` spin_lock pattern). **Regression test**: `kernel/test/test_9p_srvconn_transport.c::kernel_attached_skips_teardown_on_handle_close` (two-part: control teardown + kernel-attached no-teardown). **No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in `srvconn.h::kernel_attached` field comment + the new test + this row + the audit. |
| Host-side pool populate via existing `stratumd + stratum-fs` (host build infra) | `tools/build.sh::build_stratum_pool_fixture` (orchestrates stratumd start, stratum-fs writes for each corpus file, stratumd stop); Stratum-side: NO new code -- uses already-shipped `~/projects/stratum/v2/build/.../stratumd` + `stratum-fs` binaries unchanged | P6-pouch-stratumd-boot 16c -- the host-side "installer" that pre-populates `pool.img` with Thylacine's boot binary corpus (joey, corvus, stratumd, pouch-hello-*, /system.key) before QEMU boot. Equivalent in role to a Linux distro's installer copying squashfs contents onto a fresh disk, run at HOST build time instead of at runtime. Without it, joey cannot pivot to the disk-backed FS (the binaries would be unreachable post-pivot, breaking every joey spawn that resolves a `/sbin/*` or `/pouch-hello-*` path from devramfs). **The 'installer' is shell orchestration of existing Stratum v2 tools**: `stratum-mkfs` (already wired) creates the empty pool; `stratumd` is started in the background listening on a temp Unix socket; `stratum-fs write` (the audited 9P-CLI client subcommand that does `lcreate + buffered Twrite + auto-fsync`) writes each corpus file under its target path; stratumd is shut down. NO new Stratum-side code; NO Stratum-side audit-bearing surface; the populate path exercises the already-audited stratumd Twrite/Tcreate + the already-audited stratum-fs 9P-client. **Invariants**: the populate is HOST-side only (read-only access to the host file tree; the only Thylacine-side artifact is the pool.img bytes); it does NOT modify Thylacine kernel state or affect runtime behavior beyond the static contents of pool.img; the populated FS is a snapshot of the host directory at build time (no incremental update -- pool.img is rebuilt from scratch each invocation). v1.x runtime installer (a Thylacine `/sbin/installer` that runs in an alternate boot path) would PROVE the same flow via the runtime 9P write surface from inside Thylacine -- 16c v1.0 ships the bake-at-build-time expedient that reuses the SAME Stratum-side Twrite/Tcreate code paths. NOT audit-bearing on the Thylacine kernel surface (host build infra). **No new spec** -- prose validation in `docs/reference/86-pouch-stratumd-boot.md` "### The 16c live-medium + host-bake + pivot design" section + this row. |
| FS-mutation syscalls (create / fsync / readdir) | `kernel/syscall.c` (`sys_walk_create_handler` / `sys_fsync_handler` / `sys_readdir_handler`), `kernel/dev9p.c` (real `dev9p_create` -- today a stub -- + new `dev9p_fsync` / `dev9p_readdir`), `kernel/devramfs.c` (create/fsync/readdir impls), `kernel/include/thylacine/dev.h` (new `.fsync` / `.readdir` vtable slots), `kernel/include/thylacine/syscall.h` (`SYS_WALK_CREATE = 54` / `SYS_FSYNC = 55` / `SYS_READDIR = 56` + ABI), `usr/lib/libt/include/thyla/syscall.h` + `usr/lib/libthyla-rs` wrappers | Convergence-detour FS foundation (IDENTITY-DESIGN.md section 9.2), pulled ahead of A-1b corvus persistence per the 2026-05-28 sequencing decision (real persistence needs create+fsync+readdir; none existed at A-1a). The kernel 9P client already implements the wire half (`p9_client_lcreate`/`mkdir`/`fsync`/`readdir`) and `Dev` already has a `.create` slot -- this is syscall wrappers + the real `dev9p_create` + two new vtable slots, NOT new protocol. **The create + write + fsync path is the AEGIS/mallocng-adjacent surface from Phase 6 -- prosecute hard.** Invariants: rights gates (`RIGHT_WRITE` on parent for create + on fd for fsync; `RIGHT_READ` for readdir); single-component name bounds + `/`/`\0` reject; `perm` reserved-`DM*`-bit reject; `DMDIR`-fold dispatch (mkdir vs lcreate); create carries caller `primary_gid` into the 9P gid field (full owner attribution = A-2c/A-2d/A-3 SEAM); `readdir` buffer bounds (<= `SYS_RW_MAX`) + Spoor-offset advance + EOD=0; durability "write-then-fsync" contract on the integrity FS; dev9p fid lifecycle on the failed-create path (no UAF / no fid leak). Per-file rwx ENFORCEMENT (no id bypass, I-22) is A-2d, NOT this surface -- there is nothing to bypass yet. **No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in IDENTITY-DESIGN.md section 9.2 + this row + the audit + the runtime test suite. Split FS-alpha (create) / FS-beta (fsync+readdir) / one focused audit. |
| FS-mutation syscalls (rename / unlink) -- FS-gamma | `kernel/syscall.c` (`sys_rename_handler` / `sys_unlink_handler`), `kernel/dev9p.c` (new `dev9p_rename` -> `p9_client_renameat`, `dev9p_unlink` -> `p9_client_unlinkat`), `kernel/include/thylacine/dev.h` (new `.rename` / `.unlink` vtable slots, NULL-permitted like `.fsync`/`.readdir`), `kernel/include/thylacine/syscall.h` (`SYS_RENAME = 57` / `SYS_UNLINK = 58` + `SYS_UNLINK_REMOVEDIR = 0x200` + ABI), `usr/lib/libt/include/thyla/syscall.h` + `usr/lib/libthyla-rs` wrappers | Convergence-detour FS-gamma (IDENTITY-DESIGN.md section 9.3), pulled ahead of A-1b (user-chosen 2026-05-29) to give corvus's identity-DB persistence the classic write-tmp + fsync + atomic rename-swap substrate instead of an append-only log -- and rename/unlink are owed for the A-2 coreutils (mv/rm/rmdir) regardless. **The kernel 9P client already implements the wire half (`p9_client_renameat`/`unlinkat`) but those functions are IMPLEMENTED-YET-UNEXERCISED -- no syscall has ever driven them -- so this audit is their FIRST end-to-end prosecution; same AEGIS/mallocng-adjacent write-path class as section 9.2 -- prosecute hard.** Invariants: rights gates (`RIGHT_WRITE` on EVERY directory fd mutated -- BOTH for rename); single-component name bounds + `/`/`\0`/`.`/`..` reject on every name; the **cross-Dev + same-session reject** (rename runs DIRECTLY on the two looked-up dir Spoors -- NO clone-walk, since renameat/unlinkat operate on the dirfid by name without transitioning it, unlike create's Tlcreate -- and requires the SAME Dev, with dev9p_rename adding the same-p9_client-session check; rejected at the handler before any Dev op; a 9P renameat is within one session); `flags` validated against `{0, SYS_UNLINK_REMOVEDIR}` (rmdir-vs-unlink mode select; other bits reject); the no-fid-leak property (rename/unlink BORROW the caller's dir fid and allocate NO transient fid, so the §9.2 failed-create UAF/fid-leak class structurally cannot arise here); rename's POSIX atomic-replace semantics (dest replaced atomically -- A-1b's DB-swap relies on it); the rename-swap durability detail (corvus does a post-rename `SYS_FSYNC` on the parent dir fd as the metadata barrier -- whether Stratum honors Tsync-on-a-directory is validated end-to-end by the A-1b cross-reboot persistence test). The pre-existing `void (*remove)(struct Spoor *)` Plan 9 slot is LEFT AS-IS (wrong shape: no name, no error return, target-not-parent; `SYS_UNLINK` uses the new `.unlink`). devramfs leaves both slots NULL (ramfs mutation deferred; load-bearing target is dev9p). **No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in IDENTITY-DESIGN.md section 9.3 + this row + the audit + the runtime test suite. |
| File metadata: owner/group + chmod/chown (A-2a) | `kernel/include/thylacine/syscall.h` (`struct t_stat` 72 -> 80 with `uid`@72 + `gid`@76 + asserts; `SYS_WSTAT = 59`; `T_WSTAT_MODE/UID/GID` + `T_WSTAT_MODE_MASK`), `kernel/include/thylacine/dev.h` (new NULL-permitted `.wstat_native` slot), `kernel/dev9p.c` (`dev9p_stat_native` -> `p9_client_getattr`; `dev9p_wstat_native` -> `p9_client_setattr`; `T_WSTAT_* == P9_SETATTR_*` static_asserts), `kernel/devramfs.c` (`devramfs_stat_native` stamps `PRINCIPAL_SYSTEM`/`GID_SYSTEM`), `kernel/syscall.c` (`sys_wstat_handler` + `spoor_wstat_native` + dispatch), `usr/lib/libt/include/thyla/syscall.h` (`t_stat` mirror +uid/gid + `t_wstat`/`t_chmod`/`t_chown`), `usr/lib/libthyla-rs` (`Metadata::uid()/gid()` + `t_wstat`), `usr/lib/pouch/patches/0010-pouch-fstat-lseek.patch` (80-byte `t_stat` + `st_uid`/`st_gid` translate), `usr/joey/joey.c` (`/system.key` A-2a reject-path probe) | Convergence-detour A-2a (IDENTITY-DESIGN.md section 9.5). The chmod/chown + owner/group READ+WRITE MECHANISM the kernel rwx layer (A-2d) reads. **The kernel 9P client already implements the wire half (`p9_client_getattr`/`setattr`) but those were IMPLEMENTED-YET-UNEXERCISED -- no syscall had driven them -- so this audit is their FIRST end-to-end prosecution; same AEGIS/mallocng-adjacent metadata-write-path class -- prosecute hard.** Invariants: `SYS_WSTAT` rights gate (`RIGHT_WRITE` on `KOBJ_SPOOR`; rejects `KOBJ_SRV`); mask sanity (`valid != 0`, no reserved bit); mode hygiene (9 rwx bits ONLY -- setuid/setgid/sticky + any bit outside `0777` REJECTED, no-setuid per section S5); uid/gid `INVALID`-sentinel reject; `struct t_stat` layout pinned by `_Static_assert`s on size + every offset (a field add cannot land without bumping the asserts) -- AND the pouch `0010` patch's hand-rolled `t_stat` MUST grow to 80 in lockstep (the kernel writes 80 bytes; a 72-byte stack buffer is an overflow); `T_WSTAT_* == P9_SETATTR_*` pinned so the mask maps with no translation; the no-fid-leak property (`wstat_native` BORROWS the caller's fid, allocates none -- the 9.2 failed-create UAF class structurally cannot arise); `dev9p_stat_native` maps the `Tgetattr` response uid/gid (server/connection identity; A-3 completes per-user attribution); devramfs `.wstat_native` NULL -> chmod on the read-only boot FS returns -1. **Per-file rwx ENFORCEMENT (who may chmod/chown -- owner-only chmod, CAP_HOSTOWNER chown; no id bypass, I-22) is A-2d, NOT this surface -- A-2a is the unenforced mechanism, I-22 stands because nothing enforces rwx yet.** **No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in `docs/reference/99-fs-permission.md` + IDENTITY-DESIGN.md section 9.5 + this row + the audit + the runtime test suite (dev9p getattr/setattr loopback + devramfs sentinels + joey `/system.key` reject-path probe). |
| Kernel rwx enforcement layer (A-2d) | `kernel/syscall.c` (`perm_check` insertions: `sys_walk_open_handler` [X on src + R/W on target], `sys_walk_create_handler` [W+X on parent], `sys_wstat_handler` [chmod/chown/chgrp policy]), new `kernel/perm.c` (or fold into `kernel/proc.c`) `perm_check` + `proc_in_group`, `kernel/dev9p.c` (`dev9p_stat_native` gated on the `Rgetattr` valid mask -- closes A-2a F2) | Convergence-detour A-2d (IDENTITY-DESIGN.md sections 3.7.1 + 9.6; privilege model voted 2026-05-30). **The FIRST real exercise of I-22's enforcement obligation -- privilege boundary; prosecute hard.** Linux-VFS model (kernel enforces rwx at the FS chokepoint; Stratum enforces dataset-scope ONLY, not file rwx -- section 3.7). Invariants: owner-first POSIX algorithm (owner bits authoritative even when group/other grant more; no group/other leak when owner bits deny); group membership = `primary_gid` OR `supp_gids[0..count)` (`GID_INVALID` never a member); enforcement points = walk (X on the searched dir; per-component since walk is one-name-per-call), open (R/W per omode; `O_PATH` exempt from R/W but NOT from the path X-search), create (W+X on parent), wstat (the policy); read/write NOT re-checked (open-time snapshot); the handle RIGHT (capability axis) AND the rwx check (identity axis) are BOTH required + orthogonal. **`CAP_HOSTOWNER` is the unified v1.0 fs-admin authority** (DAC-override + chmod-any + chown-any + chgrp-any) -- a capability (elevation-only, console-gated, never rfork-able), NEVER an identity, so **no `principal_id` -- not even `PRINCIPAL_SYSTEM` -- bypasses (I-22 preserved)**; owner keeps owner|self-group chmod + chgrp-to-own-group; **no-give-away chown** (`CAP_HOSTOWNER` only). Fail-closed on a NULL `stat_native` Dev. **Honest scope**: per-principal-real on devramfs (system-owned world-r/x -> boot chain owns everything, no brick; a `CAP_SET_IDENTITY`-spawned non-system child is denied write -- testable NOW, not gated on login A-5). **dev9p enforcement DEFERRED to A-3** (user-signed-off 2026-05-30): ground truth shows uniform dev9p enforcement bricks the boot -- the host-bake stamps pool entries owned by the host uid (0644/0755), and the `PRINCIPAL_SYSTEM` boot chain (no `CAP_HOSTOWNER`) as *other* cannot write the pool, so the post-pivot creates (`/var/lib/corvus`, `susan`) would be denied. Gated by a new `Dev.perm_enforced` flag (devramfs=true, dev9p=false; the A-3 activation is a one-line flip); dev9p stays handle-RIGHT-gated only at v1.0 (no regression). The wstat ownership-change policy is also `perm_enforced`-gated -> dormant + unit-tested at v1.0, activates with dev9p at A-3. Closes A-2a audit F2. A-2b create-check folds in; A-2c mount-cape stays a seam; A-4 splits a finer `fs-admin` clearance (`CAP_DAC_OVERRIDE`+`CAP_CHOWN`) -- additive. **No new spec** per the 2026-05-23 broadening -- prose validation in IDENTITY-DESIGN.md sections 3.7.1 + 9.6 + `docs/reference/99-fs-permission.md` + this row + the audit + the runtime test suite (devramfs deny/allow via CAP_SET_IDENTITY; `perm_check`/`proc_in_group` unit tables; wstat policy paths; dev9p loopback; joey `/system.key` boot regression). |
| A-3: 9P identity presentation + dev9p enforcement activation | `usr/lib/pouch/patches/0006-pouch-sockets.patch` (SO_PEERCRED shim: `ucred.uid = info.principal_id` + `ucred.gid = info.primary_gid` -- was `0`/`0`, a pre-A-1a stub), `kernel/dev9p.c` (`.perm_enforced = true` flip), `kernel/syscall.c` (`sys_walk_open_handler` **F1**: derive the KOBJ_SPOOR handle rights from `omode` [OREAD->RIGHT_READ, OWRITE->RIGHT_WRITE, ORDWR->R+W, OEXEC->RIGHT_READ, +OTRUNC->+RIGHT_WRITE; `T_OPATH` keeps the A-1.7/F5 born-R+W navigation base, no TRANSFER; normally-opened keeps RIGHT_TRANSFER]; `sys_rename_handler` [BOTH dirs] + `sys_unlink_handler` **F2**: add `perm_check(parent, PERM_W+PERM_X)` gated on `dev->perm_enforced`; `sys_attach_9p_handler` + `sys_attach_9p_srv_handler` **M4**: substitute the caller `principal_id` for the `n_uname` Tattach field), `tools/build.sh::build_stratum_pool_fixture` (passes `--bake-owner-uid 4294967294`=`PRINCIPAL_SYSTEM` + `--bake-owner-gid GID_SYSTEM`), Stratum (branch `thylacine-pouch-arm`) `src/cmd/stratumd/{run.c,serve.c}` (new `--bake-owner-uid`/`--bake-owner-gid` flag overriding `s->auth_uid`/`s->auth_gid` at/before `stm_9p_server_create`) | Convergence-detour A-3 (IDENTITY-DESIGN.md section 9.7 + the section 3.5 F-4 correction + the section 3.7.1 activation note; **two user votes 2026-05-31**: SO_PEERCRED channel over literal-F-4 n_uname; flip dev9p enforcement now over defer-to-login). **Activates dev9p rwx enforcement -- the privilege boundary; AEGIS/mallocng-adjacent write path -- prosecute hard.** **Corrects F-4**: ground truth (two Explore passes) showed Stratum IGNORES `n_uname` (`server.c:1007-1008`) and reconciles identity via `SO_PEERCRED` only -- which pouch already marshals from the kernel's unforgeable `SYS_srv_peer` (the `srv_peer_info` carries `principal_id` since A-1a) but the shim hardcoded `ucred.uid = 0`. So the load-bearing trusted-local channel is **`SO_PEERCRED`-carries-principal** (kernel-stamped, NOT client-asserted -- a connecting Proc cannot forge it), not n_uname. Reconciliation = host-bake stamps the pool `PRINCIPAL_SYSTEM`-owned (`--bake-owner-uid`; NOT an on-disk-format change -- `si_uid`/`si_gid` already exist, only the value changes) + the pouch SO_PEERCRED fix, so the kernel-side `perm_check` is coherent and the boot chain (owner) is not bricked. **Invariants:** I-22 preserved (kernel-stamped principal, no identity self-elevation; `CAP_HOSTOWNER` remains the sole DAC-override); I-2/I-4/I-6 unaffected (no cap/transfer added; F1 *narrows* the handle envelope to omode-derived rights -- monotonic-friendly); A-1.7/I-23 preserved (F1's `T_OPATH` carve-out keeps the storage-capability base born R+W); no-brick (boot OK + cross-reboot PASS are the gate). Closes A-2d audit **F1** (handle-rights outran the checked omode) + **F2** (rename/unlink were RIGHT-gated only) in the same pass as the flip. The `n_uname` forwarding (M4) is kept but **demoted to the v1.x foreign/authenticated path** (a server with no SO_PEERCRED -- remote/TCP -- where the corvus trust-stamp gate then matters); the trust-stamp gate is a **v1.x SEAM** (every v1.0 attach is local SO_PEERCRED-bearing -> no untrusted-assertion to gate; no `trusted_for_identity_fwd` field exists today -- clean add). Per-user stratumd `--role client` (Stratum A2, **verified merged** on `thylacine-pouch-arm`) is proven via a dataset-scope `EACCES`-at-Tattach probe; the per-login spawn is the **A-5** consumer. Split: A-3a (reconciliation: pouch + Stratum bake flag + n_uname) -> A-3b (flip + F1 + F2) -> A-3c (per-user-stratumd mechanism + trust seam) -> one focused audit. **No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in IDENTITY-DESIGN.md section 9.7 + this row + the audit + the runtime test suite (`CAP_SET_IDENTITY` non-system child denied write to SYSTEM-owned dev9p + allowed read/exec; F1 OREAD->RIGHT_READ-only handle + O_PATH->R+W; F2 non-owner cannot mutate a no-other-w dir; out-of-scope dataset->EACCES; joey boot regression + cross-reboot no-brick). |
| Group termination / cross-thread shootdown (`SYS_EXIT_GROUP`) | `kernel/proc.c` (`proc_group_terminate` + the single set-once `group_exit_msg` on `struct Proc`; the group-status last-Thread-out ZOMBIE reap; `exits`/`thread_exit_self` group-status path), `arch/arm64/exception.c` (`el0_return_die_check` at the sync-from-EL0 tail), `arch/arm64/vectors.S` (the NEW IRQ-from-EL0 return-tail die-check -- `#713`-safe), `kernel/torpor.c` (`torpor_wake_all_for_proc`), `kernel/smp.c` (`smp_resched_others` -- broadcast `IPI_RESCHED`), `kernel/syscall.c` (`SYS_EXIT_GROUP = 60` handler + the `kill`-cascade replacing the multi-thread refusal in `sys_postnote`), `kernel/include/thylacine/syscall.h` (`SYS_EXIT_GROUP = 60` ABI), `usr/lib/pouch/patches/0001-pouch-syscall-seam.patch` (`__NR_exit_group` 0 -> 60), `usr/lib/libt` + `usr/lib/libthyla-rs` wrappers | SYS_EXIT_GROUP (#809, `89456e9`; pulls the documented v1.x lift forward -- #808 audit F3). **Invariant I-24** (group termination atomic + exactly-once + no-lost-wakeup; ARCH §7.9.1). Privilege/lifetime boundary AND a wait/wake surface -- prosecute hard. AS-BUILT model = flag-and-self-terminate at the EL0-return checkpoint via a SINGLE per-Proc set-once `group_exit_msg` (NULL-sentinel CAS = die-flag + last-out status) + `torpor_wake_all_for_proc` + broadcast `smp_resched_others` (NOT the abandoned `die_requested`/per-Thread-`cpu`/`group_exiting`/`group_exit_status`/targeted-IPI design -- F2 reconcile). Plan 9 / Linux / Zircon convergent (seL4 sync-stall rejected). Invariants: I-9 (sleeper-wake = register-then-observe under the per-condition lock -- `torpor_lock` for torpor, per-Thread `wait_lock` for all other rendez sleeps per task #811; lock order `g_proc_table_lock -> wait_lock -> r->lock`); #713 (the IRQ-from-EL0 die-check sits before the DAIF-masked eret window; die path noreturn); #788 (`on_cpu`-spin before any peer `thread_free`); I-8 (every flagged Thread reaches its checkpoint -- broadcast IPI + timer-tick floor + sleeper-wake). Closes the `exits`-with-live-peers extinction (the test.sh flake) + `kill -> -EIO in multi-thread Proc` (13b R1-F9). **v1.0 residual RESOLVED by task #811** (the #809 audit F1 showed the residual is a non-reaping HANG -- an indefinite poll(-1)/pipe/devnotes_read sleeper is un-woken; the original "dies at call completion" framing was WRONG; #811 makes the cascade wake total via universal death-interruptible sleep, ARCH §8.8.1; [OPEN Q 7.9.A] = B, user-voted 2026-05-31). Multi-thread **fault** path (`proc_fault_terminate`) is a tracked follow-up (#810), NOT this chunk. **No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in ARCH §7.9.1 + this row + the audit + the runtime tests. |
| Universal death-interruptible sleep (`*_INTR`) | `kernel/sched.c` (`sleep`/`tsleep` generalized register-then-observe of `group_exit_msg` + the `SLEEP_INTR`/`TSLEEP_INTR` return), `kernel/include/thylacine/thread.h` (per-Thread `wait_lock` protecting `rendez_blocked_on`; ONLY the owner mutates it), `kernel/include/thylacine/rendez.h` (the `*_INTR` sentinel + contract), `kernel/proc.c` (`proc_group_terminate` walks `p->threads` + wakes each sleeper via `wait_lock`->`rendez_blocked_on`; `exits()` folds into the SAME universal wake), and EVERY blocking site's "on `*_INTR` -> cleanup -> return" arm: `kernel/poll.c`, `kernel/pipe.c` (read + write), `kernel/devnotes.c`, `kernel/srvconn.c` (client + server recv), `kernel/devsrv.c` (accept), `kernel/irqfwd.c`, `kernel/proc.c` (`wait_pid`) | Task #811 -- the F1=B completion of `SYS_EXIT_GROUP` (#809-audit F1); ARCH §8.8.1. **The wait/wake primitive itself -- prosecute hard.** Invariant: I-9 GENERALIZED (no death-wake lost between a sleeper's cond-check and its sleep, for EVERY rendez sleep -- register-then-observe under the per-Thread `wait_lock`, the Plan 9 `p->rlock` analog); completes I-24's "no Thread runs at EL0 after ZOMBIE" for the indefinite-sleeper class. Lock order: `wait_lock` is the OUTERMOST wait-lock (`wait_lock -> g_timerwait.lock -> r->lock`; waker `g_proc_table_lock -> wait_lock -> wakeup`); acyclic because only the owning Thread WRITES `rendez_blocked_on` and no sleeper holds `g_proc_table_lock` below `wait_lock` (`wait_pid` drops it first). Death unwinds at the EL0-return tail (`el0_return_die_check`), NEVER inside `sleep()` (would strand caller locks). Re-validate each site's cleanup + I-9 in the audit (dirty-class follow-up per the #809 close). Closes #809-audit F4 (`exits()` fold-in). **No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in ARCH §8.8.1 + this row + the audit + per-site regression tests. |
| A-4 capability model + legate elevation (`rfork` I-2 strip; `cap` device clearance grant/redeem) | `kernel/include/thylacine/caps.h` (`CAP_GRANT_CLEARANCE`=1<<6 fork-grantable; `CAP_DAC_OVERRIDE`=1<<7 / `CAP_CHOWN`=1<<8 / `CAP_KILL`=1<<9 elevation-only; `CAP_ELEVATION_ONLY` expands to all four elevation-only bits), `kernel/proc.c` (`rfork_internal` ANDs `~CAP_ELEVATION_ONLY` -- the A-4-pre I-2 fix; `legate_scope_id`/`legate_session_id`/`legate_valid_until` on `struct Proc`; scope-teardown via `proc_group_terminate`), `kernel/devcap.c` (the `grant`/`use` files generalized from CAP_HOSTOWNER-only to arbitrary clearance cap-sets + `valid_until` -- the `grant` file is LENGTH-discriminated [16 = hostowner, 32 = clearance], the `use` file does ONE locked kind-branched redeem; the redeem rides the EXISTING `/cap/use` file -- NO new REDEEM syscall -- and CREATES a legate via `kernel/proc.c::proc_become_legate`; the clearance GRANT rides the new `SYS_CAP_GRANT_CLEARANCE` = 61 syscall [the grant-side bridge mirroring `SYS_CAP_GRANT`: corvus is chrooted to its storage cap and reaches the cap device by syscall, NOT a `/cap` file walk, exactly as the hostowner grant already does -- the 32-byte `/cap/grant` Dev write stays the conceptual path for un-chrooted writers + tests]), `kernel/perm.c` (honor `CAP_DAC_OVERRIDE` in `perm_check` + `CAP_CHOWN` in `perm_wstat_check`); A-4a-3 lands `SYS_CAP_GRANT_CLEARANCE` + the libthyla-rs `cap.rs` clearance grant/redeem wrappers + the corvus CLEARANCE verbs (14-17) + the E2E legate prover | A-4-pre + A-4a. **Invariants I-2 (the elevation-only strip -- the named P5-hostowner hole), I-25 (legate scope bounded + fully revoked), I-22 (no identity carries ambient authority).** Highest-stakes privilege surface -- prosecute hard. Prosecute: no elevation-only leak across `rfork`/`SYS_SPAWN_WITH_CAPS`; the `cap`-device grant lifecycle (no replay, no cross-stripes redeem, `valid_until` honored, `self_restriction` only reduces); scope teardown exactly-once + reuses #809/#811 correctly (no orphaned elevated Proc). **No new spec** per the 2026-05-23 broadening -- prose validation in IDENTITY-DESIGN.md §9.8 + this row + the audit + tests. |
| A-4b cross-process kill (`/proc/<pid>/ctl` + `CAP_KILL`) | `kernel/devproc.c` (the `ctl` write parses `kill`/`killgrp` -> `proc_group_terminate` uniformly under `g_proc_table_lock` via the `proc_for_each` resolve+authorize idiom -- the `#811` wake-total primitive; new `stat_native` reporting the target's `principal_id`/`primary_gid`/`0600`; **`perm_enforced = false`** -- the two-axis check (owner [same `principal_id` on the `0600` ctl] OR `CAP_HOSTOWNER` OR `CAP_KILL`; computed DIRECTLY, NOT via the `perm_check` DAC-override -> `CAP_DAC_OVERRIDE` is NOT a kill axis, keeping fs-admin orthogonal to process-kill) runs at the WRITE site in `devproc_write`, NOT open: the SHARED open chokepoint hard-rejects pre-`devproc.open` so the gate-at-open sketch could not host the `CAP_KILL` axis -- reconciled 2026-06-01, user vote). USER-REACHABILITY of `/proc` is a Utopia namespace seam (the `namec` multi-component mount-crossing resolver + a boot-path mount); A-4b lands + kernel-unit-tests the mechanism + authority. | A-4b. **Invariant I-26 (cross-process control two-axis; composes I-22 + I-1).** Privilege boundary -- prosecute hard. Prosecute: no identity bypass of the kill gate; namespace-visibility containment; the parent-of-target case still works; no UAF resolving `<pid>` -> Proc under the lock (resolve+authorize+kill all under `g_proc_table_lock`); multi-thread cascade correctness. **No new spec** -- prose validation in IDENTITY-DESIGN.md §9.8 + this row + the audit + tests. |
| A-4c trusted path: kernel console RX + SAK | `arch/arm64/uart.c` (RX IRQ + IMSC.RXIM unmask + RX-FIFO drain + `DR.BE` BREAK detect; PL011 SPI 33 hardcoded as the QEMU-virt fallback -- DTB `interrupts`-parsing is a Lazarus seam), `kernel/main.c` (boot wiring: `gic_attach`+`gic_enable_irq` for the UART SPI, alongside the timer), `kernel/irqfwd.c` (reserve the UART SPI INTID, like the timer, so userspace `SYS_IRQ_CREATE` cannot claim it), `kernel/cons.c` (`devcons_read` real blocking read on a `Rendez` + single-reader busy-guard + a console input ring; the IRQ handler is wakeup-only -> the `console_mgr` kproc kthread does the deferred privileged work in process context [SAK = serial BREAK recognized pre-EL0; Ctrl-C -> `interrupt` note]; `notes_post`/`poll_waiter_list_wake` are NOT IRQ-safe -- only `wakeup()` on a `Rendez` is), `kernel/proc.c` (`proc_revoke_console_attached` [atomic `proc_flags` clear] + the single `g_console_owner` pointer under `g_proc_table_lock` + `exits()` clear-on-owner-exit + re-grant to corvus via `g_console_trusted_proc`, FAIL-SAFE revoke-only if absent + notify), `kernel/devcap.c` (the redeem gate keys on `PROC_FLAG_CONSOLE_ATTACHED`) | A-4c-1 (console RX pull-forward, Phase-4-G work) + A-4c-2 (SAK + handoff). **Invariant I-27 (trusted path: unspoofable elevation prompt).** New EL0-bound input path + a privilege boundary -- prosecute hard. Prosecute: the RX ring (no overflow, no missed-wakeup on the `Rendez`); Ctrl-C delivery; the SAK recognizer cannot be starved/spoofed by crafted input (structural -- BREAK is a line condition, not data); the console-attach revoke/re-grant is atomic + leaves exactly one owner; only the console-attach holder can redeem elevation. On the kernel UART console Dev (`dc='c'`) -- the userspace VirtIO-input path is unaffected (ARCH §17.1). **Test note**: the harness cannot inject UART RX non-interactively (`-serial mon:stdio` + `< /dev/null`, one PL011, no QMP serial channel) without touching the boot-banner test ABI -> proven by in-kernel unit tests (synthetic RX-handler/recognizer/owner-transition drive) + boot survival + the interactive `Ctrl-A b` BREAK path. SAK = serial BREAK (line condition, not data -> unforgeable + stateless recognizer). **No new spec** -- prose validation in IDENTITY-DESIGN.md §9.8 + this row + the audit + tests. |
| Interactive Ctrl-C: `interrupt` as a real note (LS-5) | `kernel/include/thylacine/syscall.h` + `kernel/syscall.c` (new `SPAWN_PERM_CONSOLE_OWNER` spawn-perm bit + grant gate, like `SPAWN_PERM_MAY_POST_SERVICE`; `g_console_owner = child` at spawn), `kernel/proc.c` (owner-set-at-spawn; joey relinquish unchanged), `usr/login/src/main.rs` (login spawns `ut` WITH the perm), `kernel/notes.c` (`notes_deliver_at_el0_return` -- P2 `interrupt` default-terminate + the self-managing-fd gate) + `kernel/devnotes.c` / `kernel/include/thylacine/notes.h` (the notes-fd-open => self-managing mark), `kernel/sched.c` + `kernel/proc.c` + `kernel/torpor.c` + every #811 blocking site (P3-terminate: widen the wake predicate to death OR a pending terminate-disposition `interrupt`), `usr/utopia/libutopia/src/eval/stmt.rs` (`wait_pids_interruptible` forwards `interrupt` -- unchanged), `tools/interactive/ls-5.exp` | LS-5 (Life Support; `docs/LIFE-SUPPORT.md` + ARCH 8.8.2). **Makes `interrupt` a real catchable Plan 9 note / Unix SIGINT** -- P1 delivery (console-owner) + P2 default-terminate-if-uncaught (self-managing gate) + P3-terminate (#811-reusing blocked-child wake). **Death-path + notes lineage (#788/#806/#807/#808/#860/#809/#811/#926 -- the most bug-prone in the tree) AND a privilege-adjacent ABI addition (a new SPAWN_PERM bit) -- prosecute hard.** Invariants: **I-19** (the `interrupt` per-note default disposition; ordering/exactly-once/N-4 untouched), **I-9 / 8.8.2** (death-wake generalizes to death-or-terminate-`interrupt`, reusing the per-Thread `wait_lock` register-then-observe -- re-validate every #811 site), **I-27** (console-*owner* strictly distinct from console-*attach*; the owner bit NEVER confers attach -> the SAK/elevation gate untouched; the per-bit separation is load-bearing), **I-2** (a spawn-time `perm_flags` decision, never `rfork`-propagated). Prosecute: the owner-grant gate (only a console-owner/trusted spawner confers it); the self-managing gate (shell exempt, dumb child not; a blocked child cannot register a handler to escape terminate); the P3-terminate wake (no lost/double wake; single-thread disposition-fixed-at-post; lock order unchanged from #811); the LS-5/LS-8 boundary (P3-deliver/`-EINTR` is NOT built here -- it is LS-8). **No new spec** per the 2026-05-23 broadening -- prose validation in ARCH 8.8.2 + LIFE-SUPPORT.md (LS-5) + this row + the focused audit + the #811 per-site re-validation + the `ls-5` LS-CI scenario. |
| A-5 login + session lifecycle + per-user encrypted home | NEW `usr/login/` (native `/sbin/login`, libthyla-rs: SAK-gated `/dev/cons` prompt -> corvus `AUTH` client -> `CAP_SET_IDENTITY` stamp via `SYS_SPAWN_FULL_ARGV`'s `SPAWN_IDENTITY_SET` -> per-user `--role client` stratumd + `/home/<user>` bind -> spawn `ut` as the session leader; logout = group-terminate + unmount + corvus `SESSION_CLOSE`), `usr/joey/joey.c` (spawn `/sbin/login` post-pivot + relinquish its boot console-attach at the bringup->session boundary), `kernel/proc.c` (the joey console-attach relinquish + the OPTIONAL `SPAWN_PERM_CONSOLE_OWNER` -> `g_console_owner`-without-attach, for Ctrl-C-to-shell), `kernel/include/thylacine/syscall.h` + `kernel/syscall.c` (`SPAWN_PERM_CONSOLE_OWNER` if added); **Stratum-side** (`thylacine-pouch-arm`) `src/sync/sync.c` (deferred-unwrap soft-skip of out-of-scope CURRENT slots, `sync_unwrap_cb`) + a runtime DEK install/evict consumer (reusing `stm_corvus_unwrap`) + the per-dataset `/ctl` `provision-dek`/`install-dek`/`evict-dek` control kinds (`src/ctl/synfs.c`) + `stm_fs_create_dataset_corvus` (the first-login ensure-home mint+WRAP) + the login token-forward | Convergence-detour A-5 (IDENTITY-DESIGN.md §9.9; 3 votes + a refining 4th, 2026-06-02). The capstone integration -- composes I-1 / I-22 / I-27 + the A-4 caps; adds NO new ARCH §28 invariant. **The DEK handoff is AEGIS/mallocng-adjacent -- prosecute hard.** Prosecute: **I-27** (login + the user shell are NEVER console-attached; the joey relinquish preserves "corvus is the sole console-attached Proc during a session"; no interposer between the SAK and the corvus prompt); the **identity stamp** (`CAP_SET_IDENTITY` gate; login stamps only the principal corvus authenticated; no forge); the **DEK handoff** (login never holds the raw DEK; the token-forward leaks no secret via argv/files; the coordinator install/evict has no UAF/leak; eviction actually zeroes); the **first-login provision** (`provision-dek` ensure-home is idempotent -- no double-mint of a user's DEK, `STM_EEXIST -> OK`, a returning user's provision-dek never re-wraps; the create+WRAP+seal path is AEGIS/mallocng-adjacent -- prosecute hard); **user-vs-user isolation** (a 2nd user's session cannot unwrap or attach the first's dataset -- dataset-scope EACCES + the per-user-sealed DEK); the **session teardown** (no orphaned session Proc; the kill cascade is total per #811); the **Stratum deferred-unwrap** (a soft-skipped dataset is provably unreadable until its DEK is installed; the install validates the forwarded token). **A-5b DEK transport (RESOLVED 2026-06-02, user-voted; corrects the same-day "no corvus lift" note):** the coordinator PULLS the DEK with the login-forwarded token over its own corvus connection (§6.3), enabled by (a) the pouch `connect()` walking to corvus's `ctl` fid (`usr/lib/pouch/patches/0006-pouch-sockets.patch`, audit-bearing boundary-line; the kernel 2-arg `SYS_srv_connect` already drives the walk) and (b) the **corvus session-ownership lift** (`usr/corvus/src/main.rs` -- the AUTH-SESSION owning-connection tag + the `close_conn` clear-gate: clear the global AUTH session only on the OWNING connection's close or explicit SESSION_CLOSE, never on a non-owning bearer-token connection's close -- else the coordinator's transient connection wipes login's live session mid-session and breaks A-4 legate elevation). PROSECUTE both new surfaces: the corvus session-ownership change (no cross-session wipe; owning-connection tag unforgeable; the §4.2/§6.2 intent realized) + the pouch ctl-walk transport. corvus-PUSH rejected (role inversion; corvus lacks the storage layout). The security property (at-rest + session-scoped, login-never-holds-raw-DEK, evict-at-logout) is FIXED. Split A-5a (login core; Stratum-independent) / A-5b (encrypted home + the Stratum sub-chunk) / A-5c (RECOVER + hostowner-c) -- a focused round each. **No new spec** per the 2026-05-23 broadening -- prose in IDENTITY-DESIGN.md §9.9 + this row + the per-sub-chunk audits + the runtime + cross-reboot + login-E2E tests. |
| A-5c RECOVER recovery keyslot (corvus) | `usr/corvus/src/main.rs` (the new `VERB_RECOVER`=8 handler [`subject_kind` 0=system / 1=user]; the BIP-39 wordlist + phrase gen/decode/checksum; the per-user `recovery.corvus` mint at `handle_user_create` + the phrase returned in the OK response; the keypair re-wrap + rename-swap of `hybrid.corvus` / `system-wrap`; the fresh-phrase roll), `usr/login/` (the login recovery path + the create-time phrase surfacing); **A-5c-b host-bake (user-voted 2026-06-05)**: NEW `usr/lib/corvus-crypto/` (shared `no_std` crypto lib extracted from corvus -- the CRVS wrap layout + `argon2id_kek` + AEGIS wrap/unwrap + BIP-39 codec + keypair-gen, parameterized over an RNG), NEW `usr/corvus-mint/` (host-target minter, `OsRng`), `tools/build.sh` (build corvus-mint host-side + bake the `/var/lib/corvus` dir-chain + `system-wrap` / `system-recovery-wrap`, SYSTEM-owned), `usr/corvus/src/main.rs` (`system_identity_load` at boot + the real `handle_admin_elevate` argon2id+AEGIS unwrap of `system-wrap`, replacing the `b"thylacine"` byte-compare + RECOVER(`subject_kind=0`) console gate) | Convergence-detour A-5c (IDENTITY-DESIGN.md §9.9 design pass + CORVUS-DESIGN §5.5.1/§5.6/§6.4/§8/§9; user votes 2026-06-05: user-held-only [NO hostowner escrow] + mandatory enrollment + host-bake the system identity). **AEGIS/mallocng-adjacent recovery crypto -- prosecute hard.** A recovery keyslot = a 2nd wrap of the SAME hybrid keypair under a recovery-phrase KEK (`recovery.corvus` / `system-recovery-wrap`); it re-wraps the KEYPAIR not the DEK -> the Stratum DEK envelopes stay valid -> **NO Stratum / kernel surface**. Invariants: **C-28 no-escrow** (corvus stores no copy of a user's keypair/DEK recoverable by any authority other than that user's passphrase OR own phrase; the hostowner has NO user-data-recovery verb -> D3 mutually-encrypted-homes survives a malicious hostowner); **C-27** (per-user keyslot minted at USER_CREATE, displayed once, never persisted; same keypair as `hybrid.corvus`; domain-separated AD `"thylacine-corvus-recovery-v1"`); **C-20** generalized. Prosecute: the **gate** (RECOVER(user) = phrase + rate-limit ONLY, no session / no `CAP_HOSTOWNER`; RECOVER(system) additionally console-attached -- a token-only or non-console bypass is the bug); **phrase brute-force** (the C-16 rate-limit is the online defense; a weak-RNG / partial-checksum bug would matter); **secret hygiene** (mlock + `explicit_bzero` of phrase, recovery_KEK, keypair, new_KEK on EVERY path incl. error); the **twin-wrap atomicity** (re-wrap `hybrid.corvus` AND roll `recovery.corvus` -- a crash between must not strand the user un-recoverable nor leave the old phrase live; rename-swap, A-1.6); **no-DEK-rewrite correctness** (the recovered keypair must byte-equal the original so existing envelopes decapsulate); the **USER_CREATE OK-response growth** (callers parse the appended phrase; no overrun). **A-5c-b (host-bake) additionally**: the `corvus-crypto` extraction MUST preserve byte-identical wraps (the A-5c-a / A-1b / A-5b wrap + DEK-envelope paths are behavior-unchanged -- re-validate the boot self-test + USER_CREATE enrollment E2E); a host-minted `system-wrap` and the on-device unwrap must round-trip (same CRVS layout / AD / Argon2id preset -- no drift between `corvus-mint` and corvus); the real `handle_admin_elevate` fails CLOSED on a missing / corrupt / wrong-passphrase `system-wrap` (no byte-compare fallback survives); RECOVER(`subject_kind=0`) requires console attachment; the bake lands the files SYSTEM-owned where corvus's chroot resolves them (joey's runtime `mkdir_or_open` no-ops over the baked dir-chain). **No new spec** per the 2026-05-23 broadening -- prose validation in IDENTITY-DESIGN.md §9.9 + CORVUS-DESIGN §5.5.1/§5.6 + this row + the audit + corvus unit tests + the boot E2E (create -> capture phrase -> lose passphrase -> RECOVER -> re-login -> home decrypts; system: ADMIN_ELEVATE + RECOVER(system)). |
| `SPAWN_PERM_MAY_POST_SERVICE` one-hop delegation (A-5b #827b) | `kernel/syscall.c` (the `SYS_SPAWN_*` perm-grant gate, now PER-BIT: `SPAWN_PERM_CONSOLE_TRUSTED` requires console-attach; `SPAWN_PERM_MAY_POST_SERVICE` requires console-attach OR the caller ALREADY holds `proc_may_post_service(p)`), `kernel/include/thylacine/proc.h` (`proc_may_post_service`), `kernel/joey.c::joey_thunk` (stamps `PROC_FLAG_MAY_POST_SERVICE` on init -- the persistent grant-root so the post-relinquish getty can confer it on each login; #827b-beta), `usr/joey/joey.c` (spawn `/sbin/login` WITH `SPAWN_PERM_MAY_POST_SERVICE`), `usr/lib/libthyla-rs/src/process.rs` (`Command` gains a `perm`/service-post setter), `usr/login/src/main.rs` (login confers `MAY_POST_SERVICE` on the per-user `--role client` proxy it spawns; attaches it via the `ds:<user>` aname), Stratum `src/9p/server.c` (the new `ds:<name>` aname form selecting a child dataset -- #827b-beta) | A-5b #827b (user-voted 2026-06-04; the design fork dissolved per the Plan 9 private-`/srv` idiom + per-territory `/srv` isolation). **Privilege-gate change -- prosecute hard.** Lets a non-console-attached but trusted session authority (`/sbin/login`, spawned by the console-attached joey *with* the bit) confer `MAY_POST_SERVICE` on per-user proxies, so login can stand up `/srv/home-<user>` in the session's private (per-territory) `/srv`. **#827b-beta** adds (a) the init grant-root (joey holds the bit past its console relinquish, so the getty getty-loop can keep conferring it) and (b) the Stratum `ds:<name>` 9P aname form -- the per-user child-dataset selector, access-gated by the proxy `--datasets-allowed ds:<user>` (the I-1 boundary) + the 0700 dataset-root owner (kernel rwx) + the installed DEK (an un-unlocked dataset attach is inert). Prosecute: the PER-BIT separation -- a `MAY_POST_SERVICE` holder can NEVER confer `SPAWN_PERM_CONSOLE_TRUSTED` (the SAK anchor stays console-attach-only -> I-27 unchanged); the bit is never `rfork`-propagated (still a `perm_flags`-only spawn-time decision, NOT a `cap_mask` bit -> I-2 unaffected); the delegation root is the console-attached joey (no path for an ordinary Proc to acquire the bit -- soundness rests on *who holds it*: only joey's deliberately-conferred OS servers, corvus / login / the proxy). **No new spec** per the 2026-05-23 broadening -- prose validation in IDENTITY-DESIGN.md §9.9 + ARCH §25.4 + this row + the #828 audit + the grant-gate regression test. |
| Pathname resolution (`stalk`) + namespace-resident `/srv` | the resolver (`stalk` + `cross_mounts`/`domount` + the in-call `trail`; folds in / supersedes `sys_walk_open_handler`), `kernel/include/thylacine/territory.h` + `kernel/territory.c` (stalk-2: `PgrpMount` re-keyed from `path_id_t` to the full Plan-9 mount-point Spoor identity `(dc, devno, qid.path)` -- the `mp_devno` axis is LOAD-BEARING [every dev9p session shares `dc='9'`+root `qid.path 0`, so `(dc,qid.path)` alone collides corvus vs a per-user stratum-fs]; `mount`/`unmount`/`mount_lookup` Spoor-keyed; size-pinned `Territory` static_asserts re-bumped 16->32 B/entry), `kernel/include/thylacine/spoor.h` + `kernel/spoor.c` (stalk-2: new `u32 Spoor.devno` = Plan-9 `Chan.dev` + `spoor_next_devno()`), `kernel/dev9p.c` (stalk-2: `dev9p_attach_client` stamps a fresh devno per attach session), `kernel/stalk.c` (`cross_mounts` cross-on-descent + `STALK_MOUNT` no-cross-final), `kernel/devramfs.c` (stalk-2: synthetic `/srv`+`/proc` mount-point dirs, D4 M1), `kernel/syscall.c` (`SYS_OPEN(path)` multi-component; `SYS_MOUNT`/`UNMOUNT` PATH-keyed via `STALK_MOUNT`; retire `SYS_SRV_CONNECT`+`SYS_POST_SERVICE`+`SYS_POST_SERVICE_BYTE` [stalk-3c]), `kernel/devsrv.c`+`kernel/srvconn.c` (stalk-3: heap+refcounted per-territory `SrvRegistry` reached through the mounted devsrv root Spoor [D7]; `devsrv_attach_registry`; `devsrv_open`=connect [two-step D5: 9p->dev9p root via `p9_srvconn_transport`+`p9_attached`, byte->stream]; `devsrv_create`=post [`DMSRVBYTE` perm bit, D6]; 9P-unification = retire embedded `srvconn_client_*`; `SYS_ATTACH_9P_SRV` retarget KObj_Srv->KOBJ_SPOOR; remove `SRV_CONN_PER_PROC_MAX`), every `/srv` client (joey/corvus/login/legate-prover) + `usr/lib/pouch/patches/0006-pouch-sockets.patch` | Convergence-detour A-5b-0 (`docs/STALK-DESIGN.md` section 5; user-voted full Plan-9 spine 2026-06-02; stalk-3 sub-design D5/D6/D7 signed off 2026-06-02). Path resolution is a privilege boundary -- prosecute hard. **Invariant I-28** (path-resolution containment + per-component X-search) + **I-1** (per-territory `/srv` makes a 2nd user's coordinator unnameable). Prosecute: `..` escape above `root_spoor`; per-component X-search bypass (symlink / `..` / mount-cross tricks); Spoor lifetime across N hops on the `trail` AND across a cross (the `clone_walk_zero` transient) (UAF / double-clunk / leak); mount-key collision (two sessions same `dc`+`qid.path`, distinct `devno`); STALK_MOUNT-vs-cross final-element correctness (MREPL re-keys the same point); mount-cross into a tree lacking X (the MOUNTED root's perms govern); **[stalk-3a]** the per-Spoor `SrvRegistry` ref lifecycle (one ref per devsrv-Spoor-instance-with-aux=reg; clone+walk0 bumps, `devsrv_close` drops; drain-on-last-unref; no UAF/double-free; registry-scoped tombstone); **[stalk-3b]** the connection-handle reconciliation (KOBJ_SPOOR endpoint; `devsrv_conn_of` for `SO_PEERCRED`; non-dup-able conn Spoor; the 9P-unification's blocking attach-at-open + the dev9p `attached_owner` refcount reuse); **[stalk-3c]** per-territory `/srv` isolation + the ABI-break surface (AEGIS/mallocng-adjacent via the corvus DEK path it unblocks). The **F1 amode guard in `stalk()` must gain any new amode** a sub-chunk adds (stalk-2 added STALK_MOUNT; stalk-3 adds none -- post/connect ride existing syscalls). One focused round per sub-chunk (stalk-1/2/3a/3b done; stalk-3c audit CLEAN 0/0/0/3 -- all P3 doc-staleness; stalk-3 ARC COMPLETE). **No new spec** per the 2026-05-23 broadening -- prose in `docs/STALK-DESIGN.md` + ARCH §9.6.7 + this row + the audit + tests. |
| Per-Proc cwd (`SYS_CHDIR` / `SYS_GETCWD` + the `SYS_OPEN` relative->cwd join) | `kernel/include/thylacine/territory.h` + `kernel/territory.c` (`Territory.dot_path` -- the cleaned absolute cwd string, `NULL`==`"/"`, heap-allocated lazily, bounded by `SYS_OPEN_PATH_MAX`, size/offset `_Static_assert`s re-bumped; `territory_clone` deep-copies it [child inherits the parent snapshot, independent thereafter]; `territory_unref` final release frees it; a `territory_setdot`/`territory_getdot` helper pair under the territory lock), `kernel/syscall.c` (`sys_chdir_handler` [resolve `clean(join(dot_path,path))` from `root_spoor` via `stalk` -> require `QTDIR` + `perm_check` X -> swap `dot_path` under the territory lock -> clunk the verification Spoor]; `sys_getcwd_handler` [copy out `dot_path`, `-ERANGE`]; `sys_open_handler` [the relative->cwd join at the sentinel-start-fd + relative-path branch, BEFORE `stalk`]), `kernel/include/thylacine/syscall.h` (`SYS_CHDIR = 69` / `SYS_GETCWD = 70` + ABI), `usr/lib/libt` + `usr/lib/libthyla-rs` wrappers (the `fs::` relative mutations join via `getcwd`) | LS-4 (LIFE-SUPPORT.md; user-voted "kernel per-Proc cwd" 2026-06-09 over shell-side argv-expansion). **Path-resolution + a new syscall surface -- prosecute hard, but the design DELIBERATELY adds NO new I-28 mechanism.** Invariant **I-28 PRESERVED** (the cwd-join resolves from `root_spoor` exactly like an absolute path; `stalk` clamps `..` at `root_spoor`, so a hostile un-cleaned join cannot escape -- the join is a convenience, NOT a containment authority); the chdir **X-search gate** is the per-Proc-identity check (a Proc may only `cd` into a directory it can search -- the open-path `perm_check`); threads share `dot_path` (POSIX per-process cwd) under the territory lock (no torn read/swap); `territory_clone` snapshot inheritance (no shared-mutable cwd across the fork boundary unless the Territory is `RFNAMEG`-ref-shared); `dot_path` lifetime (lazily allocated, freed exactly once at `territory_unref`, no leak/double-free/UAF; the join bounds-checks against `SYS_OPEN_PATH_MAX` -> `ENAMETOOLONG`). **Name-based (a string), NOT handle-based (a `dot` Spoor), for v1.0** -- because `stalk` contains `..` at its trail-floor `start` (a handle-`dot` start strands `cd ..` without a new device-`..`-walk mechanism on the I-28 surface) AND the symlink/`..` correctness argument is moot pre-symlinks (G11); handle-based is the v1.x upgrade, landing with symlinks. **No new spec** per the 2026-05-23 broadening -- prose validation in LIFE-SUPPORT.md (LS-4) + STALK-DESIGN.md (the cwd-join note in 4.3) + `docs/reference/104-stalk.md` + this row + the focused audit + the kernel unit tests (`chdir`/`getcwd` round-trip; relative open resolves against dot; `..` clamps at root; X-search enforced) + the LS-CI relative-`cat` E2E. |
| Loom -- the io_uring-inverted 9P ring transport (KObj_Loom + SQ/CQ rings + registered handles + the pluggable-completion 9P-engine seam) | `kernel/include/thylacine/loom.h` (the ABI: `loom_sqe`/`loom_cqe`/`loom_ring_hdr`/`loom_params` + the `_Static_assert`s + `struct Loom`), `kernel/loom.c` (the KObj_Loom ring substrate -- `loom_create`/`loom_ref`/`loom_unref` + the registered-handle table), `kernel/syscall.c` (`sys_loom_setup_for_proc` / `sys_loom_register_for_proc` + the SVC handlers + dispatch; `SYS_LOOM_SETUP = 66` / `SYS_LOOM_REGISTER = 67` / [reserved `SYS_LOOM_ENTER = 68`]), `kernel/handle.c` + `kernel/include/thylacine/handle.h` (`KOBJ_LOOM` + the fourth partition mask `KOBJ_KIND_LOOM_MASK` + the acquire [`loom_ref`, reached via `handle_get`] / release [`loom_unref`] branches); **Loom-2b adds** `kernel/9p_client.c` (the pluggable-completion seam -- `p9_rpc.on_complete`: `WAKE_RENDEZ` vs `POST_CQE` on the in-flight `p9_rpc` + `p9_client_submit_async` + `p9_client_reader_pump_once` + `p9_client_handoff_reader`) + `kernel/loom.c::loom_post_cqe` (the CQ writer, `CqNeverOverfull`); **Loom-3 adds** `kernel/loom.c` (`loom_enter` [SQ-index consume from the kernel-private `sq_head` + the TOCTOU SQE copy + the SQ-index range check] + `loom_submit_one` [SQE dispatch: `LOOM_OP_NOP` inline + `LOOM_OP_FSYNC` via the submit-time pin; reserved/payload opcodes -> `-ENOSYS`/`-EINVAL` error CQEs -- payload ops land with Loom-6 registered buffers] + `struct loom_async_op` [the production container, `rpc`@0, the I-30 `spoor_ref` pin, owned by `l->inflight_ops`] + `loom_async_complete` [CQE post + terminal + `async_inflight`-- under `l->lock`, NO sleep] + `loom_reap_terminal` + the **`loom_free` quiesce-before-free, #898**), `kernel/9p_client.c::p9_client_abandon_async` (the #845 Tflush-on-abandon for an async op, under `c->lock`), `kernel/dev9p.c::dev9p_client_fid` (resolve a registered Spoor -> `(client, fid)`), `kernel/syscall.c::sys_loom_enter_for_proc` + the SVC handler (`SYS_LOOM_ENTER = 68`) | Loom arc (`docs/LOOM.md`, signed off 2026-06-05; the io_uring inversion -- userspace posts 9P-shaped ops into a shared-Burrow SQ ring, the #841 elected-reader client drives them, R-messages return as CQEs; the opcodes ARE the `p9_client_*` set). **Shared-memory async boundary -- prosecute hard.** Reserves ARCH §28 **I-29** (completion integrity: no lost / double / stale; CQ back-pressure) + **I-30** (submit-time capability pin -- resolve + snapshot at submit, never re-resolve at completion = the io_uring credential-vs-work CVE class; ring TOCTOU). Prosecute: ring TOCTOU (the kernel copies every SQE field to kernel memory before validating/acting, never re-reads a shared-ring field after the check); the KObj_Loom + ring-Burrow lifecycle (the #847 dual-refcount keeps the pages alive while EITHER the Loom [handle_count] OR the user mapping [mapping_count] holds a ref -- no UAF / leak; `loom_unref`'s last drop clunks every registered Spoor + `burrow_unref`s the ring; `handle_get`/`handle_put` balance via `loom_ref`/`loom_unref`); the registered-handle table (the held `spoor_ref` is the I-30 pin substrate, decoupled from the caller's fd; the rights snapshot captured at register; KObj_Loom is non-transferable + non-dup-able so `handle_dup` rejects it); the SYS_LOOM_SETUP map+copy rollback (a copy-out fault fully reclaims the handle + the VMA); the pluggable-completion refactor of the audited #841 engine (the seam must NOT break the synchronous path -- a LOOM op has no blocked submitter thread, so the elected-reader handoff must skip it AND a session death must complete it with an error CQE, not wake a nonexistent rendez) [Loom-2b]; **the SQPOLL poll-thread + CQ wait-queue [Loom-4, LANDED]** -- `kernel/loom.c` (`loom_sqpoll_main` the per-ring `kproc()` kthread + `loom_start_sqpoll` + `loom_drain_sq` [factored from `loom_enter`] + the `loom_free` JOIN [set `sqpoll_stopping` + wake `sqpoll_park` + spin `sqpoll_exited` + `thread_free`, BEFORE the #898 quiesce] + the IRQ-masked EXITING terminal handshake [`spin_lock_irqsave(NULL)` across `state=EXITING` + the `sqpoll_exited` release -- a kproc kthread cannot `thread_exit_self`, so this is the `wait_pid` reap minus the zombie bookkeeping] + the `loom_register_handles` deadline-capable gate + the F2 admittability park cond + the F1 `loom_first_inflight_client` borrow-guard `spoor_ref`), `kernel/9p_client.{c,h}` (4a: `set_recv_deadline` + `p9_client_reader_pump_once_deadline` + `p9_client_recv_is_deadline_capable`), `kernel/9p_{srvconn,spoor}_transport.c` + `kernel/9p_transport_loopback.c` (the `set_recv_deadline` backends; spoor=NULL=rejected-from-SQPOLL, srvconn+loopback capable), the 4b `cq_waiters` CQ wait-list + `loom_wait_for_completions`, `kernel/syscall.c::sys_loom_setup_for_proc` (`LOOM_SETUP_SQPOLL` spawn). Prosecute: the kthread lifetime (the join terminates; the EXITING handshake's preempt-loss-freedom + the release/acquire EXITING-before-`thread_free`'s not-RUNNING gate + the on_cpu spin); the join-vs-mid-recv wake (the frame-boundary idle-deadline bounds a between-frames recv; the deadline gate keeps it interruptible -- mid-frame is unbounded = the F4 v1.x untrusted-server seam); the lock-free park cond (race-free ONLY because async_inflight==0 at park -> the kthread is the sole `cq_tail`/`sq_head` writer); the CQ wait-list no-missed-wake (I-9, register-then-observe); the borrowed-client lifetime across the pump (F1: a concurrent reap + re-register must not free the Spoor -> the `p9_client` mid-deref). **Audit closed 0 P0 / 1 P1 / 1 P2 / 3 P3** (F1 borrowed-client UAF + F2 CQ-full park busy-loop both FIXED; DIRTY close [F2 changes the park wait/wake predicate] -> round-2 on the fixes; `memory/audit_loom_closed_list.md` Loom-4 section). OWED: the deterministic two-thread-same-`loom_fd` + cross-Proc-death + live-FSYNC SMP harness (the 4c/4d tests are NOP-only) -- carried to Loom-6 + #841/#907; multishot one-SQE->many-CQE [Loom-5]. **Spec-first RE-ENABLED for this surface** (LOOM.md §7): the Loom spec SUITE (clean + liveness + the per-bug cfgs) is a pre-commit gate; a new mechanism extends the suite FIRST, then the impl. Split Loom-2a (substrate) / 2b (engine seam) / 3 (dispatch) / 4 (SQPOLL) / 5 (multishot+LINK) / 6 (regbuf + native API + bench) -- one focused audit over the audit-bearing sub-chunks. **Loom-5 added TWO new focused spec modules** -- `specs/loom_multishot.tla` (the multiset-CQ stream lifecycle: I-29 generalized to exactly-one-terminal + the I-30 pin held across shots + per-shot back-pressure + teardown-quiesce; clean + liveness + 5 buggy cfgs) + `specs/loom_order.tla` (LINK/DRAIN admission ordering + chain-cancel completeness; clean + liveness + 4 buggy cfgs) -- leaving the audited `loom.tla` UNTOUCHED (its single-CQE-per-op `cq \subseteq Ops` is gate-tied + cannot represent a multiset CQ; refactoring it would invalidate its 8 landed cfgs), the `sched_oncpu.tla` + `sched_alpha.tla` precedent. **Loom-5 IMPL adds** (5a `cbba808` multishot + 5b `3cf1852` LINK/DRAIN, against synthetic NOP/FSYNC vehicles): `kernel/loom.c` (5a -- `loom_async_complete` MORE-vs-terminal + `op->{multishot,shot_limit,rearm}` + the new `loom_rearm_pending` re-issue in BOTH drive loops OUTSIDE c->lock + the `rearm_pending` SQPOLL park-cond arm; 5b -- `struct loom_chain_op` + the `l->chain`/`chain_tail`/`chain_len` held-submission chain + `loom_chain_drain_admits` [the drain gate `async_inflight==0 && rearm_pending==0` -- audit F1] + `loom_admit_chain` [link cancel-cascade + link/drain gates + CQ-room back-pressure + the F2 cancel-retry-on-backpressure] + `loom_reclaim_chain` [terminal-only-when-no-HELD] + the `loom_drain_sq` chain routing + the `has_chain` park-cond arm + the `loom_free` chain teardown + `loom_chain_done` under l->lock [audit F3]), `kernel/include/thylacine/loom.h` (the `rearm_pending` + chain fields on `struct Loom`; the `loom_chain_op` fwd decl), `kernel/include/thylacine/errno.h` (`T_E_CANCELED = 125` POSIX-ECANCELED, append-only `_Static_assert`-pinned, for the LINK cancel CQE). **Loom-5 audit closed 0 P0 / 0 P1 / 2 P2 / 4 P3** (one Opus prosecutor + self-audit, cross-confirmed F1): F1 [P2] drain admitted early past a rearm-pending FAST multishot (DrainOrdered) -> FIXED (`rearm_pending` term) + a deterministic regression test; F2 [P2] cancel-CQE droppable under concurrent over-admit -> cancel leg FIXED (revert-to-HELD + retry); F3 [P3] `loom_chain_done` lock-free state write -> FIXED (under l->lock). F4/F5/F6 [P3] = the single-driver-per-ring concurrency contract + OOM-degrades-LINK + LINK-group single-producer -> DOCUMENTED (107-loom.md "Known caveats"). The F2 dispatch-leg over-admit residual (inherited Loom-3 window; a dropped chain TERMINAL = a missed completion, chain continues) + the exact-concurrent-admission coordination = OWED to Loom-6 with the deterministic two-thread-same-loom_fd SMP harness. NOT a dirty close (0 P0/P1, localized gate/lock/retry fixes); `memory/audit_loom_closed_list.md` Loom-5 section. **Loom-6a/6b adds** (registered buffers + the uniform `p9_client_*` payload surface; `738bd3e` 6a + `2592560` 6b-1 + `e329126` 6b-2): `kernel/loom.c` (`loom_register_buffers`/`loom_resolve_buf` -- vma_lookup -> one writable-anon VMA -> a `burrow_ref` I-30 BUFFER pin, all-or-nothing install, W^X RW-only, the load-bearing BURROW_TYPE_ANON contiguity guarantee; `loom_submit_payload` the generalized opcode -> (builder, primary-right, secondary-right) dispatch for READ/WRITE [6a] + READDIR/READLINK/GETATTR/STATFS [6b-1 read-shaped, completion-copy] + SETATTR/MKDIR/MKNOD/SYMLINK/UNLINKAT/RENAMEAT/LINK [6b-2 mutation: names/the input `struct p9_setattr` read FROM the pinned buffer in the build thunk, scalar reply]; the THREE I-30 pins -- `op->pinned` [primary fid spoor_ref] + `op->pinned2` [the SECOND fid for the two-fid RENAMEAT/LINK, SAME-session `cl1==cl2` enforced] + `pinned_buf` [the buffer burrow_ref] -- resolved under ONE l->lock, released symmetric at reap/abandon/loom_free via the goto-fail epilogue; the TWO submit-time MEMORY-SAFETY gates on the kernel SQE snapshot `op->sqe` [a two-name split `0 < _resv1[1] < len`; SETATTR `len >= sizeof(struct p9_setattr)=56`] bounding every build-thunk read into the pinned slice; `loom_payload_result` the completion-time copy clamped to the slice], `kernel/include/thylacine/loom.h` (the ABI: `struct loom_buf_reg` 16B, `LOOM_SQE_BUF_OFF`/`LOOM_SQE_FID2`, the per-opcode field map, the `p9_setattr`/`p9_attr`/`p9_statfs` size+OFFSET `_Static_assert`s). Prosecute: the 3 I-30 pins balance on every path (no double-clunk/leak/UAF); the goto-fail ladder (locals init before the first goto); the two-fid same-session gate + the single borrow-guard's sufficiency [cl1==cl2]; the memory-safety sub-length/struct-span gates; the registered-buffer slice overflow-safety + W^X; the completion-copy min-clamp under a hostile over-count. **Loom-6c adds** (the multi-in-flight harness + the focused audit close; THIS sub-chunk): `kernel/9p_transport_mq.{c,h}` (a NEW byte-FIFO queueing test transport that stages N in-flight replies -- the single-slot `p9_loopback` structurally cannot, the gap the Loom audits carried since #841) + the 2 deterministic multi-in-flight tests (`9p_client.loom_multi_inflight_e2e` FSYNC x6 + `loom_multi_inflight_read_e2e` READ x4) driving the multi-entry `inflight_ops` list + `async_inflight>1` + the `loom_first_inflight_client` borrow-guard across a REAL pump + the multi-entry `loom_reap_terminal`, the borrow-guard balance asserted by "the registered Spoor frees exactly once" (a leaked guard ref would prevent it). **Audit closed 0 P0 / 0 P1 / 1 P2 / 3 P3** (one Opus prosecutor + a concurrent self-audit, SOUND-set cross-confirmed): F1 [P2] a >INT32_MAX byte-count CQE result (WRITE's unclamped server count the realistic leg) aliasing onto -errno -> FIXED (`loom_count_result` clamp to 0x7FFFFFFF on every count arm); F2 [P3] the two-name split allowed `==count` -> empty 2nd name (SYMLINK accepted, RENAMEAT rejected -- inconsistent) -> FIXED (gate tightened to `0 < split < count`); F3 [P3] the BURROW_TYPE_ANON contiguity rested on a comment -> FIXED (a "do not widen this gate" anchor); F4 [P3] the ABI asserts pinned size not field offsets -> FIXED (offsetof asserts on `p9_setattr`/`p9_attr`/`p9_statfs`). NOT a dirty close (0 P0/P1; a clamp + a gate constant + a comment + asserts -- no wait/wake-predicate change). **OWED to Loom-6d** (the native `libthyla_rs::loom` API = the first REAL multi-in-flight async driver + restored TSan): the CONCURRENT two-thread-same-`loom_fd` reap-vs-pump + cross-Proc-death SMP stress harness -- its documented home across all 5 Loom closed lists; the deterministic single-reader multi-in-flight coverage lands HERE at 6c. `memory/audit_loom_closed_list.md` Loom-6 section. **Loom-6d adds** (the native userspace API + the harness/bench, the ARC CLOSE; `28967e7` 6d-1 + `caedb60` 6d-2 + `57ce416` 6d-3): `usr/lib/libthyla-rs/src/loom.rs` (the native `Ring` over SYS_LOOM_SETUP/REGISTER/ENTER -- the `repr(C)` Sqe/Cqe/BufReg/Params byte-pinned to `loom.h` with `offset_of!` asserts [6d-audit F1]; the SPSC release/acquire ring memory model paired field-for-field with the kernel `__atomic_*`; register handles/buffers; try_submit/enter/reap; Drop via the #847 dual-refcount) + the 3 svc wrappers in `lib.rs` (`t_loom_setup`/`register`/`enter`; `T_SYS_LOOM_* = 66/67/68`); `usr/loom-smoke` (the pre-pivot boot gate: NOP + register + non-9P-handle rejection); `usr/loom-stress` (the OWED CONCURRENT two-thread-same-`loom_fd` async-FSYNC stress over the shared dev9p client + the cross-Proc-death #898 quiesce, joey-spawned post-pivot, run every ci-smp-gate boot under -smp 4/8); `usr/loom-bench` (the trap-amortization latency bench: ~7.7x NOP one-enter-vs-batched; honest ~1.0x for commit-bound FSYNC). NOT a kernel privilege surface -- client code over the frozen ABI; the kernel validates everything (copies every SQE, keeps sq_head/cq_tail private), so a buggy producer corrupts only its OWN ring view. **Audit closed 0 P0 / 0 P1 / 3 P3, all FIXED** (one Opus prosecutor + a concurrent self-audit, 2/3 cross-confirmed): F1 [P3] missing `repr(C)` field-offset asserts (the drift defense the kernel added at 6c F4; the Rust mirror lacked it) -> `offset_of!` asserts; F2 [P3] loom-bench's unbounded NOP reap loops swallowing `enter` errors -> guard + propagate; F3 [P3] loom-stress phase-3 in-flight-at-death timing-dependence -> documented best-effort with the measured ~1000x margin. NOT dirty (behaviorally-inert fixes). The OWED concurrent SMP harness (carried since #841 across all 5 closed lists) is DELIVERED -- the SMP gate (default+UBSan x smp4/smp8 N=10) PASS 0 corruption; the EXACT borrow-guard free-race stays reasoned+round-2-audited (inherent to the bug class) + TSan unrestored (the multi-boot gate is the witness). The Tapestry seam (`impl tapestry::Loom for libthyla_rs::loom::Ring`) is a one-line handoff documented in 107-loom.md. **THE LOOM ARC IS COMPLETE.** `memory/audit_loom_closed_list.md` Loom-6d section. Prose validation in `docs/LOOM.md` + `docs/reference/107-loom.md` (the Loom-6 section) + this row + the audit + the kernel tests. |
| Proc exit handle-close (#926) | `kernel/proc.c` (`proc_close_handles_at_exit` -- the new static helper; the top-of-`exits()` gated call `if (p->thread_count == 1) proc_close_handles_at_exit(p)` placed AFTER `srv_proc_exit_notify`/`cap_proc_exit_notify` + BEFORE the `g_proc_table_lock` acquire; the `thread_exit_self()` no-op note on the become_zombie path; the `proc_free` fallback comment), `kernel/handle.c` (`handle_table_free` lockless-safety comment -- the #844 justification updated for the new ALIVE-Proc call site) | #926 (U-6f command-substitution prerequisite; user-voted "fix the kernel" 2026-06-08). The kernel previously closed a Proc's handle table ONLY at reap (`handle_table_free` in `proc_free`, ZOMBIE-gated), so a child's inherited pipe write end stayed open in the zombie until the parent reaped it -- a shell draining `$(cmd)`'s stdout to EOF hung forever (no `write_eof`). FIX: a SINGLE-thread Proc closes its handle table at the TOP of `exits()` while still RUNNING + ALIVE + `thread_count==1` -- the only sound spot (t RUNNING so a sleep-capable close hook like a 9P clunk is legal -- sleeping while EXITING trips sched()'s "not RUNNING" assert; p ALIVE so `wait_pid` cannot reap+thread_free mid-close; `thread_count==1` so no peer shares the table). Inherited fds thus close at process termination (correct Unix/Plan 9 semantics); the parent's drain-before-reap sees EOF immediately. **Death-path surface (the #788/#806/#807/#808/#860/#809/#811 lineage) -- the most bug-prone in the tree.** Invariants: **I-7** (the #847 per-Burrow dual refcount -- free only when handle_count==0 AND mapping_count==0 -- makes the inverted handle-close-at-exit / vma-drain-at-reap ordering safe: dropping handle_count at exit while a VMA still maps does NOT free; vma_drain at reap drops the last mapping ref); no double-free (`handle_table_free(NULL)` no-ops; `proc_close_handles_at_exit` NULLs `p->handles`, so `proc_free`'s call no-ops; it remains the real close for multi-thread + the direct `state=ZOMBIE; proc_free()` orphan/rollback paths); no reaper UAF; no sleep-in-EXITING; the `thread_count==1` lockless read is stable (decrement is reap-only via `thread_unlink_from_proc`<-`thread_free`, and the sole caller-thread is not spawning). MULTI-thread Procs keep close-at-reap: their last-Thread EXITING mark is atomic-under-lock with the last-Thread determination, leaving no RUNNING window for a sleeping close -- a v1.x EXITING-protocol restructure (which also closes the killed-single-thread asymmetry: a single-thread Proc killed via `proc_group_terminate`->`thread_exit_self` defers to reap too). **SMP-gated** (default+UBSan x smp4/smp8 N=10, 0 corruption) + focused Opus audit (0 P0 / 0 P1 / 0 P2 / 3 P3, all doc: F1 the handle.c lockless justification updated, F2 the kill-path asymmetry documented, F3 `$errstr`-unchanged-on-subst-spawn-fail cosmetic). Verified by the `u-test::flow_process_pipe` part-(c) regression (drain a child's stdout to EOF BEFORE reaping -- the exact pattern formerly documented as "block forever") + the rewritten `test_sys_spawn_with_fds_child_rights_subset_of_parent` (spawns pipe-sink which BLOCKS reading fd 0, keeping it live for the rights inspection) + `/u-subst-test`. **No new spec** per the 2026-05-23 broadening -- prose in `docs/reference/14-process-model.md` + ARCH section 25.4 + this row + the audit + the regression tests. |
| `wait_pid` selection + non-blocking (`SYS_WAIT_PID` v2 / `wait_pid_for`, U-7-pre) | `kernel/proc.c` (`wait_pid_for(want_pid, flags, status_out)` the new core + the filtered `wait_pid_cond` taking `struct wait_cond_ctx` + the thin `wait_pid` wrapper), `kernel/include/thylacine/proc.h` (`WAIT_WNOHANG` + the decls), `kernel/syscall.c` (`sys_wait_pid_handler` 3-arg + the `SYS_WAIT_PID` dispatch), `kernel/include/thylacine/syscall.h` (the ABI doc), `usr/lib/libt/include/thyla/syscall.h` (`t_wait_pid_for` + `t_wait_pid` wrapper + `WAIT_WNOHANG`), `usr/lib/libthyla-rs/src/lib.rs` (`t_wait_pid_for` + `t_wait_pid` wrapper + `T_WAIT_WNOHANG`), `usr/lib/libthyla-rs/src/process.rs` (`Child::wait` by-pid + `Child::try_wait`) | U-7-pre (user-voted 2026-06-08; the documented `joey.c:2451` owed lift "a kernel `wait_pid_for(pid)`"). **Death-path surface (the #788/#806/#807/#808/#860/#809/#811/#926 lineage -- the most bug-prone in the tree).** A SYSCALL ABI change: `SYS_WAIT_PID` goes (status_out) -> (want_pid, flags, status_out); `pid=-1, flags=0` exactly reproduces the legacy reap-any-blocking behavior (the `wait_pid` wrapper + all 25 joey reap sites + the libthyla-rs `Child::wait` unchanged in effect). Adds a pid filter (`want_pid>0` selects one child; closes the documented wrong-pid extinction hazard) + `WAIT_WNOHANG` non-blocking (returns 0 = "not ready", an unambiguous sentinel since pid 0 is never a reapable child -- `g_next_pid` starts at 1). **The reap teardown (proc_unlink_child / on_cpu spin / thread_free walk / proc_free / #811 SLEEP_INTR) is copied verbatim -- only the SELECTION + the blocking decision + the cond filter change.** Invariants: I-8 (a WNOHANG poll must not starve; a by-pid blocking wait still progresses), I-9 (no lost wake -- the per-parent rendez wakes on every child exit, the filtered `wait_pid_cond` re-checks under r->lock; register-then-observe unchanged), death-path lifetime (exactly-once reap; the `wait_cond_ctx` is stack-local to `wait_pid_for`, outliving the `sleep`; the `c->pid` filter is an alloc-stable read so the P3-A no-`g_proc_table_lock`-in-cond reasoning holds). Closes #856 (`Child::try_wait`/by-pid `wait`). **No new spec / no new ARCH invariant** (refines an existing syscall's selection logic) per the 2026-05-23 broadening -- prose validation in `docs/reference/14-process-model.md` + this row + the focused audit + the 3 `proc.wait_pid_for_*` kernel tests (no_match / wnohang_alive_then_reap / selects_target) + SMP gate (default+UBSan x smp4/smp8 N>=10). Owed consumers (separable): tighten joey's stratumd-coexist waits to by-pid (retires the `joey.c:2451` hazard); wire pouch `wait4` -> `SYS_WAIT_PID` (retire the `0xFFFF` ENOSYS stub). |
| Initial bringup | `kernel/main.c`, `init/init.c` | Boot ordering correctness |
| Boot banner | `kernel/main.c` | Tooling ABI per `TOOLING.md §10` |

The trigger list is *cumulative*: as new audit-bearing files appear (e.g. Phase 4 adds the 9P client; Phase 5 adds futex / poll / notes / pty), they're appended here in the same PR that introduces them.

### How to run an audit round

1. Spawn a soundness-prosecutor agent (general-purpose subagent, `run_in_background: true`). Use the most capable model available.
2. In the prompt, include `memory/audit_rN_closed_list.md` contents as the "already fixed — do not re-report" preamble.
3. Scope the prompt to the surface you changed.
4. Tell the agent explicitly to prosecute, not defend. Brutal but grounded.
5. Wait for the completion notification. Do not poll.
6. Trust but verify: the agent's summary describes intent; validate quoted file:line references.
7. Fix every P0/P1/P2 finding before merge. P3 findings get tracked or closed with explicit justification.
8. Append the round's closed list to `memory/audit_rN_closed_list.md` for the cumulative do-not-re-report set.

### Prosecutor agent prompt template

```
You are an adversarial soundness prosecutor auditing {scope} against the
invariants listed in ARCHITECTURE.md §28 (the enumerated invariants).

# Scope

Commits: {SHA1}, {SHA2}, ...
Files in scope: {list}

# Invariants that MUST hold

{enumerate from ARCH §28, briefly}

# Adversarial categories to prosecute

- Privilege escalation (capability bypass, namespace escape, handle forge)
- Race conditions (wait/wake, IPI ordering, scheduler concurrency, refcount races)
- Lifetime violations (UAF, double-free, dangling Chan, dangling VMO)
- Memory safety (W^X violation, integer overflow on size paths, out-of-bounds)
- Crypto / integrity (Stratum integrity surface; janus key handling)
- Format / protocol (9P malformed messages, ELF malformed segments, DTB malformed)
- Resource exhaustion (handle table, VMO, fid pool)
- (extend per domain)

# Procedure

1. Read memory/audit_rN_closed_list.md to know the do-not-report set.
2. For each file in scope, read fully. Do NOT skim.
3. Catalog findings by severity:
   - P0: actively-broken (reproducible correctness / security / safety violation).
   - P1: latent-broken (correct today under exact test coverage, wrong under realistic deviation).
   - P2: hazard + should-land-before-merge.
   - P3: nice-to-have.
4. For each finding: file + line + prosecution chain (state → step → step → violation) + suggested fix.
5. Withdraw findings guarded by existing code. Don't re-report closed items.

Report format per finding:
## Finding F<NUM> [P<severity>]: <title>
**File**: path:line
**Invariant**: <which from §28>
**Prosecution**:
1. state that <X>.
2. attacker/crash/retry does <Y>.
3. observes <violation>.
**Suggested fix**: <1-2 sentences>

At the end: Summary with counts by severity + confidence notes on
areas you couldn't audit as deeply as you wanted.

Be brutal but grounded. Quote code; don't paraphrase it.
```

---

## Invariants that must hold

Verbatim from `ARCHITECTURE.md §28`. These are the TLA+ proof obligations AND the audit invariants. Keep in sync with ARCH.

| # | Invariant | Enforcement | Spec |
|---|---|---|---|
| I-1 | Namespace operations in process A don't affect process B | Kernel namespace isolation | `namespace.tla` |
| I-2 | Fork-grantable capability set monotonically reduces (`rfork` only reduces). Elevation-only capabilities (`CAP_HOSTOWNER`) are the sole sanctioned growth — conferred only via the `cap` device for a console-attached Proc, never by `rfork` (CORVUS-DESIGN.md §5.5.1 / C-21) | Syscall gate; `cap` device redemption | `handles.tla` |
| I-3 | Mount points form a DAG, never a cycle | Kernel mount validation | `namespace.tla` |
| I-4 | Handles transfer between processes only via 9P sessions | Syscall surface (no direct-transfer syscall exists) | `handles.tla` |
| I-5 | `KObj_MMIO`, `KObj_IRQ`, `KObj_DMA` cannot be transferred | Transfer syscall has no code path; static_assert | `handles.tla` |
| I-6 | Handle rights monotonically reduce on transfer | Syscall-level check | `handles.tla` |
| I-7 | VMO pages live until last handle closed AND last mapping unmapped | Refcount; runtime check | `vmo.tla` |
| I-8 | Every runnable thread eventually runs | EEVDF deadline computation | `scheduler.tla` |
| I-9 | No wakeup is lost between wait-condition check and sleep | Wait/wake protocol | `scheduler.tla`, `poll.tla`, `futex.tla` |
| I-10 | Per-9P-session tag uniqueness | Per-session tag pool with monotonic generation | `9p_client.tla` |
| I-11 | Per-9P-session fid identity is stable for fid's open lifetime | Per-session fid table | `9p_client.tla` |
| I-12 | W^X: every page is writable XOR executable | PTE bit check + mprotect rejection + ELF loader rejection | runtime + `_Static_assert` |
| I-13 | Kernel-userspace isolation: TTBR0 / TTBR1 split | Page table setup | runtime |
| I-14 | Storage integrity: every block from Stratum is integrity-verified | Stratum's responsibility (Merkle layer); OS observes via 9P | (Stratum-side spec) |
| I-15 | Hardware view derives entirely from DTB | No compile-time hardware constants outside `arch/arm64/<platform>/` | code review + audit |
| I-16 | KASLR randomizes kernel image base at boot | Boot init randomizes TTBR1 base | runtime + `/ctl/kernel/base` audit |
| I-17 | EEVDF latency bound: delay between runnable and running ≤ slice_size × N | EEVDF deadline math | `scheduler.tla` |
| I-18 | IPIs from CPU A to CPU B are processed in send order | GIC SGI ordering | `scheduler.tla` |
| I-19 | Note delivery preserves causal order within a process | Note queue per Proc | `notes.tla` |
| I-20 | PTY master ↔ slave atomicity | PTY data path locked | `pty.tla` |
| I-21 | Kernel executes uniformly at EL1h (`SPSel=1`); `SP_EL0` is exclusively the userspace stack | Boot sets `SPSel=1`, never lowered; per-thread kernel stack carries exception frames | `sched_ctxsw.tla` |

---

## Regression testing

- Every audit finding that can be made to fail without the fix MUST land a regression test. The test fails on the pre-fix code; passes on the post-fix code.
- Every spec-level bug demonstrated by a `{spec}_buggy.cfg` must have a corresponding runtime regression test (when feasible — some concurrency bugs are hard to trigger deterministically; in those cases the buggy-config serves as the durable regression).
- Test matrix baseline: default build + AddressSanitizer + UndefinedBehaviorSanitizer + ThreadSanitizer (from Phase 2 onward when SMP is enabled).
- Pre-commit for every substantive change: full test suite on the default build. Pre-merge for invariant-bearing changes: all matrices + all specs.

Example commands (adapt per phase):

```bash
# Default build + tests
tools/build.sh kernel && tools/build.sh userspace && tools/test.sh

# ASan
tools/build.sh kernel --sanitize=address
tools/test.sh

# UBSan
tools/build.sh kernel --sanitize=undefined
tools/test.sh

# TSan (Phase 2+)
tools/build.sh kernel --sanitize=thread
tools/test.sh

# All specs
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
cd specs && for s in *.tla; do
  java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock \
    -config "${s%.tla}.cfg" "$s" 2>&1 | tail -3
done
```

---

## Implementation patterns

### Idempotency on retry

Any function that writes durable state MUST short-circuit on clean state. If the function's contract is "on success, durable state X is recorded," then calling it twice with the same inputs and no intervening mutations must produce byte-identical durable state.

**Pattern**: carry a dirty flag. Mutations set dirty. Commits check dirty; if clean AND a durable result already exists, return cached result. If dirty, do the work + clear dirty.

### Compile-time invariants

Every on-disk, on-wire, or ABI-exposed format gets:
- `_Static_assert` (C/C++) on struct size, alignment, and discriminant ranges.
- Explicit version constants.
- Compat / ro-compat / incompat feature-flag tiers (where applicable).

Catches format drift at build time, not at runtime.

For Thylacine specifically:
- ELF loader: `_Static_assert` on ARM64 e_machine, ABI version.
- 9P wire format: `_Static_assert` on message header sizes, fid widths, tag widths.
- Handle table layout: `_Static_assert` on `struct Handle` size + alignment.
- Page table entry bit layout: `_Static_assert` on PTE bit positions (W^X invariant).
- DTB parse: `_Static_assert` on FDT magic, version expectations.

### Split big chunks into sub-chunks

When an implementation chunk exceeds one commit's reasonable scope, split into sub-chunks named Xa / Xb / Xc. Each sub-chunk lands independently with its own status-doc row, commit message, and tests. Handoff points between sub-chunks mean a context compaction at any boundary is recoverable.

### Chunk completeness — pull dependencies forward; deferral needs signoff

If the current chunk's **proper and complete** implementation depends on an item that is later on the roadmap, or on an item deferred in an earlier chunk, **strongly prefer pulling that item into the current chunk** — complete the chunk to the fullest specification possible rather than shipping a half-version built against a missing dependency. The pull-forward is the **default**, not a deviation: note it in the chunk's commit message + status row and proceed (it does not, by itself, need signoff — it is the act of *finishing the chunk*).

**Deferral is the exception, and it needs the user's signoff.** If deferring the dependency genuinely makes more sense (truly separable, large enough to be its own chunk, or better audited on its own), do not silently ship the half-version — surface it as a structured choice (the design-conversation pattern) and get the user's vote first.

**Why this is binding:** too many quiet deferrals compound into **silent omissions** — the system ends up not actually doing what scripture says it does, and nobody decided that on purpose. The default must bias toward completeness; the burden of proof is on *deferring*, not on *building*.

This is the chunk-scoped form of the convergence-bar "build vs seam" test (IDENTITY-DESIGN.md §8.1): a *dependency of the current chunk* defaults to **BUILD-now** (pull it forward); only a genuinely-separable, foreseeable-but-not-yet item is a **SEAM** — and turning a real current-chunk dependency into a seam is the thing that needs signoff. Worked example: **A-1.6 (FS-gamma)** — A-1b's persistence needed `rename`/`unlink` (roadmap-later coreutils items); rather than ship an append-log workaround around the missing syscalls, they were pulled forward (the substrate choice itself went to the user's vote).

### Crash-injection + fault-injection testing

For torn-write-sensitive paths (Stratum mount transition, persistent state machines, multi-phase commits), wire fault-injection hooks at every durable write. Test that recovery from each injection point produces a valid state. Same pattern applies to interrupt injection in schedulers, fault injection in fault-tolerant networking, and partial-failure injection in distributed systems.

For Thylacine: kernel panic during ramfs → Stratum transition; driver process kill mid-IO; 9P session drop mid-walk.

---

## Autonomy + escalation

**Default stance**: When the user grants autonomy ("you can proceed autonomously," etc.), proceed on implementation, testing, formal modeling, audit triage, commit, and push to your own branch.

**Always escalate** (autonomy does NOT cover these):

- Format breaks (on-disk version bumps, wire-protocol ABI changes, syscall interface changes).
- Destructive operations (`git push --force`, branch/tag deletion, hard reset of shared branches, database drops).
- Architectural deviations from `ARCHITECTURE.md` — either update ARCH first (with user approval) or revert the deviation.
- Cross-phase scope pivots — pulling *unrelated* future scope into the current phase, OR **deferring an item the current chunk depends on** (see "Chunk completeness — pull dependencies forward"), must be confirmed. Pulling a genuine *dependency* forward to complete the current chunk to its fullest spec is preferred and does NOT need confirmation — note it and proceed.
- Anything unclear in ARCH / ROADMAP / NOVEL / VISION / TOOLING.
- Anything visible to others (pushes to shared branches, PR creation, external API calls, Slack/email posting).
- Spending significant compute or external budget.
- Halcyon-related decisions that might change the v1.0-vs-v1.1 ship calculus (per ROADMAP §11.5 — Halcyon is final phase; v1.0-rc.1 is the shippable fallback).

**Deviation tracking**: If implementation diverges from ARCH / ROADMAP, surface it explicitly:

- In the commit message (the WHY of the deviation).
- In the affected phase status doc.
- If the deviation is load-bearing, propose an ARCH update; do not silently normalize the deviation.

---

## Git + commit discipline

- **Detailed commit messages** with prose rationale. Each commit message explains WHAT changed, WHY, and what the alternative was (if the decision was non-obvious). First line under ~70 chars; body has the reasoning.
- **Per-chunk commits**, not per-day. A chunk is a coherent, testable, revertable unit.
- **`Co-Authored-By` footer** on AI-assisted commits. Use: `Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>` (adjust model identifier per session).
- **Prefer new commits over `--amend`.** Amending rewrites history; hook failures may hide in the prior commit. Exception: if the user explicitly asks for an amend.
- **Never force-push to main**. Never force-push shared branches without explicit user approval.
- **Never skip hooks** (`--no-verify`, `--no-gpg-sign`) unless the user explicitly requests it. If a pre-commit hook fails, diagnose and fix the underlying issue.
- **Before committing**, run the full test suite on the default build. For invariant-bearing changes, run the full matrix + specs.

### Commit message structure for audit-bearing chunks

```
<scope>: <short summary> (P<severity counts if audit close>)

<paragraph: WHAT changed and WHY>

<paragraph: alternative considered, why rejected (if non-obvious)>

<bullet list: tests added, sanitizer matrix status, spec status>

<row in phase status doc updated>: docs/phaseN-status.md

<audit findings closed if applicable>:
  - F<num> [P<sev>]: <title> — fixed by <approach>
  - F<num> [P<sev>]: <title> — fixed by <approach>

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```

---

## Memory + session continuity

This project uses Claude Code's auto-memory at `~/.claude/projects/-Users-northkillpd-projects-thylacine/memory/`.

Maintain these files:

- `MEMORY.md` — one-line index, ~150 chars per entry.
- `project_active.md` — current state. What's landed, what's in progress, what's next. Update per commit.
- `project_next_session.md` — pickup pointer for the next session. Detailed. Written at every handoff.
- `audit_rN_closed_list.md` — cumulative do-not-report preamble for audit rounds. Append after every round.
- `user_profile.md` — user's role, preferences, preferred style.
- `feedback_*.md` — durable feedback that should survive context compaction.

### Handoff protocol

At every session boundary (compaction, explicit handoff, completing a phase/sub-chunk, any point where a new instance might pick up):

1. Update `project_active.md` with current state.
2. Update `project_next_session.md` with the pickup pointer: current tip SHA, what's landed, what's next, any invariants or traps the next session needs to know.
3. Update the affected phase status doc.
4. If audit findings remain open, summarize in memory.
5. Commit the memory + status updates.

### Handoff mode under budget pressure

When token/time budget is low:

- Stop at a clean commit boundary.
- Update memory + status docs thoroughly.
- Summarize to the user: what landed, what's queued, what the next session picks up.
- Do NOT land partial work just to close a chunk.

---

## Reference documentation discipline (load-bearing)

**Two parallel references, both maintained continuously, both binding for every PR**:

### A. Technical reference — `docs/REFERENCE.md` + `docs/reference/NN-*.md`

The **as-built** reference. Audience: developers, auditors, future maintainers. Distinct from `ARCHITECTURE.md` (which is design intent, including unimplemented work) — the technical reference describes *what exists in the tree right now*, with file:line citations and runtime semantics.

Each subsystem gets its own `docs/reference/NN-<subsystem>.md` file when the subsystem lands. Per-file template per `docs/REFERENCE.md` "How to read this":

- **Purpose** — one paragraph on what the layer does and where it sits in the stack.
- **Public API** — every exported function with its contract. Code blocks; not prose.
- **Implementation** — internal structure, invariants, known caveats. File:line citations. Algorithm explanations where non-obvious.
- **Data structures** — every struct with byte-precise layout, alignment, and `_Static_assert` notes.
- **State machines** — every state transition with the spec action that pins it.
- **Spec cross-reference** — formal modules that pin invariants for this layer; spec action ↔ source location mapping reference (the canonical mapping lives in `specs/SPEC-TO-CODE.md`).
- **Tests** — which suites exercise the layer, what they cover, what they explicitly don't.
- **Error paths** — every `-EXXX` return; what triggers it; what the caller is expected to do.
- **Performance characteristics** — measured numbers; budget compliance; where the bottleneck is.
- **Status** — what's implemented today vs. what's stubbed or deferred. Commit hashes cite the landing points.
- **Known caveats / footguns** — gotchas for callers; non-obvious lifetime requirements; ordering constraints.

The technical reference is **incredibly detailed and deep**. It is the document a future maintainer reads to understand a subsystem without having to re-derive everything from the code. If a section feels too thorough, it's probably right; if it feels concise, it's probably missing context. Treat the depth as a feature — it's the moat against future regressions.

### B. User reference — `docs/USER-MANUAL.md` + `docs/manual/NN-*.md`

The **user-facing** reference. Audience: people using Thylacine — operators, developers writing programs against Thylacine syscalls, sysadmins, container users, Halcyon end-users. Distinct from the technical reference (developers of Thylacine itself).

Each user-facing surface gets its own `docs/manual/NN-<topic>.md` file:

- **Getting started** — install, boot, first login.
- **Shells** — rc, bash, common patterns, namespace navigation.
- **Coreutils** — what's there, what's not, differences from GNU/BusyBox where they matter.
- **Namespaces** — how to construct one, how to inspect (`/proc/<pid>/ns`), how to compose with `bind`/`mount`.
- **Stratum administration** — pools, datasets, snapshots, send/recv, encryption, scrub, the synthetic `/ctl/` interface.
- **Containers** — `thylacine-run`, OCI image format, namespace construction, what's supported.
- **Networking** — interface configuration via `/net/`, common admin commands.
- **POSIX programming** — what works, what's deferred (`epoll`, `inotify`, `io_uring` post-v1.0), gotchas vs Linux.
- **Linux binary compat** — what runs (musl-static, musl-dynamic), what's best-effort (glibc-dynamic), what doesn't.
- **Halcyon** (Phase 8+) — usage, scroll buffer, image display, video player, customization.
- **Troubleshooting** — boot failures, recovery shell, common kernel panics, audit-trigger surfaces from a user perspective.
- **Reference for syscalls** — every syscall with man-page-quality detail. Argument types, return semantics, errno cases, examples.

Like the technical reference, the user manual is **detailed and deep**. The bar: a user landing on a topic page should be able to learn how to do the thing without leaving the page; a developer porting a Linux program to Thylacine should be able to find every relevant compat note in one place.

### Maintenance discipline (per-chunk; non-negotiable)

When a chunk lands (bug fix, refactor, new module, new feature), the author updates **both references** in the same PR:

1. **Technical reference**: extend or update the relevant `docs/reference/NN-*.md` section. New module → new section. Bug fix that touches a documented invariant → update the section after the spec. New term / acronym → glossary entry.
2. **User reference**: extend or update the relevant `docs/manual/NN-*.md` section if the change is user-visible (new syscall, new admin command, new error case, behavior change). Internal refactors typically don't touch the user manual; user-visible changes always do.
3. **Snapshot block** in `docs/REFERENCE.md` — refresh figures (test count, spec count, tip hash) on every chunk that changes them. Refresh the user-facing snapshot in `docs/USER-MANUAL.md` at the same cadence.

A PR that adds code without updating the relevant reference sections is incomplete. **Treat docs as code: doc-update-per-PR is non-negotiable. Missing docs are reverted along with their code.**

### Audit-policy extension to the references

The audit-trigger surfaces table in this document and in `ARCHITECTURE.md §25.4` covers code. The reference docs extend the audit policy: a change to a documented invariant in the technical reference updates the spec FIRST (per spec-first policy), then the technical reference, then the code, then the user reference if user-visible. If the four disagree, **the spec wins**, then the technical reference, then the code, then the user reference. The user reference can never be authoritative on internal semantics; it can only describe them.

### Why two references, not one

The technical reference and the user reference have **different audiences with different needs**. A user wants to know "how do I create a snapshot of my home subvolume?" — they don't care about the Bε-tree commit protocol. A developer wants to know "what happens to outstanding 9P tags when a session is dropped?" — they don't care about the `stratum snapshot` CLI usage. Splitting them keeps each focused; merging them produces a 1000-page document where neither audience finds what they need.

Both are first-class. Neither is optional.

---

## Phase status docs

Every phase has a status doc at `docs/phaseN-status.md`. It's the authoritative pickup guide for that phase.

Sections:

- **TL;DR** — one paragraph.
- **Landed chunks table** — rows of `| Commit SHA | What | Tests |`. One row per landed sub-chunk. Add immediately when the chunk commits.
- **Remaining work** — outstanding sub-chunks with scope notes.
- **Exit criteria status** — checklist from ROADMAP, ticked as deliverables complete.
- **Build + verify commands** — exact invocations.
- **Trip hazards** — invariants carrying into this phase, gotchas for subsequent sub-chunks.
- **Known deltas from ARCH** — owed follow-ups (things the impl needs but ARCH hasn't specified yet).
- **References** — pointers to relevant ARCH sections, specs, prior-phase docs.

Update status docs per chunk, not per phase.

---

## Style policies

- **Comments explain non-obvious WHY, never WHAT.** A well-named identifier already tells you WHAT. Never reference the current task / fix / PR ("used by X", "added for Y flow", "issue #123") — those belong in the PR description and rot.
- **No multi-paragraph docstrings.** One short line max where needed.
- **Terse responses, direct statements.** State results and decisions; don't narrate deliberation.
- **No backwards-compat shims** without explicit need. Delete dead code; don't leave re-exports with `// removed` comments.
- **Avoid comments that reference the author's intent** ("I chose X because..."). The reason goes in the commit message; the code stands on its own.
- **C99 idiomatic style** (kernel) — `struct Foo` not `Foo_t`; lowercase function names; explicit types; no `#define` magic; no GNU extensions. Plan 9 dialect tendencies are *not* used (no `auto`, no nested functions, no channel keywords).
- **Rust idiomatic style** (userspace) — standard rustfmt + clippy clean.

---

## Thematic naming — keep an eye out

Thylacine names things. Where a function, file, mechanism, or concept would otherwise carry a generic Unix/POSIX-shaped name, **look for a fitting thylacine-related word** that conveys the same meaning. The project's identity is a marsupial apex predator declared extinct in 1936 (and a Plan 9 lineage given a similar narrative); the naming should reflect that wherever it adds clarity or color without sacrificing communicative intent.

Examples already in use:
- **`extinction()`** for kernel panic / "panic level event" (ELE = Extinction Level Event). Function in `kernel/extinction.c`; `EXTINCTION:` is the agentic-loop ABI prefix.
- **Thylacine** — the OS itself.
- **Stratum** — the filesystem (a record preserved in layers, geological stratigraphy).
- **Halcyon** — the graphical shell (the calm before; the impossible return).
- **janus** — the key agent (two-faced; the boundary between worlds; from Stratum).

Sources to draw from:
- **Marsupial / dasyuromorph biology**: torpor (deep-sleep state), pouch / marsupium, joey, lineage, taxon, clade, crepuscular (active at twilight), nocturnal.
- **Thylacine specifics**: the wide-jaw display, the striped pelt, the high-pitched yip-bark vocalization, the Tasmanian bushland habitat (eucalypt, spinifex), the disputed late-20th-century sightings (cryptozoology / Lazarus species).
- **Apex-predator behavior**: stalk, ambush, hunt, run.
- **Extinction / rediscovery**: lazarus (a species presumed extinct then rediscovered), specimen, holotype, last known.
- **Plan 9 lineage** — already saturating the architecture (namespace, bind, mount, 9P, factotum-pattern, Dev, Chan). Don't double up; don't rename Plan 9-derived concepts.

Discipline:
- **Propose, don't unilaterally rename load-bearing identifiers.** Tooling ABI surfaces (`Thylacine boot OK`, `EXTINCTION:`), public function names already documented in reference docs, and cross-project surfaces (anything Stratum-aligned) require explicit signoff before renaming. The `panic → extinction` rename in P1-C set the precedent: user proposed mid-chunk; we coordinated the change across `kernel/`, `tools/test.sh`, `TOOLING.md`, `CLAUDE.md` in a single commit.
- **Hold for explicit signoff**:
  - `_hang` (the WFI halt loop) → `_torpor` candidate; held.
  - Audit prosecutor agent → potential rename to "tracker" / "hunter" candidate; **held with preference for "prosecutor"** — Stratum already uses the term and cross-project continuity matters more than thematic novelty.
- **Don't force it.** Some things should keep their standard name because the standard name is what readers expect (e.g. `mmu_enable`, `dtb_init`, `uart_putc`, `boot_main`). The bar: a thematic name should add clarity OR color without obscuring intent. If the rename makes the code less obvious to a reader who doesn't know the project's identity, the standard name wins.
- **Document the choice.** When a thematic name lands, the reference doc for the affected subsystem (`docs/reference/NN-*.md`) gets a short "naming rationale" paragraph. See `04-extinction.md` for the pattern.

When you spot a candidate while implementing — note it in the chunk's commit message or `phase1-status.md` trip-hazards as a held proposal. The user has explicitly invited more thematic suggestions ("don't be shy"); respond by surfacing the option, not by silently renaming.

---

## Build + test commands

Per `TOOLING.md`. Top-level wrappers:

```bash
# Build the kernel ELF
tools/build.sh kernel

# Build the musl + sysroot
tools/build.sh sysroot

# Build all Rust userspace components
tools/build.sh userspace

# Assemble the disk image
tools/build.sh disk

# Build everything
tools/build.sh all

# Run tests against a fresh QEMU VM
tools/test.sh

# SMP soundness gate (single boots lie -- multi-boot or it didn't happen).
# Builds default + UBSan kernels, multi-boots smp4/smp8 x default/UBSan N>=10,
# classifies CORRUPTION vs benign host-TIMING. Fails iff any boot corrupts.
tools/ci-smp-gate.sh                    # full matrix, N=10 (or: make smp-gate)
SMP_GATE_CONFIGS="default-smp4 ubsan-smp4" tools/ci-smp-gate.sh   # amplifier subset

# Interactive E2E regression net (LS-CI): expect/PTY drives a REAL console --
# login + assert rendered command output (the test that would have caught LS-1).
# Optional gate (SKIPs without `expect`). THYLACINE_ACCEL=tcg default; bounded
# retry (LS_CI_ATTEMPTS=3) tolerates host-timing flakes.
tools/test-interactive.sh               # full set (or: make test-interactive)
tools/test-interactive.sh ls-ci         # one scenario by name

# Launch a dev VM
tools/run-vm.sh

# Snapshot management
tools/snapshot.sh save <name>
tools/snapshot.sh restore <name>
```

The `Makefile` at the root provides `make kernel`, `make all`, `make test`, etc. as conventional aliases.

---

## Boot banner contract (kernel ABI with the development tooling)

Per `TOOLING.md §10`. Non-negotiable for the agentic loop to work.

The kernel prints this banner during boot. `boot_main()` (`kernel/main.c`) prints the multi-line header during late bring-up; the final `Thylacine boot OK` line is printed by `boot_mark_complete()` when **init (joey) signals `SYS_BOOT_COMPLETE`** -- after joey's boot-test asserts pass, just before it transitions to the persistent session supervisor (getty-loops `/sbin/login`). Since A-5a joey is the long-running init and does NOT exit on success, so the banner can no longer ride its reap. `SYS_BOOT_COMPLETE` is one-shot + gated on the caller being console-attached (so a spawned child cannot fake a premature banner -> a false PASS); a boot failure before the signal extincts in `joey_run` and the banner never prints.

```
Thylacine vX.Y-dev booting...
  arch: arm64
  cpus: N
  mem:  XXXX MiB
  dtb:  0xADDR
  hardening: MMU+W^X+extinction+KASLR+vectors+IRQ+canaries (unconditional); PAC/BTI/LSE conditional (P1-H; Lazarus W1)
  features: PAC,BTI,LSE,CRC32 (CPU-implemented)
  kernel base: 0xADDR (KASLR offset 0xADDR)
Thylacine boot OK
```

The `hardening:` / `features:` lines are informational (only `Thylacine boot OK` + the `EXTINCTION:` prefix are the binding tooling ABI). Since Lazarus W1 (`PORTABILITY.md §4`) the `hardening:` line lists the unconditional set and marks PAC/BTI/LSE runtime-conditional; the `features:` line reports what the running CPU implements.

A kernel **extinction** (ELE — Extinction Level Event; the thematic name for kernel panic) prints `EXTINCTION: <message>` as a recognizable prefix. Use `extinction(msg)` or `extinction_with_addr(msg, addr)` from `kernel/extinction.c`; `ASSERT_OR_DIE(expr, msg)` for assert-style checks. These two strings (boot banner success line + EXTINCTION prefix) are part of the kernel ABI with the development tooling. They do not change without updating `tools/run-vm.sh`, `tools/test.sh`, `tools/agent-protocol.md`, and this document in the same commit.

---

## The crossover at Utopia

Per `TOOLING.md §7.1`:

**Before Utopia (Phases 1-4): human-primary, agent-assisted.** Kernel skeleton, process model, device layer require close human oversight. Slow feedback loop (boot, observe, reboot); failure modes are catastrophic (kernel panic). The agent implements, runs phase exit criteria, reports clearly. Never proceeds past a panic without human review.

**After Utopia (Phases 5-8): agent-primary, human-directed.** Once Utopia boots, the agent operates with much greater autonomy. Implements a subsystem, deploys via 9P share, runs tests, iterates. Runs audit rounds. Human reviews diffs and sets direction.

This means: in Phases 1-4, ask before significant decisions; in Phases 5+, proceed and report.

---

## Stratum coordination

**Stratum is in scope: operate on it like it's your own (user-authorized 2026-05-29).** The Stratum tree at `~/projects/stratum/v2` (branch `thylacine-pouch-arm`) may be modified directly as part of Thylacine work -- root-cause, fix, test, and commit Stratum-side bugs (e.g. the `bdev_thylacine` virtio-blk port, the pouch boundary-line, the 9P server) without asking first. The Thylacine<->Stratum boundary is a single engineering surface for this project; a bug that surfaces in Thylacine but lives in Stratum gets fixed in Stratum. Standing constraints still apply on the Stratum tree: ASCII commit messages, no force-push, the user pushes (you commit), and `third_party/` stays byte-pristine. Stratum's own on-disk-format / wire-ABI breaks remain escalation-worthy (they ripple to the installer/upgrade path) -- but ordinary code fixes do not.

**Stratum v2 is feature-complete and shipping.** The POSIX surface (P8) and the 9P client interfaces (P9) both landed during 2026 Q1-Q2; Stratum exposes three concurrent ABIs that Thylacine Phase 5 binds to:

| Stratum ABI | Form | Stability | Thylacine consumer |
|---|---|---|---|
| **9P2000.L wire** | Unix socket (`stratumd`'s FS socket) or TCP | Stable; matches Linux v9fs | Thylacine kernel 9P client (the primary integration; ~Phase 5 §1) |
| **`libstratum-9p` C ABI** | `libstratum_9p_client.a` + `include/stratum/9p_client.h` | Stable per `stratum/v2/docs/ARCHITECTURE.md §10.2` | Userland tools written against Stratum's client lib (e.g., `stratum-fs-e2e`); optional for Thylacine — we'd typically reach the same FS via the kernel's mounted-9P-tree |
| **`libstm_fs` in-process C** | `libstm_fs.a` (UNSTABLE; bound to `STM_UB_VERSION`) | NOT stable | NOT consumed by Thylacine. Reserved for in-process bypass; per OS-INTEGRATION.md "always go through 9P." |

The integration target is the 9P2000.L wire surface with Stratum extensions. Per `stratum/v2/docs/OS-INTEGRATION.md`, the recommended deployment is the Linux v9fs-equivalent model: `stratumd` is a userspace daemon (one process per pool), bound to a Unix socket; the OS kernel speaks 9P over that socket; the Stratum-side server multiplexes per-connection fid namespaces. Thylacine consumes this with its own kernel 9P client — the v9fs-equivalent at the Thylacine layer.

Stratum extensions Thylacine speaks (per `stratum/v2/docs/REFERENCE.md` 9P chapter):
- `Tsync` — explicit sync barrier on a fid.
- `Treflink` — single-dataset reflink (cross-dataset is gated on Stratum's rekeying primitive; deferred upstream).
- `Tbind` / `Tunbind` — per-connection subvolume composition (Stratum-side territory; complements Thylacine's per-Proc territory at the kernel level).
- `Txattrwalk` + xattr family — POSIX xattrs end-to-end.
- 9P2000.L core: `Tlopen`, `Tlcreate`, `Tsymlink`, `Tmknod`, `Trename`, `Treaddir`, `Tstatfs`, `Tgetattr`, `Tsetattr`, `Treadlink`, `Tlock`, `Tgetlock`, `Tlink`, `Tmkdir`, `Trenameat`, `Tunlinkat`.

Boot path discipline (per Stratum OS-INTEGRATION.md §4):
- `.key` sidecar lives separately from the pool block device; the separability is the second security factor. Initramfs unwraps and feeds it to `stratumd`; never embed in the pool header.
- `stratumd` owns the block device exclusively after the initramfs hands it over.
- The presence of the FS Unix socket is the readiness signal — don't read it before it binds.
- Failure modes the boot path must surface: `STM_ECORRUPT` (Merkle mismatch — refuse to boot), `STM_EBADTAG` (AEAD MAC failure), `STM_EBADKEY` (wrong `.key`), `STM_EWEDGED` (fs marked wedged at prior unmount).

Admin surface coordination:
- `/ctl/` is itself a synthetic 9P filesystem served by `stratumd` (typically on a second Unix socket). Topology is documented in `stratum/v2/docs/reference/22-ctl.md` (pools / datasets / snapshots / scrub / events / metrics / Prometheus).
- Thylacine's `/ctl` (Phase 4 P4-D — `kernel/ctl.c`) is a *separate* kernel admin surface for OS-level introspection. The Stratum `/ctl/` is consumed BY Thylacine userspace as just another mounted 9P tree (typically at `/srv/stratum-ctl/`).

POSIX surface available from Stratum (Thylacine consumes via 9P; no per-feature work needed unless the kernel needs to mediate):
- Live in v2.x: inodes + dirents + xattrs + file seals (`F_SEAL_*`) + advisory locks (`flock` / OFD locks) + `statx` + `name_to_handle_at` + `copy_file_range` (whole-file MVP) + `reflink` (single-dataset) + `rename` family (`RENAME_EXCHANGE` / `_WHITEOUT` / `_NOREPLACE`) + `fallocate` (PUNCH/COLLAPSE/INSERT/ZERO/UNSHARE) + symlinks + hard links + `O_TMPFILE` + `posix_fadvise` + inline-data optimization + snapshots (create/delete/hold/release/rollback).
- Deferred upstream (Thylacine accommodates as Stratum lands): cross-dataset reflink, `inotify`/`fanotify`, FS-verity API, `O_DIRECT`, OTLP exposition, learned tier policy, content-defined chunking.

Coordination rules:
- Thylacine Phases 1-4 already proceeded with no Stratum dependency. Phase 5 entry depends on Stratum v2 being available, which it now is.
- Phase 5+ stays within Stratum's stable ABI envelope. Any breaking Stratum on-disk format bump (`STM_UB_VERSION`) gets reflected in Thylacine's installer / upgrade path; Stratum's ABI compatibility envelope (mount-side compat for at least one major version) covers normal in-place upgrades.
- Stratum's audit-trigger surfaces remain Stratum's responsibility; Thylacine's audit covers the OS-side integration (9P client, mount path, key handling, `/ctl/` consumption).
- Slate (Plan-9-shaped TUI daemon also served as a 9P filesystem) is shipped by Stratum at `stratum/v2/src/slate/`. Thylacine's Halcyon (Phase 8) can adopt slate directly OR build an equivalent. The adoption story is documented in OS-INTEGRATION.md §17 — Halcyon's design pass should weigh it.

Stratum's repo is at `~/projects/stratum/v2/` (use the v2 path — v1 was the earlier prototype). Reference docs of interest:
- `stratum/v2/docs/OS-INTEGRATION.md` — the integration manual (canonical for Thylacine Phase 5+).
- `stratum/v2/docs/REFERENCE.md` and `stratum/v2/docs/reference/20-9p.md` — as-built 9P semantics.
- `stratum/v2/docs/REFERENCE.md` 22-ctl chapter — admin surface trust boundary.
- `stratum/v2/docs/SLATE-DESIGN.md` — slate schema contract (Halcyon-side input).

---

## Native vs ported userspace programs (Plan 9 split)

Binding scripture under U-1 (the Utopia scripture commit): `docs/ARCHITECTURE.md §3.5` + `docs/UTOPIA-SHELL-DESIGN.md §3`. When adding a new userspace program, the decision rule is one question:

> Is this program authored within Thylacine, OR is it a port of foreign code that already expects POSIX?

- **Authored within Thylacine** → **native libthyla-rs**. The program builds against `usr/lib/libthyla-rs/` (no_std Rust, direct Thylacine syscalls). NO musl. NO Pouch boundary-line patches. Examples: `ut` (the shell), `libutopia`, the coreutils, corvus, the virtio-* drivers, the hello/probe binaries.
- **Ported foreign code** → **Pouch**. The program builds via the Pouch cross-compilation environment (musl + the `usr/lib/pouch/patches/*` boundary-line patches). Examples: stratumd, libsodium, Helix, future ports of ssh / git / python.

The boundary determines the runtime substrate. The rationale mirrors Plan 9's `libc.h` (native) / APE (POSIX ported) split: native programs benefit from being Thylacine-shaped — smaller binaries, faster startup, no impedance mismatch, fewer patches to maintain — while ported programs get POSIX-shape via the pouch boundary-line, which is the right place to do the translation work once per surface rather than at every program's syscall site.

Operational implications:
- A new utility we're authoring → libthyla-rs. If a Rust ecosystem crate seems convenient but assumes std, prefer to hand-roll the no_std equivalent (or extend libthyla-rs to provide what's needed) over reaching for Pouch.
- A new ported dependency → Pouch. Pouch-patch growth is expected and audit-bearing; new POSIX surfaces touched by a port get their own patch under `usr/lib/pouch/patches/*` and follow the existing pouch audit discipline.
- A native program SPAWNING a ported program → fine (they're separate processes; the boundary is fd-level, not library-level). Example: `ut` (native) spawns `hx` (ported via Pouch).
- A native program LINKING a ported library → not part of v1.0. If the situation arises, escalate; we'd have to design a sysroot for the native target that re-exports musl shapes, which is a meaningful new direction.

`tools/build.sh` enforces the split: the Utopia workspace builds via the `aarch64-thylacine` Rust target (no_std on libthyla-rs); ports build via Pouch's sysroot. The two paths are clearly separated.

---

## Parallel auxiliary track — native userspace apps (worktree-isolated)

A **parallel auxiliary agent** runs in a separate git worktree (sibling dir, typically `../thylacine-aux`) on branch **`aux/userspace-apps`**, established 2026-06-07 to use spare quota in parallel with the main (kernel) track. Its mission: **author NATIVE `libthyla-rs` userspace applications from the system documentation, build them to compile (never run), and produce a per-app test plan + a documentation-completeness gap report.** Every session reads this file, so the boundary is common knowledge:

- **The aux track owns `usr/apps/**` only** (its apps + a vendored `usr/apps/vendor/` for native crate forks like ratatui→nora). Its constitution is `usr/apps/AUX-DIRECTIVE.md`; its worklist + across-compaction memory is `usr/apps/AUX-ROADMAP.md`; its top deliverable is `usr/apps/DOC-GAP-REPORT.md`.
- **It never touches the main track's surface:** no `kernel/` / `arch/` / `mm/` / `specs/` / `tools/`, no edits to `docs/reference/*` (it only READS them and RECORDS gaps), and it never extends `libthyla-rs`. **It never boots QEMU** — `cargo build` is its verification ceiling (so it never contends with the main track's QEMU boots, the host-oversubscription flake source).
- **For the MAIN (this) agent:** don't edit `usr/apps/**` — it's the aux branch's domain (avoid a cross-branch collision). When `aux/userspace-apps` merges (clean — disjoint trees), its `usr/apps/<app>/TEST-PLAN.md` files become a ready-to-run in-VM test backlog, and its `DOC-GAP-REPORT.md` is the input for a docs pass: **fold verified findings into `docs/reference/*`** (the main agent owns the real reference docs). The only shared-file touchpoint is the `libthyla-rs` module list at Loom-6 (when the aux track adopts the native Loom API for its Phase-B server apps) — a trivial members-list merge.
- Roadmap shape (in `AUX-ROADMAP.md`): Phase A = native coreutils + the Thylacine-distinctive `ns`/`bind`/`srv`/`9p`/`ps`/`cap` tools + the **nora** editor arc (ratatui forked native); Phase B = Loom/network apps (design+skeleton+test-plan until Loom-6 + the net stack land); Phase C = ports (Zig/SDL) as feasibility/gap-analysis only (ports are pouch, not native, and need execution to verify).

---

## Ship-and-fallback structure (Halcyon-as-last-phase)

Per `ROADMAP.md §10` and `§11`:

- **Phase 7 produces v1.0-rc.1** — a complete, hardened, audited, Linux-binary-compatible textual OS with the network stack live, fuzz-tested for 1000+ CPU-hours per surface, 8-CPU 72-hour stress passed, all latency budgets met. This is the **shippable fallback**.
- **Phase 8 = Halcyon + v1.0 final**. Halcyon is held to last because it's the highest-risk angle. If Halcyon hits a wall, **v1.0-rc.1 ships as v1.0 and Halcyon becomes v1.1**.

Implication for sessions in Phase 7+: treat the v1.0-rc as a real ship target. Don't take Halcyon-blocking risks at Phase 7. Ship v1.0-rc cleanly even if Halcyon work is happening in parallel.

---

## Session-state files

- Built artifacts go to `build/`; not in git. `.gitignore` excludes.
- Snapshots in `build/snapshots/`; not in git.
- TLA+ tools at `/tmp/tla2tools.jar`. Install instructions above.
- 9P host share at `./share/` (created on first `tools/run-vm.sh`); not in git.

---

## When to recommend `/compact`

When all of the following hold:

- Working tree is clean (everything committed).
- Test matrix is green (default + ASan + TSan if applicable).
- The most recent audit round (if any) is closed.
- The next chunk would benefit from fresh context — typically when:
  - Cumulative tokens consumed exceed ~60-70% of the model's context budget.
  - The next chunk involves a fresh subsystem (not the one currently in cache).
  - An audit roundtrip + fix loop is queued (audit agent output is dense).

Recommendation format: short, includes rationale. "Working tree clean at tip X; tests/specs green; next chunk Y would benefit from fresh context. Suggest `/compact` here. Handoff doc updated for clean pickup."

Do NOT recommend compaction mid-chunk or with uncommitted state.

---

## When to recommend `/effort max`

For sessions involving:

- Multi-step audit roundtrips with triage + fixes + re-audit.
- Composition-heavy chunks crossing 3+ modules.
- Format-break work (on-disk version bumps, ABI changes, syscall interface changes).
- Spec-first work where the spec needs careful invariant design.
- Recovery from an audit P0/P1 that requires deep tracing.

Suggest the user run `/effort max` if not already set. Quality over speed in these contexts is non-negotiable.

---

## Self-audit before formal audit

Before spawning the formal audit agent, do a 30-60 second self-review pass on the impl + tests for known-hazard categories:

- **Lock ordering**: every multi-lock acquire matches the global rule.
- **Lifetime**: borrowed pointers documented; UAF surfaces traced.
- **Error-path cleanups**: every early-return path releases acquired resources.
- **Idempotency on retry**: dirty-flag short-circuits where applicable.
- **State-machine guards**: every transition matches its spec action.
- **Compile-time invariants**: format changes have static_asserts.
- **Boundary conditions**: integer overflow, empty inputs, max bounds.

Findings from self-review either land as a fix-in-the-same-chunk OR as an explicit "self-found before audit" addendum commit (so the audit's closed-list preamble accounts for them). Self-audit is not redundant with the formal audit; it absorbs class P1s that would otherwise be embarrassing for the formal round to find.

---

## Audit-in-flight parallel work

When the focused audit prosecutor is running in the background, do NOT idle and do NOT poll for completion (the runtime delivers a notification on completion). Two activities happen in parallel — both required, in this order:

1. **Useful non-colliding work first.** Identify work that doesn't touch the audit's file scope. Examples: documentation updates, status-doc refresh, memory-file maintenance, scripture renumbering, prep notes for the next chunk, a separate-subsystem refactor, sibling-test additions. The agent's prompt scoped its file list explicitly — treat that list as off-limits while the agent runs (don't risk creating a merge conflict with the agent's deductions).

2. **Then a self-audit on the same surface as the agent.** Prosecute the audited code adversarially yourself. Re-read every modified file. Trace each invariant. Find what the agent might miss. Two independent prosecutors catch different issues — the agent and you bias toward different categories. Treat your findings with the same authority as the agent's.

When the agent completes:
- **Merge findings**: combine its report with your self-found ones. Disposition together; do not segregate "agent findings" vs "self findings" — they're all findings with the same severity rigor.
- **Cross-check**: if the agent missed something you found (or vice versa), the gap itself is signal about audit coverage. Note it for the next prosecutor prompt's "focus areas."

This discipline is **non-optional** for any audit-bearing chunk. The cost is small (the self-audit is anyway a refinement of the pre-audit self-review per §"Self-audit before formal audit"); the value is real — round 2 prosecutors and self-audits running concurrently with round 1 have caught real P0/P1s the single-pass formal audit missed.

---

## Re-audit on dirty close

A close is **dirty** if any of:
- Any P0 returned.
- (P1 + P2 count) ≥ 6.
- The fixes themselves were structurally invasive (restructured a load-bearing mechanism, lifted a lock-order rule, changed a wait/wake protocol, removed a primitive).

On a dirty close, the fixes themselves may introduce new bugs — **schedule a follow-up audit round on the audit-close state**. The follow-up:

1. Treats the round-N closed list as do-not-re-report preamble (just like any audit).
2. Focuses prosecutor attention on **the fixes themselves**, named explicitly in a "round N+1 focus areas" section. Invasive restructures often introduce new lock-order issues, lifecycle hazards, or memory-ordering gaps.
3. Runs the audit-in-flight parallel-work discipline (above): useful non-colliding work + self-audit on the same surface.
4. Repeats until the round returns clean (0 P0, 0 P1, only documented-as-deferred P3s).

A clean close that completed via N > 1 rounds is still clean. Multiple rounds aren't a defect; they're the discipline doing its job. Each round's findings + dispositions get appended to the cumulative closed-list memory file.

The pattern caught real bugs in our practice: a round-1 audit close restructured a wait/wake mechanism (devnotes_read from single-waiter Rendez to multi-waiter poll_waiter_list to break an ABBA deadlock); the round-2 audit found that the restructure introduced a new pop-and-copy race window that lost notes under contention — a defect the round-1 fixes created that round-1 review didn't see.

---

## Plain ASCII commit messages

Commit message bodies (and the first line) use plain ASCII. Specifically:
- **No em-dashes** (`—`). Use `--` instead.
- **No Unicode arrows** (`→`, `←`). Use `->`, `<-`.
- **No section signs** (`§`). Use `section` or just the number.
- **No Unicode quotes** (`"..."`, `'...'`). Use `"..."`, `'...'`.
- **No comparison glyphs** (`≥`, `≤`, `≠`). Use `>=`, `<=`, `!=`.
- **No emoji** unless the user explicitly requests them in the message.

Why: clean diff against `git log`, clean grep over the log, consistent rendering across terminals and CI dashboards, and one fewer thing for a future maintainer's editor / pager to mishandle. Doc files (`docs/*.md`, `CLAUDE.md`) and code comments may use Unicode freely; **commit messages stay ASCII**.

Pass commit message bodies via a HEREDOC for the same robustness reason:
```bash
git commit -m "$(cat <<'EOF'
Title line under 70 chars.

Body paragraphs use plain ASCII (-- not em-dash; -> not arrow; etc.).

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Audit-close commit anatomy

A clean audit close should be two commits (recommended pattern; deviate when the trivial fixup feels excessive):

1. **Substantive close**: all P0/P1/P2 fixes + selected P3 fixes + new regression tests + updated docs + status row with `*(pending)*` placeholder. Commit message structure:
   - First line: `Phase N RXX (<chunk> scope) audit close: <P0> P0 + <P1> P1 + <P2> P2 + <P3> P3`
   - Body: per-finding section (Fixed / Deferred), one paragraph each.
   - Tests section: what was added, current counts.
   - Footer: posture (suite × sanitizer × specs status).

2. **Hash fixup**: trivial commit replacing `*(pending)*` with the actual hash from commit #1. Plus any reference-snapshot refresh.

This makes audit closes immediately greppable in `git log` and keeps status docs accurate without temporal lag.

---

## Deferred-finding discipline

When an audit surfaces findings that genuinely belong in a future chunk:

- The close commit message MUST explicitly enumerate the deferred items by priority + finding number + brief rationale.
- The future chunk is named (e.g., "deferred to P5-N replace-in-flight flag").
- If the finding is purely doc/cosmetic and can be deferred indefinitely, it goes into the relevant reference doc's "Known caveats" section with a reference number.
- Silent drops are forbidden — if a finding is dropped, the close commit must state "withdrawn: <reason>".

This protects against audit findings being lost across session boundaries. The next-session handoff doc lists any open deferred findings.

---

## Operational summary patterns

End-of-iteration summaries (the response to a completed audit / chunk) follow a consistent structure for fast review:

```
**This iteration landed (N new commits, tip <hash>)**:
- <hash1> — <one-line scope>
- <hash2> — <one-line scope>
- ...

**Posture**: <suites> × (default + ASan + TSan) green. <spec count> specs
clean. test_<X> at <count>.

**Next**: <queued chunk(s) with deps>.

**Memory**: <files updated>.
```

This structure lets the user (or a future session reading the conversation log) reconstruct state in under 30 seconds.

---

## When in doubt

1. Re-read VISION + ARCH + ROADMAP for the relevant section.
2. Check if a TLA+ spec covers it; if so, the spec wins.
3. Check the audit-trigger table; if the change touches a trigger surface, audit before merge.
4. **If you are chasing an elusive bug** — a corruption-class symptom, inconsistent repro, a cross-layer fault, or a bug a prior session "resolved" that recurred — **read `docs/DEBUGGING-PLAYBOOK.md` BEFORE theorizing** (the `elusive-bug-hunt` skill auto-surfaces the condensed method). Ground truth over theory; suspect masking-bug stacks; distrust hollow "AUDITED CLEAN" closes.
5. If still uncertain, ask the user. Confirming is cheap; getting it wrong is expensive.

The thylacine is real. So is this.
