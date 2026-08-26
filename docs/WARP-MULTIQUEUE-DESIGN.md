# Warp multi-queue submit-fence redesign (the F3 seam) -- design foundation

**Status: DESIGN FOUNDATION (not yet scripture-ratified).** Operator greenlit
"pull multi-queue forward" for the GPU-submit chunk (2026-08-26) and directed the
IMPLEMENTATION to a fresh session. This doc is the design foundation that session
implements against; the open forks in section G still need the operator's
decision (they are the design conversation to run BEFORE code -- scripture before
code). Research: the design-brief fork this session + my direct reads; every
claim carries file:line -- re-verify before relying.

Prerequisite context: `docs/WARP-V3-DESIGN.md` (the Warp arc), the V-3b-3c-2b
close (`e34760d8` + mesa `d7f4ef1`/patch 0014), `memory/audit_v3b3c2b_closed_list.md`
(F1-F9). This redesign is I-45-bearing -> its own holotype audit.

## A. The decisive finding: NEW MECHANISM required (a redesign, not a mapping)

Neither existing fence path carries a per-VkQueue GPU-completion signal:

- **The per-ring fence file (`ring/<ridx>/fence` + `completed_seq` + `poll_ring_fences`)
  is the V-3a echo-drain ACK, and is structurally DISABLED for Venus (host3d)
  rings.** `completed_seq` advances only in `wring_complete` (server.rs ~6961),
  driven by the `wring_kick` echo-drain ("V-3a acknowledges (echo)", ~6921) -- a
  CONSUMPTION ack, not GPU completion. And `wring_kick` returns **E_OPNOTSUPP for
  a host3d ring** (server.rs ~6876: "virglrenderer POLLS a host3d ring, so its
  kick verb is PERMANENTLY E_OPNOTSUPP"). So a Venus ring's `completed_seq` never
  advances -> `WFK_RING_FENCE` parks forever.
- **The real Venus GPU-completion fence is CTX-WIDE and carries no queue
  identity.** `warp_venus_submit` -> `gpu.submit_3d` -> `FenceTag { fence_id,
  ctx_pub, readback, comp, abandoned, ok }` (gpu.rs ~785) -- **no ring_idx**.
  `warp_service_fences` retires by matching `tag.ctx_pub` (server.rs ~8199) -> a
  per-CTX `fence_signaled++` (~8234). The #210 dense-count fix explicitly assumes
  ONE ring per ctx ("completions are FIFO within the single ring", ~8227).

So a per-queue fence requires threading queue identity through the GPU-completion
pump: a cross-repo change to `FenceTag`, `warp_service_fences`, and the per-ctx
counter. This is the mechanism that does not exist today.

## B. The current fence model (both paths)

**Path 1 -- ctx-wide Venus GPU-completion (THE REAL ONE).** mesa
`thylacine_submit` (vn_renderer_thylacine.c ~620) -> `warp_venus_submit(cs)`
returns a monotonic seq; syncs bind to it (~627), NO ring_idx. Server:
`gpu.submit_3d(venus_ctx, ctx_pub, stream)` -> `FenceTag { ctx_pub }`; retire via
`warp_service_fences` keyed by ctx_pub -> per-ctx `fence_signaled` (WarpCtx field
~1456). Client observes: `ctx/<id>/ctl` `fence-signaled` (~9669) or a blocking
`ctx/<id>/fence` (WFK_FENCE ~9774). **Retires on GPU-WORK completion** (gpu.rs
~365, VIRTIO_GPU_FLAG_FENCE -- the host does not complete the response until its
fence signals). So single-queue copy->submit->wait->map IS sound today.

**Path 2 -- per-ring V-3a echo (superseded for Venus).** Ring header
HEAD/TAIL/IDLE/SEQ; `wring_kick` echo-drain -> `wring_complete` ->
`completed_seq++`; E_OPNOTSUPP for host3d; observe via `ring/<ridx>/fence`
(WFK_RING_FENCE) + `poll_ring_fences`. IDLE/consumption-keyed, NOT GPU-completion,
and never driven for a Venus ring.

## C. The Venus queue -> ring_idx mapping (mesa) + the namespace TRAP

`batch->ring_idx` (vn_renderer.h ~99) identifies a TIMELINE, "bound during VkQueue
creation"; ring_idx 0 = the CPU timeline. `vn_instance_acquire_ring_idx`
(vn_instance.h ~102): `ffsll(~used_mask)-1`, gated `>= max_timeline_count -> -1`;
ring 0 reserved. **`max_timeline_count=2` -> only ring_idx 1 -> ONE queue** (the
V-3b-3b F3 cap-of-1 we lifted 1->2 at V-3b-3c-2b). Acquired at device create
(vn_device.c ~83); `submit->external_payload.ring_idx = queue->ring_idx`
(vn_queue.c ~521) -> `batch->ring_idx` (~1923). **`thylacine_submit` ignores it.**

**TRAP (undocumented today):** mesa's `ring_idx` is a TIMELINE index (0=CPU,
1..N=queues); tapestryd's `ridx` is a HOST3D RING SLOT (0-63, `WARP_RINGS_PER_CTX`).
Two namespaces that overlap numerically -- the redesign MUST NOT conflate them.
The mapping (venus_ctx, mesa-timeline-ring_idx) -> tapestryd-host3d-ridx must be
explicit.

