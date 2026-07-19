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
now enumerated as **I-40** with its kernel share half ENFORCED. G-0 (the
screendump harness, headless-proven) and G-1 (the resident `gpud` over
virtio-PCI + the per-boot pattern-persists gate) are landed — **Phase 9's
tooling + device substrate is complete**; G-3 (tapestryd stage 0) is next and
consumes the G-2 substrate + the G-1 device.

## Landed chunks

| Commit | Chunk | What | Tests |
|---|---|---|---|
| *(pending)* | G-1 | The resident GPU driver: NEW `usr/gpud` (libdriver persistent; the netd precedent) — probe = `PciDev::claim(16)` + the common-cfg handshake + the 7-command 2D bring-up (the P4-L pattern); serve = READY then the 2 Hz render loop (8×8 center heartbeat, compile-asserted clear of the `-v` sample points; partial-rect TRANSFER+FLUSH, IRQ-completed). **The transport pivot, measured not theorized**: a persistent claimant on the shared virtio-mmio slot page (`0xa003000`, all six devices, page-exclusive claims) starved netdev-driver AND stratumd's disk (rc=-207 boot-fatal) → **virtio-gpu-pci** (`virtio-pci:16`, own BARs — the netd/#140 move); the MMIO device stays as `gpu-mmio0` for the one-shot kernel-test probe. crash-probe re-homed to the warden-SYNTHETIC `restart-test` node (F15: resource-less DeviceNode; grant mmio=0 irq=0 dma=0). The **pattern-persists gate** in `tools/test.sh` (screendump `-v` post-banner, bounded retry, `gpu-gate` FAIL arm; skips under NO_GPU/NO_QMP/GPU_GATE=0) — runs in every ci-smp-gate boot. TAPESTRY §18.9 as-built note (G-3's tapestryd binds `virtio-pci:16`); `docs/reference/138-gpud.md`. Kernel byte-unchanged. | Suite 1170/1170 + boot OK; the pattern gate GREEN inside test.sh; ecosystem restored (netdev ARP 24/24 + stratumd serving + crash-probe synthetic ladder); liveness pixel-proven (block(64,64) yellow↔black across a tick, neighbors stable); reduced SMP amplifier |
| 5317ea7d | G-0 | The "agentic eyes" capture step: `tools/screendump.sh` — QMP `screendump` to PNG over the standing `build/qmp.sock` (P4-K wiring), targeting the `gpu0` qdev id; `-v` asserts the P4-L 4-quadrant pattern via a PPM sibling dump (quadrant-center sampling, tol ≤8). TOOLING.md §3.1 addendum. **Headless capture EMPIRICALLY PROVEN**: `VERIFY OK — 128x128 shows the P4-L 4-quadrant pattern` captured under the standing `-nographic` boot (no display backend, no VNC — a VNC-listener differential confirmed listener-independence; HVF and TCG both). **The investigation's load-bearing finding**: a post-reap capture reads a blank surface NOT because headless capture fails but because the **RW-7 proc-death quiesce** (`kernel/virtio.c::virtio_mmio_reset_in_range`) resets the dying driver's virtio devices (DMA soundness: no in-flight device write into freed KObj_DMA pages) — a virtio-gpu reset destroys host-side resources + disables the scanout. Ground-truthed by a held-alive instrumented probe run: guest read-back = pattern, QEMU `xp` at `fb_pa` = pattern, screendump = pattern while alive; black only after reap; kernel/test/QEMU all exonerated as bugs — the behavior is correct + audited. So **a scanout lives exactly as long as its driving Proc** — the persistent capture target is G-1's resident driver (its "pattern persists" gate re-runs `-v` for keeps), and a compositor crash blanks the display until warden restarts it (the TAPESTRY crash contract's visible half). The one-shot probe gained only a comment recording the caveat; instrumentation reverted. | The captured artifact `build/screendump-g0-held.png` (VERIFY OK, all four quadrants exact); harness exercised against live/absent/reaped-scanout VMs; suite + boot unchanged (kernel byte-identical; probe binary byte-identical minus comment) |
| 6599519d + the close commit | G-2 | The DMA-weave share admission: `SYS_DMA_CREATE_WEAVE` (99; the kernel-minted create-immutable `KObj_DMA.weave` subtype — the §18.12 R2-F1 structural close; ABI user-signed-off 2026-07-19) + the `burrow_share_into`/`SYS_WEFT_SHARE` admission gates (ANON or weave ONLY; device-command DMA as unshareable as MMIO) + `weft_claimed_kind` (type-authoritative kind + server-declaration cross-check) + the WEAVE binding kind (no ring view; the kind-gated `validate_rw` closes all three Tweftio fast paths — §18.11 F10) + `SYS_WEFT_UNSHARE` (100; the F3/R2-F5 disarm: removal-before-free + fail-closed late claim = the `Map`-guard NoStaleMap kernel half; closes #289) + the per-client shared-in budget (`Proc.shared_map_pages` @348, 128 MiB, the I-32 fifth axis — R2-F3) + the weave-fid clunk-unmap (`ClunkMap`, F1-identity-guarded via `weft_binding_clunk_unmap`) + `libthyla_rs::{t_dma_create_weave, t_weft_unshare}`. ARCH §28 **I-40** enumerated (share half ENFORCED; present half at G-3); CLAUDE.md + ARCH §25.4 Weft-row addenda; `docs/reference/125-weft.md` "The weave share (G-2)"; SPEC-TO-CODE `tapestry_present.tla` section. **AUDIT CLOSED CLEAN: Fable-5-max holotype (MODEL start==end) + concurrent self-audit — 0 P0 / 0 P1 / 0 P2 / 3 P3, NOT dirty** (F1 stale-VA clunk-unmap → FIXED + revert-probed regression; F2 integration coverage → the clunk-unmap half discharged, the Tweft-round-trip E2E G-3-owed; F3 bare-pid → closed-as-documented). `memory/audit_g2_closed_list.md`. | 1170/1170 (+5: the 4 gate/budget regressions + `weft.weave_clunk_unmap_guard`; BOTH probe families revert-probed — the gate weakenings → 1167/1169, the F1 guard drop → 1169/1170, each failing exactly its targets) + boot OK; spec gate GREEN (tapestry 5413 + liveness + 4 buggy firing; weft 1412 + readiness re-ran); FULL SMP gate 40/40 PASS 0 corruption + the reduced amplifier on the F1 delta |

## Remaining work

| # | Chunk | Scope | Gate |
|---|---|---|---|
| G-3 | tapestryd stage 0 | The V1 minimal compositor over the G-2 substrate: one fullscreen surface, `/dev/tapestry` `ctl`+`surface/`, present/event fids, FRAME clock, virtio-input; F2 per-session qid scoping; the R2-F3 force-reclaim grace (the crash contract's kernel leg); the SPEC-TO-CODE present-half extension | tapestry-demo plasma LIVE via screendump |
| G-4 | Aurora renderer MVP | Cell grid + the Cornucopia baked atlas + VT-parser subset; the `/dev/cons` drain/feed backend (kernel, audit-bearing; `SPAWN_PERM_CONSOLE_RENDERER` per R2-F6) | the fbcon claim: login + `ls` via screendump |
| G-5 | Graphics audit round | The reserved rows enforced; the I-40 present half + tapestryd/cons-backend focused audit; SMP gate | clean close |
| G-6..G-9 | Phase 10 | Compositor (pane/layout) → SDL+Quake → Halcyon core → integration (§18.9) | per row |

## Trip hazards

- **The RW-7 proc-death quiesce blanks a dead driver's display** (G-0
  finding): `virtio_mmio_reset_in_range` resets every virtio device in a
  dying Proc's MMIO window — correct + load-bearing for DMA soundness, do
  NOT weaken it for display persistence. Consequences the arc must build
  around: (a) the scanout lives exactly as long as its driving Proc — G-1's
  driver must be persistent; (b) a compositor crash → quiesce-blank →
  warden restart → re-init repaints (the TAPESTRY crash contract's visible
  half); (c) `tools/screendump.sh -v` against a one-shot probe races its
  reap — only assert `-v` against a resident scanout owner.
- **A persistent driver CANNOT claim a shared virtio-mmio page** (G-1,
  measured): all six populated QEMU-virt MMIO slots share ONE page-exclusive
  4-KiB page whose lifetime belongs temporally to the transient probes and
  then permanently to stratumd. Any future resident MMIO-transport driver on
  that bank re-creates the G-1 boot-fatal starvation — resident drivers go
  PCI (per-function BARs). G-3's tapestryd binds **`virtio-pci:16`**.
- **A post-warden gpud crash has no restarter** (the boot-probe warden
  exits after its bind loop; shared posture with netd): the display stays
  quiesce-blank until reboot. The resident-warden lift is the v1.x seam.
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
tools/build.sh kernel && tools/test.sh        # 1170/1170 + boot OK + the G-1 pattern gate
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
