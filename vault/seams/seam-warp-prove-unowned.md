---
id: seam-warp-prove-unowned
type: seam
title: "the Warp gate test-infra (warp-prove + the venus verdict) is UNOWNED -- no dossier"
status: open
surface: [sub-tapestryd]
opened-by: chg-2026-08-19-v3a-ring
tracker: "V-3a coherent-ring doc pass, 2026-08-19"
created: 2026-08-19
updated: 2026-08-24
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

**The venus gate tools join this class (V-3b-1a, 2026-08-24).**
`tools/warp-host.sh` (the `venus` verb + its `venus-verdict` sub-verb, the
GL-host boot driver + the two-leg discrimination) and
`tools/test-venus-verdict.sh` (the crafted-log unit test that sabotage-checks
the verdict without a boot) are ALSO `quaestor owner`-UNOWNED, and are the same
kind of surface as the prover: test-infra whose discrimination logic documents
itself at the point it runs. The V-3b-1a HOST3D-substrate gate lands its
host3d assertions in exactly these two files. Same routing decision, same
sweep — recorded here so the next doc pass does not re-litigate it. (`warp-host.sh`
is separately PINNED by `abi-boot-banner` as a boot-output consumer; that pin
is orthogonal to this ownership routing and is untouched by the venus-verdict
gate, which reads no boot-banner string.)

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