## D. The minimal redesign delta

**mesa (vn_renderer_thylacine.c + warp_client.{c,h}):**
- `thylacine_submit` passes `batch->ring_idx` to `warp_venus_submit`.
- The one ctx-wide `tly->warp.issued`/`signaled` pair becomes PER-RING
  (`issued[ring]`/`signaled[ring]` on `warp_conn`); the sync-only "bind to last
  issued" arm (~588-610) and `thylacine_sync.signal_seq` become per-ring.
- `thylacine_wait` (~637) waits per-ring.
- Lift `max_timeline_count` (see fork G.1).
- The submit verb + the fence read carry ring_idx.
- **The owed F6/F8 per-renderer mutex becomes REQUIRED** (multiple queues =
  concurrent submit threads) -- now a DEPENDENCY of this chunk, not a seam.

**tapestryd (server.rs + gpu.rs):**
- `warp_venus_submit` accepts ring_idx -> `gpu.submit_3d`.
- **Add ring_idx to `FenceTag`** + thread submit->completion.
- `warp_service_fences` retires per-(ctx, ring) -> **`fence_signaled` becomes
  per-ring** (array/map on WarpCtx); re-establish the #210 dense-count invariant
  PER RING.
- Expose per-ring signaled; map (venus_ctx, ring_idx) -> the queue's host3d ring.
- **KEEP** the ctx-wide fence for ring_idx 0 (CPU timeline) + teardown -- the
  change is ADDITIVE.

## E. Options for the per-queue fence + recommendation

1. **Per-ring `FenceTag` + per-ring ctx counter (extend the completion pump).**
   Add ring_idx to FenceTag; retire per-(ctx,ring); `fence_signaled[ring]`.
   Soundest -- the fence lands where GPU-completion actually is (the pump), reuses
   the pump, never misuses the echo. Most work.
2. **Reuse `ring/<ridx>/fence` for host3d, sourced from the pump.** Keep the fence
   FILE + `poll_ring_fences` (park/poll already per-ring); for a host3d ring, feed
   `completed_seq` from `warp_service_fences` instead of the disabled echo.
   Reuses the most ABI, but `completed_seq` then means TWO different things by ring
   flavor (echo vs GPU-completion) -- a #254 "true-of-the-wrong-flavor" footgun
   needing a hard guard. Requires Option 1's FenceTag change anyway.
3. **Keep ctx-wide, over-wait client-side (no server change).** Cheapest, but a
   wait on idle queue A blocks on busy queue B -- serialized-queue, not true
   multi-queue. REJECT for a real multi-queue milestone; acceptable only as a
   documented interim.

