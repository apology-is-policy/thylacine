# CLAUDE.md

Operating notes for Claude Code instances working on Thylacine OS.

This document is binding scripture for every implementation session. It holds the rules that must be in mind at all times. Procedure and reference detail lives in `docs/agent/`, in skills (`.claude/skills/`) and in path-scoped rules (`.claude/rules/`), which load when they are needed. Every pointer below says **when** to read its file; the files it points at are as binding as this one. (Trimmed 2026-09-23 from 218 KB; the moved text is verbatim in its new home.)

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
| the vault (`vault/`, in-tree on `main`) + `docs/reference/NN-*.md` (LEGACY) | As-built technical reference. Per-subsystem; deep; binding. **Being retired into the vault (2026-09-06): `docs/reference` is frozen; new prose goes to a dossier.** The vault lives in-tree on `main`, so you edit it in your own worktree. See "Reference documentation discipline" Part A. |
| `docs/OPERATORS-MANUAL.md` + `docs/manual/NN-*.md` | The Thylacine Operator's Manual (operator-facing; one section per facility; shipped in-OS at `/manual`). Written to `docs/thylacine-operators-manual-writing-guide.md`, which is binding; encoded, installed and read per `docs/MANUAL-DESIGN.md`, also binding. See Part B below. |
| `docs/AUDIT-TRIGGERS.md` | The full audit-trigger surface table (moved verbatim from this file 2026-08-05). One row per audit-bearing surface: files + invariants + the per-chunk prosecution addenda. Cumulative; binding. |
| `docs/ERRORS.md` | Error-code system. Errno registry (Thylacine-wide, POSIX-aligned values), `snare:*` fault-note family (thematic; replaces EL0-unhandled-fault extinction with per-Proc termination), exit-status semantics, boundary-line translation policy. ABI-bearing; updates require user signoff. |
| `CLAUDE.md` (this) | Operational framework for Claude Code sessions. |
| `docs/DEBUGGING-PLAYBOOK.md` | **Mandatory reading when an elusive bug appears** (corruption-class symptom, inconsistent repro, cross-layer, or a recurred "resolved" bug). The AEGIS-corruption-triplet case study + the ground-truth-first method. The `elusive-bug-hunt` skill auto-surfaces the condensed method; this doc is the full journal. |

Read first, in this order: VISION → ARCHITECTURE → ROADMAP → CLAUDE.md → the relevant phase status doc.

**The agent annex (`docs/agent/`)** -- binding, read on demand:

| File | Read it when |
|---|---|
| `docs/agent/SPEC-POLICY.md` | A change touches a mechanism modelled in `specs/`, or a feature might need a TLA+ model |
| `docs/agent/DESIGN-FORKS.md` | Before surfacing a design fork, or when a chunk raises a design question scripture does not answer |
| `docs/agent/AUDIT-TRIGGERS-INDEX.md` | Looking up which audit-trigger row owns a file (then read that row in `docs/AUDIT-TRIGGERS.md`) |
| `docs/agent/DOC-DISCIPLINE.md` | Updating a dossier, the Operator's Manual, or a phase status doc |
| `docs/agent/GATES.md` | Running any gate beyond the core commands below |
| `docs/agent/SESSION-DISCIPLINE.md` | The full checkpoint contract, the journal rules, and the compaction rationale |
| `docs/agent/NATIVE-VS-PORTED.md` | Adding a new userspace program |
| `docs/agent/STRATUM-COORDINATION.md` | Touching Stratum or the 9P/Stratum boundary |
| `docs/agent/AUX-TRACK.md` | Coordinating with the aux track, or a timing-sensitive gate on the shared host |
| `docs/agent/THYLA-PI.md` | Using the thyla-pi host |
| `docs/agent/BOOT-BANNER.md` | Touching the boot banner or `EXTINCTION:` strings |
| `docs/agent/NAMING.md` | Naming a new mechanism |

Skills: `audit-round` (every audit round, start to close) and `hand-back` (the full summary when actually stopping).

---

## Scripture before code

