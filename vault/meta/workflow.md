# The Vault Workflow — retargeting the operational loop

**Status: ACCEPTED (user signoff 2026-07-31) — BINDING as of commit 0 on
`vault/bootstrap`, companion to the schema. This document rewrites every
durable-store instruction in the current operating framework (CLAUDE.md + the
memory system + the doc disciplines) to target the vault, and records the
friction points the drafting surfaced (§6). The two schema amendments it
forced (§6.1, §6.2) are FOLDED into the schema as of commit 0; §6.1/§6.2
below are their historical record.**

---

## 1. The target operating loop (a session, vault edition)

1. **Prime.** Task names a surface → open its `sub-*` dossier → follow
   `guarded-by` / `hazards` / open `seam-*` links → skim the generated
   Provenance tail (last N `chg-*`) for recent motion. One hop, no grepping
   across four stores.
2. **Work.** Unchanged: implement, test, self-audit. The self-audit checklist
   is `wkf-self-audit`; the dossier's Prosecution section is the
   surface-specific half.
3. **Audit** (audit-bearing chunks). Spawn the prosecutor with the *generated*
   `view-closed-<surface>` as the do-not-re-report preamble; reviewer emits
   findings as `fnd-*`-shaped blocks (§4.2); author lands them with
   dispositions.
4. **Close.** One commit carries: the fixes + regression tests + the dossier
   edits + the `chg-*` note (edges: `touched`/`closed`/`opened`/
   `mirrors-checked`) + any `fnd-*`/`adt-*`/`dec-*`/`seam-*` notes + linter-
   regenerated views. The commit message stays the *prose* home (§6.2).
5. **Checkpoint.** Unchanged contract (running/handoff/next/ahead). Handoff
   files stay harness-side (§3.3); the vault's `dashboard` regenerates at
   commit.

## 2. CLAUDE.md disposition — constitution stays, content moves

The classification rule: **CLAUDE.md keeps what must bind every turn without
retrieval; the vault holds what is retrieved when relevant.** Auto-load is a
privilege the vault cannot replicate (§6.3), so the split is by binding mode,
not by topic.

| CLAUDE.md section | Disposition |
|---|---|
| Mission; whole-system stewardship; forbidden-disownment; a-surfaced-problem-preempts | **Stays** (per-turn constitution) |
| Scripture table | Stays; rows updated (REFERENCE.md row → vault; + schema/workflow rows) |
| Spec-first policy + suspension | Policy paragraph **stays**; the six per-surface re-enablement histories → `dec-*` notes + `spec-*` notes |
| Research-prior-art; design→scripture-commit patterns | One-paragraph rule stays; full procedure → `wkf-design-fork` |
| **Audit-trigger table** | The per-row prose → each dossier's **Prosecution** section (single home). CLAUDE.md keeps a one-line-per-surface trigger *list*: surface → `[[sub-*]]` link + "read before touching". `view-audit-triggers` renders the full table |
| Reviewer-agent policy (highest Fable, fallback Opus, MODEL start/end) | **Stays** (one paragraph); procedure + prompt template → `wkf-audit-round` |
| §28 invariants table | → `view-invariants` (generated). CLAUDE.md keeps the authority-chain line only |
| Regression-testing rules; chunk-completeness; autonomy/escalation; git + ASCII discipline; checkpoint contract; compaction/effort guidance; operational summary shape; when-in-doubt | **Stays** (constitution / harness behavior) |
| Implementation patterns (idempotency, static-asserts, crash-injection) | → `wkf-*` / `haz-*` notes; one-line pointers stay |
| Self-audit; audit-in-flight; dirty-close; close anatomy; deferred-finding discipline | → `wkf-audit-round` + `wkf-self-audit`. Note: the deferred-finding rule ("silent drops forbidden") becomes **structural** — a `fnd` with `status: deferred` must carry a `seam-*` link or the linter fails the commit (§5) |
| Memory + session continuity | **Rewritten** → the router (§3) |
| Reference-documentation discipline | **Replaced** by the per-PR loop (§4.1) |
| Phase status docs | → `arc-*` notes + `view-roadmap`; trip-hazards → `haz-*`/`seam-*`; build commands → `wkf-build` (the short command block stays in CLAUDE.md — per-turn useful) |
| Boot-banner ABI; Stratum coordination; native-vs-ported; aux track; ship-and-fallback | → `abi-boot-banner`, `system/stratum/`, `dec-*`, dossiers; short pointers stay for the coordination-critical ones (aux boundary stays inline) |
| Thematic naming | Rule stays; per-name rationale → `gls-*` |

