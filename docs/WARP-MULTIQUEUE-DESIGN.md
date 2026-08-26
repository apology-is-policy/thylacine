# Warp multi-queue submit-fence redesign (the F3 seam) -- design foundation

**Status: RATIFIED (2026-08-26; the fence-ABI half re-voted the same day).**
Operator greenlit "pull multi-queue forward" for the GPU-submit chunk and voted
the two open ABI/scope forks (section G); the design is binding for the
implementation. Decisions: (1) 4 timelines / 3 queues (max_timeline_count = 4);
(2) fence ABI **v2 -- the timelines file + the shared park** (the first vote's
ring/<ridx>/fence hybrid was REFUTED by the implementation code-read -- a queue
timeline has no host3d ring -- and re-escalated the same day; section E.1 has
the refutation + the ratified shape); (3) the F6/F8 per-renderer mutex is
PULLED IN (now a dependency of concurrent multi-queue submits); (4) #210's
per-ring FIFO is a risk to BOUND at the I-45 audit, not a pre-code decision.
Research: the design-brief fork + direct reads; every claim carries file:line --
re-verify before relying.

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

(Updated for the ratified-v2 exposure, section E.1.)

**mesa (vn_renderer_thylacine.c + warp_client.{c,h}):**
- `thylacine_submit` passes `batch->ring_idx` (the TIMELINE) to
  `warp_venus_submit`.
- The one ctx-wide `tly->warp.issued`/`signaled` pair becomes PER-TIMELINE
  (`issued[t]`/`signaled[t]` on `warp_conn`); the sync-only "bind to last
  issued" arm (~588-610) and `thylacine_sync` bind (timeline, seq), not a bare
  seq.
- `thylacine_wait` (~637) waits per-(timeline, seq) through the one-parker
  fence-waiter protocol (E.1): a single thread parks the shared fence file
  with the transport mutex dropped; wakers re-read `ctx/<id>/timelines`.
- Lift `max_timeline_count` to 4 (G.1) + stop forcing
  `supports_multiple_timelines` to 0 (the seam now carries timelines; the
  field is only asserted by other backends, never branched on by the driver
  core -- but the forced 0 is a false statement once this lands).
- The submit verb carries the timeline index.
- **The F6/F8 per-renderer mutex lands here as the TRANSPORT mutex** (short
  ops only -- never held across the park), plus the fence-waiter condvar.

**tapestryd (server.rs + gpu.rs):**
- `warp_venus_submit` accepts the timeline -> `gpu.submit_3d` (which sets the
  virtio-gpu hdr `ring_idx` + `VIRTIO_GPU_FLAG_INFO_RING_IDX` for a nonzero
  timeline -- the standard virtio-gpu per-context fence ring).
- **Add ring_idx to `FenceTag`** + thread submit->completion.
- `warp_service_fences` retires per-(ctx, timeline): bump BOTH
  `timeline_signaled[t]` and the existing ctx-wide `fence_signaled` total (the
  park file + every existing consumer unbroken); re-establish the #210
  dense-count invariant PER TIMELINE.
- NEW read-only `ctx/<id>/timelines` file (one `timeline <t> <signaled>` row
  per timeline).
- **KEEP** the ctx-wide fence semantics for timeline 0 + teardown -- the
  change is ADDITIVE; the V-3a ring/echo path untouched.

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

**SUPERSEDED (the first G.2 vote, 2026-08-26 morning):** Option 1's mechanism
exposed through the existing `ring/<ridx>/fence` file for host3d rings. The
implementation code-read REFUTED its premise the same day (see E.1 below): a
queue timeline has NO host3d ring, so there is no ridx for its fence file to
live at. Recorded, not deleted -- the refutation is the load-bearing part.

### E.1 The refutation + the corrected surface (ratified same day)

What the code actually says (re-verified 2026-08-26, mesa tip d7f4ef1):

- **vkQueueSubmit never reaches the renderer submit op.** It is encoded into
  the PRIMARY ring (`vn_submit_vkQueueSubmit(dev->primary_ring, ...)`,
  vn_queue.c:1037) that virglrenderer polls; its GPU-completion wait is the
  venus fence-feedback machinery, not `thylacine_wait`.
- **The renderer op carries nonzero ring_idx only on the sync-export path**:
  `vn_create_sync_file` builds a batch of `vkWaitRingSeqnoMESA` + syncs with
  `.ring_idx = external_payload->ring_idx` (vn_queue.c:1918-1930) -- "signal
  these syncs when the queue's timeline passes the ring seqno".
- **A queue is a pure fence timeline.** `vn_device.c:83-92` acquires a
  timeline index (`vn_instance_acquire_ring_idx`, a bitmask -- no memory) and
  binds it host-side via the protocol (`.ringIdx` on the device-queue info);
  no shmem / host3d ring is minted per queue, ever. Minting one purely as a
  fence-file anchor would be the section-C conflation verbatim.
- The server park machinery is a per-ctx single cursor
  (`fence_signaled`/`fence_reported`, server.rs ~9798: a parked read consumes
  the report) -- two concurrent parked readers would steal each other's wakes.
- The ctx ctl's client-critical prefix is budgeted at 255 bytes and GUARDED
  (server.rs ~9674, F11) -- per-timeline rows do not safely fit there.

