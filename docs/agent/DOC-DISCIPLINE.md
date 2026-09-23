# Reference documentation discipline

> Moved verbatim from `CLAUDE.md` on 2026-09-23 (the CLAUDE.md trim). CLAUDE.md keeps the per-chunk summary. The retired `../thylacine-vault` separate-repo wording was corrected in the same commit.
> Section headings keep their original levels.

## Reference documentation discipline (load-bearing)

**Two parallel references, both maintained continuously, both binding for every PR**:

### A. Technical reference — the vault (`vault/`, in-tree on `main`), absorbing `docs/reference/NN-*.md`

**RETIREMENT IN PROGRESS (operator-ratified 2026-09-06).** The technical reference is moving into **the vault**: the registrar-linted, code-verified graph of per-surface dossiers under `vault/system/`. `docs/reference/` is now the **legacy** tree — frozen (no new content) and being absorbed subsystem-by-subsystem into redirect stubs (see `docs/reference/18-territory.md` for the shape: an `[ABSORBED INTO THE VAULT]` pointer to the owning dossier, plus what the old file got wrong by the time it was absorbed). New technical-reference prose goes to a **dossier**, never a new `docs/reference` section (the routing is enforced by step 0 below). ~32 of 157 files were absorbed as of the flip; the rest carry full parallel content until the sweep reaches them.

**THE VAULT AGENT IS RETIRED (operator-directed 2026-09-15). There is no separate vault track to ring.** Every agent now **owns the documentation for the surfaces it touches**: when your chunk changes a surface, you update its owning dossier **yourself**, in the same chunk. The vault is **in-tree** (`vault/` on `main`), so the dossier is edited in your own worktree and **co-staged in the same commit as the code**. (Corrected 2026-09-23: this paragraph previously said the dossier lived in a separate repo at `../thylacine-vault` and could not be co-staged -- that described the retired `vault/bootstrap` checkout and contradicted step 0 below.) For an **unowned or vault-orphaned** surface, the touching agent **takes ownership** -- create or adopt the dossier and maintain it thereafter. The `quaestor owner` check and the doc-update-per-PR discipline below are UNCHANGED; only the delegation is gone. **Everywhere below that says "ring vault" now means "update it yourself".** The `No-dossier-change: <why>` trailer remains the escape for a fold that genuinely belongs to a later chunk.

**Interact with the vault THROUGH `quaestor`, never by raw grep/edit of vault files (operator-directed 2026-09-15).** `quaestor` (the in-tree CLI, run from your own worktree as `go -C vault/meta/quaestor run . <verb> --root "$(pwd)"`, and the `mcp__quaestor__*` MCP tools) is the interface for every vault operation — `owner` / `note` / `backlinks` / `query-findings` / `query-seams` / `stale` / `closed-preamble` to read and query, `new-note` and note update to author, `lint` and `render` to validate + regenerate the tracked views (e.g. `vault/views/view-code-coverage.md`). It enforces the registrar graph, the R6 mirror sets, and the coverage-view currency the commit-msg hook checks; a raw edit bypasses all of that and desyncs the graph. `use quaestor, not grep` is the standing rule — it is now also how you WRITE.

The **as-built** contract is unchanged, only its home is: the reference describes *what exists in the tree right now*, with file:line citations and runtime semantics, distinct from `ARCHITECTURE.md` (design intent, including unimplemented work). Audience: developers, auditors, future maintainers.

A dossier covers the same ground the legacy per-file template did (kept here as the depth bar the dossier meets), per `docs/REFERENCE.md` "How to read this":

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

### B. The Operator's Manual — `docs/OPERATORS-MANUAL.md` + `docs/manual/NN-*.md`

The **operator-facing** reference: the *Thylacine Operator's Manual*. Audience: people who run and use Thylacine — operators, administrators, developers writing programs against it, container users, Halcyon users. Distinct from the technical reference (developers of Thylacine itself).

**LIVE, grown section by section.** Revived 2026-09-05 (operator-directed, superseding the 2026-05-31 deferral to v1.0-rc); renamed from the User Manual 2026-09-16. The index `docs/OPERATORS-MANUAL.md` lists the sections, one file each under `docs/manual/`. The source ships in the OS at `/manual` and is read through a Beacon-emitting reader: rich under Halcyon, plain text on serial and through pipes. **The reader and the `/manual` staging are built BEFORE further sections are written** (operator decision 2026-09-16), and the reader defines the source format — a section uses only the Markdown forms the reader maps onto Beacon, never markup it does not. **`docs/MANUAL-DESIGN.md` is binding** for that format, the `manual` command, and the installation. Pages not yet written to the guide live in `docs/manual-drafts/` and are neither checked nor installed.

**The writing guide is binding for every manual sentence:** `docs/thylacine-operators-manual-writing-guide.md`. Read it before drafting. It fixes the section shape (an untitled opening, then **In Practice**, then **Technical Details**), the reference voice, the language to avoid, and the verification bar: every command, option, default and error checked against the current tree, and planned, test-only or superseded behaviour never presented as current.

The bar: an operator landing on a section completes the common tasks without leaving it; a reader of the whole book comes away understanding the system.

### Maintenance discipline (per-chunk; non-negotiable)

When a chunk lands (bug fix, refactor, new module, new feature), the author updates **both references** in the same PR:

0. **Check the vault first.** Before documenting a changed surface, run quaestor
   **from your own worktree** — `main` carries the code, the whole vault under
   `vault/`, AND quaestor's own source, so your worktree already has everything
   current. There is no separate vault checkout to `cd` into and no vault session
   to route through:

   ```bash
   go -C vault/meta/quaestor run . owner <changed paths> --root "$(pwd)"
   ```

   **Since 2026-09-06 (operator-ratified) the technical reference IS the vault** — `docs/reference` is frozen legacy (Part A). **Exit 0** — the vault carries that surface: update the owning dossier **yourself, in this worktree, co-staged with the code**. **Exit 1** — no dossier yet: a **new dossier** is owed — **author it yourself** under `vault/system/`, never a new `docs/reference` section. **With several paths the answer is usually MIXED and the exit status reports only half of it** — read the summary line, which names both sets; a dossier is owed for each either way (update for the covered, create for the uncovered).

   **The vault agent is RETIRED (2026-09-15, operator-ratified): every track owns and updates the dossiers it touches, in its own worktree.** There is no "ring vault" delegation any more — the mediator model let the separate `vault/bootstrap` checkout rot 150+ commits behind `main`, so quaestor there could not see the very files a track had just changed, and tracks read that as "not mine to do." Because the vault is in-tree, you edit the dossier in the same worktree and the same commit as the code; the render + lint below run locally.

   Read any `ALSO named by` line in the output. A note that merely **pins** a file (an `abi-*` registry pins VALUES or STRINGS) cannot hold a description of a mechanism — so the reference section is still owed, AND that note may need the same change.

   This step exists because the alternative is a protocol whose first move is remembering to tell someone. It rides the doc-update step precisely so it cannot be skipped separately from it.

   **Since 2026-09-06 this is enforced mechanically, not only by convention** (operator-ratified). A `commit-msg` hook runs `quaestor dossier-gate`: staging code owned by an `audit: hard` dossier **blocks** the commit unless that dossier is co-staged OR the message carries a `No-dossier-change: <why>` trailer (non-empty reason required); any other owned surface **warns**. So the reminder to update — or consciously defer — a dossier fires the moment the code lands, on every track sharing the hook. A track **co-stages the owning dossier itself**, in its own worktree; the pre-commit `quaestor lint --staged` runs in every worktree too, so a broken or incomplete dossier (dangling link, missing section, absent `code:` path) blocks the commit wherever you author it. The `No-dossier-change:` trailer is the escape for a fold that genuinely belongs to a later chunk — not a way to hand the dossier off, which is what the retired "ring vault" delegation used to mean. Details + the fail-open/commit-msg-placement rationale: `vault/meta/schema.md` section 8 (check 9). `--no-verify` skips it and is the sanctioned emergency bypass.

1. **Technical reference (the vault)**: extend or create the owning dossier under `vault/system/` **in your own worktree, co-staged with the code**, per step 0. New module → new dossier. Bug fix that touches a documented invariant → update the dossier after the spec. New term / acronym → a vault glossary note. **`docs/reference` is frozen legacy — never add to it or create a new `NN-*.md`;** it is being absorbed into redirect stubs subsystem-by-subsystem (Part A).
2. **User reference**: extend or update the relevant `docs/manual/NN-*.md` section if the change is user-visible (new syscall, new admin command, new error case, behavior change). Internal refactors typically don't touch the user manual; user-visible changes always do.
3. **Snapshot blocks — NO LONGER A PER-CHUNK OBLIGATION** (operator-answered
   2026-09-16). `docs/REFERENCE.md`'s Snapshot follows `docs/reference` into
   the 2026-09-06 freeze: it is a historical artifact of Phase 5, not a
   current claim, and must not be refreshed per chunk. The LIVE figures are
   the quaestor-rendered vault views (`vault/views/view-code-coverage.md`,
   `view-audit-trigger-coverage.md`), which `quaestor render` regenerates and
   the pre-commit lint keeps current — self-maintaining, where the old rule's
   only enforcement was remembering it. That is how it reached six weeks
   stale, unnoticed, while every chunk quietly skipped it.
   The Operator's Manual index keeps no snapshot.

A PR that adds code without updating the relevant reference sections is incomplete. **Treat docs as code: doc-update-per-PR is non-negotiable. Missing docs are reverted along with their code.**

### Audit-policy extension to the references

The audit-trigger surfaces table in this document and in `ARCHITECTURE.md §25.4` covers code. The reference docs extend the audit policy: a change to a documented invariant in the technical reference (now the vault dossier — or the legacy `docs/reference` section until its subsystem is absorbed) updates the spec FIRST (per spec-first policy), then that technical reference, then the code, then the user reference if user-visible. If the four disagree, **the spec wins**, then the technical reference (the vault), then the code, then the user reference. The user reference can never be authoritative on internal semantics; it can only describe them.

### Why two references, not one

The technical reference (now **the vault**) and the user reference (`docs/manual`) have **different audiences with different needs**, and the retirement does not merge them — it only moves the technical one into the vault. A user wants to know "how do I create a snapshot of my home subvolume?" — they don't care about the Bε-tree commit protocol. A developer wants to know "what happens to outstanding 9P tags when a session is dropped?" — they don't care about the `stratum snapshot` CLI usage. Splitting them keeps each focused; merging them produces a 1000-page document where neither audience finds what they need. (The Operator's Manual is a separate track, unaffected by the docs/reference retirement; its status is in Part B.)

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