Net effect: CLAUDE.md shrinks to roughly a third — constitution, coordination,
commands, and the trigger list — and stops carrying any fact the vault owns.

## 3. The memory router — where a durable fact goes now

The harness memory directive (platform-injected) keeps functioning; the vault
does not fight it. The split: **harness memory = session continuity + the
auto-loaded/recall index; vault = durable knowledge.**

### 3.1 Routing table

| Today's habit | Vault edition |
|---|---|
| `feedback_*.md` (binding lesson) | `haz-*` (failure class) or `wkf-*` (procedure) note in the vault **+ a thin memory stub**: one-liner + `[[wikilink]]` + vault path. The stub is mandatory — recall only surfaces memory-dir files (§6.4) |
| `bug_*.md` | `fnd-*` (+ `lin-*` membership, + `seam-*` if open); memory stub only while the hunt is live, deleted at close |
| `audit_*_closed_list.md` | Retired: `fnd-*` notes are the record; `view-closed-<surface>` is the generated preamble. (Also fixes a latent defect: today's closed lists are **untracked** at the repo root — unversioned, single-machine. The vault versions them) |
| `project_*.md` (arc state) | `arc-*` note + `dashboard`; memory stub with the pickup pointer while active |
| `reference_*.md` (GCP builders, host quirks) | `system/substrate/` dossiers |
| `project_active.md`, `project_next_session.md` | **Unchanged, harness-side** (R10: working state never enters the vault) |
| MEMORY.md | Unchanged role (auto-loaded index, 24KB recall guard, M/A owner tags); entries increasingly one-liner-plus-vault-pointer, which *relieves* its size pressure |

### 3.2 The bug rule, restated

"Encounter a bug → enqueue a bug" becomes: TaskCreate entry (unchanged — the
ephemeral work queue) **+** `seam-*` or draft `fnd-*` in the vault (the durable
record), cross-linked via `tracker:`. Tasks schedule; seams/findings remember.
A task that survives its session without a vault node is the new
walked-past-in-slow-motion (§6.8).

### 3.3 What memory keeps forever

Session scratch, the auto-loaded index, live-arc pickup stubs, and thin recall
stubs for the lessons class. Everything else migrates.

## 4. Workflow rewrites

### 4.1 The per-PR loop (replaces "Reference documentation discipline")

A chunk PR ships, in one commit:

1. Code + tests.
2. **Dossier edits** — present-tense only (R2); the Prosecution section
   updated if the attack surface changed.
3. **Registry touches** — new lock → `lock-*`; new ABI/field → `abi-*` (+
   `mirrors` current); new invariant → `inv-*` + ARCH §28 (§6.1); new term →
   `gls-*`.
4. **The `chg-*` note** — edges mandatory (`touched`, `established`, `closed`,
   `opened`, `mirrors-checked`), body = one-paragraph synthesis; the commit
   message remains the full prose home (§6.2).
5. **Views regenerate** (linter; a stale view fails the commit).

Missing vault updates are reverted with their code — the existing rule,
retargeted verbatim.

### 4.2 The audit round (delta to today's procedure)

- **Preamble**: paste the generated `view-closed-<surface>` instead of the
  hand-appended memory file.
- **Prompt template delta**: the reviewer's per-finding report format becomes
  an `fnd-*`-shaped block (YAML fields + Prosecution body verbatim). The
  reviewer READS dossiers as part of scope (they now carry the invariant
  transclusions + Prosecution list the prompt used to inline).
- **Landing**: the author lands `fnd-*` notes (dispositions are authorial,
  never the reviewer's), the `adt-*` round note (scope, MODEL start/end,
  verdict, counts), and flips closure fields only per schema §5.3.
- **Draft-until-close**: during a round, `fnd-*`/`adt-*` files are working
  drafts; Record immutability starts at the close commit (§6.7).
- Dirty-close recursion, self-audit-in-parallel, severity bars: unchanged.

### 4.3 Session-start priming (new, replaces ad-hoc re-reading)

Read order for a task on surface X: CLAUDE.md (auto) → MEMORY.md (auto) →
`sub-X` dossier → its `hazards` + open `seams` → `view-closed-X` if auditing.
Enforcement candidate: a PreToolUse hook that warns when an `audit: hard`
code path is edited in a session that never Read its dossier (§6.3).

## 5. Linter additions (beyond schema §8)

- `fnd` with `status: deferred` and no `seam-*` link → fail (no silent drops).
- `chg` whose `touched` names an `audit: hard` dossier but whose commit
  carries no dossier diff and no explicit `no-dossier-change: <why>` line →
  warn.
- A memory-router escapee heuristic: none — routing is habit; the mitigations
  are the stub convention and review (§6.5 honesty).

## 6. Friction points surfaced (the answer to "will this surface friction?")

Yes — twelve, two of which force schema amendments.

### 6.1 The vault boundary vs. out-of-vault scripture (SCHEMA AMENDMENT)

The schema declared `vault/` self-sufficient because *code* citations are
symbolic. But ARCH §28, the design docs, and `specs/` are *documents* the
vault genuinely wants to wikilink and transclude — and Obsidian can only link
within its root. If the Obsidian root is `vault/`, `inv-*` notes cannot
transclude into/from `docs/ARCHITECTURE.md`, and the §28 statement-home
question (does the invariant text live in ARCH or in `inv-*`?) has no clean
answer. **Amendment: the Obsidian root is the repo root** (`.obsidian/`
gitignored; `build/` etc. excluded in-app); `vault/` remains the notes tree.
Then: `inv-*` becomes the statement's single home, and ARCH §28 keeps the
table with one-line summaries + links — scripture keeps the *set*, the vault
keeps the *text*. Same pattern for §25.4: the table survives as the trigger
registry, its prose lives in dossiers.

### 6.2 `chg` notes vs. commit messages (SCHEMA AMENDMENT)

Drafted as specified, every rich `chg` note duplicates its commit message's
What/Why/Alternatives — an R1 violation built into the schema itself, and a
real per-PR tax. **Amendment: the `chg` note's unique value is its EDGES and
its linkability; the commit message remains the prose home.** `chg` body =
one-paragraph synthesis + the SHA links; `depth: rich` is reserved for
chunks whose reasoning genuinely exceeds a commit message (multi-commit
sagas, diagrams). This also matches the original design intent: "reasonable
level of detail, pointing to the commits for full detail."

### 6.3 Auto-load vs. instructed-load

CLAUDE.md binds without retrieval; vault notes bind only if read. Moving the
trigger-table prose to dossiers converts a structural guarantee into a habit.
**Resolved posture (three tiers — encoded in the `phase-zero-vault` fork's
Enforcement section): contract for judgment, linter for structure, session
hooks for attention.** The git pre-commit linter is MANDATORY (deterministic,
zero-false-positive: Record immutability, dangling links, deferred-without-
seam, mirror sweeps, stale views — commit is the enforcement point). Session
hooks are installed but ADVISORY, never blocking (they lack semantic context
and would false-positive on legitimate mechanical edits): a PostToolUse-Read
hook logs which dossiers the session read; a PreToolUse-Edit hook warns when
an `audit: hard` code path is edited without its dossier in the read log; an
optional Stop hook reminds about the missing `chg-*` at checkpoint. The
trigger *list* stays auto-loaded in CLAUDE.md with "read `[[sub-*]]` before
touching" as a binding one-liner. Residual risk (a session that ignores the
warning) is caught at commit by the linter's dossier-diff check (§5).