**RECOMMENDATION: Option 1 as the mechanism, exposed through the existing
`ring/<ridx>/fence` file (Option 2's file+poll reuse) for host3d rings, sourced
from the pump.** The new mechanism is narrow -- a ring_idx on FenceTag + a
per-ring `fence_signaled` -- reusing the completion pump, the fence file, and the
park/poll delivery. **HARD GUARD:** a host3d ring's `completed_seq` comes from the
GPU pump, NEVER the echo (which stays E_OPNOTSUPP); a `_Static_assert`/runtime
guard keyed on the ring flavor at the source. (This fork is G.2 -- operator's ABI
call.)

## F. I-45 isolation delta -- NO weakening

The isolation unit stays the **CONTEXT**, not the queue. All queues of one ctx
share `venus_ctx` + the ctx's buffers (mutually trusting, like threads sharing an
address space). Per-queue fences are a **liveness/correctness** feature (don't
conflate independent queues' completion), NOT a new authority boundary. So
GPU-DESIGN section 8 / the I-45 row ("a context's fault is fatal to that context
alone"; "a submission executes only against buffers attached to the submitting
context") are UNCHANGED -- ring_idx selects a timeline WITHIN the ctx. **The audit
must confirm:** ring_idx stays ctx-local (resolved under the existing `owner_conn`
gate, like today's ridx) and cannot name another ctx's ring/timeline.

## G. Open forks -- the design conversation to run BEFORE code (operator's call)

1. **How many queues for v1.0?** Bounds `max_timeline_count` (e.g.
   graphics+compute+transfer = 3 queues -> count 4, since ring 0 = CPU). Server
   bound is `WARP_RINGS_PER_CTX=64`. My default rec: 4 (3 queues), tunable.
2. **Fence-file ABI (ESCALATION-WORTHY -- ABI surface).** Option 1+2 hybrid
   (recommended, section E) vs a NEW per-ring venus-fence file vs a ring_idx ARG
   on `ctx/<id>/fence`. Overloading `completed_seq` (Option 2) needs the hard
   flavor guard.
3. **Pull the F6/F8 per-renderer mutex into this chunk?** It is now a DEPENDENCY
   (concurrent multi-queue submits), not a separable seam -> my rec: YES, pull it
   forward (the "pull dependencies forward" default). Confirm.
4. **Does #210's per-ring FIFO hold when the host interleaves queues?** Needs
   per-queue FIFO retirement -- virglrenderer-internal; verify or bound at the
   audit. The comment "FIFO within the single ring" (server.rs ~8227) is true
   today but becomes a load-bearing multi-queue assumption the redesign must
   defuse.

## H. Comment-vs-code flags (from the research)

- #210's "FIFO within the single ring" (server.rs ~8227) -- true today, becomes a
  load-bearing multi-queue assumption to defuse.
- vn_renderer.h ~100-103 describes per-queue signaling `thylacine_submit` does NOT
  yet honor -- the comment is the spec, the code is behind it (the redesign brings
  the code up to the comment).
- The mesa-timeline-idx vs tapestryd-ridx namespace overlap (section C) is
  undocumented -- document it at implementation.

## I. The chunk also carries (not the redesign, but land together)

- **The F1 fix-proof** (the high-value witness the V-3b-3c-2b audit chain owes):
  allocate HOST_VISIBLE -> vkCmdCopyBuffer a pattern in -> vkQueueSubmit ->
  vkWaitForFences -> FIRST vkMapMemory -> the pattern SURVIVES (proves reify-at-
  alloc beat the mint-at-map zeroing; the allocate+map E2E cannot show this).
  Single-queue suffices for the proof; it rides the new multi-queue path.
- The F2 cap-exhaustion witness (allocate past 64 MiB -> free -> realloc) + the
  F4 placed-map refusal test -- both fit here.
- The hostile-vkWaitRingSeqnoMESA server-park (I-45 isolation) analysis -- owed.
