---
id: arc-vault
type: arc
title: "The vault migration"
status: active
design: [vault/meta/schema.md, vault/meta/workflow.md]
chunks: [chg-2026-07-31-vault-commit-0, chg-2026-07-31-ninep-pilot]
follow-ons: []
exit-criteria:
  - "[x] Pilot: the 9P client end-to-end across all planes"
  - "[ ] Sweep by subsystem (reference docs absorbed, stubs left)"
  - "[ ] Registry passes (invariants, specs, ABIs, locks, hazards, glossary, gates, seams, measurements)"
  - "[ ] View cutover (CLAUDE.md shrinks to constitution + pointers)"
  - "[x] Session hooks wired in .claude/settings.json (tier 3)"
  - "[ ] Stub deletion after the full-corpus verification chg"
created: 2026-07-31
---
## Goal

Absorb the technical reference and every hand-maintained knowledge table into
the vault, per the schema (`vault/meta/schema.md`) and the operating loop
(`vault/meta/workflow.md`), then cut the operational workflow over to it.

## Planned chunks

Commit 0 (schema + linter + spine) -> the 9P-client pilot -> the
per-subsystem sweep -> the registry passes -> the view cutover + CLAUDE.md
rewrite -> stub deletion. Merge to main at a clean main-track arc boundary.

## Close summary

(written at status flip to complete)
