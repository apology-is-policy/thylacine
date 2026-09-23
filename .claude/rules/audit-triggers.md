---
paths:
  - "kernel/**"
  - "arch/**"
  - "mm/**"
  - "usr/**"
  - "specs/**"
  - "tools/**"
---

# Audit-trigger surfaces

Many files under these paths are audit-trigger surfaces. Before modifying one:

1. `grep -n '<path or symbol>' docs/agent/AUDIT-TRIGGERS-INDEX.md docs/AUDIT-TRIGGERS.md`
2. If it matches, `Read` ONLY that row's line window in `docs/AUDIT-TRIGGERS.md` (never the whole ~440 KB file) and follow its prosecution addenda.
3. The change then needs an audit round before merge: use the `audit-round` skill.

A chunk that creates a new audit-bearing surface appends its full row to `docs/AUDIT-TRIGGERS.md` and a one-line entry to `docs/agent/AUDIT-TRIGGERS-INDEX.md` in the same PR.
