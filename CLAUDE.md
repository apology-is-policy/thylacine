# CLAUDE.md

Operating notes for Claude Code instances working on Thylacine OS.

This document is binding scripture for every implementation session. It encodes the operational framework, the discipline expected, the build commands, the audit-trigger surfaces, and the invariants that must hold. Read this before doing anything else; refer back to it often.

---

## Mission

Thylacine is a Plan 9-heritage operating system targeting ARM64, designed to be a real OS — not a toy, not a research prototype. It is built on three convictions: Plan 9's ideas were correct; the shell is sufficient as a UI; the filesystem is the OS. The fourth, methodological conviction — the one that binds the project at every level — is that **complexity is permitted only where it is verified**: maximum implementation rigor, formal specifications for every load-bearing invariant, adversarial audit before every invariant-bearing merge, and no shortcut implementations even when "we'll fix it later" would save weeks.

See `docs/VISION.md` for the full mission statement.

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
| `CLAUDE.md` (this) | Operational framework for Claude Code sessions. |

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
| Exception entry | `arch/arm64/start.S`, `arch/arm64/exception.c`, `arch/arm64/vectors.S` | Every syscall / IRQ / fault path. Privilege boundary. |
| Page fault + COW + W^X | `arch/arm64/fault.c`, `mm/vm.c`, `mm/wxe.c` | Lifetime, demand-page, COW, W^X invariant (I-12) |
| Allocator | `mm/buddy.c`, `mm/slub.c`, `mm/magazines.c` | Allocation correctness, lock-free invariants |
| Scheduler | `kernel/sched.c`, `kernel/eevdf.c`, `arch/arm64/context.c`, `arch/arm64/ipi.c` | EEVDF correctness, SMP, wakeup atomicity (I-8, I-9, I-17, I-18) |
| Namespace | `kernel/namespace.c` | Cycle-freedom (I-3), isolation (I-1) |
| Handle table | `kernel/handle.c` | Rights monotonicity (I-2, I-6), transfer-via-9P (I-4), hardware-handle non-transferability (I-5) |
| VMO | `kernel/vmo.c`, `mm/vmo_pages.c` | Refcount, mapping lifecycle (I-7) |
| 9P client | `kernel/9p_client.c`, `kernel/9p_session.c`, `kernel/9p_attach.c` | Wire protocol, fid lifecycle (I-11), tag uniqueness (I-10), pipelining |
| Notes / signals | `kernel/notes.c`, `compat/signals.c` | Delivery ordering (I-19), async safety |
| Capability checks | All syscall entry points | Privilege correctness |
| KASLR / ASLR | `arch/arm64/start.S`, `arch/arm64/kaslr.c` | Entropy quality, layout correctness (I-16) |
| ELF loader | `kernel/elf.c` | RWX rejection, relocation correctness |
| `mprotect` / `mmap` | `mm/vm.c` syscall handlers | W^X enforcement at runtime (I-12) |
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
| I-2 | Capability set monotonically reduces (`rfork` only reduces) | Syscall gate | `handles.tla` |
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
- Cross-phase scope pivots — if what you're about to do pulls work from Phase N+1 into Phase N, confirm.
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

`boot_main()` in `kernel/main.c` must print, as its final act before entering the init process:

```
Thylacine vX.Y-dev booting...
  arch: arm64
  cpus: N
  mem:  XXXX MiB
  dtb:  0xADDR
  hardening: KASLR+ASLR+W^X+CFI+PAC+MTE+BTI+LSE+canaries
  kernel base: 0xADDR (KASLR offset 0xADDR)
Thylacine boot OK
```

A kernel **extinction** (ELE — Extinction Level Event; the thematic name for kernel panic) prints `EXTINCTION: <message>` as a recognizable prefix. Use `extinction(msg)` or `extinction_with_addr(msg, addr)` from `kernel/extinction.c`; `ASSERT_OR_DIE(expr, msg)` for assert-style checks. These two strings (boot banner success line + EXTINCTION prefix) are part of the kernel ABI with the development tooling. They do not change without updating `tools/run-vm.sh`, `tools/test.sh`, `tools/agent-protocol.md`, and this document in the same commit.

---

## The crossover at Utopia

Per `TOOLING.md §7.1`:

**Before Utopia (Phases 1-4): human-primary, agent-assisted.** Kernel skeleton, process model, device layer require close human oversight. Slow feedback loop (boot, observe, reboot); failure modes are catastrophic (kernel panic). The agent implements, runs phase exit criteria, reports clearly. Never proceeds past a panic without human review.

**After Utopia (Phases 5-8): agent-primary, human-directed.** Once Utopia boots, the agent operates with much greater autonomy. Implements a subsystem, deploys via 9P share, runs tests, iterates. Runs audit rounds. Human reviews diffs and sets direction.

This means: in Phases 1-4, ask before significant decisions; in Phases 5+, proceed and report.

---

## Stratum coordination

Stratum is feature-complete on Phases 1-7 of its own roadmap. Phase 8 (POSIX surface — inodes, dirents, xattrs, ACLs, modern POSIX) is in progress. Phase 9 (9P server + Stratum extensions: `Tbind`, `Tunbind`, `Tpin`, `Tunpin`, `Tsync`, `Treflink`, `Tfallocate`) is the integration target for **Thylacine Phase 4**.

Coordination:

- Thylacine Phases 1-3 proceed in parallel with Stratum's Phase 8-9 work — no dependency.
- Thylacine Phase 4 entry depends on Stratum Phase 9 9P server availability.
- Any Stratum 9P extension that emerges late in Stratum's Phase 9 is added to Thylacine's 9P client at Phase 4 (or at v1.1 if late enough).
- Stratum's audit-trigger surfaces are *Stratum's* responsibility; Thylacine's audit covers the OS-side integration.

Stratum's repo is at `~/projects/stratum/`; reference its `docs/` for protocol details when implementing the 9P client.

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
4. If still uncertain, ask the user. Confirming is cheap; getting it wrong is expensive.

The thylacine is real. So is this.
