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

**A surfaced problem preempts everything — and the host is never the default culprit.** The instant a real defect surfaces — a fault, a corruption, an unexplained exit, a result that appears on one run and not the next — **ALL other work stops.** That problem becomes the *sole* focus until one of exactly two things is true: it is **eradicated**, or the emulator/host (QEMU) is **proven** — by rigorous demonstration, not assertion — to be at fault. There is no third exit; you do not get to set it down and continue the chunk. In particular, **"host load", "host timing", "host contention", "host stall", "benign timing", and "flake" are FORBIDDEN non-explanations** — there is no "host load" you may invoke to make a red result go away; it is the convenience-seeking dodge wearing a mechanism's clothing. A failure that reproduces on one run and not the next is *nondeterministic*, and nondeterminism is the signature of a **race to be hunted — in the guest (Thylacine) until proven otherwise** — never a phantom "load" to wave at. "The host was busy" is a conclusion you may reach *only after* you have measured it and ruled out every guest cause; you may never reach for it first. This is the runtime twin of the deterministic-boot bar ("unless QEMU is at fault, which must be **proven**") and the same convenience-seeking instinct as the flake-dismissal and disownment dodges above — resist all three. (Worked failure, this project: a userspace probe SEGV'd once and passed once; the instinct invented "heavy host load" to explain the difference — pure fabrication, zero measurement — when the honest reading was "a real SMP race whose cause is unknown." Inventing the host explanation *is* the breach; the correct move is to drop everything and hunt the race.)

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
| `docs/AUDIT-TRIGGERS.md` | The full audit-trigger surface table (moved verbatim from this file 2026-08-05). One row per audit-bearing surface: files + invariants + the per-chunk prosecution addenda. Cumulative; binding. |
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