Implementation is bound by the scripture above. If implementation needs something scripture does not cover, update the scripture document first (user signoff for any binding change), then implement. Never silently deviate. The active phase status doc is `docs/phaseN-status.md`; update it per chunk.

**Spec-first is suspended** (since 2026-05-23) except on the surfaces where it was re-enabled (recorded in `specs/SPEC-TO-CODE.md`). New invariant-bearing work is validated by prose reasoning in the file header + commit + dossier, plus the audit round and the tests. **Still binding:** any change to a mechanism modelled in `specs/` re-runs that spec's buggy cfgs; the section-28 invariants remain proof obligations. Detail and TLA+ setup: `docs/agent/SPEC-POLICY.md`.

**A design question mid-chunk** stops the implementation: research the heritage (Plan 9) and SOTA answers first, then surface the options with that research attached via `AskUserQuestion`, land the decision as a scripture commit, then implement, then audit. Detail: `docs/agent/DESIGN-FORKS.md`.

---

## Audit-triggering changes

Any change to a surface in `docs/AUDIT-TRIGGERS.md` MUST get a focused adversarial soundness audit before merge -- each round has historically found bugs the tests did not. Run it with the `audit-round` skill (holotype-reviewer on the highest available Fable at max effort, else the highest Opus; **never skip a round for want of Fable**). Fix every P0/P1/P2 before merge; P3s are tracked or closed with a stated reason; silent drops are forbidden.

The path-scoped rule `.claude/rules/audit-triggers.md` reminds you when you read a file on a trigger surface. To find a file's row: grep `docs/agent/AUDIT-TRIGGERS-INDEX.md` or `docs/AUDIT-TRIGGERS.md` for the path.

**Context economy on `docs/AUDIT-TRIGGERS.md` (binding).** The file is ~440 KB (~110K tokens); a whole-file `Read` is never justified and the default Read cap would silently truncate it anyway. Locate the row first (grep by path or surface keyword), then `Read` ONLY that row's line window. To append a chunk's new row: `Edit` after that windowed read. Never `Write` (whole-file replace) it. The same discipline applies to every large scripture file: grep to locate, window to read, `Edit` to change.

The trigger list is *cumulative*: a chunk that adds an audit-bearing surface appends its full row to `docs/AUDIT-TRIGGERS.md` and a one-line entry to `docs/agent/AUDIT-TRIGGERS-INDEX.md`, in the same PR.

---

## Invariants that must hold

One line each. The authoritative text, with the full enforcement cells, is `ARCHITECTURE.md` section 28 -- read it before reasoning about any of these. **Keep the ROW SET in sync with ARCH section 28** (`tools/check-invariants.py`, run by `build.sh`, fails on drift or misordering).

