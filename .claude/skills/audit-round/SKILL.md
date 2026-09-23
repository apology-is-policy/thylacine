---
name: audit-round
description: Use when a change touches an audit-trigger surface (docs/AUDIT-TRIGGERS.md) and needs its adversarial soundness audit, or when running, triaging, re-running or closing an audit round -- spawning the holotype-reviewer prosecutor, the prompt template, self-audit before and during the round, dirty-close re-audit, the audit-close commit anatomy, and deferred findings.
---

# Audit rounds

> Moved verbatim from `CLAUDE.md` on 2026-09-23 (the CLAUDE.md trim). CLAUDE.md keeps only the trigger rule.
> Section headings keep their original levels.

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
