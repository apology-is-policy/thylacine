# Graphics arc status — G-0..G-9 (Phases 9/10; TAPESTRY.md §18.9)

The per-chunk status doc for the Tapestry/Halcyon build arc, chartered by the
user 2026-07-19 ("continue with the G items; ABI automatically signed off").
The binding stage table is `docs/TAPESTRY.md` §18.9; the design closed over two
Fable holotype rounds (§18.11/§18.12) with the spec landed model-first
(`specs/tapestry_present.tla`, V3).

## TL;DR

G-3 (tapestryd stage 0 + the R2-F3 kernel reaper) is LANDED — **I-40 is
COMPLETE** (share half G-2, present half G-3): the compositor owns scanout
from day 0, `tapestry-demo` presents the -v pattern + live plasma through
the FULL protocol (the per-boot gate now proves the whole G-2/G-3 path,
with a new liveness leg), and the warden's `gather` mode carries both
graphics-path PCI functions in one I-34 allowance. Earlier: G-2 (the
kernel share delta) landed first per the 2026-07-19 directive; G-0 (the
screendump harness) + G-1 (the resident gpud, now absorbed/retired) built
the substrate. G-3 is CLOSED (holotype 0/0/1/3 NOT dirty; all gates green). Next: G-4
(Aurora renderer MVP).

## Landed chunks

