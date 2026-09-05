---
id: spec-tapestry-present
type: spec
title: "tapestry_present.tla"
models: [sub-tapestryd, sub-kernel-weft]
pins: [inv-i40]
cfgs:
  - "tapestry_present.cfg -- clean: destroy + reweave both enabled, 5413 states"
  - "tapestry_present_liveness.cfg -- EventuallyRetired: a destroy always drains to gone"
  - "tapestry_present_buggy_premature_reuse.cfg -- RecycleGate violated: the slot recycles at submit-ack, not at the terminal CQE"
  - "tapestry_present_buggy_retire_during_transfer.cfg -- NoTornScanout violated: the destroy path frees pages the host is still reading"
  - "tapestry_present_buggy_reweave_without_quiesce.cfg -- NoTornScanout / DisplayedBacked violated: the old weave frees the moment the new one exists"
  - "tapestry_present_buggy_map_after_retire.cfg -- NoStaleMap violated: a claim token resolves against a retiring weave"
gate: "any change to the retire ordering, the reweave fence, or the present completion model — above all, any move to a pipelined controlq"
created: 2026-08-02
updated: 2026-08-02
---
## Abstraction

A weave's whole life as four booleans — `backed`, `serverRef`, `mapped`,
`armed` — plus a small set of in-flight presents and a `displayed`
pointer. Two generations (`g1`, `g2`) so a reweave has somewhere to put
the old one.

The shape of the danger is a page with **three independent holders**: the
server's own mapping, the client's mapping, and the host's DMA read
during a transfer. Each of the four buggy configs is one holder being
forgotten:

- `RecycleGate` — the client reuses a slot the host is still reading.
  The D1 recycle gate is the *terminal* completion, not the submit ack.
- `NoTornScanout` — the destroy path frees pages under a live transfer
  or a live scanout reference.
- `NoTornScanout` / `DisplayedBacked` again, via the reweave path — the
  old generation freed the instant the new one exists, while the display
  is still composing from it.
- `NoStaleMap` — a claim token that resolves against a weave already
  retiring. The claim raced teardown and won.

`NoStaleMap` is the one that reaches across the process boundary: the
token is kernel-side and the claim is the client's `SYS_WEFT_MAP`, so
the property is only true if the server's unshare *precedes* its page
free. That ordering is the reason `retire`'s five steps are in the order
they are in.

## Action-site map

| Action | Site |
|---|---|
| `WeaveFirst` | `tapestryd server.rs::Comp::create` → `alloc_weave` — DMA create + map + zero + `RESOURCE_CREATE_2D` + whole-weave `ATTACH_BACKING` |
| `Reweave` | `Comp::resize_ack` — mint the new generation first, displace the old |
| the arming half of `Map` | `Comp::weft_ensure` — `t_weft_share` once, the stored id echoed thereafter (lazy, at the first `Tweft`) |
| `Submit` / `Complete` | `Conn::present` — validate → `transfer` → `flush` → `Rwrite`, all inside one dispatch |
| `RetireDisplaced` | `present`'s tail — the first post-fence present drops the displaced generation |
| `Destroy` / `ServerRelease` | `Comp::retire` and `Comp::release_gen` — the five-step order |
| `ServerDeath` | the kernel weft reaper, force-reclaiming orphaned client mappings after a bounded grace |

## The quiesce is free, and that is a property of the code, not the model

`ServerRelease` is guarded on `intransfer = 0`. In the implementation
that guard is discharged **by construction**: `gpu.rs` is synchronous, so
a present's transfer window opens and closes inside one 9P dispatch, and
the in-flight set is empty at every retire decision point. There is no
drain because there is nothing that can be in flight.

That makes the model's most important guard the one with no
corresponding code. It is satisfied by an architectural fact rather than
by a check — which is fine, and is stated in both the module header and
the server's, but it means **the guard cannot fail loudly.** A pipelined
controlq would not make `ServerRelease` false; it would make it
unimplemented, silently, with the spec still green because the spec is
not the thing that changed.

Both source files carry the warning. This note carries it too, because
the gate on this module is not "did the spec change" — it is "did the
construction that discharges it survive."

## The fence

`Reweave` needs the new generation visible to every subsequent present.
The implementation gets that from reply ordering rather than from
tagging: `resize_ack`'s `Rwrite` completes only after the allocation, and
the connection's frame stream is FIFO, so a client that has read the ack
cannot have a pre-fence present still arriving. `ReweaveOrdered`
(`g2 ≠ none ⇒ g1 ≠ none`) is the model's bound on how many generations
can exist; `E_AGAIN` on a second reweave is the code's.

The displaced generation is never read again but stays *displayed* until
the first post-fence present — which is what keeps the reweave
tearing-free, and is also why it cannot simply be freed at ack time
(`buggy_reweave_without_quiesce`).

## Beneath the model

Everything about *pixels*: the composed blit, the letterbox, the damage
rects, the multi-rect validation. The pane tree and the layout algebra.
The event queue and its never-drop policy. The per-connection ownership
gate — F2 is an isolation property, not a lifetime one, and lives in
[[sub-tapestryd]]. The virtio command engine's own correctness, including
the used-ring-not-ISR completion authority whose absence once desynced
the engine permanently (#31).

The kernel half of the share — `SYS_WEFT_SHARE`, the consume-once claim,
the cross-process dual refcount that actually frees the pages — is
[[spec-weft]]'s and #847's.