### 6.4 Harness recall cannot be replaced

The memory system's recall surfaces memory-dir files only. A lesson moved
wholesale to the vault silently exits recall. Resolution: the lessons class
is **stub-and-point, never move-and-delete** (§3.1). MEMORY.md's pinned
feedback section survives as one-liners.

### 6.5 Two live systems during migration

Until the vault merges to main and per-store cutovers land, the main track
runs the old workflow. Double-entry is the failure mode. Resolution: a
**store-liveness table** in the migration doc — each store (closed lists,
reference docs, trigger table, memory routing) cuts over in its own commit,
old store frozen-with-stub at that moment, never both live. Order: closed
lists (after the pilot proves `fnd-*`) → reference docs (per swept subsystem)
→ trigger table (at sweep completion) → memory routing (last, after merge).

### 6.6 Main-track drift under the sweep

The sweep runs on `vault/bootstrap` while main advances; dossiers written
against the merge-base go stale before merge. Resolution: rebase cadence per
sweep batch + a merge-gate pass that diffs `git log merge-base..main` per
touched code path and re-verifies affected dossiers. (Timing rule from the
arc memory stands: merge at a clean main-track arc boundary.)

### 6.7 Record immutability vs. audit iteration

Findings change severity during verification; rounds converge over days.
Resolution: `fnd-*`/`adt-*` are drafts until the close commit lands them;
immutability (schema R3) attaches at landing, matching git's own semantics.

