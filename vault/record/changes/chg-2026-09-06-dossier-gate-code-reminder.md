---
id: chg-2026-09-06-dossier-gate-code-reminder
type: chg
title: "quaestor dossier-gate: the tiered code->dossier reminder (commit-msg hook) -- staged audit:hard-owned code blocks unless the dossier is co-staged or a No-dossier-change trailer is present; other owners warn"
date: 2026-09-06
arc: arc-vault
commits: ["6ba2970d"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
Operator-ratified 2026-09-06 (AskUserQuestion: "reliable reminder to update
dossiers?" -> "Tiered: block audit:hard, warn rest"). The mirror image of the
existing chg->dossier advisory in `stagedChecks`: there a staged CHG touching
an `audit: hard` dossier warns when the dossier is not co-staged; here staged
CODE owned by a dossier reminds you the dossier may be owed an update.

- **New `dossier_gate.go`**: `dossierGate(root, reg, msg)` walks the staged set
  (via the extracted `stagedEntries`, now the ONE reading of the git index --
  `stagedChecks` was refactored onto it), resolves each staged source file
  (`srcRe`: `kernel|arch|mm|usr/....{c,h,S,rs}`, plus the `.c`/`.h` twin) to its
  owning `sub` dossiers via `ownerIndex`, and reports owners not co-staged:
  `audit: hard` -> FAIL (blocks), else -> WARN. Deduped by note id. Escape:
  `No-dossier-change: <why>` (a commit trailer, non-empty reason required -- a
  bare key is a silent off-switch; the same field on a staged chg is honoured).
- **Placement is load-bearing**: a `commit-msg` hook, not `pre-commit`, because
  only the message carries the trailer and only `commit-msg` sees it. The code
  tracks (main, aux) write no vault chg notes and ring the vault for owned prose
  rather than co-stage it in a kernel commit, so the trailer is the one escape
  that serves every track. Fails OPEN on an empty registry (pre-commit is the
  authoritative infra gate and runs first); `--no-verify` skips both and is the
  sanctioned bypass.
- **`schema.md` section 8** gains the `dossier-gate` paragraph + check 9;
  **CLAUDE.md** step 0 (the doc-update discipline) records that the reminder is
  now enforced mechanically, not only by convention.

Tests (`dossier_gate_test.go`, 9): audit:hard blocks / audit:light warns (the
tier boundary, the key discrimination); co-stage clears it; trailer AND chg-field
escapes work; an empty/commented trailer does NOT escape; the `.c`/`.h` twin
resolves; unowned + non-code edits are silent; one owner of two staged files is
reported once. Sabotage-verified: never-block fails the block tests, always-block
fails the warn test, ignore-trailer fails the escape test. Integration-tested
against real staged `kernel/burrow.c` (audit:hard, sub-kernel-burrow) via both
the command and the hook script, absolute + relative `$1`; tree restored exactly.
The hook is a local install (like pre-commit; not tracked); coordinated to
main+aux via yip before going live.

Why it matters: main flagged three de-stales by hand this run alone (0058
halcyond PL-4, 0059 presenters PL-5, plus abi-errno). This gate fires the same
reminder automatically at commit time, closing the silent-drift gap the manual
flags were papering over.