**RATIFIED v2 (operator vote 2026-08-26, superseding): the timelines file +
the shared park.** The pump mechanism from the first vote is UNCHANGED --
ring_idx (the TIMELINE) on `FenceTag`, retirement per-(ctx, timeline). The
exposure:

- **Server (additive only):** a retirement bumps BOTH the per-timeline
  `timeline_signaled[t]` and the existing ctx-wide `fence_signaled` total (so
  every existing consumer, the park file included, is unbroken). A NEW tiny
  read-only `ctx/<id>/timelines` file serves one `timeline <t> <signaled>` row
  per timeline -- its own file, its own budget, no interaction with the ctl's
  255-byte prefix discipline. The `ctx/<id>/fence` park file is BYTE-IDENTICAL
  server-side (the audited single-cursor machinery untouched). The V-3a
  `ring/<ridx>/fence` + echo path: untouched entirely -- the flavor-guard
  hazard from the superseded vote vanishes by construction.
- **Client (mesa):** per-timeline `issued[t]`/`signaled[t]`; **one parker at a
  time** -- a fence-waiter protocol (mutex + condvar) where a single thread
  parks on the shared fence file with the TRANSPORT MUTEX DROPPED across the
  park, re-reads `timelines` on wake, publishes to per-timeline waiters. Two
  parked readers are forbidden by construction (the single-cursor server would
  let them steal each other's wakes). Spurious wakes are bounded by
  cross-queue traffic (<= 3 queues at the ratified count).
- **The submit carrier is the FILE, not an in-band index** (implementation
  refinement, same day): the submit payload is opaque bytes (an in-band index
  would change the byte format) and a Twrite offset cannot carry it (the
  client's `t_write` implicit offset already arrives nonzero at this file) --
  so nonzero timelines ride NEW per-timeline write-only files
  `ctx/<id>/submit1..3` (venus-only; a GL client writing one is refused
  E_OPNOTSUPP), and `submit` itself stays timeline 0, byte-identical for
  every existing writer.

### E.2 The two-parked-readers corner (found at implementation; closed both sides)

The pre-code design said "one parker" but the transport's OWN throttle could
park too: `fenced_write` waits for lane room by reading the fence file, and a
venus submit runs under the transport mutex -- so a parker (mutex dropped) plus
a throttling submitter (mutex HELD) made two parked readers. The server's
first-match-consumes sweep then woke exactly one: if the submitter's own fence
retired but the parker consumed the report, the submitter stayed parked HOLDING
THE MUTEX with nothing left in flight -- a permanent instance wedge. Closed at
both layers:

- **Server:** `poll_fences` advances `fence_reported` AFTER the sweep, so one
  retirement wakes EVERY parked reader of the ctx (deliver-to-all). The fence
  file is a doorbell whose content is documented as coalesced-and-unparsed, so
  waking all is exactly doorbell semantics -- and the seam is no longer one
  client bug away from a self-strand (the constraint at the boundary that
  admits the vector).
- **Client:** `warp_venus_submit` is NON-PARKING -- a full throttle or lane
  reports `again` and the renderer runs a one-parker cycle (mutex dropped) and
  retries; with nothing of its own in flight (foreign contention) it yields
  off-mutex instead, since no fence of ours could fill a park. The GL winsys
  path keeps the parking `fenced_write` (single-reader by its own serializing
  mutex).

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

## G. The forks -- RESOLVED (operator vote 2026-08-26; binding)

1. **Queue count for v1.0: 4 timelines / 3 queues** (operator-voted). CPU
   timeline (ring 0) + graphics + async-compute + async-transfer -- the standard
   Vulkan queue-family split. `max_timeline_count = 4` in mesa; the per-ring
   fence state in tapestryd sizes to the rings actually minted (server hard
   bound stays `WARP_RINGS_PER_CTX=64`).
2. **Fence-file ABI: the TIMELINES FILE + SHARED PARK** (operator-voted TWICE
   -- the first vote's hybrid was refuted by the implementation code-read the
   same day and re-escalated; section E.1 carries the refutation + the full
   ratified v2 shape). ring_idx (the timeline) lands on `FenceTag`;
   retirement bumps per-timeline `timeline_signaled[t]` AND the ctx-wide
   total; a NEW read-only `ctx/<id>/timelines` file exposes the per-timeline
   counters; the `ctx/<id>/fence` park file and the whole V-3a
   `ring/<ridx>/fence`/echo path are untouched. Client: one-parker + condvar
   fan-out, transport mutex dropped across the park.
3. **The F6/F8 per-renderer mutex is IN this chunk** (dependency pull-forward
   confirmed): concurrent multi-queue submits make the torn-RMW on
   `warp_conn`/`ring_bitmap`/`mem_bitmap`/the per-ring seq pairs a live defect,
   not a latent one. It lands with the mesa-side per-ring state.
4. **#210's per-ring FIFO under host interleave: bound at the I-45 audit.** Not
   a pre-code decision. The redesign re-establishes the dense-count invariant
   PER RING (section D); the audit must verify the per-ring FIFO assumption
   against virglrenderer's actual retirement order or replace dense counting
   with explicit seq matching where it cannot be proven.

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