### 6.8 Two trackers

TaskCreate and `seam-*` overlap. Resolution: tasks = ephemeral scheduling,
seams = durable debt, `tracker:` cross-links both ways, and the bug rule
(§3.2) requires the durable half for anything that outlives its session.

### 6.9 Who writes findings

The reviewer reports; the author records. Keeping dispositions authorial
preserves the prosecute-not-defend separation — the reviewer never writes its
own `status: withdrawn`.

### 6.10 View freshness vs. uncommitted state

A prosecutor spawned mid-cycle needs findings not yet committed. The linter
renders views from the working tree, not git — sufficient.

### 6.11 The aux-track writer boundary

Aux owns `usr/apps/**`; in the vault it owns the matching dossier subtree
(`system/userspace/ports/apps/` or similar) and nothing else. Its
DOC-GAP-REPORT findings become `seam-*`/`haz-*` notes filed by MAIN at merge,
same as today's fold-in rule. MEMORY.md owner tags unchanged.

### 6.12 The per-PR overhead, honestly accounted

New cost per chunk: the `chg-*` note (edges + a paragraph). Retired cost per
chunk: the status-doc row, the REFERENCE.md snapshot refresh, the closed-list
append, the memory-file note, and the "authoritative copy" synchronization
across CLAUDE.md/ARCH rows. Net: approximately neutral on a normal chunk,
strictly cheaper on an audit-bearing one — and several prose disciplines
(no-silent-drops, mirror sweeps, stale tables) convert from habits into
linter checks, which is where the retarget genuinely pays.

## 7. Cutover summary

1. Schema + this doc land as scripture (commit 0, with the §6.1/§6.2
   amendments folded into vault/meta/schema.md).
2. Pilot (9P client) exercises the full §4 loop once, including one real
   audit round run vault-style.
3. Sweep + registry passes (per schema §10), store-liveness cutovers per
   §6.5.
4. CLAUDE.md rewrite lands as its own reviewed commit (the §2 table executed).
5. Memory routing flips last; stubs backfilled for the lessons class.