| # | Invariant | Validation |
|---|---|---|
| I-1 | Territory operations in Proc A don't affect Proc B | `territory.tla` |
| I-2 | Fork-grantable caps only shrink; `CAP_ELEVATION_ONLY` stripped at every fork; growth only via the `cap` device. Re-derive the elevation set from `caps.h`, never from a prose list | `handles.tla` |
| I-3 | Mount points form a DAG, never a cycle | `territory.tla` |
| I-4 | Handles transfer between Procs only via 9P sessions | `handles.tla` |
| I-5 | `KObj_MMIO`/`IRQ`/`DMA`/`Loom` non-transferable; one kernel-mediated exception: `SYS_SEAT_IMPORT` | `handles.tla` + static_asserts + `seat` tests |
| I-6 | Handle rights only shrink on transfer/dup/endow | `handles.tla` |
| I-7 | BURROW pages live until last handle closed AND last mapping unmapped | `burrow.tla` |
| I-8 | Every runnable thread eventually runs | `scheduler.tla` liveness |
| I-9 | No wakeup lost between cond-check and sleep (incl. death-wake, interrupt, readiness ring) | `scheduler`/`poll`/`tsleep`/`death_wake`/`reader_frame` .tla + others |
| I-10 | Per-9P-session tag uniqueness | `9p_client.tla` |
| I-11 | Per-9P-session fid identity stable for the fid's open lifetime | `9p_client.tla` |
| I-12 | W^X on every page, plus provenance: exec file maps need a `may_back_exec` Dev AND a non-`MNOEXEC` mount | runtime + `_Static_assert` |
| I-13 | Kernel-userspace isolation: TTBR0/TTBR1 split | runtime |
| I-14 | Stratum block integrity (Merkle); hostile Rlerror ecodes bounded | Stratum-side + test |
| I-15 | Hardware view derives from DTB (documented PL011 fallbacks excepted) | review + audit |
| I-16 | KASLR randomizes the kernel base at boot | `verify-kaslr.sh` |
| I-17 | EEVDF latency bound -- DESIGN TARGET, not yet enforced | `scheduler.tla` (qualitative) |
| I-18 | IPIs from CPU A to B processed in send order | `scheduler.tla` |
| I-19 | Note delivery causal order; exactly-once; `kill` uncatchable; one default action per note (`g_known_notes` `dfl`); unification OWED (#15 F5) | prose + tests |
| I-20 | PTY master/slave atomicity -- ENFORCED | `pty.tla` + `pty_stop.tla` |
| I-21 | Kernel uniformly EL1h; `SP_EL0` exclusively the user stack | `sched_ctxsw.tla` + `test_smp` |
| I-22 | No identity carries ambient super-authority; elevation only via the legate | prose + tests |
| I-23 | A service's FS authority bounded by its endowed storage capability | prose + tests |
| I-24 | Group termination atomic + exactly-once; no EL0 after ZOMBIE | `death_wake.tla` + prose |
| I-25 | Legate authority scope-bounded, fully revoked on scope exit; propagating scopes per IMPERIUM 11.4 | prose + tests + `imperium.tla` |
| I-26 | Cross-Proc kill is two-axis (owner OR `CAP_HOSTOWNER`/`CAP_KILL`) | prose + tests |
| I-27 | Trusted path: SAK unspoofable; ATTACH distinct from OWNER; enforced on serial (IM-1) and graphical (Lictor seat) | TRUSTED-PATH + IMPERIUM 11.3 + tests |
| I-28 | Path resolution contained at `root_spoor` + per-component X-search; symlinks contained by the same machinery | STALK-DESIGN + DISTRO + tests |
| I-29 | Loom: exactly one terminal CQE; no stale; CQ never overfilled | `loom*.tla` |
| I-30 | Loom submit-time capability pin; no post-check re-read of the shared ring | `loom.tla` buggy cfgs |
| I-31 | ASID rollover: no cross-generation aliasing | `asid.tla` |
| I-32 | Resource floor: pages/VMAs per ADDRESS SPACE (cap on the AddrSpace), threads/children per Proc; fail clean, never extinct | ARCH 28 + prose + tests |
| I-33 | Namespace name retention is non-load-bearing (resolver never reads `->path`) | prose + tests |
| I-34 | Driver authority bound to its warden-granted allowance (MMIO/IRQ/DMA/PCI); never widened; revoked on unbind | `allowance.tla` + tests |
| I-35 | Mandate attenuation + revocation -- RESERVED | planned `mandate.tla` |
| I-36 | File-backed demand-paged exec soundness (the 7 conditions) | prose + R-5 audit + tests |
| I-37 | Weft capability network dataplane integrity | `weft.tla` + `weft_readiness.tla` |
| I-38 | Larder cache coherence under close-to-open | `fs_cache.tla` |
| I-39 | Debug authority bounded: two-axis gate; execution control stopped-only; no text writes; die-with-launcher (EXITKILL) | `debug_stop.tla` + `debug_step.tla` + tests |
| I-40 | No torn scanout / surface-share integrity -- kernel share half ENFORCED | `tapestry_present.tla` |
| I-41 | NOT ALLOCATED in section 28 (AG-2 reserves it in ADVANCED-GO-DESIGN.md only) | -- |
| I-42 | JIT is a capability (`CAP_JIT`); W^X holds across the publish -- ENFORCED | prose + CL-7k audit |
| I-43 | A phenotype confers ABI SHAPE, never AUTHORITY | prose + tests + gate |
| I-44 | Address-space integrity under sharing + COW -- ENFORCED | `cow.tla` + LINEAGE + audit |
| I-45 | GPU authority bounded by the context; guest half ENFORCED, host half on virgl/Venus trusted | prose GPU-DESIGN 8 + audits |
| I-46 | RESERVED: Nocturne audio authority + no-stall cycle | planned `nocturne_cycle.tla` |
| I-47 | RESERVED: inline media -- decode outside halcyond, bounded place-request | planned prose + audit |

---

## Regression testing

- Every audit finding that can be made to fail without the fix lands a regression test that fails before the fix and passes after.
- Every spec bug shown by a `{spec}_buggy.cfg` gets a runtime regression test where feasible; otherwise the buggy cfg is the durable regression.
- Pre-commit: the full suite on the default build. Pre-merge for invariant-bearing changes: all sanitizer matrices + all affected specs.
- Build + test commands: below, and `docs/agent/GATES.md`.

---

## Implementation patterns

Kernel coding patterns (idempotency on retry, compile-time invariants, crash/fault injection) live in `.claude/rules/kernel-code.md` and load when kernel files are read.

### Split big chunks into sub-chunks

When an implementation chunk exceeds one commit's reasonable scope, split into sub-chunks named Xa / Xb / Xc. Each sub-chunk lands independently with its own status-doc row, commit message, and tests. Handoff points between sub-chunks mean a context compaction at any boundary is recoverable.

**Chunk completeness.** If the chunk's complete implementation depends on a later or earlier-deferred item, **pull it forward** by default and note it in the commit + status row -- no signoff needed. **Deferring** a real dependency is the exception and needs the user's vote (surface it as a structured choice). Quiet deferrals compound into silent omissions.

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

**Surface an escalation or decision as a BLOCKING question, not a passive summary (autonomous mode; user-directed 2026-08-19).** When autonomy is active (no `.claude/.no-stop-nudge` dotfile), put any escalation above, any genuine decision you need from the user, any *accumulated* pending votes, or any vote that is *blocking your intended path*, via **`AskUserQuestion`** — never buried in a closing summary a user who may be away for hours never reads. `AskUserQuestion` is a tool call, not a turn-end: it blocks for the answer and keeps you IN-TURN, which is *more* aligned with the no-unearned-yield rule, not less. Pure waits (a running gate, quota, hardware — nothing to decide) and already-answered questions stay clean stops, not blocking questions. This is enforced at turn-end by `tools/stop-hook.sh`; the `.no-stop-nudge` dotfile suppresses the hook and this directive together.

**Deviation tracking**: If implementation diverges from ARCH / ROADMAP, surface it explicitly:

- In the commit message (the WHY of the deviation).
- In the affected phase status doc.
- If the deviation is load-bearing, propose an ARCH update; do not silently normalize the deviation.

---

## Git + commit discipline

- **Detailed commit messages** with prose rationale: WHAT changed, WHY, and the alternative if non-obvious. First line under ~70 chars.
- **Per-chunk commits**, not per-day. A chunk is coherent, testable, revertable.
- **Attribution footer**: use the lines the harness's attribution reminder gives for this session.
- **Prefer new commits over `--amend`.** Never force-push main or shared branches. Never skip hooks unless the user asks.
- **Plain ASCII** in commit messages: `--` not em-dash, `->` not arrows, no section sign, straight quotes, `>=`/`<=`/`!=`, no emoji. Pass bodies via a quoted HEREDOC (`<<'EOF'`).
- **Before committing**, run the full suite on the default build; invariant-bearing changes run the full matrix + specs.
- Audit-bearing commit structure and the audit-close anatomy: the `audit-round` skill.

---

## Memory + session continuity

Auto-memory lives at `~/.claude/projects/-Users-northkillpd-projects-thylacine/memory/`: `MEMORY.md` (one-line index), `project_active.md` (current state), `project_next_session.md` (pickup pointer), `audit_rN_closed_list.md` (cumulative do-not-report sets), `user_profile.md`, `feedback_*.md`, `TASK-ARCHIVE.md`.

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

### The checkpoint contract

At every resting point, whether or not you yield (full text in `docs/agent/SESSION-DISCIPLINE.md`):
1. **Account for every shell, monitor and background task** -- kill finished ones by PID, name any left running and why, or say "nothing running".
2. **Keep the handoff already written**, not offered -- so the user can compact at any moment. If the state is not compactable, name the one blocker.
3. **Say what is next** -- one `Next` action and one `Ahead` line to the arc's close.

### The run journal

`docs/JOURNAL.md`: append one entry per autonomous run, newest first, as the run goes -- the reasoning, the wrong turns and what caught them, evidence (hash, number, file:line) on every claim, exactly what "fixed" covers, and which decisions the user made. Not a changelog, not a status doc.

### Phase status docs

`docs/phaseN-status.md` gets a row per landed chunk; the section template is in `docs/agent/DOC-DISCIPLINE.md`.

---

## Reference documentation (per chunk; non-negotiable)

- **The technical reference is the vault** (`vault/`, in-tree on `main`); `docs/reference/` is frozen legacy. Run `go -C vault/meta/quaestor run . owner <changed paths> --root "$(pwd)"`: update the owning dossier, or author a new one when none exists, **co-staged in the same commit as the code**. The commit-msg `dossier-gate` hook enforces it; `No-dossier-change: <why>` is the escape for a fold that genuinely belongs to a later chunk.
- **Use quaestor, never raw grep/edit, for vault operations.**
- **The Operator's Manual** (`docs/manual/`) is updated for every user-visible change, written to `docs/thylacine-operators-manual-writing-guide.md` and `docs/MANUAL-DESIGN.md` (both binding).
- Docs are code: missing docs are reverted with their code. Full rules: `docs/agent/DOC-DISCIPLINE.md`.

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

**Naming.** Where a generic name would do, look for a fitting thylacine-themed one; **propose, never unilaterally rename** load-bearing identifiers (tooling ABI strings, documented public names, Stratum-aligned surfaces). Candidates and precedent: `docs/agent/NAMING.md`.

---

## Build + test commands

**A BARE `tools/build.sh` IS NOT THE GATE IMAGE.** With no flags it builds the Halcyon-default image (`HALCYON_SESSION=y`), where login runs the tiled environment instead of `ut`. The gate fleet (`tools/test-interactive.sh`, anything asserting on a post-login shell) needs `tools/build.sh --config ci`.

```bash
tools/build.sh kernel                    # default (Halcyon) image
tools/build.sh kernel --config ci        # the gate image
tools/test.sh                            # suite against a fresh QEMU VM
tools/ci-smp-gate.sh                     # SMP soundness gate (multi-boot; single boots lie)
tools/test-interactive.sh [scenario]     # interactive E2E; refuses if a VM from this tree is running
tools/test-fault.sh                      # hardening witnesses fire
tools/verify-kaslr.sh                    # I-16 runtime witness
tools/check-v80-floor.py                 # ARMv8.0 floor; make test-a72 for the LSE-blind gap
```

Every other gate, and what each one proves: `docs/agent/GATES.md`.

**Boot banner and `EXTINCTION:` strings are tooling ABI**: rewording one is a format break (escalate). Find their consumers with `quaestor owner`; detail in `docs/agent/BOOT-BANNER.md`.

**Hosts.** thyla-pi (`ssh thyla-pi`) is a permanent ARM64/KVM/V3D box: one QEMU at a time, and `pool.img` + `ramfs.cpio` ship together or not at all -- read `docs/agent/THYLA-PI.md` first. Stratum host sanitizers run on a disposable GCP VM, never locally.

---

## Neighbouring trees and tracks

- **Stratum is in scope**: fix Stratum-side bugs directly in `~/projects/stratum/v2` (branch `thylacine-pouch-arm`) -- ASCII commits, no force-push, the user pushes, `third_party/` stays pristine; Stratum format/ABI breaks escalate. Integration contract: `docs/agent/STRATUM-COORDINATION.md`.
- **Native vs ported**: a program authored in Thylacine uses native libthyla-rs; ported foreign code uses Pouch; first-party `std` Rust on Pouch is sanctioned for new programs. Decision rule: `docs/agent/NATIVE-VS-PORTED.md`.
- **The aux track** works `../thylacine-aux` (read its branch off the worktree). The shared surfaces are `kernel/`, `tools/`, `docs/reference/`; coordinate via yip. Host contention can explain wall-clock only, never a wrong value -- announce the resource and the uncertainty, not a duration. Detail: `docs/agent/AUX-TRACK.md`.
- **Ship fallback**: v1.0-rc.1 (Phase 7) is the shippable fallback; Halcyon is last and may slip to v1.1. Take no Halcyon-blocking risks in Phase 7.

---

## Session-state files

- Built artifacts go to `build/`; not in git. `.gitignore` excludes.
- Snapshots in `build/snapshots/`; not in git.
- TLA+ tools at `/tmp/tla2tools.jar`. Install instructions in `docs/agent/SPEC-POLICY.md`.
- 9P host share at `./share/` (created on first `tools/run-vm.sh`); not in git.

---

## Context economy and compaction

**The compaction line is 400k.** Below it, run through checkpoints: land a chunk, give the three-line checkpoint, open the next one in the same run. At the `ctx-hook` CHECKPOINT WINDOW line (400k; the thresholds live in `.claude/ctx-thresholds`, read by both `~/.claude/ctx-hook.sh` and `tools/stop-hook.sh`), carry to a clean boundary, make your last message the resume note (what is in flight and what must NOT be redone), then run `tools/thyla-selfcompact.sh "<reason>"`. Past 500k: wind down, start no new arc. 880k: commit, hand off, yield. Invoke the self-compact only on the real signal -- a queued `/compact` cannot be cancelled by you, only by the operator. A belay (HEAD unmoved across two self-compactions) is a stop: hand back. If no CONTEXT line has ever appeared by about two thirds of the budget, treat the hook as absent and use judgement. Rationale: `docs/agent/SESSION-DISCIPLINE.md`.

**Every call re-reads the whole context**, so the cost of a round trip grows with the context. Therefore:
- **Batch.** Memory stamps, status rows, index lines and journal fills go in ONE script, not one call each. Put independent calls in one message.
- **Never poll a background job** -- wait for its completion notification.
- **Load deferred tools once**, in one `ToolSearch select:` for the whole set, right after a compaction.
- **Cap output.** Count before you dump (`grep -c`, `wc`), read windows not whole files; never `cat` a directory's headers. A result that spills to a file is a sign the command was wrong -- narrow it, do not Read the spill whole.
- **Compact before researching the next chunk**, not after: when a chunk closes within ~100k of the line, compact first.
- **Delegate bulk writing** (dossier passes, closed lists, journal entries) to a subagent when the main context is large; keep its result to a line.
- **Do not leave a session idle at high context** -- self-compact or `/clear` from the handoff first; a cold cache re-writes the whole context at 2x.

**Stopping vs finishing.** The full summary (the `hand-back` skill) is for STOPPING: the compaction line, an escalation item, a genuine block, or the user asks. At a checkpoint you run through, three lines -- what landed, what is running, what is next -- then the next tool call. `tools/stop-hook.sh` asks once if you stop mid-budget without one of those reasons.

---

## When in doubt

1. Re-read VISION + ARCH + ROADMAP for the relevant section.
2. Check if a TLA+ spec covers it; if so, the spec wins.
3. Check the audit-trigger table; if the change touches a trigger surface, audit before merge.
4. **If you are chasing an elusive bug** — a corruption-class symptom, inconsistent repro, a cross-layer fault, or a bug a prior session "resolved" that recurred — **read `docs/DEBUGGING-PLAYBOOK.md` BEFORE theorizing** (the `elusive-bug-hunt` skill auto-surfaces the condensed method). Ground truth over theory; suspect masking-bug stacks; distrust hollow "AUDITED CLEAN" closes.
5. If still uncertain, ask the user. Confirming is cheap; getting it wrong is expensive.

The thylacine is real. So is this.