| Commit | Chunk | What | Tests |
|---|---|---|---|
| 2961256e | G-3d | The audit close (Fable-5-max holotype, MODEL start==end, + concurrent self-audit): **0 P0 / 0 P1 / 1 P2 / 3 P3, NOT dirty, all addressed** -- F1 [P2] the reaper's per-page TLBI unmap ran IRQs-OFF under gptl (proc_for_each is irqsave) -> FIXED via the find-and-lock callback (vma_lock taken under the walk, HELD PAST it; the unmap runs IRQs-on under vma_lock alone) enabled by `vma_drain` now TAKING `p->vma_lock` (exemption retired; pgtable_destroy is after vma_drain in proc_free, so pgtable_root stays valid); F2/F4 contract rewording (the budget DOES charge the orphan; liveness = "a client whose session OBSERVED the death"); F3 global FRAME coalesce; + self-audit SA-12 (exact gather guard) + SA-15 (the libtapestry `closed` latch). `memory/audit_g3_closed_list.md` = the G-4/G-5 preamble | 1173/1173 + boot OK + the evolved gate GREEN on the fixed code; the reduced amplifier (default-smp4 + ubsan-smp4 N=10) 20/20 PASS 0 corruption on the close delta (the FULL 40/40 ran on the pre-fix commits) |
| 88547181 | G-3a | tapestryd stage 0: NEW `usr/tapestryd` (gather-bound persistent compositor -- the absorbed gpud GPU half generalized [per-surface resources, whole-weave backing, offset TRANSFER, the retire pair]; poll-mode virtio-keyboard-PCI eventq; the /dev/tapestry 9P server with the I-40 present engine [synchronous presents => the quiesce set empty at every retire BY CONSTRUCTION; UNSHARE-before-free retire ordering; scanout at first-present-COMPLETE], F2 per-conn scoping + generation gates, F9 caps, R2-F4 WEDGE; the US-QWERTY keymap + FRAME clock) + NEW `usr/lib/libtapestry` + `usr/tapestry-demo` (the POC model cashed onto libthyla_rs::loom; quadrants + live plasma -- the G-2-F2 Tweft round-trip E2E discharged) + the libdriver/warden `gather = all` mode (resolve_gathered + pci_extra + the pcix codec key; ONE grant over both PCI functions) + the devdev tapestry mount stub + the joey mount/probe/spawn + virtio-keyboard-pci in run-vm.sh + the EVOLVED pattern gate (liveness double-dump) + gpud retired | Suite 1173/1173 + boot OK at 1280x800; the gate GREEN (quadrants through the full path + the dumps differ); 86/86 libdriver host tests incl. 4 gather legs; spec gate GREEN (tapestry 5413 unperturbed + liveness + 4 buggy; weft + readiness families) |
| 6140cef2 | G-3b | The R2-F3 orphaned-weave reaper (the ServerDeath leg's kernel half): WEAVE bindings register at the SYS_WEFT_MAP CAS-win / unregister at dev9p_close; a kproc kthread (parks empty; 1 s cadence) force-reclaims a binding whose serving session is dead past the 2 s grace -- cross-Proc unmap under gptl+vma_lock with the G-2-F1 identity guard re-checked, budget uncharged, the registration pin dropped (the chunk frees AT reclaim); `g_weft_reap_lock` serializes reaper-vs-close (burrow NULLed vs unregister-before-read) | The 3 sweep-driven regressions `weft.reap_{orphan_reclaimed,live_session_untouched,close_unregisters}` (in the 1173) |
| b2c8a6f5 | G-1 | The resident GPU driver: NEW `usr/gpud` (libdriver persistent; the netd precedent) — probe = `PciDev::claim(16)` + the common-cfg handshake + the 7-command 2D bring-up (the P4-L pattern); serve = READY then the 2 Hz render loop (8×8 center heartbeat, compile-asserted clear of the `-v` sample points; partial-rect TRANSFER+FLUSH, IRQ-completed). **The transport pivot, measured not theorized**: a persistent claimant on the shared virtio-mmio slot page (`0xa003000`, all six devices, page-exclusive claims) starved netdev-driver AND stratumd's disk (rc=-207 boot-fatal) → **virtio-gpu-pci** (`virtio-pci:16`, own BARs — the netd/#140 move); the MMIO device stays as `gpu-mmio0` for the one-shot kernel-test probe. crash-probe re-homed to the warden-SYNTHETIC `restart-test` node (F15: resource-less DeviceNode; grant mmio=0 irq=0 dma=0). The **pattern-persists gate** in `tools/test.sh` (screendump `-v` post-banner, bounded retry, `gpu-gate` FAIL arm; skips under NO_GPU/NO_QMP/GPU_GATE=0) — runs in every ci-smp-gate boot. TAPESTRY §18.9 as-built note (G-3's tapestryd binds `virtio-pci:16`); `docs/reference/138-gpud.md`. Kernel byte-unchanged. | Suite 1170/1170 + boot OK; the pattern gate GREEN inside test.sh; ecosystem restored (netdev ARP 24/24 + stratumd serving + crash-probe synthetic ladder); liveness pixel-proven (block(64,64) yellow↔black across a tick, neighbors stable); reduced SMP amplifier |
| 5317ea7d | G-0 | The "agentic eyes" capture step: `tools/screendump.sh` — QMP `screendump` to PNG over the standing `build/qmp.sock` (P4-K wiring), targeting the `gpu0` qdev id; `-v` asserts the P4-L 4-quadrant pattern via a PPM sibling dump (quadrant-center sampling, tol ≤8). TOOLING.md §3.1 addendum. **Headless capture EMPIRICALLY PROVEN**: `VERIFY OK — 128x128 shows the P4-L 4-quadrant pattern` captured under the standing `-nographic` boot (no display backend, no VNC — a VNC-listener differential confirmed listener-independence; HVF and TCG both). **The investigation's load-bearing finding**: a post-reap capture reads a blank surface NOT because headless capture fails but because the **RW-7 proc-death quiesce** (`kernel/virtio.c::virtio_mmio_reset_in_range`) resets the dying driver's virtio devices (DMA soundness: no in-flight device write into freed KObj_DMA pages) — a virtio-gpu reset destroys host-side resources + disables the scanout. Ground-truthed by a held-alive instrumented probe run: guest read-back = pattern, QEMU `xp` at `fb_pa` = pattern, screendump = pattern while alive; black only after reap; kernel/test/QEMU all exonerated as bugs — the behavior is correct + audited. So **a scanout lives exactly as long as its driving Proc** — the persistent capture target is G-1's resident driver (its "pattern persists" gate re-runs `-v` for keeps), and a compositor crash blanks the display until warden restarts it (the TAPESTRY crash contract's visible half). The one-shot probe gained only a comment recording the caveat; instrumentation reverted. | The captured artifact `build/screendump-g0-held.png` (VERIFY OK, all four quadrants exact); harness exercised against live/absent/reaped-scanout VMs; suite + boot unchanged (kernel byte-identical; probe binary byte-identical minus comment) |
| 6599519d + the close commit | G-2 | The DMA-weave share admission: `SYS_DMA_CREATE_WEAVE` (99; the kernel-minted create-immutable `KObj_DMA.weave` subtype — the §18.12 R2-F1 structural close; ABI user-signed-off 2026-07-19) + the `burrow_share_into`/`SYS_WEFT_SHARE` admission gates (ANON or weave ONLY; device-command DMA as unshareable as MMIO) + `weft_claimed_kind` (type-authoritative kind + server-declaration cross-check) + the WEAVE binding kind (no ring view; the kind-gated `validate_rw` closes all three Tweftio fast paths — §18.11 F10) + `SYS_WEFT_UNSHARE` (100; the F3/R2-F5 disarm: removal-before-free + fail-closed late claim = the `Map`-guard NoStaleMap kernel half; closes #289) + the per-client shared-in budget (`Proc.shared_map_pages` @348, 128 MiB, the I-32 fifth axis — R2-F3) + the weave-fid clunk-unmap (`ClunkMap`, F1-identity-guarded via `weft_binding_clunk_unmap`) + `libthyla_rs::{t_dma_create_weave, t_weft_unshare}`. ARCH §28 **I-40** enumerated (share half ENFORCED; present half at G-3); CLAUDE.md + ARCH §25.4 Weft-row addenda; `docs/reference/125-weft.md` "The weave share (G-2)"; SPEC-TO-CODE `tapestry_present.tla` section. **AUDIT CLOSED CLEAN: Fable-5-max holotype (MODEL start==end) + concurrent self-audit — 0 P0 / 0 P1 / 0 P2 / 3 P3, NOT dirty** (F1 stale-VA clunk-unmap → FIXED + revert-probed regression; F2 integration coverage → the clunk-unmap half discharged, the Tweft-round-trip E2E G-3-owed; F3 bare-pid → closed-as-documented). `memory/audit_g2_closed_list.md`. | 1170/1170 (+5: the 4 gate/budget regressions + `weft.weave_clunk_unmap_guard`; BOTH probe families revert-probed — the gate weakenings → 1167/1169, the F1 guard drop → 1169/1170, each failing exactly its targets) + boot OK; spec gate GREEN (tapestry 5413 + liveness + 4 buggy firing; weft 1412 + readiness re-ran); FULL SMP gate 40/40 PASS 0 corruption + the reduced amplifier on the F1 delta |

## Remaining work

| # | Chunk | Scope | Gate |
|---|---|---|---|
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
- **tapestryd's retire ordering is LANDED and binding**: `t_weft_unshare`
  runs BEFORE any backing free (`Comp::retire`); a reordering reopens the
  spec's `MapStale`. And the stage-0 quiesce property is STRUCTURAL
  (synchronous presents) — **a pipelined/async controlq (G-6+) must land a
  real in-flight drain before touching retire** (the SPEC-TO-CODE line is
  the obligation).
- **The R2-F3 reaper is LANDED**: reaper-vs-close serializes on
  `g_weft_reap_lock` (the reaper NULLs `wb->burrow`; the close unregisters
  before reading the binding) — any new close/teardown path for a WEAVE
  binding must keep the unregister-first order. Lock order:
  `g_weft_reap_lock -> g_proc_table_lock(irqsave) -> vma_lock -> v->lock
  -> buddy`; register/unregister stay lock-free call sites.
- **The gather grant**: `resolve_gathered` re-checks every extra node's
  bind membership (I-34 never-fabricated, per axis); a new axis added to
  the fold must preserve that re-check.
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
