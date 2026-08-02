---
id: arc-vault
type: arc
title: "The vault migration"
status: active
design: [vault/meta/schema.md, vault/meta/workflow.md]
chunks:
  - chg-2026-07-31-vault-commit-0
  - chg-2026-07-31-ninep-pilot
  - chg-2026-07-31-ninep-area-sweep
  - chg-2026-07-31-quaestor
  - chg-2026-07-31-larder-sweep
  - chg-2026-07-31-srv-sweep
  - chg-2026-07-31-netd-sweep
  - chg-2026-07-31-stalk-sweep
  - chg-2026-08-01-territory-sweep
  - chg-2026-08-01-proc-thread-sweep
  - chg-2026-08-01-sched-sweep
  - chg-2026-08-01-mm-ipc-sweep
  - chg-2026-08-01-pouch-sweep
  - chg-2026-08-01-substrate-sweep
  - chg-2026-08-02-stratum-sweep
  - chg-2026-08-02-authority-sweep
  - chg-2026-08-02-introspection-sweep
  - chg-2026-08-02-console-sweep
  - chg-2026-08-02-entry-sweep
  - chg-2026-08-02-async-sweep
  - chg-2026-08-02-boot-sweep
  - chg-2026-08-02-devices-interrupt-time-sweep
  - chg-2026-08-02-devices-hwcap-sweep
  - chg-2026-08-02-devices-discovery-sweep
  - chg-2026-08-02-devices-content-sweep
  - chg-2026-08-02-absorption-reconciliation
  - chg-2026-08-02-registry-pass
follow-ons: []
exit-criteria:
  - "[x] Pilot: the 9P client end-to-end across all planes"
  - "[ ] Sweep by subsystem (46/147 documents absorbed -- see view-absorption)"
  - "[ ] Registry passes -- a PREREQUISITE for absorption, not a successor to it: a table-bearing document cannot be replaced until its tables have a boundary note to live in. THREE kinds, not one: (a) the enumerated-value registries [DONE: errno, caps, handle-rights, note-names]; (b) the STRUCT layouts (t_stat + its six mirrors, the Loom ring structures, the 9P wire structures) -- what actually unblocks 107-loom.md; (c) the six missing spec notes (task #37, see view-spec-coverage) -- what actually unblocks 19-handles.md, whose blocker batch 23 misdiagnosed as a boundary registry"
  - "[ ] Absorb the twelve documents whose prose is swept and whose tables await a registry"
  - "[ ] Sweep the three orphaned files (task #32) and delete the over-claim notices"
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