The committed spec inventory is **34 modules** (`ls specs/*.tla | grep -v TTrace`
-- re-derive it rather than trusting this number; it was stale at 28 until
LINEAGE L-4 measured it, and the ARCH table was stale by the same six rows, so
the two agreed with each other instead of with the tree). The authoritative table lives
in `ARCHITECTURE.md §25.2`): `scheduler` / `territory` / `handles` / `burrow`
/ `9p_client` / `poll` / `pipe` / `tsleep` / `corvus` / `sched_ctxsw` /
`sched_oncpu` / `sched_alpha` / `asid` / `death_wake` / `loom` /
`loom_multishot` / `loom_order` / `cons_poll` / `loom_devgone` / `allowance` /
`net_poll` / `net_poll_teardown` / `weft` / `weft_readiness` /
`sched_tickless` / `sched_rebalance` / `fs_cache` / `debug_stop`, each with clean
cfg(s) + buggy-cfg counterexamples (121 buggy cfgs as of 2026-08-16 -- re-derive
with `ls specs/*buggy*.cfg | wc -l` rather than trusting this; it read 100 for
long enough to be off by 21). Three of the Phase-0 planned nine
(`futex.tla`, `notes.tla`, `pty.tla`) were dropped per the 2026-05-23
suspension — torpor + notes are prose-validated; PTY is unbuilt (LS-8, #952).

Features that clearly benefit: scheduler IPI, territory bind/mount, handle transfer, BURROW lifecycle, 9P pipelining, poll wait/wake, futex wait/wake, note delivery, PTY master/slave atomicity.

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

**RE-ENABLED, surface-by-surface (six instances of re-enabling point (a); the
verbatim records moved to `specs/SPEC-TO-CODE.md` "Spec-first re-enablement
record" 2026-08-05):** the SMP scheduler/thread-lifecycle (`sched_oncpu` +
`sched_alpha`, 2026-06-05), the ASID generation-rollover (`asid`, 2026-06-10),
the death-wake cascade (`death_wake`, 2026-06-10), the hardware allowance
(`allowance`, I-34, 2026-06-15), the capability network dataplane (`weft`,
I-37, 2026-06-20), and the debugger stop/continue/step machine (`debug_stop`,
I-39, 2026-07-14). Later re-enablements are recorded per-row in
`docs/AUDIT-TRIGGERS.md`. Re-enabled for THOSE surfaces only; the broader
suspension stands elsewhere.

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

**The full trigger table lives in `docs/AUDIT-TRIGGERS.md`** (binding
scripture; moved there verbatim 2026-08-05 so this always-loaded file stays
small -- the per-row prosecution detail is ALSO expanded in
`ARCHITECTURE.md §25.4`, which many rows declare authoritative; unifying
the two is task #152). **Before modifying any file on the index below, read
that surface's full row.**

**Context economy on `docs/AUDIT-TRIGGERS.md` (binding).** The file is
~440 KB (~110K tokens); a whole-file `Read` is never justified and the
default Read cap would silently truncate it anyway. Locate the row first
(grep the index below, or grep the file by surface keyword), then `Read`
ONLY that row's line window. To append a chunk's new row: `Edit` after
that windowed read -- `Edit` requires *a* prior read of the file, not the
whole file. Never `Write` (whole-file replace) it. The same discipline
applies to every large scripture file: grep to locate, window to read,
`Edit` to change.

The index (one line per row; refresh with the table):

- **Exception entry + EL0-entry trampolines** -- `arch/arm64/start.S`, `arch/arm64/exception.c`, `arch/arm64/vectors.S`, `arch/arm64/userla ...
- **Halls of Extinction crash dump** -- `arch/arm64/halls.c`, `arch/arm64/halls.h`, `arch/arm64/exception.c` (the four entry wrapp ...
- **Page fault + COW + W^X** -- `arch/arm64/fault.c` (`userland_demand_page` holds `p->vma_lock`), `kernel/syscall.c` (`SY ...
- **REVENANT file-backed demand-paged exec (the Image cache + the `BURROW_TYPE_FILE` fault arm)** -- `kernel/exec.c` (`exec_setup_from_spoor` + `exec_read_header` [OOB header-buffer guard] + ...
- **Boot-time LSE alternatives-patcher (Lazarus W1.5)** -- `arch/arm64/alternatives.{c,h}` (the `ALTERNATIVE()` macro emitting `.altinstructions` + ` ...
- **GICv2 driver + EL1 virtual timer (Lazarus W2)** -- `arch/arm64/gic.c` (the new GICv2 path: `gic_init_v2` + `dist_init_v2` + `gic_cpu_config_v ...
- **Kernel CSPRNG (software-RNG, Lazarus W3)** -- `kernel/chacha20.{c,h}` (the pure ChaCha20 keystream primitive), `kernel/random.c` (the fo ...
- **Hardware allowance / I-34 (Menagerie build-arc 2)** -- NEW `kernel/include/thylacine/allowance.h` (`struct Allowance` { `mmio[]` PA windows + `ir ...
- **devpci: mediated PCI topology + the devhw synth child (Menagerie 6b-1)** -- NEW `kernel/devpci.c` (`dc='P'`, name="pci"; the devdev/devctl read-only dir-Dev: reuse-`n ...
- **dev9p.poll readiness bridge + the 9P-client async reply-dispatch (net-6b-2b)** -- NEW `kernel/dev9p_poll.c` (the `dev9p_poll` Dev `.poll` slot + the global poll-pump kthrea ...
- **netd: the network daemon -- smoltcp embedding + NIC ownership (NET-DESIGN.md the #68 charter)** -- NEW `usr/netd` (the warden-bound `virtio-pci:1` driver: `impl libdriver::Driver`; the smol ...
- **Syscall byte-I/O staging + bulk uaccess + the per-service bulk ring (CF-3 A+B)** -- `kernel/include/thylacine/syscall.h` (`SYS_RW_MAX` 4096 -> 128 KiB + the new `SYS_RW_STACK ...
- **Allocator** -- `mm/buddy.c`, `mm/slub.c`, `mm/magazines.c`
- **Userspace virtio-PCI transport (`KObj_PCI` + the 3 PCI syscalls; pci sub-arc)** -- `kernel/pci_handle.{c,h}` (the `KObj_PCI` kobj: exclusive per-(bus,dev,fn) `g_pci_claims` ...
- **ASID generation-rollover (HOLOTYPE RW-1 B-F1)** -- `arch/arm64/asid.{c,h}` (the rolling allocator: `asid_generation` + the ASID bitmap + per- ...
- **Scheduler (SMP redesign, deep-smp-review 2026-06-05; #360 preemption discipline 2026-07-04; #33 SYS_YIELD 2026-07-05)** -- `kernel/sched.c` (the `on_cpu` protocol + the multi-step switch + `try_steal`/`pick_next` ...
- **Tickless idle (NO_HZ_IDLE; the TI arc)** -- `arch/arm64/timer.c` (`timer_arm_oneshot_cnt` + the pure `timer_oneshot_tval` -- the one-s ...
- **Territory** -- `kernel/territory.c`
- **Handle table** -- `kernel/handle.c`
- **VMO / BURROW** -- `kernel/burrow.c`, `kernel/include/thylacine/burrow.h`
- **Weft cross-Proc Burrow-share (the I-37 dataplane substrate)** -- `kernel/burrow.c` (NEW `burrow_share_into(dst, v, vaddr, prot)` -- map an existing Burrow ...
- **tapestryd: the compositor + the orphaned-weave reaper (Tapestry G-3; I-40 present half)** -- `usr/tapestryd/` (the warden gather-bound persistent compositor: `gpu.rs` the synchronous ...
- **tapestryd V-3a coherent ring (I-9 kick + I-45 ring scope + the drain-cap DoS bound; Warp-6 V-3a)** -- `usr/tapestryd/src/server.rs` (the `ctx/<id>/ring/<ridx>/{info,map,kick,fence}` subtree: `WARP_RING`=1<<43 + the disjointness assert; `wring_mint`/`wring_kick` [I-9 re-scan + the `WARP_RING_MAX_DRAIN_PER_KICK` cap]; the fence park + `poll_ring_fences`; `wring_teardown`; the ring test levers), `usr/tapestryd/src/gpu.rs` (`create_ring_blob`), `usr/warp-prove/src/main.rs` (`ring_prove`) ...
- **tapestryd V-3b-1a HOST3D + MAP_BLOB ring substrate (Model B; venus-ctx scope; I-45/I-32; Warp-6 V-3b-1a)** -- `usr/tapestryd/src/gpu.rs` (`create_host3d_blob` [HOST3D nr_entries=0 -> HDR+32] + `map_blob` [MAP_BLOB -> hostmem_base+offset, `RESP_OK_MAP_INFO` residue-guarded] + `unmap_blob` + the `host3d_probe` 2-arm venus-ctx/device-global self-test + the `HOST3D_PROBE_*` static-assert), `tools/warp-host.sh` + `tools/test-venus-verdict.sh` (the venus-verdict host3d gate) ...
- **tapestryd V-3b-1b hostmem guest-map + the SYS_BURROW_FROM_HOSTMEM client binding (Model B; I-45/I-32/I-37; Warp-6 V-3b-1b)** -- `usr/lib/libthyla-rs/src/lib.rs` (`t_burrow_from_hostmem` FFI + `T_CACHE_*` consts, the V-3 client wrapper V-2 left unbuilt) + `usr/lib/libthyla-rs/src/hardware.rs` (`PciDev::burrow_from_hostmem`) + `usr/tapestryd/src/gpu.rs` (`HostmemAllocator` + `hostmem_map_probe` [host-dictated cache via `map_info_to_cache`; sentinel round-trip; `t_burrow_detach`] + the `HOSTMEM_PROBE_*` static-assert), `tools/warp-host.sh` + `tools/test-venus-verdict.sh` (the venus-verdict hostmem gate) ...
- **tapestryd V-3b-1c-1 persistent hostmem ring engine (Model B; I-45/I-32/I-7; Warp-6 V-3b-1c-1)** -- `usr/tapestryd/src/gpu.rs` (`HostmemAllocator` first-fit free-list + oob/overlap double-free guard, hoisted into a persistent `Gpu.hostmem`; the reusable `mint_host3d_ring`/`drop_host3d_ring` lifecycle [`HostRing` non-Copy single-use token, by-value drop, full error-unwind, host-dictated cache, `u32::try_from` size guard] + the 2-ring/physical-reread/re-mint-reuse `hostmem_ring_probe` + `HOSTMEM_PROBE_RES_2`), `tools/warp-host.sh` + `tools/test-venus-verdict.sh` (the venus-verdict hostmem-RING gate, 24/24) ...
- **tapestryd V-3b-1c-2a server host3d-ring path (Model B; I-45/I-32/I-7; Warp-6 V-3b-1c-2a)** -- `usr/tapestryd/src/server.rs` (`WarpCtx.venus_ctx` lazy per-client capset-4 ctx [`wctx_venus_ensure`, id=`WARP_VENUS_CTX_BASE`+slot, disjoint-band static assert -- the conv-probe alias catch]; the `host3d` ring flavor [`wring_mint`/`wring_install_host3d` via the 1c-1 engine, shared I-32 budget]; `wring_teardown` host3d arm [non-Copy move + early-return, no double-unref]; `wctx_finish` venus destroy [both arms, F1]; `wring_kick` host3d fail-closed guard; `warp_host3d_selftest`), `usr/tapestryd/src/gpu.rs` (`ctx_create_venus`), `usr/tapestryd/src/main.rs` (serve() self-test call), `tools/warp-host.sh` + `tools/test-venus-verdict.sh` (28/28) + `tools/warp/boot-probe.sh` (capture filter for the `warp host3d-ring` line) ...
- **tapestryd V-3b-1c-2b client-claimable host3d ring: F1 weft arm + SYS_HOSTMEM_REFCOUNT + F2 observe-and-reap (Model B; I-7/I-37/I-45/I-32; Warp-6 V-3b-1c-2b)** -- NEW `SYS_HOSTMEM_REFCOUNT`=108 (`kernel/syscall.c` `hostmem_refcount_query` [va->hostmem VMA->handle+mapping sum, leak-closed] + `syscall.h` + `libthyla-rs` `t_hostmem_refcount`); F1 `kernel/weft.c` `weft_binding_alloc_maponly` HOSTMEM arm; F2 `usr/tapestryd/src/gpu.rs` (`retire_host3d_ring` reap-at-refcount==1-else-PARK + `reap_hostmem_parked` mint-reclaim + cap) + `server.rs` (`wring_weft_ensure` host3d share + `wring_teardown` disarm-then-retire); tests `weft.hostmem_share` + `weft.hostmem_refcount` (the F1 in-flight-claim window). Fable audit round-1 0/1P1/1P2/1P3 all-fixed (F1 = mapping_count missed the transferred claim pin -> the SUM); DIRTY -> round-2 on the fix ...
- **tapestryd multi-queue submit-fence (the F3 seam): per-timeline venus fences + the timelines file + the non-parking submit (I-45/I-9/I-7; Warp-6 GPU-submit chunk)** -- `usr/tapestryd/src/gpu.rs` (`FenceTag.ring_idx` + INFO_RING_IDX + the vindication lane retention), `server.rs` (`timeline_signaled[4]` + `ctx/<id>/timelines` + `submit1..3` + `poll_fences` DELIVER-TO-ALL), mesa patch 0015 (per-timeline ledgers + the transport mutex + the one-parker protocol + `max_timeline_count`=4), `tools/warp/boot-probe.sh` ...
- **MMU user-PTE clear + TLBI** -- `arch/arm64/mmu.c::mmu_uninstall_user_pte / mmu_uninstall_user_range`, `kernel/burrow.c::b ...
- **Errno ABI surface + `snare:*` fault-note family** -- `kernel/include/thylacine/errno.h` (T_E_* registry; ABI-pinned by `_Static_assert`s to POS ...
- **Memory-model defense-in-depth (F3 + F4 + F5)** -- `mm/phys.c::phys_init` (RAM cap), `mm/phys.c::alloc_pages` (KP_ZERO barrier), `arch/arm64/ ...
- **9P client (pipeline restoration, #841)** -- `kernel/9p_client.c`, `kernel/9p_session.c`, `kernel/9p_transport.c`, `kernel/9p_attach.c` ...
- **Pipe wait/wake** -- `kernel/pipe.c`
- **poll** -- `kernel/poll.c`
- **Notes / signals** -- `kernel/notes.c`, `kernel/devnotes.c`, `kernel/include/thylacine/notes.h`, `kernel/proc.c` ...
- **Capability checks** -- All syscall entry points
- **KASLR / ASLR** -- `arch/arm64/start.S`, `arch/arm64/kaslr.c`
- **ELF loader** -- `kernel/elf.c`
- **ELF loader: ET_DYN placement + AT_ENTRY (DISTRO D-2)** -- `kernel/elf.c` (the `e_type` gate + the one-place PIE bias), `kernel/inclu ...
- **`AT_HWCAP` exec-auxv CPU-feature word (the CF-4 A AEAD lever)** -- `arch/arm64/hwfeat.{c,h}` (`g_hw_features.linux_hwcap` — the Linux-uapi-numbered word deri ...
- **`burrow_attach` / `burrow_detach`** -- `kernel/syscall.c` handlers, `kernel/burrow.c`, `kernel/vma.c`
- **Overcommit memory: lazy-anon demand-zero + decommit + the I-32 VMA-count axis** -- `kernel/syscall.c` (a NEW `sys_burrow_attach_lazy_for_proc` + handler — eager `sys_burrow_ ...
- **`torpor_wait` / `torpor_wake`** -- `kernel/torpor.c`, `kernel/syscall.c` handlers, `arch/arm64/uaccess.S` (new `uaccess_load_ ...
- **`thread_spawn` / `thread_exit` / multi-thread exit** -- `kernel/thread.c::thread_create_user`, `arch/arm64/context.S::thread_user_trampoline`, `ke ...
- **pouch AF_INET socket-compat boundary-line (net-5)** -- `usr/lib/pouch/patches/0016-pouch-net-sockets.patch` (the 11-file musl stacking patch, on ...
- **pouch pthread boundary-line** -- `usr/lib/pouch/patches/0004-pouch-pthread.patch` (the 8-file boundary-line patch against v ...
- **argv pass-through to spawn** -- `kernel/syscall.c` (new SYS_SPAWN_* with argv buffer OR extended SYS_SPAWN_WITH_PERMS), `u ...
- **stratumd HW-cap spawn** -- `usr/joey/joey.c` (calls `t_spawn_with_perms` granting `CAP_HW_CREATE` to stratumd), `kern ...
- **stratumd virtio-blk driver arm** -- (Stratum branch `thylacine-pouch-arm`) `src/io/bdev_thylacine.c` (~500-800 LOC port of the ...
- **native fstat + lseek + pread/pwrite + wstat-posture surface** -- `kernel/include/thylacine/syscall.h` (SYS_FSTAT=50 + SYS_LSEEK=51 + **SYS_PREAD=85 + SYS_P ...
- **pouch abort -> _Exit override** -- `usr/lib/pouch/patches/0011-pouch-abort.patch` (overrides musl's `src/exit/abort.c` to `_E ...
- **Stratum bdev_thylacine rights mirror** -- (Stratum-side `thylacine-pouch-arm` branch) `src/block/bdev_thylacine.c` `T_RIGHT_SIGNAL` ...
- **pouch mallocng assert -> _Exit override** -- `usr/lib/pouch/patches/0012-pouch-mallocng-crash.patch` (overrides mallocng's `src/malloc/ ...
- **Thylacine mkfs RNG seed pinning** -- `tools/build.sh::build_stratum_pool_fixture` (THYLACINE_MKFS_SEED + THYLACINE_MKFS_PRESERV ...
- **9P-srvconn transport adapter** -- `kernel/9p_srvconn_transport.{c,h}` (new; parallel to `kernel/9p_spoor_transport.{c,h}`); ...
- **`SYS_ATTACH_9P_SRV` syscall** -- `kernel/syscall.c::sys_attach_9p_srv_handler` (new; parallel to `sys_attach_9p_handler`), ...
- **`SYS_PIVOT_ROOT` syscall + `territory_pivot_root`** -- `kernel/syscall.c::sys_pivot_root_handler` (new), `kernel/territory.c::territory_pivot_roo ...
- **`kernel_attached` SrvConn gate (16c-integration)** -- `kernel/srvconn.{c,h}::srvconn_set_kernel_attached` + `srvconn_is_kernel_attached`; `struc ...
- **Host-side pool populate via existing `stratumd + stratum-fs` (host build infra)** -- `tools/build.sh::build_stratum_pool_fixture` (orchestrates stratumd start, stratum-fs writ ...
- **FS-mutation syscalls (create / fsync / readdir)** -- `kernel/syscall.c` (`sys_walk_create_handler` / `sys_fsync_handler` / `sys_readdir_handler ...
- **FS-mutation syscalls (rename / unlink) -- FS-gamma** -- `kernel/syscall.c` (`sys_rename_handler` / `sys_unlink_handler`), `kernel/dev9p.c` (new `d ...
- **File metadata: owner/group + chmod/chown (A-2a)** -- `kernel/include/thylacine/syscall.h` (`struct t_stat` 72 -> 80 with `uid`@72 + `gid`@76 + ...
- **Kernel rwx enforcement layer (A-2d)** -- `kernel/syscall.c` (`perm_check` insertions: `sys_walk_open_handler` [X on src + R/W on ta ...
- **O_PATH byte-I/O block (`CWALKONLY`, #81)** -- `kernel/include/thylacine/spoor.h` (new `CWALKONLY` flag in the Spoor `flag` field), `kern ...
- **A-3: 9P identity presentation + dev9p enforcement activation** -- `usr/lib/pouch/patches/0006-pouch-sockets.patch` (SO_PEERCRED shim: `ucred.uid = info.prin ...
- **Group termination / cross-thread shootdown (`SYS_EXIT_GROUP`)** -- `kernel/proc.c` (`proc_group_terminate` + the single set-once `group_exit_msg` on `struct ...
- **Universal death-interruptible sleep (`*_INTR`)** -- `kernel/sched.c` (`sleep`/`tsleep` generalized register-then-observe of `group_exit_msg` + ...
- **A-4 capability model + legate elevation (`rfork` I-2 strip; `cap` device clearance grant/redeem)** -- `kernel/include/thylacine/caps.h` (`CAP_GRANT_CLEARANCE`=1<<6 fork-grantable; `CAP_DAC_OVE ...
- **A-4b cross-process kill (`/proc/<pid>/ctl` + `CAP_KILL`)** -- `kernel/devproc.c` (the `ctl` write parses `kill`/`killgrp` -> `proc_group_terminate` unif ...
- **A-4c trusted path: kernel console RX + SAK** -- `arch/arm64/uart.c` (RX IRQ + IMSC.RXIM unmask + RX-FIFO drain + `DR.BE` BREAK detect; PL0 ...
- **`/dev` namespace front-door + the I-27 gate-at-namespace-open (#57b)** -- `kernel/devdev.c` (NEW: the aggregating directory Dev, dc='d', name="dev"; `.attach` -> QT ...
- **Interactive Ctrl-C: `interrupt` as a real note (LS-5)** -- `kernel/include/thylacine/syscall.h` + `kernel/syscall.c` (new `SPAWN_PERM_CONSOLE_OWNER` ...
- **A-5 login + session lifecycle + per-user encrypted home** -- NEW `usr/login/` (native `/sbin/login`, libthyla-rs: SAK-gated `/dev/cons` prompt -> corvu ...
- **A-5c RECOVER recovery keyslot (corvus)** -- `usr/corvus/src/main.rs` (the new `VERB_RECOVER`=8 handler [`subject_kind` 0=system / 1=us ...
- **`SPAWN_PERM_MAY_POST_SERVICE` one-hop delegation (A-5b #827b)** -- `kernel/syscall.c` (the `SYS_SPAWN_*` perm-grant gate, now PER-BIT: `SPAWN_PERM_CONSOLE_TR ...
- **Pathname resolution (`stalk`) + namespace-resident `/srv`** -- the resolver (`stalk` + `cross_mounts`/`domount` + the in-call `trail`; folds in / superse ...
- **Symlink expansion in `stalk` (DISTRO D-1; the I-28 refinement)** -- `kernel/stalk.c` (`stalk_expand_link` + the `restart:` label + the `base` re-anchor), `k ...
- **Exec from the namespace (spawn binary resolution)** -- `kernel/syscall.c` (`exec_load_from_namespace` -- the new helper: resolve the program name ...
- **Namespace layout: /proc + /ctl mounts (#57a)** -- `kernel/devramfs.c` (the synthetic `/ctl` mount-point dir added to `g_ramfs_synth_dirs[]` ...
- **Per-Proc cwd (`SYS_CHDIR` / `SYS_GETCWD` + the `SYS_OPEN` relative->cwd join)** -- `kernel/include/thylacine/territory.h` + `kernel/territory.c` (`Territory.dot_path` -- the ...
- **Loom -- the io_uring-inverted 9P ring transport (KObj_Loom + SQ/CQ rings + registered handles + the pluggable-completion 9P-engine seam)** -- `kernel/include/thylacine/loom.h` (the ABI: `loom_sqe`/`loom_cqe`/`loom_ring_hdr`/`loom_pa ...
- **Proc exit handle-close (#926)** -- `kernel/proc.c` (`proc_close_handles_at_exit` -- the static helper, since #68 setting `exi ...
- **`wait_pid` selection + non-blocking (`SYS_WAIT_PID` v2 / `wait_pid_for`, U-7-pre)** -- `kernel/proc.c` (`wait_pid_for(want_pid, flags, status_out)` the new core + the filtered ` ...
- **Resource/DoS floor -- per-Proc page/thread/child caps (#65)** -- `kernel/include/thylacine/proc.h` (`PROC_PAGE_MAX` / `PROC_THREAD_MAX` / `PROC_CHILD_MAX` ...
- **Namespace name retention -- Spoor.path (#66)** -- NEW `kernel/include/thylacine/path.h` + `kernel/path.c` (`struct Path { int ref; u32 len; ...
- **Wall clock + RTC (PL031) + time/identity syscalls (LS-K)** -- `arch/arm64/rtc.{c,h}` (the PL031 driver: `dtb_get_compat_reg("arm,pl031", ...)` discovery ...
- **Monotonic-clock vDSO page (#343)** -- NEW `kernel/vdso.c` (`vdso_init` -- one kernel-owned `burrow_create_anon(PAGE_SIZE)` held ...
- **Kaua console-TUI substrate: the cons/consctl backend + the ut raw-mode dance (LS-7)** -- `usr/lib/kaua/src/term.rs` (the backend: the VT/ANSI input parser fd 0 -> KeyEvent; the da ...
- **Pollable console + termios/`consctl` line discipline (LS-8)** -- `kernel/cons.c` (LS-8a: a `poll_waiter_list` embedded in the cons layer + the IRQ-set `pol ...
- **`/dev/cons` drain/feed renderer backend + `SPAWN_PERM_CONSOLE_RENDERER` (Tapestry G-4)** -- `kernel/cons.c` (the `cons_drain` mirror tap in `cons_emit` + the bounded drop-OLDEST ring ...
- **`/env` per-Proc environment device (Go Stage 4a, G15)** -- NEW `kernel/env.{c,h}` (`struct Env` { atomic `ref` + `lock` + `entries[ENV_MAX_ENTRIES]` ...
- **POUNCE: fused walk+getattr resolution (`Twalkgetattr` 140/141 + the stalk pounce + `SYS_STAT` = 88)** -- `kernel/9p_wire.{c,h}` + `kernel/9p_client.c` (`p9_client_walkgetattr`; NOFID query mode), ...
- **The Larder: guest-side 9P FS cache (L1c substrate + attr sub-cache; L1d dentry sub-cache; L1e page sub-cache + cacheability gate)** -- NEW `kernel/larder.c` + `kernel/include/thylacine/larder.h` (`struct larder` on `p9_client` -- a near-leaf ...
- **FID-LIFECYCLE: async-clunk + cached-open (the fidless close-to-open open)** -- **async-clunk**: `kernel/9p_client.c` (`p9_client_clunk_async` fire-and-forget + the owner ...
- **Kernel debug surface: `/proc/<pid>` debug-fs (stop/resume + regs/mem/kregs/kstack/wait; I-39; Go IDE Stage 8a-1)** -- `kernel/devproc.c` (the debug files + `devproc_debug_authorized` [the I-39 two-axis gate] ...
- **PTY / job control: sessions + process groups + the pts registry + the tty seam + the job-control stop (I-20 stop leg; PTY-1 kernel arc)** -- `kernel/proc.c` (`proc_setsid`/`setpgid`/`getpgid`/`getsid`; `notes_post_pgrp`/`notes_post ...
- **ptyfs: the pseudoterminal server + /dev/pts (I-20 data path; the PTY-2 arc)** -- `usr/ptyfs/` (the native device-less /srv 9P server: the Conn/fid table + dispatch; the `P ...
- **pouch pty boundary-line (PTY-3)** -- `usr/lib/pouch/patches/0021-pouch-pty.patch` (sixteen files: the seam numbers 89-92 + 95-9 ...
- **JIT code Burrow + `SYS_ICACHE_SYNC` + `CAP_JIT` (the Clade arc, CL-7k; I-42)** -- **AS-BUILT at CL-7k** (`1f0e66c0` + `5633d056`) (`docs/LLVM-DESIGN.md` §8 + `docs/JIT-ON-W ...
- **Spawn-time page-budget (the Clade arc F4; I-32 composition, CL-5)** -- `kernel/include/thylacine/proc.h` (`Proc.page_budget` + `Proc.page_peak` + `PROC_PAGE_HARD ...
- **Process creation: `execve` + shared address spaces + COW `fork` (the LINEAGE arc, L-1..L-7; I-44)** -- **L-1 through L-5 LANDED (stock `fork()` works); L-6 NEXT (the VIVARIUM clone/execve/wait4 ...
- **VIVARIUM: the syscall-entry phenotype branch + the spawn-time declaration (V-1b; I-43)** -- `kernel/syscall.c` (`syscall_dispatch`'s phenotype branch -> `viv_linux_dispatch`; the TIE ...
- **Warp: the GPU seam -- the GPU-BO subtype + the `/dev/warp` tree + the fenced controlq (Warp-2; I-45)** -- `kernel/dma_handle.{c,h}` (the subtype enum + the 64 MiB envelope), `kernel/syscall.c` (`SYS_DMA_CRE ...
- **File-backed EL0 mmap: the phenotype FILE arm (DISTRO D-3; I-36 GENERALIZES, I-12 + I-32 on the line)** -- **D-3a + D-3b + D-3c AS-BUILT.** `kernel/include/thylacine/viv ...
- **DISTRO D-4: the PT_INTERP rewrite to the interpreter (exec dispatch)** -- `kernel/exec.c` (NEW `exec_interp_argv` + the rewrite block ...
- **Per-mount `MNOEXEC`: the executable-mapping vouching gate (#217; I-12 PROVENANCE half)** -- `kernel/include/thylacine/territory.h` (`MNOEXEC` 0x0010 in the existing ` ...
- **Initial bringup** -- `kernel/main.c`, `usr/joey/joey.c`
- **Warp-6 V-2: host-visible BAR mapping (`SYS_BURROW_FROM_HOSTMEM` + `BURROW_TYPE_HOSTMEM` + the mmu attr-index widening + the F1 death-quiesce; I-45/I-32/I-37)** -- `kernel/syscall.c` (`sys_burrow_from_hostmem_handler` + `hostmem_resolve_subrange` + the `sys_weft_share_for_proc` gate), `kernel/burrow.{c,h}` (`burrow_create_hostmem` + the 3 type arms + share admission), `arch/arm64/mmu.{c,h}` + `arch/arm64/fault.c` (`mmu_install_user_pte_attr`), `kernel/pci_handle.{c,h}` + `kernel/proc.c` (the `hostmem_burrows` counter + `kobj_pci_quiesce_dma_only` + the death-path branch), `kernel/weft.{c,h}` (`WEFT_BIND_HOSTMEM`) ...
- **Boot banner** -- `kernel/main.c`

The trigger list is *cumulative*: a chunk that adds an audit-bearing surface appends its full row to `docs/AUDIT-TRIGGERS.md` and a one-line entry to the index above, in the same PR that introduces it.

### The reviewer agent (`.claude/agents/holotype-reviewer.md`)

The prosecutor is a **dedicated agent definition**, not an inlined general-purpose subagent: `.claude/agents/holotype-reviewer.md` pins `model: fable` + `effort: max` + the standing prosecute discipline, so every round spawns it identically with only a scoped per-round prompt on top.

**The rule: run every review on the highest available FABLE at max effort; fall back to the highest available OPUS at max effort when Fable is unavailable.** As of 2026-07-28 that reads **Fable 5 primary, Opus 5 fallback** — but the rule is the *highest available version of each family*, not those version numbers, so a later Fable/Opus supersedes them automatically without editing this row. (Fable was re-enabled by the user 2026-07-04 after the 2026-06-13..07-04 US-Government-policy restriction; `memory/feedback_reviewer_model.md` is the single home for this decision.)

**Why Fable is preferred:** Opus is the primary IMPLEMENTATION agent on this project. A prosecutor drawn from the same family shares its blind spots — the same priors about what "looks fine", the same habits of reading past a given construction. Family diversity is *one* axis of the review's value, so the reviewer should ideally come from a *fundamentally different* lineage than the author. Note the tier in the close (as prior rounds did) rather than silently treating an Opus round as identical.

**NEVER SKIP A ROUND FOR WANT OF FABLE (user, 2026-08-14).** When Fable is unavailable for ANY reason — credits exhausted, capacity, a classifier false positive — **run the highest available model below it, even though it matches the implementation agent's family.** Do not defer the round and do not leave the surface unreviewed.

The reasoning corrects an over-narrow reading of the paragraph above: family diversity is only ONE of the two things a subagent review buys. The other is **context independence** — the reviewer has not read the author's reasoning, did not watch it talk itself into anything, and is not anchored by the justifications the author wrote as it went. A same-family prosecutor keeps that second property *in full*. So a same-family round is not near-worthless; it is a genuinely independent read that shares one axis with the author. **A same-family review beats no review, every time** — the cost of an unreviewed soundness surface dwarfs the cost of being independent on one axis instead of two.

When the fallback engages, exploit what it does have: tell the prosecutor in its prompt that (a) family diversity is not what it brings this round, (b) context independence is — so it must RE-DERIVE load-bearing claims from the code rather than accept comments, commit messages, or prior self-audit arms as evidence, and (c) the one reflex it must consciously fight is agreeing with a construction *because it is the construction it would also have written*.

A fallback round that FINISHES is closed — no Fable re-run is owed (user, 2026-08-03). Only a round that DIED without producing a report gets re-spawned; if it died of credit exhaustion, go straight to the fallback tier rather than retrying Fable. Full record + the superseded clauses: `memory/feedback_reviewer_model.md`.

The agent reports `MODEL(start)` as its first output line and `MODEL(end)` as its last, independently — both should name the same Fable; a `start != end` mismatch flags a mid-run model fallback, so weigh the affected portion and re-spawn if a key surface was reviewed after it. An on-disk agent definition loads at session START: after creating/editing the `.md`, start a fresh session (or open the `/agents` UI) before `subagent_type: holotype-reviewer` resolves — in the SAME session, pass the Agent tool's per-call `model` override instead.

### How to run an audit round

1. Spawn the dedicated reviewer agent (`subagent_type: holotype-reviewer`, `run_in_background: true`). Model + effort + the prosecute-not-defend discipline come from the agent definition; the prompt carries only the round-specific scope, invariants, and adversarial categories.
2. In the prompt, include `memory/audit_rN_closed_list.md` contents as the "already fixed — do not re-report" preamble.
3. Scope the prompt to the surface you changed.
4. Wait for the completion notification. Do not poll.
5. Trust but verify: validate quoted file:line references AND check the agent's `MODEL(start)` / `MODEL(end)` lines — a mismatch means a mid-run model fallback, so weigh the post-fallback portion accordingly (re-spawn on the stronger model if a key surface was reviewed after the fallback).
6. Fix every P0/P1/P2 finding before merge. P3 findings get tracked or closed with explicit justification.
7. Append the round's closed list to `memory/audit_rN_closed_list.md` for the cumulative do-not-re-report set.

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
areas you couldn't audit as deeply as you wanted. Then the final line
`MODEL(end): <model name + id>` (per your agent definition — report it
fresh; a mismatch vs MODEL(start) flags a mid-run model fallback).

Be brutal but grounded. Quote code; don't paraphrase it.
```

---

## Invariants that must hold

Condensed from `ARCHITECTURE.md §28` (the authoritative text — read it for the
full enforcement cells). These are the proof obligations AND the audit
invariants. **Keep the ROW SET + spec column in sync with ARCH §28** (RW-10
retired the old "verbatim" copy, which had drifted to 21 rows + phantom spec
names).

| # | Invariant (condensed) | Validation |
|---|---|---|
| I-1 | Territory operations in Proc A don't affect Proc B | `territory.tla` |
| I-2 | Fork-grantable caps monotonically reduce; `CAP_ELEVATION_ONLY` (HOSTOWNER+DAC_OVERRIDE+CHOWN+KILL) stripped at every fork; growth only via the `cap` device (HOSTOWNER console-gated; clearance corvus-side-gated) | `handles.tla` |
| I-3 | Mount points form a DAG, never a cycle | `territory.tla` |
| I-4 | Handles transfer between Procs only via 9P sessions (no direct-transfer syscall; the positive 9P path is still future) | `handles.tla` |
| I-5 | `KObj_MMIO`/`KObj_IRQ`/`KObj_DMA` (and `KObj_Loom`) non-transferable | `handles.tla` + static_asserts |
| I-6 | Handle rights monotonically reduce on transfer/dup/endow | `handles.tla` |
| I-7 | BURROW pages live until last handle closed AND last mapping unmapped (#847 dual count) | `burrow.tla` |
| I-8 | Every runnable thread eventually runs (as-built: ordered dispatch + preempt; full EEVDF deferred, 2A-F6) | `scheduler.tla` liveness |
| I-9 | No wakeup lost between cond-check and sleep, incl. the death-wake (#811; frame-atomic for the elected 9P reader recv -- a mid-frame death defers its unwind to the next frame boundary, §8.8.1.1/#90) + terminate-`interrupt` (LS-5) generalizations + the Weft-4 readiness-ring single-cache-line poke (the store-buffer register-then-observe) | `scheduler.tla`, `poll.tla`, `cons_poll.tla`, `net_poll.tla`, `weft_readiness.tla`, `tsleep.tla`, `death_wake.tla`, `reader_frame.tla` (#90); torpor leg prose |
| I-10 | Per-9P-session tag uniqueness (tag==table index; no reuse until reply/Rflush, #845) | `9p_client.tla` |
| I-11 | Per-9P-session fid identity stable for the fid's open lifetime | `9p_client.tla` |
| I-12 | W^X: every page writable XOR executable (PTE checks + ELF reject + no prot-mutation syscall exists; W1.5 transient RW+XN alias) + the #217 PROVENANCE half: a file-backed executable mapping requires BOTH a Dev with `may_back_exec` set (the allowlist floor) AND a mount not marked `MNOEXEC` | runtime + `_Static_assert` |
| I-13 | Kernel-userspace isolation: TTBR0/TTBR1 split | runtime |
| I-14 | Stratum block integrity (Merkle); OS observes via 9P, bounds hostile Rlerror ecodes | Stratum-side + `9p_client.rlerror_hostile_ecode_bounded` |
| I-15 | Hardware view derives entirely from DTB (documented PL011 QEMU-virt fallbacks are the argued exceptions) | code review + audit |
| I-16 | KASLR randomizes kernel base at boot (never-zero slide) | runtime + `/ctl/kernel-base` |
| I-17 | EEVDF quantitative latency bound — DESIGN TARGET; needs the deferred EEVDF math (2A-F6 → RW-13) | `scheduler.tla` (qualitative only) |
| I-18 | IPIs from CPU A to B processed in send order | `scheduler.tla` |
| I-19 | Note delivery causal order; non-`kill` consumed exactly once; `kill` non-catchable; uncaught `interrupt` default-terminates (LS-5); each note has ONE default action, held in the `g_known_notes` `dfl` column -- TERMINATE / STOP (`tty:susp`) / IGNORE (#15). PARTIAL unification: the terminate-class latch + `SYS_NOTED(NDFLT)` read the column; the uncaught STOP is still decided by `job_stop_cb` at POST time and the IGNORE class has no uncaught reader (the note is retained, not ignored) -- unifying those is OWED (#15 F5) | prose §7.6.7 + tests (notes.tla dropped) |
| I-20 | PTY master↔slave atomicity — ENFORCED (PTY-1 kernel seam + the PTY-2 userspace ptyfs data path; byte conservation + SignalXorByte + drain-then-EOF + HupAtMostOnce-by-construction; the 2e openpty E2E proves the signal/teardown legs live) | `pty.tla` + `pty_stop.tla` + the §25.4 PTY rows + the 2e audit |
| I-21 | Kernel uniformly EL1h (`SPSel=1`); `SP_EL0` exclusively the user stack | `sched_ctxsw.tla` + `test_smp` |
| I-22 | No identity carries ambient super-authority; elevation only via the legate | IDENTITY-DESIGN §3.3/§8.2 prose + tests |
| I-23 | A service's FS authority bounded by its endowed storage capability (cooperative chroot at v1.0) | ARCH §3.6 + A-1.7 prose + tests |
| I-24 | Group termination atomic + exactly-once; no EL0 after ZOMBIE; cascade loses no wakeup | `death_wake.tla` + ARCH §7.9.1 prose |
| I-25 | Legate authority scope-bounded + fully revoked on scope exit | IDENTITY-DESIGN §3.1/§9.8 prose + tests |
| I-26 | Cross-Proc kill is explicit two-axis (owner-identity OR CAP_HOSTOWNER/CAP_KILL) | IDENTITY-DESIGN §9.8 prose + tests |
| I-27 | Trusted path: SAK unspoofable; console-ATTACH (elevation gate) distinct from console-OWNER (Ctrl-C target) per `@2608c88` (the SAK revokes ATTACH + sets `owner=NULL` + attaches corvus, NEVER owns corvus); the `/dev/cons` namespace front-door gates at `devdev.open` identically to `SYS_CONSOLE_OPEN` (#57b). **MEDIUM-INDEPENDENT** (`TRUSTED-PATH.md`, 2026-06-15): generalized off serial -- same SAK+ATTACH-to-corvus rides any renderer; framebuffer = corvus cell-grid -> kernel trusted sink (sole painter, renderer suspended); graphical SAK = kernel-scanned key-combo via the MENAGERIE trusted-tier keyboard; framebuffer enforcement reserved-then-enforced (at impl); serial live (A-4c) | IDENTITY-DESIGN §9.8 + ARCH §17.1/§9.4 + `docs/TRUSTED-PATH.md` prose + tests |
| I-28 | Path resolution contained at `root_spoor` + per-component X-search; mount-cross keyed by full Spoor identity; symlink expansion contained by the SAME machinery (DISTRO D-1, AS-BUILT: absolute targets re-anchor at the caller's CURRENT `root_spoor`, expanded components re-enter the gate family, follows bounded at 40; the re-anchor is only witnessable through a non-root base, i.e. `SYS_OPEN` with a dirfd -- the `SYS_WALK_OPEN` twin does not expand, #184) | ARCH §9.6.7 + STALK-DESIGN prose + `docs/DISTRO.md` + tests |
| I-29 | Loom completion integrity: exactly-one terminal CQE; no stale; CQ never overfilled | `loom.tla` + `loom_multishot.tla` + `loom_order.tla` |
| I-30 | Loom submit-time capability pin; kernel never re-reads a shared-ring field post-check | `loom.tla` buggy cfgs |
| I-31 | ASID rollover safety: no cross-generation ASID aliasing; rollover never yanks an active/reserved ASID | `asid.tla` (clean + 5 buggy cfgs) |
| I-32 | Resource floor (DoS bound), on **TWO granularities since LINEAGE L-1** -- this row said "per-Proc" until 2026-08-16 and that is wrong for the page axis. The **page axis is per-ADDRESS-SPACE**: a non-TCB `AddrSpace`'s live anon pages / VMAs / shared-in pages are bounded by the cap carried on that `AddrSpace` (`AddrSpace.page_budget`, seeded at `addrspace_alloc` from the creating Proc's authorization, default `PROC_PAGE_MAX`; `PROC_VMA_MAX`; `PROC_SHARED_MAP_MAX_PAGES`). The **thread + child axes stay per-Proc** (`PROC_THREAD_MAX`/`PROC_CHILD_MAX`). `Proc.page_budget` is the **authorization** -- what this Proc may seed or raise an AddrSpace to -- NOT the enforced bound. Sharing an address space shares the cap, and that is not escalation (RFMEM siblings can already write each other's memory). The inverse (counter on the AddrSpace, cap on the Proc) is REJECTED: two RFMEM siblings would return different verdicts on one counter, so the bound would depend on which sibling faulted -- hence `addrspace_charge_pages` reads the cap off the AddrSpace it charges and must NEVER take it as a caller-supplied parameter, which is that rejected shape in disguise. On a bound, creation fails clean (-ENOMEM/-EAGAIN), never box-extincting; `PRINCIPAL_SYSTEM` (the TCB) is exempt + unforgeable; graceful-OOM on every creation path is the backstop (resource axis, not privilege -- orthogonal to I-22) | ARCH §28 I-32 (authoritative) + prose IDENTITY-DESIGN §3.8 + `docs/reference/110-resource.md` + the focused audit + tests |
| I-33 | Namespace name retention is non-load-bearing: every Spoor carries a refcounted copy-on-walk `Path` (its cleaned namespace name -- the Plan 9 `Chan.path`), but the resolver is WRITE-ONLY to it (stalk/walk/create append; nothing reads `->path` to resolve/perm-check/cross), so a wrong/stale/absent/failed Path changes only the cosmetic content of the introspection readers (`SYS_FD2PATH`/`/proc/fd`/`/proc/ns`), never a resolution/permission/syscall result; a path-alloc failure leaves Path NULL and the WALK SUCCEEDS. Path lifetime is subordinate to its Spoor's (one ref/Spoor, atomic, freed with the last Spoor); the string is immutable once built (only `path->ref` is concurrent) | prose ARCH §9.6.9 + `docs/reference/30-dev-spoor.md` + STALK-DESIGN + the focused audit + tests + the SMP gate |
| I-34 | Driver authority bound (ENFORCED -- Menagerie build-arc 2): a driver's hardware authority is exactly its warden-granted **allowance** -- a per-Proc set of MMIO PA windows / IRQ INTIDs / a DMA per-buffer cap / **PCI `(bus,dev,fn)` functions** (the 4th axis, build-arc step 6a). NARROWED (`p->allowance != NULL`) bounds `SYS_MMIO/IRQ/DMA_CREATE` **+ `SYS_PCI_CLAIM`** (gated on the resolved `(bus,dev,fn)` via `kobj_pci_resolve_bdf` + `HW_RES_PCI`) to the conferred set; BROAD (`allowance == NULL` -- the warden + the existing trusted servers) is bounded only by the I-5 reservation (the as-built v1.0 path, unchanged). Never widened (windows immutable post-confer; a forked child inherits an equally-narrow copy via `allowance_clone_into` -- the I-2 hardware-axis analog); fully revoked on unbind/removal/crash (`proc_revoke_allowance` + `proc_group_terminate`). A narrowed driver also CANNOT spawn a child Proc (drivers are leaves -- MENAGERIE §13.2 "sources, not spawners; one auditable chokepoint"; `rfork_internal` fail-closed denies a narrowed parent a child, 5e-4 F2), so no hw-capable grandchild can inherit/be-conferred an allowance that survives the per-Proc revoke + thread-group-scoped terminate. The central hazard -- an in-flight `SYS_*_CREATE` racing a `DeviceRemoved` revoke -- is closed by the two-step create (the lock-free `allowance_permits` gate then the `allowance_handle_alloc` install under a `revoked` re-check under `allowance->lock`). The I-25 analog for hardware; generalizes pci-1b (a PCI device's allowance IS its claimed BARs -- the per-`(bus,dev,fn)` PCI axis enforced at `SYS_PCI_CLAIM` since step 6a); preserves I-5 (the bounded authority-to-create passed down, never pre-minted handles) | `specs/allowance.tla` (clean cfg TLC-green + the 4 buggy cfgs: revoke_race / revoke_leak / confer_widen / self_widen; the PCI axis is the runtime-tested per-kind predicate -- no spec change, the 4 cfgs re-run green) + prose `docs/MENAGERIE.md` §4 + `docs/reference/117-allowance.md` + the focused audit + the `allowance.*` tests (incl. `handle_alloc_revoked_aborts` + `narrowed_proc_cannot_spawn` + `pci_membership`) + the SMP gate |
| I-35 | Mandate attenuation + revocation (persistent attenuated delegation, `docs/MANDATE-DESIGN.md`): a standing grant confers <= its issuer's held authority, never widened, revocable — RESERVED; OWED at the MA arc (Phase 8, after net) | `specs/mandate.tla` (reserved, spec-first re-enabled) + prose MANDATE-DESIGN.md |
| I-36 | File-backed demand-paged exec soundness (REVENANT): the kernel demand-pages an executable's read-only segments (text + R-only rodata since #45) from the FS iff the 7 conditions hold jointly (install-once, death-interruptible page-in, fail-close on I/O error, W^X, I-cache sync, Image-cache eviction safety, pin-at-exec). AS-BUILT (R-5 CLOSED CLEAN + the #45/read-ahead addenda). DISTRO D-3 (design 2026-08-05) generalizes the 7 conditions to phenotype mmap-time library maps — read-only/exec userspace file maps admitted, writable stays banned (a writable MAP_PRIVATE request terminates in a private eager copy) | prose EXEC-LOAD-DESIGN.md + ARCH §6.5 + `docs/DISTRO.md` §6 + the R-5 audit + tests + the SMP gate |
| I-37 | Capability network dataplane integrity (Weft): the per-flow zero-copy shared-Burrow path is sound — registration-is-the-capability, no per-op mediation, the F_NOTIF buffer lifetime (no in-flight-page UAF), ring TOCTOU closed, the share bounded by the flow. AS-BUILT (the Weft arc COMPLETE at Weft-7) | `weft.tla` + `weft_readiness.tla` (clean + liveness + buggy cfgs) + the Weft-7 audit |
| I-38 | Larder cache coherence (the guest-side FS cache): a hit returns exactly what a fresh RPC would under close-to-open — Open-revalidate + Read-serve + OwnWrite-invalidate; incl. the write-behind staged legs. AS-BUILT (the L1 arc COMPLETE at L1f + the F1 wb / term-4 addenda) | `fs_cache.tla` (clean + external + liveness + 5 buggy cfgs) + prose LARDER-DESIGN.md + the L1f audit |
| I-39 | Debug authority bounded: debug = namespace-names-the-target + the two-axis gate (owner OR `CAP_HOSTOWNER`/`CAP_DEBUG`; `CAP_DAC_OVERRIDE` NOT an axis); user reads/writes + all execution control stopped-only (fully-stopped rejects a pending `group_exit_msg` — death wins); a read-only inspect of a SETTLED (`on_cpu==false`) thread's KERNEL stack (`/proc/<pid>/kstack`, the Linux `/proc/stack` tier, 8b) is I-39-authorized but NOT debug-stop-gated (memory-safe: bounded to the thread's own kstack + the `g_proc_table_lock` lifetime pin; best-effort-consistent; controls no execution) -- raw slid kernel addrs (which reveal the KASLR slide, an I-16 secret) go ONLY to the CAP_DEBUG/CAP_HOSTOWNER tier, the owner axis gets the KASLR-independent symbolic form (8b-1d F1); no debug op writes text (I-12/I-36 — breakpoints are hardware), escapes the target's `pgtable_root`, or strands the quarry (detach/close/debugger-death resumes an ATTACHED target — but a debugger-LAUNCHED `exitkill`-marked target is TERMINATED on debugger death, die-with-launcher / `PTRACE_O_EXITKILL`, the EXITKILL refinement [designed 2026-07-23, §5d + `debug_stop.tla::EventuallyLaunchedDies`]: `devproc_debug_release_cb` `proc_group_terminate`s a marked ALIVE target instead of `proc_debug_resume`, closing the debugger-launched-orphan leak — an explicit `detach` still resumes; AND a hardware fire racing a detach delivers only while owned — SA-1); kproc + NOTRACE refused. AS-BUILT at 8a-1 (software-checkpoint tier) + 8a-2 (the HW-breakpoint/single-step/watchpoint tier: DBGB*/DBGW* per-Proc install + `MDSCR.SS` + EC 0x30/0x32/0x34); 8b = the settled-thread inspect + the cross-boundary unified stitch (kernel DWARF deferred to 8c); 8c-2 = the stop-of-a-sleeper (a nested stop-detour inside `sleep()`/`tsleep()` so a multi-thread Go target -- whose idle futex-parked Ms never reach the tail -- becomes fully-stoppable; DEATH still wins [die-check-first]; #88 records a detour-parked thread's EL0-entry frame at the EL0-sync choke point so `/proc/regs` works for a syscall-blocked head; **8c-3** (#89) releases the elected-9P-reader role on a stop -- the reader's recv *primitive* is FRAME-ATOMIC [`reader_recv_frame` sets `Thread.stop_unwinds = (got==0)` per-chunk + holds `stop_no_park`: a stop UNWINDS only at a frame boundary -> the detour returns `SLEEP_INTR` and BLOCKS THROUGH mid-frame (else it would desync the shared stream -- the holotype F1 correction)] so `client_wait` can hand the role to a runnable survivor [the handoff skips debug-stopped owners] before parking role-free, while the *syscall* is still preserved [re-elect + re-block on resume] -- so a stop never freezes the shared FS client for survivor Procs; the identical death-path mid-frame unwind is task #90 -- the #90 block-through design [ARCH §8.8.1.1 + `specs/reader_frame.tla`, signed off 2026-07-19]) | `specs/debug_stop.tla` (clean + 6 buggy cfgs incl. `fault_stop_ungated` + `BUGGY_STOP_SKIPS_SLEEPER`, model-first; the `sleep` PC + `StopWakesSleeper` added at 8c-2; 8c-3 is below the model -- no change) + `specs/debug_step.tla` (the step machine) + `docs/reference/134-debug-fs.md` + the 8a-1c/8a-2c/8c-2/8c-3 holotypes + the in-guest `/debug-probe` + `/hwbp-verify` + `/ambush-probe` E2Es + the SMP gate |
| I-40 | T-1 no torn scanout / surface-share integrity (TAPESTRY §18.8; STAGED per the I-20 RESERVED→ENFORCED precedent): every page of a weave stays backed + mapped-membership-immutable from first client map to retire; a present op's lifetime brackets its `TRANSFER_TO_HOST_2D`; a weave retires only after quiesce + scanout-composition release. **KERNEL SHARE HALF ENFORCED at G-2** (ABI user-signed-off) | `tapestry_present.tla` (model-first; 4 clean + 8 buggy cfgs, gated by `specs/check-tapestry.sh`). Since **Warp-C C-1** (2026-08-16) the module also carries the GPU-COMPOSED present behind `ALLOW_COMPOSE` -- `NoTornCompose` (the composed drain) + `NoStaleCompose` (the P2 cross-context ordering hazard) -- and since **C-6's spec (2026-08-18)** the compositor READBACK class behind the same switch: `ComposeReadbackIssue`/`Complete` (a fenced host DMA-WRITE into the client BO's pages) + `NoTornReadback` with `DrainedOfReadbacks` on retire (`buggy_readback_free`). Both extensions are ADDITIVE by measurement, not by assertion: with the switch off the six pre-existing cfgs reproduce 5413 distinct states exactly. Since **Warp-WSI W-3b (2026-08-26)** the module also carries the PRESENTABLE class behind its own `ALLOW_PRESENTABLE` — a venus swapchain image as a shareable non-mappable HOST3D blob (`NoTornPresentable`: the display never observes a retired presentable; `PGoneClean`: I-7/I-37 extended by the two observer arms; the display-safe teardown's `PUnbound`+`PDrained` conjuncts on `PServerRelease`/`PFree`; `buggy_punbind_skipped`/`buggy_pdrain_skipped`) — same additivity bar: all four pre-existing clean counts reproduced exactly (5413/5413/94680/94680, the composed pair now pinned in the gate) |
| I-41 | **NOT ALLOCATED in §28.** `ADVANCED-GO-DESIGN.md` AG-2's software-breakpoint isolation reserves the number in its own doc and has never been promoted to a §28 row. Cite it as AG-2's, never as a §28 invariant | (reserved in ADVANCED-GO-DESIGN.md only) |
| I-42 | JIT-as-a-capability: executable code emission is a capability (`CAP_JIT`), not an ambient power; W^X holds across the publish (`docs/LLVM-DESIGN.md` §8, the Clade arc). **ENFORCED at CL-7k** | prose + the CL-7k focused audit (W^X-adjacent — prosecute hard) |
| I-43 | A phenotype confers ABI **SHAPE**, never **AUTHORITY** (`docs/VIVARIUM.md` §8/§12.1): a Linux-phenotype Proc gets Linux syscall *numbering/semantics* and not one bit of extra privilege | prose + the V-8 audit + `vivarium.*` tests + `sys_spawn_full_argv.validate_req_pheno_flags` + the two-vantage in-guest gate |
| I-44 | Address-space integrity under sharing + COW (`docs/LINEAGE.md`): an AddrSpace's pages live until the last referencing Proc drops it AND the last mapping is gone; a COW break yields a PRIVATE page equal to the shared page at fault time, leaves every other sharer's view UNCHANGED, and no page is writable through one mapping and executable through another. **ENFORCED** (2026-08-16; this row said "RESERVED; ENFORCED at L-4/L-5" long after its own trigger fired -- vault caught the lag). Every stated precondition is in the tree: `struct AddrSpace` exists with the enforced per-AS `page_budget`; `SYS_RFORK` = 105 with `sys_rfork_core`/`sys_rfork_handler`; `kernel/cow.c` + the `VMA_FLAG_COW` break arm in `arch/arm64/fault.c` under the global COW lock; and the L-7 arc-close holotype `b647a6c4` closed 0 P0 / 0 P1, NOT dirty | **spec-first RE-ENABLED**: `specs/cow.tla` landed TLC-green BEFORE the L-4 impl, with the three named buggy cfgs (`cow_buggy_break` break-vs-break, `cow_buggy_teardown` break-vs-teardown, `cow_buggy_vfork` lost-VFORK-wake) + LINEAGE.md + the L-7 audit + the SMP gate |
| I-45 | GPU authority is bounded by the context (`docs/GPU-DESIGN.md` §8, the Warp arc; **STAGED — the halves are DIFFERENT CLAIMS, so name the half you mean**): GPU work reaches only what its context owns; a submission executes only against buffers attached to the submitting context, bounded by address-translation hardware the trusted server programs, **never by inspecting the command stream**; buffers live until last client unmap AND last in-flight submission retires; teardown (incl. client death) quiesces without disturbing other contexts; a context's fault is fatal to that context alone. **GUEST-EXPOSURE HALF ENFORCED** (one ctx per client, no cross-ctx resource naming, submit-time capability pin; cross-ctx blit refusal measured @`7b1ff07f`). **HOST HALF on virgl/Venus RESERVED-NOT-ENFORCED — the host is documented TRUSTED** (GPU-DESIGN §9.2). **v3d is where it becomes ours to keep** (fork F3, unbuilt) | prose GPU-DESIGN §8 + the Warp-5 + C-0d audits + `warp-prove` on thyla-pi (KVM, real V3D); **no spec module** |

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
- `TASK-ARCHIVE.md` — completed tasks booted from the live task list (see
  "Task-list hygiene" below). Subject lines verbatim; the lookup for past
  closes.

### Task-list hygiene (binding; the 65% lesson)

The harness re-injects the ENTIRE live task list — every task's subject
AND description — into the conversation after tool batches. Measured
2026-08-10: ~345 KB per injection, 23 injections in one working day =
65% of that day's context volume; over the session's life, half the
transcript. The injection cost scales with the LIVE list only, so the
rule is:

- **The live list carries OPEN work only.** Tasks may be as long,
  descriptive, and detailed as the work deserves — verbosity is fine
  precisely BECAUSE the list stays small.
- **The moment a task completes, boot it**: finalize its subject as the
  close record (hash, verdict, counts — the existing style), append that
  subject line to `memory/TASK-ARCHIVE.md`, then delete the task
  (`TaskUpdate` status `deleted`). Archive-then-delete, never the
  reverse.
- **Sweep at every checkpoint.** A completed task may linger at most
  until the current chunk's close. If the live list exceeds ~40 entries,
  something is being hoarded — prune before starting the next chunk.
- The deeper records remain git log (close-commit bodies),
  `docs/phaseN-status.md` rows, and `memory/MEMORY-ARCHIVE.md`; the task
  archive is the fast index into them.

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

### The checkpoint contract (binding; every time you hand back)

A **checkpoint** is any **resting point** in the work — a landed chunk, a closed
audit, a surfaced fork, a stopping report. Note that a checkpoint is *not* by
itself a decision to hand back: whether you yield or roll straight into the next
chunk is governed by the 600k line in §"When to recommend `/compact`", and the
default under granted autonomy is to **keep going**. What follows is owed at
every checkpoint either way — including the ones you run straight through, where
it is the only thing keeping the tree pickup-ready. Do these three WITHOUT BEING
ASKED. They exist because the user cannot see what you can see, and the cost of
them guessing is real.

**1. Account for every attached shell, monitor, and background task.**

Enumerate what is still running and dispose of it explicitly:

- Kill anything whose work is finished or whose exit condition can no longer be
  met, then say so.
- For anything deliberately left alive, name it and why in one line
  ("`ci-smp-gate` running, ~20 min, this is the gate we're waiting on").
- If nothing is running, **say "nothing running"** — silence is not the same
  statement.

**Why this is binding, not tidiness:** an attached session reads to the user as
"Claude is still working, do not interrupt." A stray poller therefore does not
just waste a process — it silently converts a finished turn into an apparent
in-progress one and stalls the human. Verify with a real check (`ps` scoped to
the tree/session path), not from memory of what you launched; kill strays by
explicit **PID**, never by pattern (see the unscoped-`pkill` rule). Worked
failure, this project: two clusters in ONE session — three self-matching
`pgrep -f` loops, then six `until grep` waiters whose patterns were
unsatisfiable (one producer stopped, another's marker filtered away by the
author's own `| tail -N`) — each spun for over an hour while the user watched an
"active" session that was doing nothing. See
[[feedback-unbounded-until-waiters]].

**2. Leave the handoff already written, not offered.**

If the tree is in a compactable state (clean, green, no open audit round), the
handoff must ALREADY be current when you hand back — `project_active.md`,
`project_next_session.md`, the phase status doc, any open-finding notes — so the
user can type `/compact` or "keep going" without a preparatory round-trip. Do
not ask "shall I write the handoff?"; do not wait for the compaction to be
announced. The Handoff protocol above says what to write; this says *when*:
**at every checkpoint, in advance.** State the posture in one clause
("handoff current at `<tip>`") so the user knows the choice is free.

If the state is NOT compactable (uncommitted work, red tests, an audit round in
flight), say that instead and name the one thing that would make it compactable.

**3. Say what is next, and show the road.**

After the substantive report, always close with:

- **Next**: the single immediate next action.
- **Ahead**: a one-line progression of the queued chunks on the current arc, in
  order, ending at the arc's close (e.g.
  `#118 libunwind FDE segv -> gl_probe gate -> port patches -> CL-7b close`).

Keep it to one line. The purpose is orientation — the user should be able to see
where the current chunk sits in the arc without opening the tracker.

---

## The run journal (`docs/JOURNAL.md`) — binding, user-requested 2026-08-16

After a long autonomous run the user has to reconstruct what happened, and doing
that from `git log` + six status rows + a memory directory is work they should
not have to do. **`docs/JOURNAL.md` is the single narrative thread**: what
landed, in order, why, what it cost, what it left open.

**Append an entry per autonomous run**, newest run first, as part of the run —
not reconstructed at the end from memory, which is how the interesting parts get
lost. A checkpoint you run *through* still earns its paragraph.

What belongs there, and what does not:

- **NOT a changelog.** `git log` owns the commits; duplicating them here rots.
  **NOT a status doc** — `docs/phaseN-status.md` owns per-chunk rows.
- **The reasoning, the wrong turns, and the findings nobody planned.** A wrong
  turn that got caught is worth more than a win, because the catch is the
  reusable part. Record what caught it — a control, a sabotage, a measurement —
  not just that it was caught.
- **Evidence on every claim**: a hash, a measured number, a file:line.
- **Exactness about what "fixed" covers.** Half a defect closed is written as a
  half, with the other halves named. A run that reads as uniformly successful is
  usually a run that was written up carelessly.
- **Decisions that needed the user**, and what they chose — so the next session
  can tell a ratified decision from an assumed one.

This is the operator's window into an unattended run. Treat a missing entry the
same as a missing status row: the work is not finished without it.

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

0. **Check the vault first.** Before writing or extending a `docs/reference/NN-*.md` section, run:

   ```bash
   cd ~/projects/thylacine-vault && vault/meta/quaestor/quaestor owner <changed paths>
   ```

   **Exit 0** — the vault carries that surface: the prose belongs there, so ring vault over yip rather than writing the section here. **Exit 1** — no dossier: write the reference section as today, and file the sweep. **With several paths the answer is usually MIXED and the exit status reports only half of it** — read the summary line, which names both sets; both actions are then owed.

   Read any `ALSO named by` line in the output. A note that merely **pins** a file (an `abi-*` registry pins VALUES or STRINGS) cannot hold a description of a mechanism — so the reference section is still owed, AND that note may need the same change.

   This step exists because the alternative is a protocol whose first move is remembering to tell someone. It rides the doc-update step precisely so it cannot be skipped separately from it.

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
# classifies CORRUPTION vs EXTERNAL-KILL vs benign host-TIMING vs OTHER. Fails
# iff any boot corrupts, is externally killed, or is unclassified.
# EXTERNAL-KILL has TWO detectors (#222): QEMU's own 'terminating on signal'
# report (#88) sees only CATCHABLE signals -- SIGKILL is uncatchable, so the
# arm that most needed the bucket could not reach it, and the #200 sightings
# landed in OTHER. The second reads the shell's job notification from the
# HARNESS log, gated on test.sh's qemu_alive_at_teardown=0 so the harness's own
# teardown kill cannot trip it. Captures are ARCHIVED, never deleted (#223) --
# re-running a label to investigate it must not destroy the evidence.
tools/ci-smp-gate.sh                    # full matrix, N=10 (or: make smp-gate)
SMP_GATE_CONFIGS="default-smp4 ubsan-smp4" tools/ci-smp-gate.sh   # amplifier subset

# The gate's classifier, tested without booting (fast; sources the real ladder).
tools/test-smp-classify.sh

# Hardening witnesses (#245). Both of these were invoked by NOTHING until
# 2026-08-18 -- and both were already named in this file, inside the boot-banner
# paragraph below, purely as CONSUMERS of the ABI strings (things that would
# break if you reworded one), never as gates to run. That is the entire
# difference between a mention and a command, and it is why they rotted while
# `test-a72` and `check-v80-floor` -- one screen down, in this block -- did not.
# test-fault builds one kernel per provoker and PASSes iff each EXTINCTIONs with
# its expected message: the ONLY proof that the canary / W^X / BTI / the two
# stack guards / the idle guard / the recursion arm actually FIRE, as opposed to
# merely being compiled in. Its absence from every gate is how #244 --
# recursive_kernel_fault emitting nothing at all -- hid for about a month.
tools/test-fault.sh                 # all 7 variants    (or: make test-fault)
tools/test-fault.sh canary_smash    # one variant       (-v for log dumps)

# verify-kaslr multi-boots and PASSes iff the slide varies across N: ROADMAP
# section 4.2's exit criterion for I-16, and that invariant's ONLY runtime
# witness. `make test` accepts any SINGLE boot, so it is structurally blind to a
# slide that never moves -- the same shape as test.sh being blind to LSE above.
tools/verify-kaslr.sh               # 10 boots          (or: make verify-kaslr)
tools/verify-kaslr.sh -n 25 -v      # more boots, print each offset

# Warp-6 V-0 (the Venus gate). `warp-host.sh venus` boots the remote GL host
# TWICE -- once with `venus=on,blob=on,hostmem=256M` and once WITHOUT -- and
# passes only if capset id=4 (VENUS) is present in the first and ABSENT in the
# second. The control leg is not a bonus: a one-directional check is satisfied
# by a host that advertises the capset unconditionally. venus needs blob AND
# hostmem together, and QEMU refuses the device otherwise rather than degrading.
# BOTH GL hosts pass it: thyla-pi (KVM/V3D, ~220 s per boot) and thyla-gl
# (Parallels/TCG/lavapipe, ~350 s), and they report byte-identical feature
# words. test-venus-verdict drives the SAME verdict verb against crafted logs,
# so the discrimination is testable without paying two boots at all (#245: a
# checker reachable only by hand rots).
WARP_HOST=thyla-pi WARP_ACCEL=kvm tools/warp-host.sh venus   # certify (2 boots)
WARP_HOST=thyla-gl tools/warp-host.sh venus                  # iterate (2 boots)
tools/test-venus-verdict.sh         # its verdict, no boot  (or: make test-venus-verdict)

# ARMv8.0 floor guard (#91). The SOURCE + BINARY checks run automatically at the
# tail of every ramfs bake; these are the extras. `check-floor` adds the big pool
# payloads (/clade, /goroot, ~6 min); `test-a72` is PORTABILITY.md section 3's
# verification bar -- the ONLY gate that can see an LSE regression, since the
# default test.sh runs HVF -cpu host (M2, LSE present) and is structurally blind.
tools/check-v80-floor.py            # fast: source + the ramfs binaries (~7 s)
tools/check-v80-floor.py --all      # + /clade + /goroot   (or: make check-floor)
make test-a72                       # boot on -cpu cortex-a72 (ARMv8.0-only)

# Interactive E2E regression net (LS-CI): expect/PTY drives a REAL console --
# login + assert rendered command output (the test that would have caught LS-1).
# Optional gate (SKIPs without `expect`). THYLACINE_ACCEL=tcg default; bounded
# retry (LS_CI_ATTEMPTS=3) tolerates host-timing flakes.
# REFUSES to start (exit 2) if a VM from this tree is already running (#224):
# its reaper is tree-wide `pkill -9`, so it would SIGKILL a boot it does not
# own -- presenting to the other gate as "qemu GONE, guest healthy" -- and both
# gates restore the same build/fixtures/pool.img. Do not run it alongside the
# SMP gate in one tree; use a separate worktree.
tools/test-interactive.sh               # full set (or: make test-interactive)
tools/test-interactive.sh ls-ci         # one scenario by name

# Signal witness (#200): NAME the sender of the SIGKILL that makes a QEMU vanish
# with a healthy guest. SIGKILL is uncatchable, so the victim can never report
# it -- smp-multiboot's arm-2 detector proves THAT it happened but prints
# "sender NOT RECOVERABLE". macOS Endpoint Security observes signals from
# OUTSIDE the victim, so it names both ends; needs root, but NOT a SIP change.
# Watch mode REFUSES until --selftest has proven the capture can see a kill --
# an unproven watcher logs nothing and reads exactly like a quiet host.
# Routine teardown kills appear on every boot: the finding is a sender that is
# NOT ours, never the mere presence of records.
sudo tools/sigwatch.sh --selftest       # prove it, then
sudo tools/sigwatch.sh                  # watch -> build/sigwatch.jsonl

# Launch a dev VM
tools/run-vm.sh

# Snapshot management
tools/snapshot.sh save <name>
tools/snapshot.sh restore <name>
```

The `Makefile` at the root provides `make kernel`, `make all`, `make test`, etc. as conventional aliases.

---

## The thyla-pi host (permanent ARM64 / KVM / GPU box)

A Raspberry Pi 400 — BCM2711 (4× Cortex-A72 @ 1.8 GHz), 4 GB RAM, VideoCore
VI / V3D 4.2 GPU; Debian 13 arm64; QEMU 10.0.11 (distro) with
`virtio-gpu-gl-pci` + `egl-headless`; `expect`; `/dev/kvm` and
`/dev/dri/renderD128` accessible to user `cora` — is **permanently online**
(user commitment 2026-08-12) and available to every instance for anything
that benefits from real ARM64 silicon. No reservation protocol; keep QEMU
single-flight (verify no stray: `ssh thyla-pi 'ps -eo pid,args | grep
"[q]emu-system"'` — bracket-trick the pattern or it matches its own wrapper).

**Access**: LAN `ssh thyla-pi` (thyla-pi.local, user `cora`, key
`~/.ssh/thyla-pi`). From anywhere: `ssh thyla-pi-cf` (Cloudflare tunnel via
`thyla-pi-ssh.treeso.net`; cloudflared ProxyCommand — both aliases live in
`~/.ssh/config`; the Pi-side connector is a systemd service). After changing
cora's groups, drop the control master: `ssh -O exit thyla-pi`.

**Roles**:
- **The KVM GL host** (Warp arc): `WARP_HOST=thyla-pi WARP_ACCEL=kvm
  tools/warp-host.sh <sync|smoke|capset|prove|tri|bench|quake|decomp|wedge|wedge-gate>`.
  Real-silicon KVM (`-cpu host -gic-version=host`, auto-detected by
  run-vm.sh on Linux-aarch64 + rw /dev/kvm) boots the full gauntlet in
  ~210 s where Pi-TCG never finishes. Real GPU: virgl on V3D 4.2.14 (first
  Thylacine GL-on-silicon 2026-08-11; gl-host-probe rung 6 PASS) and a
  Vulkan V3D ICD (the Warp-6/Venus prerequisite).
- **Real-silicon SMP / memory-model witness**: the only non-Apple ARM
  hardware in the loop (#214 was closed on it); A72 vs M2 diversity.
- **General ARM64 Linux box**: native aarch64 builds, KVM guests, ad-hoc.

**Layout**: repo sync at `~/projects/thylacine` (push with `WARP_HOST=thyla-pi
tools/warp-host.sh sync` — git-archive of HEAD + boot artifacts + the pool via
sparse gzip; uncommitted tool scripts ride separately, re-scp after editing).
Working fixtures + logs in `~/warp/`.

**Care**:
- 4 GB RAM: ONE 2048 MiB guest at a time.
- SD-card I/O is the bottleneck — FS-round-trip-heavy guest work (go builds)
  dominates wall clock; budget bounds off the ~210 s banner, not TCG numbers.
- The Pi's `build/` holds the **certified artifact set of the last sync**
  (md5-stable). It has served as the bit-exact restore source after a local
  bake clobbered the fixtures: reverse-sync (`ssh thyla-pi 'gzip -1 -c
  .../pool.img' | gunzip | dd of=... conv=sparse bs=1m`) + md5 both sides.
- Artifacts pair cryptographically: `pool.img` + the key-bearing `ramfs.cpio`
  ship TOGETHER or the guest gets `STM_EBADTAG` (stratumd `rc=-201` ->
  `EXTINCTION: joey exited non-zero`). **A reverse-sync recovery is only valid
  if you sync BOTH and DO NOT rebuild** -- a rebuilt ramfs carries a fresh key
  that no longer matches a reverse-synced pool. If a real code change forces a
  rebuild, re-bake BOTH paired with `THYLACINE_MKFS_PRESERVE=0`, never `=1`.

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

The `hardening:` / `features:` lines are informational. **`kernel base:` is NOT** — that claim stood here until 2026-08-16 and was false: `tools/verify-kaslr.sh` parses `kernel base: 0xVA (KASLR offset 0xN, ...)` and is the **ROADMAP §4.2 exit-criterion gate for I-16**, i.e. that invariant's only runtime witness, and `tools/stall-watch.py`'s `KASLR_RE` parses the same line to symbolize a stalled guest. Their failure modes differ in the way that decides how bad a reword is: verify-kaslr fails **loud** (an unparsed offset makes every boot's offset the empty string, the distinct set collapses to 1, and the run misses its `>=0.7N` bar), while stall-watch fails **SILENT** (`if m:` with no else leaves `syms.slide` NULL and the watcher keeps running, having quietly lost the symbolization it exists to provide, exactly when a guest has stalled). So the binding set is three surfaces, not two. Since Lazarus W1 (`PORTABILITY.md §4`) the `hardening:` line lists the unconditional set and marks PAC/BTI/LSE runtime-conditional; the `features:` line reports what the running CPU implements.

A kernel **extinction** (ELE — Extinction Level Event; the thematic name for kernel panic) prints `EXTINCTION: <message>` as a recognizable prefix. Use `extinction(msg)` or `extinction_with_addr(msg, addr)` from `kernel/extinction.c`; `ASSERT_OR_DIE(expr, msg)` for assert-style checks. These strings are part of the kernel ABI with the development tooling, and changing one is a **format break** (§"Autonomy + escalation"): surface it, do not just sweep.

**Do NOT trust a hand-written co-update list here — this one was wrong in three different ways at once, and the third is the instructive one.** It named `tools/agent-protocol.md`, which was planned in Phase 1 and never written (removed 2026-08-15, main#244: an unfollowable member teaches the reader the whole list is advisory). It named `tools/run-vm.sh`, which matches **neither literal — zero hits, structurally**, because it is a QEMU *launcher* that assembles a command line and hands over an interactive UART; it never reads boot output and cannot break. An **inert** member does the same damage as a **fictional** one: a reader dutifully opens it, finds nothing to change, and concludes the rest is advisory too. And it omitted **fourteen** files that do match (`test.sh`, `smp-multiboot.sh`, `test-cross-reboot.sh`, `test-fault.sh`, `ci-idle-gate.sh`, `np3-bench.sh`, `verify-kaslr.sh`, `warp/boot-probe.sh`, and six `interactive/*.exp`), plus two comment-only mentions.

**Why it rotted, which generalizes past this list** (vault, 2026-08-16): it conflates two kinds of member. A **program** that matches the string breaks *silently and immediately*; a **document** that states it merely *becomes wrong*, and nothing fails. `CLAUDE.md` and `TOOLING.md §10` are the second kind. **A list whose members share no property has no property any member can be checked against** — which is exactly how a phantom and an inert member sat in it unremarked for the project's life. Note `tools/test-fault.sh` matches seven extinction MESSAGE bodies, not just the prefix, so rewording one for clarity makes a hardening gate report that the protection did not fire.

The authority is the vault's `abi-boot-banner` note and its `mirrors` set (R6-enforced at change time), reached the usual way — `quaestor owner <changed paths>` in the mandatory doc-update step. Consult it rather than any list transcribed here, this one included.

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
- Thylacine's `/ctl` (Phase 4 P4-D — `kernel/devctl.c`) is a *separate* kernel admin surface for OS-level introspection. The Stratum `/ctl/` is consumed BY Thylacine userspace as just another mounted 9P tree (typically at `/srv/stratum-ctl/`).

POSIX surface available from Stratum (Thylacine consumes via 9P; no per-feature work needed unless the kernel needs to mediate):
- Live in v2.x: inodes + dirents + xattrs + file seals (`F_SEAL_*`) + advisory locks (`flock` / OFD locks) + `statx` + `name_to_handle_at` + `copy_file_range` (whole-file MVP) + `reflink` (single-dataset) + `rename` family (`RENAME_EXCHANGE` / `_WHITEOUT` / `_NOREPLACE`) + `fallocate` (PUNCH/COLLAPSE/INSERT/ZERO/UNSHARE) + symlinks + hard links + `O_TMPFILE` + `posix_fadvise` + inline-data optimization + snapshots (create/delete/hold/release/rollback).
- Deferred upstream (Thylacine accommodates as Stratum lands): cross-dataset reflink, `inotify`/`fanotify`, FS-verity API, `O_DIRECT`, OTLP exposition, learned tier policy, content-defined chunking.

Coordination rules:
- Thylacine Phases 1-4 already proceeded with no Stratum dependency. Phase 5 entry depends on Stratum v2 being available, which it now is.
- Phase 5+ stays within Stratum's stable ABI envelope. Any breaking Stratum on-disk format bump (`STM_UB_VERSION`) gets reflected in Thylacine's installer / upgrade path; Stratum's ABI compatibility envelope (mount-side compat for at least one major version) covers normal in-place upgrades.
- Stratum's audit-trigger surfaces remain Stratum's responsibility; Thylacine's audit covers the OS-side integration (9P client, mount path, key handling, `/ctl/` consumption).
- Slate (Plan-9-shaped TUI daemon also served as a 9P filesystem) is shipped by Stratum at `stratum/v2/src/slate/`. Thylacine's Halcyon (Phase 8) can adopt slate directly OR build an equivalent. The adoption story is documented in OS-INTEGRATION.md §17 — Halcyon's design pass should weigh it.
- **Stratum host-side sanitizers (ASan / LeakSan / TSan) run on a DISPOSABLE GCP Linux VM, never locally — both are BROKEN on the macOS dev host** (TSan SIGSEGVs inside `__tsan::InitializePlatform` before any user code, lldb-proven 2026-07-10; ASan hangs producing zero output). Recipe + cost discipline (spot `e2-standard-4` ~$0.04/h, boot-disk-only ≈ $0.02–0.05/run, create → run → **tear down immediately**, batch queued payloads onto one VM, propose-then-execute for mutating `gcloud` ops): `~/projects/stratum/CLAUDE.md` "Sanitizers" section + the stratum session memory `reference_gcp_compute.md`. (The Thylacine KERNEL is unaffected — it has no libc/sanitizer runtime; `tools/build.sh` UBSan + the multi-boot SMP gate remain its witnesses.)

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

## Parallel auxiliary track — the aux agent (worktree-isolated)

A **parallel auxiliary agent** works a separate git worktree (sibling dir, typically `../thylacine-aux`) on branch **`aux-2`**, established 2026-06-07 to use spare quota alongside the main (kernel) track. Every session of both tracks reads this file, so the boundary is common knowledge.

**Rewritten 2026-08-16 (aux#237) after measurement found EVERY operative claim here false** — and rewritten in aux's tree the same hour, because aux carried the identical paragraph and had therefore been loading a false description of *itself* for two months. The prior text claimed a 2026-06 charter ("owns `usr/apps/**` only", "never touches `kernel/`…`tools/`", "no edits to `docs/reference/*`", "never boots QEMU", plus a constitution / worklist / deliverable under `usr/apps/`) that the tree flatly contradicts: `aux-2` changes 41 `kernel/`, 17 `docs/reference/`, 12 `tools/`, 1 `specs/` and **zero** `usr/apps/` files, and aux boots HVF VMs and runs its own SMP gate + LS-CI.

**It was not describing a track that failed. It was describing a track whose output MOVED INTO THIS TREE, and whose charter was never rewritten to say so** — the coreutils became `usr/coreutils` (51 binaries), the Tapestry POC became `usr/lib/libtapestry` + `usr/tapestry-demo` + `usr/tapestryd`, and five documents you now depend on (`MENAGERIE.md`, `TRUSTED-PATH.md`, `INSTALLER.md`, `TAPESTRY.md`, `AURORA.md`) began life as `usr/apps/*-DESIGN.md`. **Promotion is the event that invalidates a description, and it never announces itself at the old location.**

**Why this is a defect and not mere staleness:** on 2026-08-12 the deleted contention clause produced the *same wrong operational call in both readers within one day* — main reasoned from it, and aux reached the identical conclusion independently ("your specs are CPU-only and mine is one TCG VM, so we are genuinely not colliding"), about ninety minutes before measuring a TLC run at 307.5% CPU against aux's own starved boot. It misled the reader with most reason to trust it AND the reader with most reason to know better. **Neither noticing required new information — only a check that never ran.**

**Method note, because two opposite probes both lie here.** A file-existence test at a cited path answers "is this PATH stale", never "does this DOCUMENT exist" — it reported the roadmap deleted when it had merely moved. And `git log --diff-filter=D` answers "was it deleted **in a commit**": a rebase drops a file leaving *no* deletion record, so empty output reads as "never deleted" = "still present". One over-reports absence, the other over-reports presence, and running both without noticing they disagree concludes a file is simultaneously gone and alive. Only a **tip scan across all refs** answers the real question.

- **The aux track is a full second engineering track, not a userspace-apps annex.** In aux's own words (2026-08-16, landed verbatim on request): *the aux track owns the VIVARIUM arc (running unmodified Linux binaries; `docs/VIVARIUM.md`), the graphics arc G-6..G-9 and the Aurora environment, and the kernel surfaces those arcs land on — currently the notes/signals/job-control/PTY line. It runs its own full bar for that work: the suite, the SMP gate, the pty spec set, and LS-CI. Its worklist is `docs/AUX-ROADMAP.md`; its branch is whatever `git branch --show-current` says in `../thylacine-aux`.* **Read the branch off the worktree, never off a doc**: this file said `aux/userspace-apps`, the roadmap said `gfx-4`, the worktree is on `aux-2` — and `gfx-1`..`gfx-4` are all still live branches, so these are parallel refs, not a rename chain. Only the worktree is ever current.
- **The real shared surface is `kernel/` + `tools/` + `docs/reference/`.** The old `usr/apps/**` guard was a guard on an empty room, and worse than useless: it aimed attention away from where the tracks actually meet. Collisions there are routine and handled well by both sides landing findings in each other's trees (worked example: the tree-wide-`pkill` / `mktemp` hazard sweep, yip call 0009). Coordinate through **yip** — `presence` answers "is the other track gating?" with no call placed; `busy` announces a long gate; `call` for anything needing a reply.
- **HOST CONTENTION IS REAL, AND IT MAY EXPLAIN NOTHING BUT WALL CLOCK.** The contended resource is **CPU CORES**, not QEMU-ness: TCG is a pure CPU emulator, an HVF guest runs real vCPU threads, and `cargo build -j` / a TLC run / a sanitizer build each saturate every core — so builds, model checks and guest boots are **ONE contention class**. This host has 8 cores; the SMP gate's `smp8` configs alone ask for 8 vCPU threads, so a concurrent build is direct core-for-core competition. **Measured 2026-08-16, same tree and same commit** — contended: aux ran 5 LS-CI scenarios in 76 min, 4 of them burning attempt-1 retries; quiet host: **8 scenarios in 10 min, 0 retries, and all four of those scenarios pass.** Concurrently main's TLC went 307.5% -> 629.0% CPU when aux parked. Neither agent was getting the machine, and neither could see it.

  **What that licenses, exactly.** Contention may explain a **wall-clock budget miss** — a timeout, a burned retry, a thin scenario overrunning. It may NEVER explain a corruption, a wrong value, a crash, or a nondeterministic assertion failure; those stay races to be hunted per §"Whole-system stewardship". And even for a timeout it is a conclusion you MEASURE, never one you reach for: aux's diagnosis rested on process-state evidence (`RN+` — running, not sleeping) taken while it was happening, and the quiet-host green is only CORROBORATION, because **passing later never proves why something failed earlier**. A retry count with no host conditions stamped on it is the same fiction as a benchmark with no lane named.

  **Announce the RESOURCE and the UNCERTAINTY, not the duration** (aux's protocol, binding on both tracks): "unknown, 6 cores" tells the peer to serialize; "30 min" implies a bound nobody can honour. When a timing-thin gate needs a quiet host, ASK — the negotiation has been run cleanly twice (yip 0009 turns 7-11; 0014).

  The deleted clause claimed aux "never contends with the main track's QEMU boots, the host-oversubscription flake source" — false in both halves, and its parenthetical licensed, in always-loaded scripture, the exact "host load" non-explanation that §"Whole-system stewardship" forbids ~1000 lines above.
- **Kaua (LS-7) supersedes the `nora` ratatui-fork item (2026-06-13, user-directed).** MAIN owns the console TUI substrate (`usr/lib/kaua`, `docs/KAUA.md`) + the runtime editor (`usr/nora`, native libthyla-rs *on Kaua*): an editor needs ~10% of a general TUI framework, so a focused native Kaua core (immediate-mode cell-diff over cons/consctl) beats a full ratatui fork. `libtapestry` (the **graphics** weave, `docs/TAPESTRY.md`) is a different layer and unaffected. (Kaua = the **text** weave; named for the Kauaʻi ʻōʻō, the last of family Mohoidae; stands outside the Loom-woven names, `Weft` reserved -- KAUA.md §1.1.)

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

### The 600k checkpoint line — run THROUGH checkpoints until it fires

**A checkpoint is not a stopping point.** Under granted autonomy, land a chunk,
report it, and **start the next one in the same run**. Do not yield after every
chunk waiting to be told to continue; do not compact "to be safe" at 300k. The
cost of stopping early is real and asymmetric — a fresh context has to re-derive
the subsystem knowledge the current one already holds, and the re-derivation is
where wrong turns come from.

**The signal that ends the run is the `ctx-hook` CHECKPOINT WINDOW line at
600k** (`~/.claude/ctx-hook.sh`, `CTX_CKPT`; ~66% of the 900k window, which is
the "~60-70%" the bullets below already named). That hook fires on every tool
call, so it sees the budget continuously — you do not have to estimate it, and
you should not try. Three levels, three different meanings:

| Level | Fires | Means |
|---|---|---|
| **600k CHECKPOINT WINDOW** | once per crossing | **The intended compaction point.** Carry to a clean boundary, write the resume note, then run `tools/thyla-selfcompact.sh "<reason>"`. |
| **750k** | every call | Wind down. Finish the step; do not open a new arc. |
| **880k** | every call | At the wrap line. Commit, hand off, yield. |

**At 600k you compact yourself; you do not ask.** `tools/thyla-selfcompact.sh`
types `/compact` into your own tmux pane, and `~/.claude/resume-note.py`
re-injects your last message on the far side — the two steps the user was
performing by hand at every boundary. Three things follow from that:

- **Your final message before invoking it IS the resume note.** Not a report to
  a reader who will answer — a note to yourself with no memory of writing it.
  It must say what is in flight and, more importantly, **what must NOT be
  redone**: gates already green, commits already pushed, measurements already
  taken. A fresh context that re-runs a two-hour bar has been failed by that
  message.
- **Invoking it is a request, not a decision.** It refuses on a dirty tree or
  outside tmux, and it **belays** — hands back to the user — when HEAD has not
  moved across two consecutive self-compactions. That gate exists because the
  dangerous failure is not a runaway but a *quiet loop*: hit a problem,
  compact, return with less context, fail the same way. Every turn looks like
  progress and none is, and an iteration cap cannot catch it because the
  pathological case sits under the cap. Only landed work distinguishes stuck
  from thinking, so only landed work re-arms the mechanism.
- **A belay is a stop, and it is the good outcome.** When it fires, hand back
  with what was attempted and what it needs. Do not clear the state file to get
  moving again; that is disabling the one guard standing between a long run and
  an expensive one.
- **A queued self-compaction is NOT yours to cancel — only the operator's.**
  The script types `/compact` + Enter into the pane the instant you invoke it,
  so the submission is queued in the client immediately. You CANNOT retract it
  from inside your own turn: `tmux send-keys C-u` clears only the *live input
  box*, never an already-Enter-queued command — a `/compact` you "cancel" that
  way survives and fires later against whatever session is up. (Worked failure,
  2026-08-19: a self-compact invoked early at 560k, then countermanded by the
  Stop hook, was "cancelled" with `C-u`; the stray `/compact` rode the input
  queue for ~4 hours and submitted right after the *real* compaction — harmless
  only by luck, because a spurious `/compact` no-ops with "Not enough messages
  to compact.") Two rules follow. **(1) Invoke `thyla-selfcompact.sh` only on
  the real 600k signal, never in anticipation of it** — that is the one moment
  nothing will countermand you, so there is nothing to cancel. **(2) If you must
  abort a queued self-compaction anyway, you cannot do it yourself: raise a
  blocking question to the operator** (`AskUserQuestion` — it interrupts the
  turn without ending it) asking them to cancel it. The operator is the only
  actor who can clear the client's input queue; your keystrokes cannot.

Where the script is absent (a worktree that has not merged it), the hook says
"recommend `/compact`" instead and the old behaviour stands — the two arms are
discrimination-tested, not assumed.

So the rule is: **below 600k, keep working through as many checkpoints as the
work takes; at 600k, finish to a clean boundary and self-compact.**
The 600k line is advisory and deliberately fires ONCE — it is a "this is the
right moment," not an alarm. Reaching it mid-chunk does not mean stopping
mid-chunk: carry to the next clean boundary (committed, gates green, handoff
current) and recommend from there. If that boundary is genuinely far away, say
so and keep going — 750k is the level that means wind down.

**What does NOT change: the checkpoint contract still fires at every
checkpoint** (§"The checkpoint contract"). Account for running processes, keep
the handoff current, say what is next — at each one, whether or not you yield.
That is precisely what makes this rule safe: if the handoff is continuously
current, then compaction is free at *any* moment, so choosing to run on costs
nothing and the 600k line can be a recommendation rather than a scramble.

**What also does NOT change: the escalation list.** Running through checkpoints
is autonomy over *sequencing*, never over the items in §"Autonomy + escalation"
— a format break, a destructive operation, an architectural deviation, a
scripture-altering design fork still stops the run and asks, at 100k or 700k.

**If the hook is not installed, this rule has no brake.** `ctx-hook.sh` lives in
`~/.claude/`, outside this repo, so a fresh machine or a differently-configured
session may not have it — and then "run until the signal fires" means running
past the wrap line into a hard overflow, because a signal that never arrives is
indistinguishable from one that has not arrived *yet*. So: an autonomous run
that has passed roughly two thirds of its budget **without ever seeing a
CONTEXT line** should treat the hook as absent and fall back to the judgement
bullets below rather than keep waiting. Verify with
`ls -l ~/.claude/ctx-hook.sh` if in doubt; one command settles it.

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

### A checkpoint is not a stopping point (the 600k line)

**Under granted autonomy, land a chunk, report it, and start the next one IN
THE SAME RUN.** Do not yield after every chunk waiting to be told to continue;
do not compact "to be safe" at 300k. Stopping early is not free — a fresh
context re-derives subsystem knowledge the current one already holds, and the
re-derivation is where wrong turns come from.

**The signal that ends the run is the `600k CHECKPOINT WINDOW` line** emitted by
the user-level `~/.claude/ctx-hook.sh` PostToolUse hook. It has three levels
that mean three DIFFERENT things, and collapsing them into one "context is
getting high" warning is how the 600k line turns back into an alarm:

| Level | Fires | Means |
|---|---|---|
| 600k CHECKPOINT WINDOW | once per crossing | the intended compaction point — carry the current step to a clean boundary, then recommend `/compact` |
| 750k approaching | every call | wind down; start no new arc |
| 880k AT THE WRAP LINE | every call | commit, hand off, yield |

Two conditions make this safe rather than merely faster, and adopting the
run-longer half without them is **strictly worse than not adopting it**:

1. **The checkpoint contract still fires at every checkpoint**, including the
   ones you run straight through. Account for running processes, keep the
   handoff current, say what is next. A continuously-current handoff is what
   makes compaction free at any moment — which is what lets 600k be a
   recommendation instead of a scramble.
2. **If the hook is absent, the rule has no brake.** "Run until the signal
   fires" degrades into "run past the wrap line", because a signal that never
   arrives is indistinguishable from one that has not arrived YET. A run two
   thirds through its budget having seen NO `CONTEXT` line should treat the
   hook as absent and fall back to judgement.

   **The wiring is CONFIRMED for aux** (2026-08-15), which was worth checking
   because this worktree defines its own `PostToolUse` (the yip line-hook) in
   `.claude/settings.local.json` — so whether the user-level hook *also* runs
   was a question about hook merging, not something to assume. It does:
   **hooks MERGE across the user and project/local layers**, and aux runs
   both. Observed the awkward way — the hook announced itself by reporting a
   shell parse error mid-edit (`PostToolUse:Bash hook blocking error ...
   ctx-hook.sh: line 52`), and a hook that errors is still a hook that ran.
   Never having seen a `CONTEXT` line is explained by this context sitting far
   below 600k, not by an override.

   **A user-level hook is executed by every live session on every tool call**,
   so editing one in place opens a window where other sessions run a partial
   file. Write to a temp path and `mv` it over (a same-filesystem rename is
   atomic; an in-place write is not).

This is autonomy over SEQUENCING only. The escalation list is untouched —
format breaks, destructive operations and scripture forks still stop the run.

**Host holds are the cost, and they land on the other tracks.** Longer runs
mean longer exclusive holds on the one machine (an SMP gate is ~20 min, LS-CI
~30). Check `yip presence` before committing to a timed measurement, keep the
refuse-up-front gates (`849d85fc`), and say on the line when you want a quiet
host. Named here so a contention-shaped failure is never attributed to
something else later.

**The handoff is ready before you recommend anything.** Per the checkpoint
contract, a compactable state means the handoff docs are ALREADY written — so
this section is only about whether to *suggest* compacting, never about whether
the user *could*. The user decides when to compact; your job is to make sure
that decision never costs a preparatory round-trip. When the state is
compactable and you are not recommending it, still say so in a clause
("compactable if you want it; otherwise next is Y").

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
- **Multi-thread per-Proc shared state**: any structure reachable from more than one Thread of a Proc (peer threads share the address space, handle table, Territory, service connections) — or from more than one `rfork(RFNAMEG)`-sharing Proc — needs a lock or a multi-waiter. "No current program drives two threads in here" is the **latent-P1 trap**, not a safety argument: the kernel must be sound against any EL0 program, so a reachable-but-undriven race is a live defect, not "dormant." Two red flags: a **single-waiter `Rendez`** on per-Proc-reachable state (safe only if something else guarantees a single drainer — the instant a second path reaches it the assertion is an unprivileged extinction; use a multi-waiter or the `devcons` single-reader busy-guard), and a **lock added for one field** of a shared struct (LS-4's `dot_lock` guards only `dot_path` — the unguarded `mounts[]`/`root_spoor` siblings were the next finding). This is the P6-multi-thread-lift recurrent class (RW-2 2C-F1/2B-F1, RW-4 SA-F1/R2-F1; precedents #844/#713/LS-4/#847). Full write-up + the per-struct sweep: `docs/DEBUGGING-PLAYBOOK.md` §6.15.
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

**FIRST, THE TRIGGER — because this section reads as "chunk done -> write this", and that instruction is what ends the run.** Emitting the full summary IS the yield: final text ends the turn, and in this harness nothing restarts you afterwards but the user. So a summary written at a completed chunk silently converts "running through checkpoints" into "handing back", no matter what §"The 600k checkpoint line" says. Measured 2026-08-16, on the first autonomous run: the chunk landed, the summary got written because scripture said to, and the run stopped at ~160k of a 600k budget. The concrete ritual beats the abstract rule every time.

**So the full summary below belongs to STOPPING, not to finishing.** Write it when you are actually handing back: at the 600k line, on an item from §"Autonomy + escalation", when genuinely blocked, or when the user asks. At a checkpoint you are running THROUGH, the checkpoint contract is discharged in **three lines or fewer** — what landed (hash), what is running, what is next — and then you **open the next item in the same turn**, without final prose.

**The tell:** if you are writing a `Key` table, an `Arc state` field, or an `Ahead` line, you are writing a hand-back. Stop and ask whether you actually intend to stop. If you do not, delete it and make the next tool call instead.

**The `Stop` hook now exists** (`tools/stop-hook.sh`, user-requested
2026-08-16 after a run stopped at a checkpoint it should have run through --
having written the very `Ahead` line named above as the tell). This paragraph
used to say the hook was "deliberately not built"; it was built precisely
because behaviour that is only as good as remembering it was not good enough.

What it does, so you recognize it rather than argue with it: on a stop it
computes the same budget `ctx-hook.sh` does, and if you are **between 120k and
the 600k checkpoint line** and have taken **>= 6 assistant turns since the user
last spoke** -- i.e. an autonomous run, not a reply -- it blocks ONCE and asks
which of four cases applies. Three of them (an §"Autonomy + escalation" item, a
question you have now answered, a genuine block) make stopping CORRECT: name it
in a clause and stop, and it will not ask again. The fourth is the one it exists
for -- **open the chunk you just named on your own `Next` line instead of
yielding.**

It is a question, not a veto, because only you can tell an earned yield from an
unearned one. It stays silent below 120k (conversational), at/above 600k
(stopping is what scripture wants there, and a second voice contradicting
`ctx-hook.sh` would be worse than silence), and whenever `stop_hook_active` is
already set. **It fails OPEN on every error path** -- a Stop hook that failed
closed could trap a session in a loop it cannot talk its way out of, which is
far worse than a missed nudge. Discrimination-tested across all seven
conditions, including the two legs most likely to be silently wrong: a
tool-result and a system notification must NOT count as "the user spoke", or the
counter resets constantly and the hook goes quiet during exactly the runs it is
for.

It lives IN THE REPO rather than in `~/.claude/` (where `ctx-hook.sh` sits, and
whose absence §"The 600k checkpoint line" already flags as leaving that rule
with no brake). One copy, no sync obligation, and it survives a fresh clone;
`~/.claude/settings.json` points its `Stop` event at this path. Install on a new
machine is that one settings entry.

End-of-iteration summaries (the response to a completed audit / chunk) follow a consistent structure for fast review.

**Order matters: orientation FIRST, detail after.** The user does not carry the
identifier map in their head and does not remember where the arc was paused.
Open with what this session was about and how it serves the arc; only then the
journal. (Ratified 2026-08-14 at the user's request.)

```
## <one-line title: what this session was about>

**Focus**: <1-2 sentences: what this session actually worked on>.
**Arc fit**: <how it serves the current arc's direction / why it was worth
doing now>.

**Arc state**: ON ARC — <the chunk being built>.
   | PAUSED: the arc is stopped at <exact position>; this session is a side
   quest on <the soundness / harness / instrument problem that preempted it>.
   **Resumption needs**: <the specific remaining items, in order>.
   (OMIT this field entirely when directly on arc — do not write "n/a".)

**Key** — every identifier used below, in words. No bare ids anywhere:
| Id | Is |
|---|---|
| <#N / C-x / P1a / I-nn> | <plain-language name, <=12 words> |

**Arc metrics** (current values; a metric with no measurement says so):
| Metric | Value | Measured | Source |
|---|---|---|---|

**Exit criteria** (the arc's ratified bar + how we move toward each):
| Criterion | Target | Now | Moving via |
|---|---|---|---|

**This iteration landed (N new commits, tip <hash>)**:
- <hash1> — <one-line scope>

<the detailed journal: what was found, what it means, what went wrong and how
it was caught. Keep this rich — it is the part worth reading.>

**Posture**: <suites> × (default + ASan + TSan) green. <spec count> specs
clean. test_<X> at <count>.

**Running**: <nothing running | what is alive and why, one line each>.

**Handoff**: current at <tip> — compactable. | NOT compactable: <the one
blocker>.

**Next**: <the single immediate next action>.
**Ahead**: <chunk> -> <chunk> -> <chunk> -> <arc close>.

**Memory**: <files updated>.
```

This structure lets the user (or a future session reading the conversation log) reconstruct state in under 30 seconds.

Field notes, each earned:

- **`Key` is not optional and not decorative.** Sessions accumulate dense
  identifier vocabularies (`C-0`, `P1a`, `#240`, `I-45`, `Warp-C`) that are
  perfectly legible in-session and opaque a day later. Expand every id used in
  the summary, including ones that feel obvious. A summary the reader must
  decode is a summary that does not work.
- **`Arc state` exists because side quests are the norm, not the exception.**
  The whole-system-stewardship rule guarantees that surfaced defects preempt
  chunk work — so the user is frequently reading a report about something
  other than the arc they last approved. Say where the arc is parked and what
  it is waiting on, or they cannot tell a detour from a change of direction.
- **`Arc metrics` + `Exit criteria` answer "are we winning?"** A chunk-by-chunk
  narrative can read as steady progress while the number that defines success
  has not moved. State the bar, the current standing against it, and the
  mechanism that closes the gap. **Never quote a metric without its
  provenance** — a figure from a different workload, lane, or host is a
  different number wearing the same units (#236 is the standing example: two
  lanes disagreed 2x on the same renderer at the same resolution).
- The last four fields are the checkpoint contract made concrete — `Running`
  answers "is Claude still working?", `Handoff` answers "can I compact right
  now?", and `Next`/`Ahead` answer "where are we in the arc?". Emit all four at
  every checkpoint even when the answer is boring; a missing field reads as an
  unknown, and the whole point is that the user should not have to ask.

---

## When in doubt

1. Re-read VISION + ARCH + ROADMAP for the relevant section.
2. Check if a TLA+ spec covers it; if so, the spec wins.
3. Check the audit-trigger table; if the change touches a trigger surface, audit before merge.
4. **If you are chasing an elusive bug** — a corruption-class symptom, inconsistent repro, a cross-layer fault, or a bug a prior session "resolved" that recurred — **read `docs/DEBUGGING-PLAYBOOK.md` BEFORE theorizing** (the `elusive-bug-hunt` skill auto-surfaces the condensed method). Ground truth over theory; suspect masking-bug stacks; distrust hollow "AUDITED CLEAN" closes.
5. If still uncertain, ask the user. Confirming is cheap; getting it wrong is expensive.

The thylacine is real. So is this.
