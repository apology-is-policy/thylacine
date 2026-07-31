# The Vault Schema

**Status: ACCEPTED (user signoff 2026-07-31) — BINDING as of commit 0 on
`vault/bootstrap`. The two amendments the workflow drafting forced
(vault/meta/workflow.md §6.1 the-repo-root-is-the-Obsidian-root; §6.2
change-notes-carry-edges-not-prose) are folded in below (§4, §5.1 inv, §5.2
chg). The vault this defines supersedes `docs/REFERENCE.md` +
`docs/reference/NN-*.md` per the migration plan (§10); the authority chain is
re-declared: spec > vault > code > user-facing docs.**

Working name: "the vault" (Obsidian's own term). Thematic candidates still
held for a naming vote, per the naming discipline: **Syntype** (the type
series — the full set of specimens beyond the holotype that together document
the taxon; sits naturally beside the existing `holotype-reviewer`) or
**Midden** (the stratified accumulation deposit read to reconstruct a
system's history). Neither is baked into any path; a later rename is an
`aliases:` exercise, not a migration.

---

## 1. Purpose

One structurally-clean knowledge graph that is simultaneously:

1. **The technical reference** (replacing `docs/reference/`) — the as-is truth
   about every subsystem, deep enough that a maintainer needs no re-derivation.
2. **The priming surface for agents** — a task names a surface; the surface's
   dossier + its invariants, hazards, and open seams are one hop away.
3. **The history and reasoning record** — for any node in the truth graph, its
   incoming change-links, walked chronologically, explain how and why it became
   what it is, with commits carrying full detail.
4. **The generator of every formerly hand-maintained table** — the audit-trigger
   table, do-not-re-report preambles, the §28 invariant matrix, the seam
   registry, the roadmap. These become materialized views over note fields.

## 2. The constitution (editorial rules; binding)

R1. **One home per fact.** Everywhere else is a `[[link]]` or a transclusion
    (`![[note#Section]]`). The tell: if you are about to restate, you are about
    to fork. (This retires the "the AUTHORITATIVE copy is the ARCH §25.4 row"
    pattern — the dossier IS the copy.)

R2. **The Present plane is present-tense only.** No archaeology in truth notes.
    Provenance is expressed as links (incoming `touched` edges), never as
    narrative ("R1 found F1 [P1]…" prose is Record-plane content).

R3. **The Record plane is append-only.** A Record note's body freezes when it
    lands. Corrections are new notes with `supersedes`. The only permitted
    later edits are the designated closure fields (§5.3), and only alongside a
    change note that links them.

R4. **Citations by symbol and test name, not file:line.** `file:line` rots;
    symbols and test names survive refactors and stay greppable. Line numbers
    are permitted only in Record notes, where they are frozen-in-time correct.

R5. **Every gate/verification claim carries `blind-to`.** A gate note must
    state what it structurally cannot catch (the
    assertion-satisfiable-by-a-broken-system lesson). A verification claim
    without a stated blind spot is incomplete.

R6. **Every ABI note enumerates its `mirrors`.** A change touching an ABI must
    check off the full mirror set in its change note (`mirrors-checked`). This
    is the `t_stat` lesson made structural: a per-mirror assert verifies only
    that mirror; only the enumerated set verifies the family.

R7. **Skeletal is honest; fake-rich is the sin.** Backfilled Record notes may
    be `depth: skeletal` (dates, one-liners, SHAs, links — an index into git).
    A skeletal note presenting as rich narrative is a schema violation.

R8. **No Obsidian-only load-bearing content.** Wikilinks + YAML frontmatter
    only (both plain text). Views are *materialized* into committed markdown by
    the linter/renderer (§8), so grep and non-Obsidian agents see everything.
    Dataview/graph/canvas are conveniences, never the sole home of a fact.

R9. **The auto-loaded layer stays thin.** `CLAUDE.md` + `MEMORY.md` carry
    mission, binding disciplines, and pointer one-liners ("audit-bearing —
    read `[[sub-kernel-ninep-client]]` before touching"). They never carry
    vault content inline.

R10. **Working state stays out.** Session scratch (`project_next_session`,
    handoff pointers) never enters the vault. The single `dashboard` note is
    generated from arc/seam status, not hand-written.

## 3. Identity

- **ID = filename** (sans `.md`), kebab-case, type-prefixed, stable forever.
  Renames happen via `aliases`, never by changing the ID.
- Record-plane IDs embed the date for natural ordering.

| Prefix | Type | Example |
|---|---|---|
| `moc-` | map of content / hub | `moc-kernel-memory` |
| `sub-` | subsystem dossier | `sub-kernel-ninep-client` |
| `inv-` | invariant | `inv-i9` |
| `spec-` | TLA+ spec module | `spec-9p-client` |
| `abi-` | boundary surface | `abi-t-stat` |
| `lock-` | lock | `lock-9p-client-c-lock` |
| `lin-` | bug lineage | `lin-death-path` |
| `haz-` | hazard class | `haz-single-waiter-rendez` |
| `gls-` | glossary term | `gls-spoor` |
| `gate-` | test/verification asset | `gate-smp` |
| `seam-` | open debt / deferred item | `seam-350-async-eagain` |
| `msr-` | measurement series | `msr-gofmt-warm` |
| `arc-` | development arc | `arc-weft` |
| `chg-` | change (landed chunk) | `chg-2026-07-13-375-spill` |
| `adt-` | audit round | `adt-2026-07-13-375-r1` |
| `fnd-` | finding | `fnd-841-r2-f6` |
| `dec-` | decision (ADR) | `dec-2026-06-20-weft-delivery` |
| `wkf-` | workflow / procedure | `wkf-audit-round` |
| `view-` | materialized view | `view-audit-triggers` |

Common frontmatter on every note:

```yaml
id: <string == filename>
type: <one of the table above>
title: <human title>
aliases: []          # thematic + legacy names; migration adds old doc paths
created: <YYYY-MM-DD>
updated: <YYYY-MM-DD>   # Present plane only; forbidden on Record plane
```

## 4. Planes and layout

Two mutability regimes. **Present** (edited in place, present-tense):
taxonomy + dossiers, invariants, specs, ABIs, locks, lineages, hazards,
glossary, gates, seams, measurements, workflows, MOCs, views. **Record**
(append-only): arcs, changes, audit rounds, findings, decisions.

```
vault/
  home.md                     # the single entry MOC
  dashboard.md                # generated: arc status + open-seam counts
  system/                     # taxonomy spine; dossiers live at their node
    kernel/{boot,memory,execution,entry,namespace,ninep,ipc-wake,
            devices,async,console-gfx,security,introspection}/
    userspace/{runtime,boot-chain,shell-tui,services,ports}/
    boundary/{syscall-abi,ninep-wire,registries,exec-contract,pouch-seam}/
    substrate/                # QEMU TCG/HVF, GIC, bare-metal, builders, harness
    stratum/                  # sibling-system integration surface
  invariants/  specs/  locks/  lineages/  hazards/  glossary/  gates/
  seams/  measurements/
  record/
    arcs/  changes/  audits/  findings/  decisions/
  workflows/
  views/                      # generated; committed; greppable
  meta/                       # this schema, the linter, templates
```

**The Obsidian root is the REPO root** (amendment §6.1, folded): the vault
must wikilink and transclude out-of-vault scripture — ARCH §28, the design
docs — and Obsidian only links within its root. `.obsidian/` is gitignored;
`build/` and friends are excluded in-app; `vault/` is the notes tree. Code
citations remain symbolic (R4), so no file-links into code exist either way.
Consequence for invariants: the `inv-*` **Statement is the single home of the
invariant text**; ARCH §28 keeps the table of numbers + one-liners + links —
scripture owns the *set*, the vault owns the *text*. Same split for §25.4:
the trigger table survives as the registry, its prose lives in dossiers.

## 5. Type catalog

Fields marked ● are required. Every type also carries the common frontmatter.

### 5.1 Present plane — taxonomy

**`moc`** — interior taxonomy node / hub. Fields: `parent`●. Body: scope
paragraph, curated child list, links to the cross-cutting nodes that most
concern this domain. MOCs contain no facts (R1) — only orientation.

**`sub`** (subsystem dossier) — the deep note; the unit of audit scoping and
agent priming. Components nest via `parent` (e.g. `sub-kernel-memory-buddy`
under `moc-kernel-memory`); one type, arbitrary depth.

```yaml
parent: moc-kernel-ninep          # ●
code: [kernel/9p_client.c, kernel/9p_session.c, ...]   # ● paths/globs
audit: hard | light | none        # ● drives view-audit-triggers
guarded-by: [inv-i9, inv-i10, inv-i11]                 # ●
validated-by: [spec-9p-client, gate-smp, ...]          # ●
locks: [lock-9p-client-c-lock, ...]
hazards: [haz-single-waiter-rendez, ...]
abis: [abi-9p-wire-extensions, ...]
design: [docs/STALK-DESIGN.md, ...]   # standing design scripture, if any
```

Body sections, in order (a dossier missing one states why in place):
**Purpose** · **Contract** (public API, symbol-cited) · **Mechanism** ·
**Data structures** · **Concurrency** (locks held, order edges, wait/wake
protocol, sleep/death interaction) · **Invariants enforced** (transclusions of
`![[inv-*#Statement]]` + enforcement-site symbols) · **Error paths** ·
**Performance** · **Prosecution** (what an auditor attacks here — absorbs the
CLAUDE.md/ARCH §25.4 row content; the single home per R1) · **Seams** (links
to open `seam-*`) · **Caveats** · **Provenance** (generated: incoming `touched`
links, newest first — never hand-written narrative, per R2).

**`inv`** — one per §28 invariant. Fields: `number`● (`I-9`),
`guards`● (sub ids), `validated-by`● (spec/gate ids or `prose`),
`strength`● (`spec|test|prose`). Body: **Statement** (the single-homed text —
everything else transcludes it), **Enforcement** (sites by symbol),
**Validation** (what pins it and `blind-to` of that pinning). Editing the
Statement is audit-bearing and requires a linking change note. Per §4/§6.1:
the Statement is the invariant text's SINGLE home — ARCH §28 carries only
the number + a one-liner + the link.

**`spec`** — one per TLA+ module (the `.tla` stays in `specs/`; this is its
dossier). Fields: `models`● (sub ids), `pins`● (inv ids), `cfgs`● (list:
name + `clean|buggy` + what the buggy cfg is a counterexample OF),
`gate`● (when a re-run is mandatory). Body: abstraction boundary (what is
deliberately beneath the model), action↔site map (absorbs `SPEC-TO-CODE.md`
for this module).

**`abi`** — boundary surfaces: syscalls (one note each), wire ops, pinned
structs, registries (errno, note names, qid bits, rights/caps/perm bits),
contracts (exec/auxv, pouch seam). Fields: `kind`●
(`syscall|wire|struct|registry|contract`), `stability`●
(`frozen|append-only|internal`), `pinned-by`● (static_asserts/tests, by
symbol), `mirrors`● (every lockstep copy, by repo + symbol — may be `[]` but
must be present and is lint-checked against R6). Body: layout/semantics ·
**Change protocol** (exactly what a change requires, e.g. "append-only; bump
asserts across all N mirrors in one commit").

**`lock`** — fields: `kind`● (`spin|spin-irqsave|leaf|…`),
`orders-before` (lock ids), `guards`● (what state). Body: acquisition
contexts, sleep-legality, the discipline. `view-lock-order` renders the global
DAG from `orders-before`.

**`lin`** — a named bug lineage (the death path, the 9P-client saga). Fields:
`surfaces`● (sub ids), `members`● (ordered fnd/chg ids; append-mostly). Body:
the saga in brief + the standing lesson it teaches.

**`haz`** — a recurring failure class (absorbs `feedback_*.md` lessons + the
playbook classes). Fields: `applies-to`● (sub ids or `global`),
`instances` (fnd ids). Body: the failure shape · the tell · the countermeasure.

**`gls`** — glossary. Fields: `refers-to` (ids). Body: definition + naming
rationale (the thematic-naming record lives here).

**`gate`** — a test/verification asset (the SMP gate, LS-CI, per-boot probes,
suites, the spec gate). Fields: `proves`●, `blind-to`● (R5 — e.g. LS-CI runs
TCG, so HVF-only behavior is invisible to it), `invocation`●. Body: method,
classification rules, history of what it has and has not caught.

**`seam`** — present-tense debt: v1.x seams, owed follow-ups, deferred
findings. Fields: `status`● (`open|closed`), `surface`● (sub ids),
`opened-by`● (chg/fnd id), `tracker` (task #), `closed-by` (chg id; set at
close). Body: what is owed · what closes it · the risk while open. Seams are
Present-plane (debt is a fact about the system now); a closed seam remains as
record with `status: closed`.

**`msr`** — a measurement series (the go-build oracle, gofmt baselines, HVF
idle). Fields: `metric`●, `unit`●. Body: method + caveats + an append-only
table `| date | value | chg |` where each row links the change that moved it.

**`wkf`** — procedures (audit round, checkpoint contract, gate discipline —
CLAUDE.md's process content relocated). Ordinary Present-plane notes.

### 5.2 Record plane

**`arc`** — fields: `status`● (`active|complete|abandoned`), `design` (doc
links / dec ids), `chunks`● (ordered chg ids; grows while active),
`follow-ons` (seam ids). Body: goal · outcome · close summary. Mutable while
`active`; frozen at close except `follow-ons` closures.

**`chg`** — one per landed chunk/PR; the atom of history.

```yaml
date: 2026-07-13                  # ●
arc: arc-go-build                 # ●
commits: [<sha>, ...]             # ●
touched: [sub-kernel-ninep-client, abi-9p-wire-extensions]   # ● Present ids
established: []                   # Present ids this chg created
closed: [fnd-375-r1-f1]           # findings fixed
opened: [seam-52-tag-slot-leak]   # seams opened
mirrors-checked: []               # ● when `touched` includes an abi with mirrors
depth: rich | skeletal            # ● (R7; backfill is usually skeletal)
supersedes: <chg id>              # corrections only
```

Body (amendment §6.2, folded): the note's unique value is its **edges** and
its linkability — the COMMIT MESSAGE remains the prose home (full
What/Why/Alternatives live there, per the existing commit discipline; a chg
that restates them is an R1 violation). Default body = one-paragraph
synthesis + the SHA links. `depth: rich` is reserved for chunks whose
reasoning genuinely exceeds a commit message (multi-commit sagas, diagrams)
and then carries **What** · **Why** · **Alternatives rejected** ·
**Verification** in full. Immutable once landed (R3; the `commits:`
SHA-fixup is the one designated closure-field exception, §5.3).

**`adt`** — one per audit round. Fields: `date`●, `scope`● (sub ids),
`reviewer`● (`fable|opus|self`), `model-start`●, `model-end`●, `verdict`●
(`clean|dirty`), `counts`● (`{p0,p1,p2,p3}`), `findings`● (fnd ids),
`round-of` (chg/arc id), `prior-round` (adt id). Body: scope summary ·
convergence narrative. A `model-start != model-end` mismatch is recorded here,
per the reviewer discipline.

**`fnd`** — one per finding; the crown jewels. Body prosecution chain freezes;
closure fields may flip later (R3), only via a linking chg.

```yaml
round: adt-2026-06-21-weft7-r1    # ●
severity: P0|P1|P2|P3             # ●
status: fixed|deferred|documented|withdrawn    # ● (closure field)
surface: [sub-kernel-ninep-client]             # ●
threatens: [inv-i9]               # ●
hazard: haz-single-waiter-rendez  # when it instantiates a known class
fixed-by: chg-...                 # closure field
regression: 9p_client.send_backpressure_multi_waiter   # test name, or a seam id if owed
```

Body: **Prosecution** (the chain, frozen verbatim) · **Disposition** (the
rationale; for `withdrawn`, mandatory — silent drops are forbidden).

**`dec`** — one per surfaced fork (ADR). Fields: `date`●, `status`●
(`standing|superseded`), `decided-by`● (`user-vote|autonomous|
research-collapsed`), `affects`● (ids), `superseded-by` (closure field).
Body: **Fork** · **Research** (heritage / SOTA / verified tree facts, per the
research-first pattern) · **Options** (each with consequences) · **The call** ·
**Rationale**. Outcomes are NOT appended here — later chg notes link back, and
backlinks carry the consequence trail.

### 5.3 Closure fields (the only Record-plane mutability)

`fnd.status`, `fnd.fixed-by`, `fnd.regression`, `fnd.seam`, `seam.status`,
`seam.closed-by`, `dec.superseded-by`, `dec.status`, `arc.status` — each may
change only in a commit that also lands the chg note effecting the closure,
and the linter verifies the chg links back (§8). Two mechanical exceptions
that need no linking chg: `chg.commits` (the `*(pending)*` -> SHA hash-fixup
is the chg referencing itself) and an ACTIVE arc's `chunks`/`follow-ons`/
`exit-criteria` growth (an arc freezes at `status: complete|abandoned`).

## 6. Edge vocabulary

All edges are frontmatter fields (queryable) and may be repeated as inline
wikilinks in prose. Inverses are backlinks — never stored redundantly (R1).

| Field | From → To | Meaning |
|---|---|---|
| `parent` | moc/sub → moc | taxonomy spine |
| `guarded-by` / `guards` | sub ↔ inv | invariant binds surface |
| `validated-by` | sub/inv → spec/gate | what pins it |
| `models` / `pins` | spec → sub / inv | abstraction coverage |
| `touched` | chg → Present ids | the provenance edge (drives dossier Provenance) |
| `established` | chg → Present ids | node creation |
| `closed` / `fixed-by` | chg ↔ fnd | finding closure |
| `opened` / `opened-by` | chg/fnd ↔ seam | debt lifecycle |
| `threatens` | fnd → inv | what the bug attacked |
| `hazard` / `instances` | fnd ↔ haz | class membership |
| `decided-by` / `affects` | sub/abi ↔ dec | design provenance |
| `supersedes` / `superseded-by` | chg/dec ↔ chg/dec | correction chain |
| `orders-before` | lock → lock | the lock-order DAG |
| `mirrors` | abi → external symbols | the lockstep set (R6) |
| `surfaces` / `members` | lin ↔ sub, fnd/chg | lineage membership |

## 7. Views (generated, committed, greppable)

Each `view-*.md` note declares its query in frontmatter; the renderer (§8)
materializes results into the note body between markers. Committed output means
grep, GitHub, and non-Obsidian agents all see current tables.

| View | Definition | Replaces |
|---|---|---|
| `view-audit-triggers` | subs where `audit: hard`, with each dossier's Prosecution one-liner | the CLAUDE.md trigger table + ARCH §25.4 twin |
| `view-closed-<surface>` | fnd where `surface` ∋ X and `status != deferred` | `memory/audit_*_closed_list.md` preambles — generated per prosecutor spawn |
| `view-seams` | seam where `status: open`, grouped by surface, with tracker # | scattered "recorded seam" prose; the debt inbox |
| `view-invariants` | inv × (`number`, `guards`, `validated-by`, `strength`) | the §28 condensed table (structurally cannot drift to phantom specs) |
| `view-heatmap` | fnd count per surface per quarter | "most bug-prone lineage" as a measured claim |
| `view-roadmap` | arcs by `status` with chunk progress | phase-status landed-chunk tables |
| `view-lock-order` | topological render of `orders-before` | the prose lock-order rules |
| `view-mirrors` | abi × `mirrors`, flagging chgs that touched an abi without `mirrors-checked` | ad-hoc mirror sweeps |
| `dashboard` | arc status + open-seam counts + last N chgs | `project_active.md`'s status half |

## 8. Enforcement: the linter

`vault/meta/lint.py` (plain YAML+markdown reader, no Obsidian dependency),
wired as a pre-commit hook. Checks:

1. `id` == filename; `type` valid; required fields present per §5; enums valid.
2. No dangling `[[links]]` or unknown ids in edge fields.
3. Record-plane immutability: a modified `record/**` file older than the commit
   fails unless the only changed keys are §5.3 closure fields AND a chg note in
   the same commit links it.
4. `updated` forbidden on Record plane; required-fresh on Present-plane edits.
5. R6: a chg whose `touched` includes an abi with non-empty `mirrors` must
   carry `mirrors-checked` covering the set.
6. Dossier section completeness (§5.1 order, or an explicit waiver line).
7. View bodies match their queries (re-render and diff — a stale committed
   view fails the commit).
8. Citation style: `file:line` patterns outside `record/` are flagged (R4).

The linter is the schema's teeth; without it this document is aspiration. It
ships in the same commit as the schema.

## 9. Worked examples (abbreviated but real)

### 9.1 `invariants/inv-i9.md`

```yaml
---
id: inv-i9
type: inv
title: "I-9 — no wakeup lost between cond-check and sleep"
number: I-9
guards: [sub-kernel-execution-sched, sub-kernel-ipc-wake-poll,
         sub-kernel-ipc-wake-pipe, sub-kernel-devices-cons,
         sub-kernel-ninep-dev9p-poll, sub-kernel-execution-torpor,
         sub-kernel-ninep-client, sub-kernel-async-weft]
validated-by: [spec-scheduler, spec-poll, spec-cons-poll, spec-net-poll,
               spec-weft-readiness, spec-tsleep, spec-death-wake,
               spec-reader-frame]
strength: spec
---
## Statement
No wakeup is lost between a sleeper's condition check and its sleep. This
includes the death-wake generalization (register-then-observe under the
per-Thread `wait_lock`), the terminate-`interrupt` extension, and the
frame-atomic refinement for the elected 9P reader: a mid-frame death defers
its unwind to the next frame boundary.

## Enforcement
`sleep`/`tsleep` (register-then-observe contract) · `torpor_wait` (lock-order
serialized) · `poll_waiter_list` discipline · `reader_recv_frame`
(frame-atomicity) · …

## Validation
Eight spec modules (above); the torpor leg is prose-validated —
**blind-to:** the specs model protocol shape, not memory-ordering of the
lock-free fast paths, which rest on the documented atomics contracts.
```

### 9.2 `record/findings/fnd-841-r2-f6.md`

```yaml
---
id: fnd-841-r2-f6
type: fnd
title: "Reader-role loss strands survivors on hand-off-target death"
round: adt-841-r2
severity: P1
status: fixed
surface: [sub-kernel-ninep-client]
threatens: [inv-i9]
hazard: haz-death-path-wake
fixed-by: chg-841-close
regression: seam-841-mi-harness   # deterministic harness owed; carried as seam
---
## Prosecution
The elected reader hands the role to a waiter whose Proc dies before assuming
it; no re-election path exists on the DIED branch; every survivor with an
in-flight op parks forever on a stream nobody reads.

## Disposition
Fixed: re-hand-off on the DIED path, `be_reader`-gated. Deterministic coverage
requires the cross-Proc multi-in-flight SMP harness — tracked as the linked
seam rather than claimed.
```

### 9.3 `system/boundary/registries/abi-t-stat.md`

```yaml
---
id: abi-t-stat
type: abi
title: "struct t_stat — the native stat ABI (88 bytes)"
kind: struct
stability: append-only
pinned-by: ["_Static_asserts on every field offset (kernel syscall.h)",
            "spoor.stat_native_stamps_devno"]
mirrors:
  - "usr/lib/libt: struct t_stat"
  - "usr/lib/libthyla-rs: Metadata"
  - "pouch patches 0010 + 0019 + 0021: hand-rolled t_stat"
  - "go fork: Stat_t + sameFile"
  - "gopls robustio: FileID{Dev, qid.path}"
---
## Change protocol
Append-only. A field append bumps the kernel asserts AND every mirror above in
one commit (`mirrors-checked` in the chg note). Lesson embedded: a per-mirror
size assert verifies only that mirror — a stale mirror passes its own build
and overflows at runtime. All `.stat_native` impls zero `sizeof(*out)` so pads
cannot leak (I-13).
```

### 9.4 `record/decisions/dec-2026-06-20-weft-delivery.md`

```yaml
---
id: dec-2026-06-20-weft-delivery
type: dec
title: "Weft delivery: grant-is-the-share"
date: 2026-06-20
status: standing
decided-by: user-vote
affects: [sub-kernel-async-weft, inv-i37]
---
## Fork
How does the per-flow shared ring reach the guest Proc?
## Research
Plan 9 (mmap-the-server-file) · Fuchsia IOBuffer RFC-0218 · seL4 coordinator —
convergent: the mapper is the kernel, the capability is the flow fid.
## Options
A. Capability delegation (Burrow handle crosses netd→guest) — rejected: opens
   the deferred I-4 path; a dup-able cross-Proc handle to police.
B. Explicit decoupled SYS_FLOW_SHARE/MAP pair — rejected: two syscalls whose
   correlation must then be policed.
C. Grant-is-the-share: opening the flow's data fid IS the mapping. — chosen.
## The call
C, user-voted. The capability is holding the namespace-gated flow fid
(I-1/I-28); no free-floating handle exists to leak or mis-target.
```

### 9.5 Dossier skeleton: `system/kernel/ninep/sub-kernel-ninep-client.md`

Frontmatter as §5.1 (audit: hard; guarded-by I-9/I-10/I-11; validated-by
spec-9p-client [clean + 5 buggy cfgs], gate-smp; hazards
single-waiter-rendez, shared-stream-desync). Body carries the present-tense
mechanism (elected reader, tag demux, flow control with spill, frame-atomic
recv) and the Prosecution section absorbed from today's CLAUDE.md row — while
the #841→#845→#349→#375→#52/#53→#89/#90 saga lives entirely in `record/` and
`lin-9p-client`, reachable from the generated Provenance section. This split
is the schema's acid test: today that row interleaves both planes in one
stream.

## 10. Migration (summary; the full plan is its own doc)

1. **Commit 0 (scripture):** this schema + the linter + templates + empty
   spine + the re-declared authority chain (spec > vault > code > user
   manual).
2. **Pilot:** the 9P client end-to-end across all planes — dossier, lineage,
   full Record backfill (its audits are the richest), decisions, seams.
   Judge the shape here before the sweep.
3. **Sweep by subsystem** (not by arc — arcs overlap surfaces): for each, fuse
   its reference doc + CLAUDE.md row + ARCH §25.4 row + memory files into the
   dossier; distill history into skeletal chg/fnd notes; leave a stub at the
   old path; retarget pointers in the same commit.
4. **Registry passes:** invariants, specs, ABIs, locks, hazards, glossary,
   gates, seams, measurements — each mined from scripture + memory.
5. **View cutover:** generate; diff against the hand-maintained tables they
   replace; retire the tables (CLAUDE.md shrinks to constitution + pointers).
6. **Stub deletion** only after the linter's dangling-link pass is clean and
   a full-corpus verification (every heading of every retired doc accounted
   for) is recorded in a chg note.

Out of scope, unchanged: the TLA+ specs themselves, the user-manual deferral
(the vault's user-facing half waits on the v1.0-rc decision), git as the
authority on code.
