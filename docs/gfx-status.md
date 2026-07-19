# Graphics arc status — G-0..G-9 (Phases 9/10; TAPESTRY.md §18.9)

The per-chunk status doc for the Tapestry/Halcyon build arc, chartered by the
user 2026-07-19 ("continue with the G items; ABI automatically signed off").
The binding stage table is `docs/TAPESTRY.md` §18.9; the design closed over two
Fable holotype rounds (§18.11/§18.12) with the spec landed model-first
(`specs/tapestry_present.tla`, V3).

## TL;DR

G-2 (the kernel delta — the zero-copy surface-share foundation) landed FIRST,
out of stage order, per the user's 2026-07-19 directive (Fable's last day goes
to the hardest, most complex work; graphics performance the priority). T-1 is
now enumerated as **I-40** with its kernel share half ENFORCED. G-0/G-1
(tooling + driver promotion) remain, buildable by any later session; G-3+
consume the G-2 substrate.

## Landed chunks

| Commit | Chunk | What | Tests |
|---|---|---|---|
| *(pending)* | G-2 | The DMA-weave share admission: `SYS_DMA_CREATE_WEAVE` (99; the kernel-minted create-immutable `KObj_DMA.weave` subtype — the §18.12 R2-F1 structural close; ABI user-signed-off 2026-07-19) + the `burrow_share_into`/`SYS_WEFT_SHARE` admission gates (ANON or weave ONLY; device-command DMA as unshareable as MMIO) + `weft_claimed_kind` (type-authoritative kind + server-declaration cross-check) + the WEAVE binding kind (no ring view; the kind-gated `validate_rw` closes all three Tweftio fast paths — §18.11 F10) + `SYS_WEFT_UNSHARE` (100; the F3/R2-F5 disarm: removal-before-free + fail-closed late claim = the `Map`-guard NoStaleMap kernel half; closes #289) + the per-client shared-in budget (`Proc.shared_map_pages` @348, 128 MiB, the I-32 fifth axis — R2-F3) + the weave-fid clunk-unmap (`ClunkMap`) + `libthyla_rs::{t_dma_create_weave, t_weft_unshare}`. ARCH §28 **I-40** enumerated (share half ENFORCED; present half at G-3); CLAUDE.md + ARCH §25.4 Weft-row addenda; `docs/reference/125-weft.md` "The weave share (G-2)"; SPEC-TO-CODE `tapestry_present.tla` section. | 1169/1169 (+4: `weft.{share_rejects_plain_dma, weave_share_and_claim, unshare_disarm, shared_map_budget_cap}`, revert-probed → 1167/1169 on the weakened gates) + boot OK; spec gate GREEN (tapestry 5413 + liveness + 4 buggy firing; weft 1412 + readiness re-ran); SMP gate + focused Fable audit at the close |

## Remaining work

| # | Chunk | Scope | Gate |
|---|---|---|---|
| G-0 | Agentic eyes | QMP `screendump` harness (`tools/`): verify capture under `-display none` FIRST; PNG into the agent loop; TOOLING.md ABI addendum (task #22) | a captured PNG of the existing one-shot test pattern |
| G-1 | Resident GPU driver | Promote `usr/virtio-gpu` to a warden-manifested persistent driver; re-home `crash-probe` off `virtio:16`; render loop + IRQ-driven flush; fix the stale probe-only comment | boots resident; pattern persists; SMP gate |
| G-3 | tapestryd stage 0 | The V1 minimal compositor over the G-2 substrate: one fullscreen surface, `/dev/tapestry` `ctl`+`surface/`, present/event fids, FRAME clock, virtio-input; F2 per-session qid scoping; the R2-F3 force-reclaim grace (the crash contract's kernel leg); the SPEC-TO-CODE present-half extension | tapestry-demo plasma LIVE via screendump |
| G-4 | Aurora renderer MVP | Cell grid + the Cornucopia baked atlas + VT-parser subset; the `/dev/cons` drain/feed backend (kernel, audit-bearing; `SPAWN_PERM_CONSOLE_RENDERER` per R2-F6) | the fbcon claim: login + `ls` via screendump |
| G-5 | Graphics audit round | The reserved rows enforced; the I-40 present half + tapestryd/cons-backend focused audit; SMP gate | clean close |
| G-6..G-9 | Phase 10 | Compositor (pane/layout) → SDL+Quake → Halcyon core → integration (§18.9) | per row |

## Trip hazards

- **The G-2 admission gates are kernel-minted-subtype-only.** Any widening to
  "any DMA" or a creator-asserted flag re-opens §18.12 R2-F1 — the revert-probe
  proved the regressions fire on exactly that weakening.
- **The charge/uncharge pairing rides `VMA_FLAG_SHARED_IN`.** Every future
  removal path of a flagged VMA must uncharge (today: `burrow_unmap` +
  `vma_drain`); a new removal path that skips it leaks budget.
- **`weft_binding_validate_rw`'s kind gate is the single chokepoint** keeping
  Tweftio off weave fids — do not add a fast-path consumer that bypasses it.
- **tapestryd's retire discipline (G-3)**: `SYS_WEFT_UNSHARE` must run
  at/before the RETIRING transition, BEFORE any weave free/reuse
  (removal-before-page-free) — that ordering is what discharges the spec's
  `Map` guard.
- **The R2-F3 force-reclaim** (orphaned weave mappings after a bounded grace,
  post-compositor-crash) is the G-3 leg — the G-2 budget bounds the pin; the
  reclaim completes the crash contract.
- G-2 landed on branch `gfx-1` based off `pty-followups` @c1205ce2 (main had
  diverged with Go 8d at branch time); the eventual merge to main is the
  user's, a normal merge.

## Build + verify

```bash
tools/build.sh kernel && tools/test.sh        # 1169/1169 + boot OK
cd specs && for c in tapestry_present tapestry_present_liveness \
  tapestry_present_buggy_{premature_reuse,retire_during_transfer,reweave_without_quiesce,map_after_retire}; do
  java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config $c.cfg tapestry_present.tla; done
tools/ci-smp-gate.sh                          # the full matrix on kernel chunks
```

## References

- `docs/TAPESTRY.md` §18 (the binding concretization; §18.9 the stage table)
- `docs/reference/125-weft.md` "The weave share (G-2)" (as-built)
- `specs/tapestry_present.tla` + `specs/SPEC-TO-CODE.md::tapestry_present.tla`
- ARCH §28 I-40 + §25.4 (the Weft-row G-2 addendum); CLAUDE.md (same)
- `memory/project_tapestry_design_pass.md` + `memory/audit_tapestry_design_closed_list.md`
