---
id: fnd-harc-r1-a1
type: fnd
title: "the composed GPU arm whole-op-blits a slot resource that never received the full frame -- the exact class the zoom fix newly routes through the letterbox arm (a single-slot client's partial-first present at a size mismatch composes untransferred host bytes, scaled)"
round: adt-harc-r1
severity: P1
status: fixed
surface: [sub-tapestryd]
threatens: []
fixed-by: chg-2026-09-06-harc-audit-close-r1
regression: "ls-gfx-panes `singleslot` partial-first client E (the CPU-path half: no latch, the quadrant lands through the scale); the GPU-path half -- a stale slot resource blitted whole -- has no HVF witness and is owed on the GL host"
created: 2026-09-06
---
## Prosecution

**File**: usr/tapestryd/src/server.rs (the GPU composed arm: `for &(x, y, pw, ph) in &rects { comp.gpu.transfer(res, offset, x, y, pw, ph) }` then `compose_gpu_slot_words(*op, res, ..)` -- a whole-op blit of `res_ids[slot]`; the direct arm's `let xfer = if stale { vec![(0, 0, w, h)] } else { rects.clone() }`; the `res_stale` writers: `create` on a GL host marks every slot stale, any CPU-arm present re-marks all; the composed arm's update `s.res_stale[slot] = !full`)
**Invariant**: I-40 present half (the compose reads only content a present transferred); I-45 guest-exposure adjacent (the host copy's untransferred bytes are the witness tokens or recycled texture)
**Prosecution**:
1. State: on a GL host every slot resource starts stale; a hide, and any CPU-arm present, re-mark all slots stale.
2. A single-slot client (thyla_tap presents slot 0 only) at a size mismatch presents PARTIAL damage first (a partial-first app, or DOSBox-X right after a hide/reveal).
3. The latch correctly does not trip (one slot), so `compose_geometry` takes the letterbox arm with `src` = the whole surface.
4. The GPU arm transfers only the damage, never reads `res_stale[slot]`, and blits the WHOLE op: the untransferred host region (tokens / the pre-hide frame / undefined texture) is composed, scaled, into the pane.
5. Pre-fix unreachable: a partial present latched before routing, so a scaled op implied a full transfer. The `singleslot` leg cannot see it (D presents FULL first) and ls-gfx-panes never runs on the GPU-composed host.
**Suggested fix**: expand the transfer to `(0,0,w,h)` before the op when `res_stale[slot]`, and make the update exact -- the current `= !full` would make the expansion fire on every partial present.

## Disposition

Fixed at the close: the composed GPU arm expands a stale slot's transfer to the full surface exactly as the direct arm does, and the slot is un-staled after any successful transfer (the host copy then mirrors the guest slot: a stale one was just transferred in full, a fresh one was complete before and received everything the client changed since) -- the old `= !full` re-marked it on every partial present and would have made the expansion fire on every other one. The CPU-path arm (E, partial-first, letterboxed, no latch, the quadrant through the scale) rides ls-gfx-panes; the GPU-path witness needs the GL host and is owed with aux's DOSBox-X re-run.
