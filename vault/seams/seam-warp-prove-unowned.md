---
id: seam-warp-prove-unowned
type: seam
title: "usr/warp-prove/src is UNOWNED -- the Warp gate prover has no dossier"
status: open
surface: [sub-tapestryd]
opened-by: chg-2026-08-19-v3a-ring
tracker: "V-3a coherent-ring doc pass, 2026-08-19"
created: 2026-08-19
updated: 2026-08-19
---
## Owed

`usr/warp-prove/src/main.rs` (~1700 lines) is the Warp gate prover — the
GL-host binary that drives `/srv/warp` through its `prove`, `ring`,
`reject`, and readback verbs, and carries the load-bearing discrimination
logic for the Warp audit regressions (the poisoned-path #175 churn bound,
the two-client #180 ownership legs, the V-3a ring's I-9 / I-45 / F1
drain-cap legs). `quaestor owner` reports it UNOWNED: no `sub` dossier
names it in `code:`, so the doc-update cutover routes its prose to
`docs/reference/149-warp.md` (the gate-prover section) rather than the
vault. This seam records that routing decision so a future coverage sweep
ratifies it rather than re-discovering it at each doc pass.

## What closes it

A sweep decision, one of two:

1. **Accept test-only** (the likely call). The prover is a test harness;
   its as-built behavior belongs in the reference doc beside the surface it
   exercises, and it earns no `sub` dossier. Close WONTFIX with that
   rationale — the vault carries runtime surfaces, and a prover's
   discrimination logic is documented where the surface it proves is.
2. **Grant a dossier.** If the prover's own guarantees (its
   fails-without-the-fix discrimination) become a surface worth pinning
   independently, mint `sub-warp-prove` and add `usr/warp-prove/src/**` to
   its `code:`.

## Risk while open

Low. The prover's reference coverage already exists (149-warp.md); nothing
is undocumented. The only standing cost is that `quaestor owner` keeps
routing its future edits to the reference doc — which is the correct answer
under option 1 anyway. The seam exists so that answer is recorded, not
re-litigated.
