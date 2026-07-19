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
the substrate. G-3 + G-4 CLOSED (the fbcon live + ls-gfx PASS; holotype 0/0/1/2 NOT dirty). smp8 re-gate PASS (20/20 default + ubsan, 0 corruption). Next: G-5 (graphics audit round)
(Aurora renderer MVP).

## Landed chunks

| Commit | Chunk | What | Tests |
|---|---|---|---|
| *(pending)* | #30 | **cora's unprovisionable home** (USER-REPORTED: `listen on /srv/home-cora failed: Permission denied`, rc=-207, while michael's home bound fine). Root: the ONE shared boot `SrvRegistry` (every getty login `territory_clone`s joey's territory → the same devsrv root Spoor; the "per-session registry" design note was never built) + tombstones-never-free + `SRV_MAX_SERVICES = 8` = EXACTLY full at the login prompt (6 permanent posts [stratum-fs, stratum-ctl, corvus, net, ptyfs, tapestry — the ptyfs+tapestry arcs took slots 7→8's neighbors] + the `pouch-sock-demo` probe tombstone + the login-E2E's `home-michael` tombstone). michael forever REBINDS his tombstone by name (no free slot needed); any OTHER user's fresh `/srv/home-<user>` → `srv_reserve_in` no-slot → `devsrv_create` −1 → the pouch bind's documented EACCES. Ground-truthed live: pre-fix boot reproduced the exact user failure on cora's FIRST interactive attempt while michael succeeded in the same boot. FIX: `SRV_MAX_SERVICES` 8→16 (the ledger + raise discipline in the header comment: each unit costs `srv_registry_drain` kernel stack) + the boot login-E2E parameterized to run a SECOND user (cora AFTER michael — a fresh-name post past the accumulated tombstones, so a capacity regression fails the BOOT) + `devsrv.registry_full_tombstone_rebinds` (the at-capacity asymmetry pinned as tested behavior). The real fix — entry-free-at-last-handle-ref (or the per-session registries the A-5b-body note intended, which also closes the cross-session /srv name visibility) — is the recorded v1.x seam (task #33). Also surfaced: devsrv has no `.readdir` (`ls /srv` → I/O error; task #32). | 1181/1181 (+1: the asymmetry test) + boot OK with BOTH E2E legs (`login E2E OK for michael` + `for cora`); post-fix in-guest E2E: cora authed + home bound + `pwd`→`/home/cora` + logout + michael bound in the SAME boot; the 16→8 revert-probe fails the boot LOUDLY (with the cora leg's tombstone in the ledger, an 8-slot registry fills before ptyfs's own post — `post /srv/ptyfs FAILED` → boot-fatal; a capacity regression cannot slip through as a silent user-facing denial again); reduced SMP amplifier |
| b1d16c0e | #31 | **The live-display controlq fix** (USER-REPORTED under `-display cocoa`: ~2s of console then scanout death). Root: `gpu.rs::submit_and_wait` broke its wait on the ISR HINT then asserted a SINGLE `used.idx` read — a mis-timed wake (stale latched INTx edge / mid-propagation store, routine under a live display backend) read `used.idx` behind → hard-fail → `seq` desync → every later command re-published a consumed avail idx + read its own zeroed response as `resp_type=0x0` (the whole observed cascade, mechanically confirmed). FIX: the used ring is the completion authority — re-read behind `virtio_rmb` per wake + a bounded spin (`USED_SPIN_PER_WAKE`), bounded by `MAX_STALE_WAKES_PER_SUBMIT`; any submit failure LATCHES `Controlq.dead` (fail-fast, no garbage cascade). Aurora: a failed present = a DROPPED FRAME (dirty rows kept → re-render+retry; decay-logged; 240-consecutive backstop; event-stream-EOF still exits; first present stays fatal). The launcher grows first-class display modes: `THYLACINE_DISPLAY=none\|cocoa\|vnc:N` (subsumes the user's local cocoa edit; vnc drops `gpu-mmio0` so gpu0 binds QemuConsole 0) + `tools/rfb-refresh.py` + **`ls-gfx-live.exp`** (the live-display CI leg the headless gate lacked: RFB attach asserted on the gpu0 geometry + session churn under 45s of real display traffic + zero-desync sweep + console-survives). HONEST LIMIT: three escalating headless repro attempts (screendump hammer @66/s = 2x cocoa cadence; a REAL live VNC backend on the right console at 25MB of dirty-rect traffic) never fired the pre-fix bug — the trigger needs the cocoa backend's thread topology; the leg is standing coverage, the cocoa acceptance is the user's window. Kernel byte-unchanged. | test.sh PASS (boot OK + the `-c` gate; exit 0) + ls-gfx PASS + **ls-gfx-live PASS** + a post-fix manual VNC stress run clean (3.3k update-reqs, 45 bursts, zero desync strings) |
| 2961256e | G-3d | The audit close (Fable-5-max holotype, MODEL start==end, + concurrent self-audit): **0 P0 / 0 P1 / 1 P2 / 3 P3, NOT dirty, all addressed** -- F1 [P2] the reaper's per-page TLBI unmap ran IRQs-OFF under gptl (proc_for_each is irqsave) -> FIXED via the find-and-lock callback (vma_lock taken under the walk, HELD PAST it; the unmap runs IRQs-on under vma_lock alone) enabled by `vma_drain` now TAKING `p->vma_lock` (exemption retired; pgtable_destroy is after vma_drain in proc_free, so pgtable_root stays valid); F2/F4 contract rewording (the budget DOES charge the orphan; liveness = "a client whose session OBSERVED the death"); F3 global FRAME coalesce; + self-audit SA-12 (exact gather guard) + SA-15 (the libtapestry `closed` latch). `memory/audit_g3_closed_list.md` = the G-4/G-5 preamble | 1173/1173 + boot OK + the evolved gate GREEN on the fixed code; the reduced amplifier (default-smp4 + ubsan-smp4 N=10) 20/20 PASS 0 corruption on the close delta (the FULL 40/40 ran on the pre-fix commits) |
| 7f0a1b1d | G-4a | The `/dev/cons` drain/feed renderer backend: the `cons_emit` mirror TAP (output + echo; the tee -- serial byte-identical) into a bounded drop-oldest ring + the feed into the UNCHANGED LS-8 discipline (`is_break` hardwired false -- no SAK forgery) + `SPAWN_PERM_CONSOLE_RENDERER` (the I-27 THIRD role: console-attach-only + single-holder CAS) + the devdev `consdrain`/`consfeed` leaves (open + per-I/O + poll gates) + the LS-8a deferred poll-wake second instance | 7 new kernel tests; suite 1180/1180 |
| d3c51ee0 | G-4b | The Cornucopia bake: `tools/bake-cornucopia.py` (outline flatten -> nonzero scanline fill at 4x supersample -> 8-bit alpha) -> the COMMITTED atlas (`usr/lib/cornucopia`, 207 glyphs, cell 10x22 baseline 18 from the font's own metrics); box drawing deliberately procedural | host-side; atlas visually verified (ASCII-art render) |
| 9a9b6d29 | G-4c | `usr/aurora` -- the renderer MVP: VT subset (truecolor SGR, real alt-screen, DECSTBM-ignored seam) + Bonfire palette + atlas blit + procedural U+2500-259F + blinking cursor; the loop BLOCKS on the Loom CQ (the non-SQPOLL pump -- a poll-only loop starves its own completions, measured); joey spawns it perm-granted as the resident boot presenter (demo stays baked, no longer spawns); run-vm keeps kbd-pci0 (un-targeted QMP injection reaches it under -nographic) | first integrated boot: "Thylacine login:" in Cornucopia at 128x36 + blink + QMP-typed input echoed on BOTH sinks |
| 464a24ac | G-4d | The fbcon gates: `screendump.sh -c` (the Aurora CONSOLE signature -- exact Bonfire bg dominant + exact-fg text; content-independent, deterministic) + `test.sh`'s per-boot gate -> `-c` + a bounded retry-COMPARE liveness (cursor blink / prompt arrival); `qmp-sendtext.sh` (un-targeted QMP typing); `ls-gfx.exp` (the E2E); docs (140-aurora.md new + TAPESTRY 18.7 + ARCH 25.4 + I-27 3-role + CLAUDE mirror + TOOLING) | **ls-gfx PASS under HVF** (login + ls rendered [-c + differing dumps] + QMP-typed pwd -> /home/michael on the serial tee) |
| 176d982a | G-4e | Self-audit hardening: proc_set_console_renderer magic/ALIVE asserts + the cons_drain_open reader_busy epoch soundness comment (the #844 pin + #811 death-unwind ordering excludes a prior-epoch reader) + the CLAUDE.md audit-trigger mirror row | suite 1180/1180 + boot OK (HVF) |
| 5cc97785 | G-4f | Audit CLOSE: Fable-5-max holotype 0 P0 / 0 P1 / 1 P2 / 2 P3, NOT dirty (kernel drain/feed/gate/role surface CLEAN). F1[P2] aurora VT erase(1) OOB (deferred-wrap cx==cols -> [..=len] panic, reachable from any console writer) -> clamp + vt.rs tests (dormant host-harness seam); F2[P3] cornucopia verify() extents/geometry; F3[P3] drain single-open footguns -> DEFERRED task #29. ALSO SMP-hardened the PRE-EXISTING srvconn.client_send_blocking_poll_edge (smp8 exact-sched over-specification; task #28) | 1180/1180 + boot OK + spec GREEN; pre-fix SMP 0-corruption/40; smp8 re-gate 20/20 PASS (ff35eab7) |
| 88547181 | G-3a | tapestryd stage 0: NEW `usr/tapestryd` (gather-bound persistent compositor -- the absorbed gpud GPU half generalized [per-surface resources, whole-weave backing, offset TRANSFER, the retire pair]; poll-mode virtio-keyboard-PCI eventq; the /dev/tapestry 9P server with the I-40 present engine [synchronous presents => the quiesce set empty at every retire BY CONSTRUCTION; UNSHARE-before-free retire ordering; scanout at first-present-COMPLETE], F2 per-conn scoping + generation gates, F9 caps, R2-F4 WEDGE; the US-QWERTY keymap + FRAME clock) + NEW `usr/lib/libtapestry` + `usr/tapestry-demo` (the POC model cashed onto libthyla_rs::loom; quadrants + live plasma -- the G-2-F2 Tweft round-trip E2E discharged) + the libdriver/warden `gather = all` mode (resolve_gathered + pci_extra + the pcix codec key; ONE grant over both PCI functions) + the devdev tapestry mount stub + the joey mount/probe/spawn + virtio-keyboard-pci in run-vm.sh + the EVOLVED pattern gate (liveness double-dump) + gpud retired | Suite 1173/1173 + boot OK at 1280x800; the gate GREEN (quadrants through the full path + the dumps differ); 86/86 libdriver host tests incl. 4 gather legs; spec gate GREEN (tapestry 5413 unperturbed + liveness + 4 buggy; weft + readiness families) |
| 6140cef2 | G-3b | The R2-F3 orphaned-weave reaper (the ServerDeath leg's kernel half): WEAVE bindings register at the SYS_WEFT_MAP CAS-win / unregister at dev9p_close; a kproc kthread (parks empty; 1 s cadence) force-reclaims a binding whose serving session is dead past the 2 s grace -- cross-Proc unmap under gptl+vma_lock with the G-2-F1 identity guard re-checked, budget uncharged, the registration pin dropped (the chunk frees AT reclaim); `g_weft_reap_lock` serializes reaper-vs-close (burrow NULLed vs unregister-before-read) | The 3 sweep-driven regressions `weft.reap_{orphan_reclaimed,live_session_untouched,close_unregisters}` (in the 1173) |
| b2c8a6f5 | G-1 | The resident GPU driver: NEW `usr/gpud` (libdriver persistent; the netd precedent) — probe = `PciDev::claim(16)` + the common-cfg handshake + the 7-command 2D bring-up (the P4-L pattern); serve = READY then the 2 Hz render loop (8×8 center heartbeat, compile-asserted clear of the `-v` sample points; partial-rect TRANSFER+FLUSH, IRQ-completed). **The transport pivot, measured not theorized**: a persistent claimant on the shared virtio-mmio slot page (`0xa003000`, all six devices, page-exclusive claims) starved netdev-driver AND stratumd's disk (rc=-207 boot-fatal) → **virtio-gpu-pci** (`virtio-pci:16`, own BARs — the netd/#140 move); the MMIO device stays as `gpu-mmio0` for the one-shot kernel-test probe. crash-probe re-homed to the warden-SYNTHETIC `restart-test` node (F15: resource-less DeviceNode; grant mmio=0 irq=0 dma=0). The **pattern-persists gate** in `tools/test.sh` (screendump `-v` post-banner, bounded retry, `gpu-gate` FAIL arm; skips under NO_GPU/NO_QMP/GPU_GATE=0) — runs in every ci-smp-gate boot. TAPESTRY §18.9 as-built note (G-3's tapestryd binds `virtio-pci:16`); `docs/reference/138-gpud.md`. Kernel byte-unchanged. | Suite 1170/1170 + boot OK; the pattern gate GREEN inside test.sh; ecosystem restored (netdev ARP 24/24 + stratumd serving + crash-probe synthetic ladder); liveness pixel-proven (block(64,64) yellow↔black across a tick, neighbors stable); reduced SMP amplifier |
| 5317ea7d | G-0 | The "agentic eyes" capture step: `tools/screendump.sh` — QMP `screendump` to PNG over the standing `build/qmp.sock` (P4-K wiring), targeting the `gpu0` qdev id; `-v` asserts the P4-L 4-quadrant pattern via a PPM sibling dump (quadrant-center sampling, tol ≤8). TOOLING.md §3.1 addendum. **Headless capture EMPIRICALLY PROVEN**: `VERIFY OK — 128x128 shows the P4-L 4-quadrant pattern` captured under the standing `-nographic` boot (no display backend, no VNC — a VNC-listener differential confirmed listener-independence; HVF and TCG both). **The investigation's load-bearing finding**: a post-reap capture reads a blank surface NOT because headless capture fails but because the **RW-7 proc-death quiesce** (`kernel/virtio.c::virtio_mmio_reset_in_range`) resets the dying driver's virtio devices (DMA soundness: no in-flight device write into freed KObj_DMA pages) — a virtio-gpu reset destroys host-side resources + disables the scanout. Ground-truthed by a held-alive instrumented probe run: guest read-back = pattern, QEMU `xp` at `fb_pa` = pattern, screendump = pattern while alive; black only after reap; kernel/test/QEMU all exonerated as bugs — the behavior is correct + audited. So **a scanout lives exactly as long as its driving Proc** — the persistent capture target is G-1's resident driver (its "pattern persists" gate re-runs `-v` for keeps), and a compositor crash blanks the display until warden restarts it (the TAPESTRY crash contract's visible half). The one-shot probe gained only a comment recording the caveat; instrumentation reverted. | The captured artifact `build/screendump-g0-held.png` (VERIFY OK, all four quadrants exact); harness exercised against live/absent/reaped-scanout VMs; suite + boot unchanged (kernel byte-identical; probe binary byte-identical minus comment) |
| 6599519d + the close commit | G-2 | The DMA-weave share admission: `SYS_DMA_CREATE_WEAVE` (99; the kernel-minted create-immutable `KObj_DMA.weave` subtype — the §18.12 R2-F1 structural close; ABI user-signed-off 2026-07-19) + the `burrow_share_into`/`SYS_WEFT_SHARE` admission gates (ANON or weave ONLY; device-command DMA as unshareable as MMIO) + `weft_claimed_kind` (type-authoritative kind + server-declaration cross-check) + the WEAVE binding kind (no ring view; the kind-gated `validate_rw` closes all three Tweftio fast paths — §18.11 F10) + `SYS_WEFT_UNSHARE` (100; the F3/R2-F5 disarm: removal-before-free + fail-closed late claim = the `Map`-guard NoStaleMap kernel half; closes #289) + the per-client shared-in budget (`Proc.shared_map_pages` @348, 128 MiB, the I-32 fifth axis — R2-F3) + the weave-fid clunk-unmap (`ClunkMap`, F1-identity-guarded via `weft_binding_clunk_unmap`) + `libthyla_rs::{t_dma_create_weave, t_weft_unshare}`. ARCH §28 **I-40** enumerated (share half ENFORCED; present half at G-3); CLAUDE.md + ARCH §25.4 Weft-row addenda; `docs/reference/125-weft.md` "The weave share (G-2)"; SPEC-TO-CODE `tapestry_present.tla` section. **AUDIT CLOSED CLEAN: Fable-5-max holotype (MODEL start==end) + concurrent self-audit — 0 P0 / 0 P1 / 0 P2 / 3 P3, NOT dirty** (F1 stale-VA clunk-unmap → FIXED + revert-probed regression; F2 integration coverage → the clunk-unmap half discharged, the Tweft-round-trip E2E G-3-owed; F3 bare-pid → closed-as-documented). `memory/audit_g2_closed_list.md`. | 1170/1170 (+5: the 4 gate/budget regressions + `weft.weave_clunk_unmap_guard`; BOTH probe families revert-probed — the gate weakenings → 1167/1169, the F1 guard drop → 1169/1170, each failing exactly its targets) + boot OK; spec gate GREEN (tapestry 5413 + liveness + 4 buggy firing; weft 1412 + readiness re-ran); FULL SMP gate 40/40 PASS 0 corruption + the reduced amplifier on the F1 delta |

## Remaining work

| # | Chunk | Scope | Gate |
|---|---|---|---|
| G-5 | Graphics audit round | The reserved rows enforced; the I-40 present half + tapestryd/cons-backend focused audit (the #31 controlq wait-loop rework joins the scope); SMP gate | clean close |
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
