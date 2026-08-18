---- MODULE tapestry_present ----
(***************************************************************************)
(* Thylacine Tapestry -- the present/recycle/reweave lifecycle (G-2), and  *)
(* the Warp-C GPU-composed present (C-1).                                  *)
(*                                                                         *)
(* Tapestry is the graphics fast-path woven on Loom (docs/TAPESTRY.md);    *)
(* tapestryd owns the GPU and scans out client surfaces. A surface's       *)
(* framebuffer is a WEAVE: a tapestryd-allocated Burrow (D2) the client    *)
(* maps via the Weft grant-is-the-share mechanism (V2 -- holding the weave *)
(* fid IS the capability; the claim token is consume-once) and draws into  *)
(* directly (zero-copy, NOVEL Angle #2). A present (LOOM_OP_WRITE of a     *)
(* tpresent descriptor, D3) makes the host DMA-read the named slot         *)
(* (TRANSFER_TO_HOST_2D) out of band; the present's terminal CQE is the    *)
(* slot's recycle gate (D1).                                               *)
(*                                                                         *)
(* This module models that lifecycle and pins T-1 (no torn scanout,        *)
(* TAPESTRY.md section 6 + section 18.8), the invariant the graphics phase *)
(* reserves an ARCH section 28 number for at G-2/G-3. Spec-first is        *)
(* RE-ENABLED for THIS surface (V3 -- the sixth instance of re-enabling    *)
(* point (a)); the shared-buffer-lifetime class is the weft.tla sibling    *)
(* (the io_uring ubuf_info race, lifted to the framebuffer).               *)
(*                                                                         *)
(* WHAT THIS SPEC PINS (TAPESTRY.md section 18.8)                          *)
(*                                                                         *)
(*   (1) T-1 proper: a weave's pages stay BACKED while any present on it   *)
(*       is in flight (the host may be DMA-reading them) and while scanout *)
(*       composition references it. Freed/reused pages under an in-flight  *)
(*       TRANSFER = the torn-scanout UAF. Pinned by NoTornScanout +        *)
(*       DisplayedBacked.                                                  *)
(*                                                                         *)
(*   (2) The recycle gate (D1): a slot is drawable again ONLY after its    *)
(*       present's terminal CQE. Freeing the slot at submit-ack lets the   *)
(*       client draw into a slot the host is still reading -- the torn     *)
(*       FRAME (content, not lifetime). Pinned by RecycleGate.             *)
(*                                                                         *)
(*   (3) The #847 dual-refcount across the share: the client's mapping     *)
(*       keeps the weave's pages backed independently of the server's      *)
(*       retire intent; GONE requires BOTH sides done (unmapped + drained).*)
(*       Pinned by MappedImpliesBacked + GoneClean.                        *)
(*                                                                         *)
(*   (4) The reweave (resize, section 18.3) is ordered: the old weave      *)
(*       outlives its last in-flight present and its scanout reference     *)
(*       (retire only after the displayed switch + quiesce); never         *)
(*       realloc-in-place -- a reweave is a NEW weave. Pinned by           *)
(*       NoTornScanout + DisplayedBacked on the reweave path +             *)
(*       ReweaveOrdered.                                                   *)
(*                                                                         *)
(*   (5) The consume-once claim (V2): a weave's map token resolves only    *)
(*       while the weave is claimable (woven/live); a claim racing         *)
(*       teardown must refuse -- a stale claim resolving against a         *)
(*       retired/freed weave maps dead pages. Pinned by NoStaleMap.        *)
(*                                                                         *)
(*   (6) WARP-C (C-1): the GPU-COMPOSED present. Pinned by NoTornCompose   *)
(*       + NoStaleCompose. See "THE COMPOSED PATH" below.                  *)
(*                                                                         *)
(* Exactly-once completion per present (section 18.8's                     *)
(* ExactlyOneTerminalPerPresent) is STRUCTURAL here (Complete consumes one *)
(* in-flight transfer that exactly one Submit produced) -- the checked     *)
(* form lives in loom.tla's I-29 (CqNeverOverfull / no-double-terminal),   *)
(* which the present op COMPOSES; this module does not re-model the CQ.    *)
(*                                                                         *)
(* WHAT LEG OF T-1 THIS MODEL PINS (round-1 holotype F16).                 *)
(* T-1 has two legs: LIFETIME (the host must not DMA-read freed/reused     *)
(* pages -- a UAF) and CONTENT (the host must not read a slot mid-redraw   *)
(* -- a torn frame). This module pins the LIFETIME leg as NoTornScanout /  *)
(* DisplayedBacked (an in-flight transfer or a scanout reference implies   *)
(* backed pages) -- the memory-safety property. The CONTENT leg is pinned  *)
(* SEPARATELY by RecycleGate (D1: a slot is never drawable while the host  *)
(* still reads it). "torn scanout" in the prose spans both; the model      *)
(* names them distinctly. The composed path repeats the split exactly:     *)
(* NoTornCompose is its LIFETIME leg, NoStaleCompose its CONTENT leg.      *)
(*                                                                         *)
(* GENERATION SCOPE (round-1 holotype F6). Gens = {g1,g2} models ONE       *)
(* reweave. The impl rule "at most one reweave in flight per surface -- a  *)
(* new reweave may not begin until the prior old weave FULLY retired"      *)
(* (TAPESTRY.md section 18.3 step 4) bounds a surface to <=2 live weave    *)
(* generations, so the 2-symbol Gens is FAITHFUL, not accidentally small;  *)
(* a resize burst queues (it does not stack g3-while-g2-drains). The       *)
(* Reweave action encodes the rule structurally: it fires only with        *)
(* wstate["g2"] = "none" (no reweave already outstanding).                 *)
(*                                                                         *)
(* THE #847 DUAL REFCOUNT (round-2 R2-F2). The weave pages are held by TWO  *)
(* independent refs: serverRef (the server's handle_count -- tapestryd's    *)
(* allocation + KObj_DMA + virtio-gpu resource) and mapped (the client's    *)
(* mapping_count -- burrow_share_into). Free needs BOTH dropped. This is    *)
(* modeled EXPLICITLY (not collapsed into `backed`) so a crash is a         *)
(* checkable, non-vacuous state -- the round-1 spec cleared only `armed`    *)
(* (which no invariant read) and reached zero new distinct states.          *)
(*                                                                         *)
(* SERVER DEATH (round-1 holotype F4, sharpened by round-2 R2-F2).          *)
(* A GRACEFUL retire drops serverRef via ServerRelease only AFTER quiesce   *)
(* (intransfer = 0). A CRASH (ServerDeath) drops serverRef AT ONCE for      *)
(* every live/woven generation -- even with a host DMA-read in flight --    *)
(* and clears the armed claim tokens, but leaves the CLIENT MAPPING. So the *)
(* crash reaches serverRef=FALSE /\ mapped=TRUE /\ intransfer>0, a state     *)
(* the graceful path CANNOT (ServerRelease requires intransfer=0). On that  *)
(* state RefImpliesBacked (either ref => backed) is the #847-across-crash   *)
(* no-UAF check: the client mapping alone keeps the pages backed with the   *)
(* server + its resource gone. The client's ClunkMap -> Free drains to gone *)
(* (the reconnect contract's teardown leg); EventuallyRetired covers it.    *)
(*                                                                         *)
(* ===================================================================     *)
(* THE COMPOSED PATH -- Warp-C C-1 (docs/GPU-DESIGN.md section 4.5)        *)
(* ===================================================================     *)
(*                                                                         *)
(* Warp-C makes the SCREEN a host-side 3D resource owned by a compositor-  *)
(* owned virgl context and composes into it with GPU blits: per frame, one *)
(* VIRGL_CCMD_BLIT per visible surface (src = the client's host resource,  *)
(* dst = the screen), all blits for a frame in ONE fenced submit_3d; on    *)
(* fence completion SET_SCANOUT(screen) + RESOURCE_FLUSH. Per-frame        *)
(* guest<->host pixel traffic becomes zero, deleting the measured 43%      *)
(* composed-path overhead (#215).                                          *)
(*                                                                         *)
(* This module extends rather than replaces the direct path, because the   *)
(* composed path ADDS a stage rather than substituting one. For a software *)
(* surface the pipeline per generation g becomes:                          *)
(*                                                                         *)
(*   guest weave slot --Submit--> [intransfer>0] --Complete--> host res    *)
(*   host res --ComposeBlit--> [inblit]  --ComposeComplete--> screen       *)
(*                                                                         *)
(* section 4.5.6 is binding on the shape of this extension: I-40 does NOT  *)
(* mandate synchrony, it mandates quiesce-before-retire, and synchrony was *)
(* merely the stage-0 mechanism discharging it BY CONSTRUCTION. The arc    *)
(* owes "a real drain plus a demonstration that it discharges              *)
(* ServerRelease and NoStaleMap as strongly as synchrony did -- not an     *)
(* amendment to I-40". Hence: no existing invariant is weakened here, and  *)
(* the composed path is added behind ALLOW_COMPOSE so the pre-Warp-C model *)
(* is recoverable EXACTLY (with ALLOW_COMPOSE = FALSE the two new          *)
(* variables never leave their initial values, so the six pre-existing     *)
(* cfgs must reproduce their distinct-state counts to the state -- that    *)
(* equality is the control proving the extension is additive).             *)
(*                                                                         *)
(* THE ATTACH IS THE AUTHORITY (P1b, measured 2026-08-16). A blit can read *)
(* a resource created by another context ONLY after an explicit            *)
(* ctx_attach_resource: with the attach the blit runs, without it vrend    *)
(* refuses by name ("Illegal resource 1080"). So composition authority is  *)
(* a deliberate per-surface grant, not ambient reach across the device --  *)
(* the I-45 conferral point, and C-2's attach verb is where it is spent.   *)
(* Modeled as `attached`; Detach may not fire under an in-flight blit.     *)
(*                                                                         *)
(* THE ORDERING HAZARD (P2). The client renders on its context and the     *)
(* compositor blits on its own; in-order controlq dequeue orders the       *)
(* COMMANDS but not GL execution across two host contexts sharing an       *)
(* object. Today the hazard is MASKED because transfer_from_3d_sync must   *)
(* produce bytes and so forces the sync as a side effect; a blit has no    *)
(* such side effect, so the hazard goes live exactly when the readback is  *)
(* removed. P2 measured 0 reorderings in 500 unsynced trials on thyla-pi   *)
(* (real V3D) with an INVERTED arm proving the probe could see one --      *)
(* which bounds the per-trial rate at ~0.6% (95%, rule of three) on THAT   *)
(* stack for THAT access pattern and says nothing about a multi-queue      *)
(* desktop GPU. A negative is not a proof, so C-1 models the hazard.       *)
(*                                                                         *)
(* WHAT THE BLIT READS -- ONE HOST RESOURCE PER SURFACE, NOT A SWAPCHAIN.   *)
(* This model first carried the in-flight blit as the SLOT it reads, on the *)
(* assumption that slots are host-side buffers, so that a client filling a  *)
(* DIFFERENT slot during a composition would be legitimate pipelining and   *)
(* only a same-slot overlap would be the hazard. TLC refuted the model      *)
(* built on that assumption, and the tree refutes the assumption itself:    *)
(* tapestryd allocates PER-SURFACE 2D resources with whole-weave            *)
(* ATTACH_BACKING, and a present is a per-present OFFSET transfer --        *)
(* "offset = slot_base + (y*res_w + x)*4 selects both the slot and the rect *)
(* origin within it" (usr/tapestryd/src/gpu.rs). The slots are GUEST-side   *)
(* staging regions and every one of them transfers into THE SAME host       *)
(* resource. Guest-side double-buffering buys no host-side concurrency, so  *)
(* naming a slot in the blit would model a swapchain that does not exist.   *)
(* `inblit` is therefore per-generation, and NoStaleCompose is the          *)
(* whole-generation exclusion.                                             *)
(*                                                                         *)
(* WHAT "FILL" MEANS, AND WHY IT IS CAPSET-NEUTRAL. `intransfer` reads as   *)
(* "a fill of this generation's host resource is in flight", whatever fills *)
(* it: TRANSFER_TO_HOST_2D for a software surface, the client's own GL      *)
(* command stream for a rendering one (which never transfers at all). That  *)
(* is exactly the pairing section 4.5.4 names -- "the client renders on its *)
(* context, the compositor blits on its own" -- and keeping the model blind *)
(* to WHICH mechanism fills the texture is what section 4.5.5 requires of   *)
(* it, so Warp-6 can extend the mechanism (a blob-mediated blit) without    *)
(* reshaping the model. section 4.5.3's "everything visible becomes a       *)
(* texture; surfaces differ only in how their texture is filled" is the     *)
(* same statement one layer up.                                            *)
(*                                                                         *)
(* THE EXCLUSION IS SYMMETRIC, SO IT IS SABOTAGED PER DIRECTION. A blit and *)
(* a fill of one host resource must not overlap, and that can be broken     *)
(* from either end: the compositor blitting while a fill is in flight       *)
(* (BUGGY_BLIT_DURING_FILL -- P2 proper, the absent cross-context sync), or *)
(* the client filling while a blit is in flight (BUGGY_FILL_DURING_BLIT --  *)
(* the buffer-in-use violation, the composed-path extension of D1). One     *)
(* flag opening both gates would prove only whichever direction TLC reached *)
(* first, so each gets its own constant and its own cfg.                    *)
(*                                                                         *)
(* THE WEAVE LIFECYCLE (per generation g; "g2" is the reweave target)      *)
(*                                                                         *)
(*   "none"     -- not allocated.                                          *)
(*   "woven"    -- tapestryd allocated the weave Burrow + the virtio-gpu   *)
(*                 resource; the claim token is ARMED; pages BACKED.       *)
(*   "live"     -- the client mapped it (the token consumed); draw +       *)
(*                 present flow.                                           *)
(*   "retiring" -- teardown/displacement began; no new client ops; in-     *)
(*                 flight presents drain (#898 quiesce).                   *)
(*   "gone"     -- pages freed. Requires unmapped + drained + not          *)
(*                 displayed (the clean Free recomposes scanout first).    *)
(*                                                                         *)
(* Per slot s: "free" -> Draw -> "drawn" -> Submit -> "pending" ->         *)
(* Complete (the terminal CQE) -> "free". intransfer[g][s] counts host     *)
(* DMA-reads in flight on the slot (clean: 0/1, tied to "pending"; the     *)
(* early-free bug decouples them, which is exactly the point).             *)
(*                                                                         *)
(* THE BUGS THIS PINS (each a BUGGY_* flag, each its own cfg)              *)
(*                                                                         *)
(*   BUGGY_EARLY_FREE -- SubmitEarlyFree recycles the slot at submit-ack   *)
(*     instead of at the terminal CQE (skips D1). The client's next Draw   *)
(*     scribbles a slot the host is still TRANSFER-reading -> RecycleGate  *)
(*     counterexample (the torn frame).                                    *)
(*                                                                         *)
(*   BUGGY_RETIRE_NO_QUIESCE -- FreeNoQuiesce frees a retiring weave's     *)
(*     pages ignoring in-flight presents / the client mapping / the        *)
(*     scanout reference -> NoTornScanout counterexample (the destroy-path *)
(*     UAF: the host DMA-reads freed pages).                               *)
(*                                                                         *)
(*   BUGGY_REWEAVE_NO_QUIESCE -- ReweaveEagerFree frees the OLD weave the  *)
(*     moment the new one exists, without waiting for the displayed switch *)
(*     + the old weave's drain -> NoTornScanout / DisplayedBacked          *)
(*     counterexample (the resize-path UAF; scanout composes freed pages). *)
(*                                                                         *)
(*   BUGGY_STALE_MAP -- MapStale lets an armed claim token resolve against *)
(*     a retiring/gone weave (the claim raced teardown and won) ->         *)
(*     NoStaleMap counterexample (the client maps dead pages).             *)
(*                                                                         *)
(*   BUGGY_DRAIN_SKIPPED (C-1) -- the retire path drains the DIRECT-path   *)
(*     in-flight class (intransfer) and is BLIND to the new one            *)
(*     (inblit): i.e. the pre-Warp-C quiesce, carried unchanged onto the   *)
(*     composed path. ServerRelease + Free lose exactly the                *)
(*     DrainedOfBlits conjunct, so the weave's pages are freed with a host *)
(*     composition blit still reading its resource -> NoTornCompose        *)
(*     counterexample (the composed-path UAF).                             *)
(*                                                                         *)
(*     Modeled as an OMITTED CONJUNCT on the real actions rather than as   *)
(*     twin buggy actions (the house style elsewhere in this module),      *)
(*     because the bug IS an omission: a twin action can drift from its    *)
(*     correct sibling in more ways than the one under test, and then the  *)
(*     counterexample no longer isolates the drain. Here the buggy arm     *)
(*     differs from the correct arm in exactly one conjunct, by            *)
(*     construction.                                                       *)
(*                                                                         *)
(*   BUGGY_BLIT_DURING_FILL (C-1) -- the compositor issues its blit while a *)
(*     fill of the source resource is still in flight: the missing          *)
(*     cross-context sync that transfer_from_3d_sync used to provide as a   *)
(*     SIDE EFFECT. P2 proper -> NoStaleCompose counterexample. Note this   *)
(*     describes a bug that CANNOT be hit today and becomes reachable at    *)
(*     exactly the sub-chunk that deletes the readback (C-4).               *)
(*                                                                         *)
(*   BUGGY_FILL_DURING_BLIT (C-1) -- the client fills the resource while a  *)
(*     composition blit is reading it: the buffer-in-use violation, i.e.    *)
(*     the composed-path extension of the D1 recycle gate. In the direct    *)
(*     path a slot is released by its present's terminal CQE; once the      *)
(*     compositor is a SECOND reader of the same host resource, that CQE no *)
(*     longer means the resource is free -> NoStaleCompose counterexample.  *)
(*                                                                         *)
(* THE READBACK ARM -- Warp-C C-6 (GPU-DESIGN section 4.5.13). Where the    *)
(* GPU cannot compose a client's frame by blit (a BO of a shape the         *)
(* compositor does not blit, an unwitnessed import, a latched compositor    *)
(* context, no 3D screen) the compositor READS THE FRAME BACK -- a          *)
(* TRANSFER_FROM_HOST_3D that DMA-WRITES the host resource's pixels into    *)
(* g's guest pages -- and composes from those pages the CPU way. C-3..C-5   *)
(* issued that readback SYNCHRONOUSLY on the present dispatch, so it lived   *)
(* inside one dispatch and needed no model of its own (I-40 by             *)
(* construction: nothing outlived the dispatch). C-0d Fable F2 found that   *)
(* wait to be the CLIENT's queue length on the console's thread; C-6 makes  *)
(* the readback FENCED with DEFERRED present completion, at most one in     *)
(* flight per generation -- and so, exactly as C-1 did for the blit, adds a *)
(* class of in-flight host work that a retire must drain:                   *)
(*                                                                         *)
(*   host res --ComposeReadbackIssue--> [inread] --ComposeReadbackComplete  *)
(*            --> g's pages hold the frame -> CPU compose -> screen          *)
(*                                                                         *)
(* The readback is a DMA WRITE into g's pages (the blit READ the host       *)
(* resource and never touched guest memory), so freeing those pages under   *)
(* an in-flight readback is a device writing freed memory -- the graver of  *)
(* the two UAFs. NoTornReadback is the LIFETIME leg; there is no CONTENT     *)
(* leg to model because the device serializes the readback against the     *)
(* client's fill of the same resource itself (in-order controlq + the       *)
(* synchronous host read is exactly the side effect P2 credits              *)
(* transfer_from_3d_sync with) -- so ComposeReadbackIssue carries no        *)
(* FillLanded guard, deliberately. Attach is NOT required: the readback     *)
(* runs under the CLIENT's own context, which is why it is the arm for the *)
(* un-imported BO. The F2b DEVICE stall the readback costs (GPU-DESIGN       *)
(* 4.5.13) is a duration, not a state, and is not modeled.                  *)
(*                                                                         *)
(*   BUGGY_READBACK_FREE (C-6) -- the retire path drains blits (C-1) and    *)
(*     the direct-path class and is BLIND to in-flight readbacks:            *)
(*     ServerRelease + Free lose exactly the DrainedOfReadbacks conjunct    *)
(*     -> NoTornReadback counterexample (the pages freed with a host DMA-   *)
(*     WRITE landing in them). The same omitted-conjunct style as           *)
(*     BUGGY_DRAIN_SKIPPED, for the same reason.                            *)
(*                                                                         *)
(* CONFIGS                                                                 *)
(*                                                                         *)
(*   tapestry_present.cfg            all BUGGY_* FALSE; ALLOW_DESTROY +    *)
(*                                   ALLOW_REWEAVE TRUE. Expected: green.  *)
(*   tapestry_present_liveness.cfg   Spec_Live; EventuallyRetired (a       *)
(*                                   destroy always drains to gone/none).  *)
(*                                   Expected: green.                      *)
(*   tapestry_present_buggy_premature_reuse.cfg        RecycleGate --      *)
(*                                   expected VIOLATED.                    *)
(*   tapestry_present_buggy_retire_during_transfer.cfg NoTornScanout --    *)
(*                                   expected VIOLATED.                    *)
(*   tapestry_present_buggy_reweave_without_quiesce.cfg NoTornScanout /    *)
(*                                   DisplayedBacked -- expected VIOLATED. *)
(*   tapestry_present_buggy_map_after_retire.cfg       NoStaleMap --       *)
(*                                   expected VIOLATED.                    *)
(*   tapestry_present_composed.cfg          ALLOW_COMPOSE; all BUGGY_*     *)
(*                                   FALSE. Expected: green.               *)
(*   tapestry_present_composed_liveness.cfg ALLOW_COMPOSE + Spec_Live;     *)
(*                                   EventuallyRetired -- the real drain   *)
(*                                   does not deadlock teardown.           *)
(*                                   Expected: green.                      *)
(*   tapestry_present_buggy_drain_skipped.cfg       NoTornCompose --       *)
(*                                   expected VIOLATED.                    *)
(*   tapestry_present_buggy_blit_during_fill.cfg    NoStaleCompose --      *)
(*                                   expected VIOLATED (P2 proper).        *)
(*   tapestry_present_buggy_fill_during_blit.cfg    NoStaleCompose --      *)
(*                                   expected VIOLATED (the other end).    *)
(*   tapestry_present_buggy_readback_free.cfg      NoTornReadback --      *)
(*                                   expected VIOLATED (C-6).             *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

Gens  == {"g1", "g2"}
Slots == {"s1", "s2"}

GenNo(g) == IF g = "g1" THEN 1 ELSE 2

MaxInflight == 2

CONSTANTS
    ALLOW_DESTROY,             \* BOOLEAN -- enable the surface-destroy path.
    ALLOW_REWEAVE,             \* BOOLEAN -- enable the resize (reweave) path.
    ALLOW_SERVER_DEATH,        \* BOOLEAN -- enable the tapestryd-crash path (F4).
    ALLOW_COMPOSE,             \* BOOLEAN -- enable the Warp-C GPU-composed present.
    BUGGY_EARLY_FREE,          \* BOOLEAN -- recycle the slot at submit-ack (skip D1).
    BUGGY_RETIRE_NO_QUIESCE,   \* BOOLEAN -- free a retiring weave without quiesce.
    BUGGY_REWEAVE_NO_QUIESCE,  \* BOOLEAN -- free the old weave eagerly on reweave.
    BUGGY_STALE_MAP,           \* BOOLEAN -- let a stale claim token resolve.
    BUGGY_DRAIN_SKIPPED,       \* BOOLEAN -- retire drains only the direct-path class.
    BUGGY_BLIT_DURING_FILL,    \* BOOLEAN -- blit a resource whose fill is still in flight.
    BUGGY_FILL_DURING_BLIT,    \* BOOLEAN -- fill a resource a blit is still reading.
    BUGGY_READBACK_FREE        \* BOOLEAN -- retire is blind to an in-flight readback (C-6).

ASSUME ALLOW_DESTROY            \in BOOLEAN
ASSUME ALLOW_REWEAVE            \in BOOLEAN
ASSUME ALLOW_SERVER_DEATH       \in BOOLEAN
ASSUME ALLOW_COMPOSE            \in BOOLEAN
ASSUME BUGGY_EARLY_FREE         \in BOOLEAN
ASSUME BUGGY_RETIRE_NO_QUIESCE  \in BOOLEAN
ASSUME BUGGY_REWEAVE_NO_QUIESCE \in BOOLEAN
ASSUME BUGGY_STALE_MAP          \in BOOLEAN
ASSUME BUGGY_DRAIN_SKIPPED      \in BOOLEAN
ASSUME BUGGY_BLIT_DURING_FILL   \in BOOLEAN
ASSUME BUGGY_FILL_DURING_BLIT   \in BOOLEAN
ASSUME BUGGY_READBACK_FREE      \in BOOLEAN

VARIABLES
    wstate,      \* [Gens -> {"none","woven","live","retiring","gone"}]
    backed,      \* [Gens -> BOOLEAN] -- the weave pages are allocated + backing intact
    serverRef,   \* [Gens -> BOOLEAN] -- the #847 SERVER-side ref (handle_count): tapestryd's
                 \*   allocation + KObj_DMA + virtio-gpu resource. A crash drops it at once
                 \*   (round-2 R2-F2 -- distinct from `backed`, so "server ref gone, client
                 \*   ref holds, pages alive" is a checkable state, not a vacuous one).
    mapped,      \* [Gens -> BOOLEAN] -- the #847 CLIENT-side ref (mapping_count): burrow_share_into
    armed,       \* [Gens -> BOOLEAN] -- the consume-once Tweft claim token (V2)
    slot,        \* [Gens -> [Slots -> {"free","drawn","pending"}]] -- client recycle state
    intransfer,  \* [Gens -> [Slots -> 0..MaxInflight]] -- host DMA-reads in flight
    displayed,   \* Gens \cup {"nothing"} -- the generation scanout composition references
    attached,    \* [Gens -> BOOLEAN] -- C-1: g's host resource is attached to the compositor
                 \*   virgl context (ctx_attach_resource). P1b measured this as the
                 \*   authority-conferral point: without it vrend refuses the cross-context
                 \*   blit by name. C-2's attach verb is where the grant is spent.
    inblit,      \* [Gens -> BOOLEAN] -- C-1: a composition blit is in flight reading g's
                 \*   host resource. Per-GENERATION, not per-slot: tapestryd allocates one
                 \*   2D resource per surface and every slot transfers into it at an
                 \*   offset, so guest-side slots buy no host-side concurrency.
    filled,      \* [Gens -> BOOLEAN] -- C-1: g's host resource has been populated at least
                 \*   once (a fill LANDED). Distinct from intransfer = 0, which is equally
                 \*   true of "the fill completed" and "no fill was ever issued" -- the
                 \*   compositor must not blit an unpopulated resource on the strength of
                 \*   a counter reading zero.
    inread,      \* [Gens -> BOOLEAN] -- C-6: a compositor READBACK is in flight: a host
                 \*   DMA-WRITE of g's host resource into g's guest pages (the fenced
                 \*   TRANSFER_FROM_HOST_3D of the composed-GL present's fallback arm), at
                 \*   most one per generation. Never leaves FALSE with ALLOW_COMPOSE off.
    staleMapped, \* BOOLEAN -- history: a claim resolved against a retiring/gone weave
    destroyReq   \* BOOLEAN -- the surface destroy was requested

vars == <<wstate, backed, serverRef, mapped, armed, slot, intransfer, displayed,
          attached, inblit, filled, inread, staleMapped, destroyReq>>

TypeOK ==
    /\ wstate      \in [Gens -> {"none", "woven", "live", "retiring", "gone"}]
    /\ backed      \in [Gens -> BOOLEAN]
    /\ serverRef   \in [Gens -> BOOLEAN]
    /\ mapped      \in [Gens -> BOOLEAN]
    /\ armed       \in [Gens -> BOOLEAN]
    /\ slot        \in [Gens -> [Slots -> {"free", "drawn", "pending"}]]
    /\ intransfer  \in [Gens -> [Slots -> 0..MaxInflight]]
    /\ displayed   \in Gens \cup {"nothing"}
    /\ attached    \in [Gens -> BOOLEAN]
    /\ inblit      \in [Gens -> BOOLEAN]
    /\ filled      \in [Gens -> BOOLEAN]
    /\ inread      \in [Gens -> BOOLEAN]
    /\ staleMapped \in BOOLEAN
    /\ destroyReq  \in BOOLEAN

Init ==
    /\ wstate      = [g \in Gens |-> "none"]
    /\ backed      = [g \in Gens |-> FALSE]
    /\ serverRef   = [g \in Gens |-> FALSE]
    /\ mapped      = [g \in Gens |-> FALSE]
    /\ armed       = [g \in Gens |-> FALSE]
    /\ slot        = [g \in Gens |-> [s \in Slots |-> "free"]]
    /\ intransfer  = [g \in Gens |-> [s \in Slots |-> 0]]
    /\ displayed   = "nothing"
    /\ attached    = [g \in Gens |-> FALSE]
    /\ inblit      = [g \in Gens |-> FALSE]
    /\ filled      = [g \in Gens |-> FALSE]
    /\ inread      = [g \in Gens |-> FALSE]
    /\ staleMapped = FALSE
    /\ destroyReq  = FALSE

(***************************************************************************)
(* C-1 helpers.                                                            *)
(***************************************************************************)

\* A composition blit is in flight reading generation g's host resource.
InBlit(g) == inblit[g]

\* No fill of g's host resource is in flight -- whether the fill is a software
\* surface's TRANSFER_TO_HOST_2D or a rendering client's own GL stream. Under
\* BUGGY_BLIT_DURING_FILL this degrades to TRUE, which is P2 proper: the blit
\* issues with the fill still outstanding because nothing orders GL execution
\* across two host contexts sharing an object.
FillLanded(g) ==
    BUGGY_BLIT_DURING_FILL \/ (\A s \in Slots : intransfer[g][s] = 0)

\* No composition blit is reading g's host resource, so the client may fill it.
\* Under BUGGY_FILL_DURING_BLIT this degrades to TRUE -- the buffer-in-use
\* violation from the other end.
ComposeIdle(g) == BUGGY_FILL_DURING_BLIT \/ ~InBlit(g)

\* The C-1 drain conjunct. Under BUGGY_DRAIN_SKIPPED this degrades to TRUE --
\* which IS the bug: the retire path keeps the direct-path quiesce (intransfer)
\* and is blind to the composition class. section 4.5.6's "a pipelined controlq
\* must implement a real drain before touching retire", stated as the one
\* conjunct whose absence is the defect.
DrainedOfBlits(g) == BUGGY_DRAIN_SKIPPED \/ ~InBlit(g)

\* C-6: a compositor readback (a host DMA-WRITE into g's pages) is in flight.
InRead(g) == inread[g]

\* The C-6 drain conjunct: no readback is landing in g's pages. Under
\* BUGGY_READBACK_FREE this degrades to TRUE -- the retire path that drains
\* transfers (#898) and blits (C-1) and is blind to the third in-flight class.
DrainedOfReadbacks(g) == BUGGY_READBACK_FREE \/ ~InRead(g)

(***************************************************************************)
(* Server: weave allocation (create-surface / the reweave CONFIGURE ack).  *)
(***************************************************************************)

WeaveFirst ==
    /\ ~destroyReq
    /\ wstate["g1"] = "none"
    /\ wstate'    = [wstate    EXCEPT !["g1"] = "woven"]
    /\ backed'    = [backed    EXCEPT !["g1"] = TRUE]
    /\ serverRef' = [serverRef EXCEPT !["g1"] = TRUE]
    /\ armed'     = [armed     EXCEPT !["g1"] = TRUE]
    /\ UNCHANGED <<mapped, slot, intransfer, displayed, attached, inblit, filled, inread,
                   staleMapped, destroyReq>>

Reweave ==
    /\ ALLOW_REWEAVE
    /\ ~destroyReq
    /\ wstate["g1"] = "live"
    /\ wstate["g2"] = "none"
    /\ wstate'    = [wstate    EXCEPT !["g2"] = "woven"]
    /\ backed'    = [backed    EXCEPT !["g2"] = TRUE]
    /\ serverRef' = [serverRef EXCEPT !["g2"] = TRUE]
    /\ armed'     = [armed     EXCEPT !["g2"] = TRUE]
    /\ UNCHANGED <<mapped, slot, intransfer, displayed, attached, inblit, filled, inread,
                   staleMapped, destroyReq>>

(***************************************************************************)
(* Client: the map claim (V2 grant-is-the-share; consume-once).            *)
(***************************************************************************)

Map(g) ==
    /\ armed[g]
    /\ wstate[g] \in {"woven", "live"}
    /\ mapped' = [mapped EXCEPT ![g] = TRUE]
    /\ armed'  = [armed  EXCEPT ![g] = FALSE]
    /\ wstate' = [wstate EXCEPT ![g] = "live"]
    /\ UNCHANGED <<backed, serverRef, slot, intransfer, displayed, attached,
                   inblit, filled, inread, staleMapped, destroyReq>>

MapStale(g) ==
    /\ BUGGY_STALE_MAP
    /\ armed[g]
    /\ wstate[g] \in {"retiring", "gone"}
    /\ mapped'      = [mapped EXCEPT ![g] = TRUE]
    /\ armed'       = [armed  EXCEPT ![g] = FALSE]
    /\ staleMapped' = TRUE
    /\ UNCHANGED <<wstate, backed, serverRef, slot, intransfer, displayed,
                   attached, inblit, filled, inread, destroyReq>>

ClunkMap(g) ==
    /\ mapped[g]
    /\ mapped' = [mapped EXCEPT ![g] = FALSE]
    /\ UNCHANGED <<wstate, backed, serverRef, armed, slot, intransfer, displayed,
                   attached, inblit, filled, inread, staleMapped, destroyReq>>

(***************************************************************************)
(* Client: draw + present. Server/host: the transfer completion.           *)
(***************************************************************************)

Draw(g, s) ==
    /\ ~destroyReq
    /\ wstate[g] = "live"
    /\ mapped[g]
    /\ slot[g][s] = "free"
    /\ slot' = [slot EXCEPT ![g][s] = "drawn"]
    /\ UNCHANGED <<wstate, backed, serverRef, mapped, armed, intransfer,
                   displayed, attached, inblit, filled, inread, staleMapped, destroyReq>>

Submit(g, s) ==
    /\ ~destroyReq
    /\ wstate[g] = "live"
    /\ mapped[g]
    /\ slot[g][s] = "drawn"
    /\ intransfer[g][s] = 0
    /\ ComposeIdle(g)
    /\ slot'       = [slot       EXCEPT ![g][s] = "pending"]
    /\ intransfer' = [intransfer EXCEPT ![g][s] = 1]
    /\ UNCHANGED <<wstate, backed, serverRef, mapped, armed, displayed,
                   attached, inblit, filled, inread, staleMapped, destroyReq>>

SubmitEarlyFree(g, s) ==
    /\ BUGGY_EARLY_FREE
    /\ ~destroyReq
    /\ wstate[g] = "live"
    /\ mapped[g]
    /\ slot[g][s] = "drawn"
    /\ intransfer[g][s] < MaxInflight
    /\ slot'       = [slot       EXCEPT ![g][s] = "free"]
    /\ intransfer' = [intransfer EXCEPT ![g][s] = @ + 1]
    /\ UNCHANGED <<wstate, backed, serverRef, mapped, armed, displayed,
                   attached, inblit, filled, inread, staleMapped, destroyReq>>

Complete(g, s) ==
    /\ intransfer[g][s] > 0
    /\ backed[g]
    /\ intransfer' = [intransfer EXCEPT ![g][s] = @ - 1]
    /\ slot' = IF slot[g][s] = "pending" /\ intransfer[g][s] = 1
               THEN [slot EXCEPT ![g][s] = "free"]
               ELSE slot
    /\ displayed' = IF wstate[g] # "live"
                    THEN displayed
                    ELSE IF displayed = "nothing"
                         THEN g
                         ELSE IF GenNo(g) > GenNo(displayed)
                              THEN g
                              ELSE displayed
    \* `filled` is a composed-path observation and is held constant when the
    \* path is off, so the pre-Warp-C model stays bit-recoverable: with
    \* ALLOW_COMPOSE = FALSE the six original cfgs must reproduce their exact
    \* distinct-state counts, which is the control proving this extension is
    \* additive rather than a rewrite. Tracking it unconditionally cost 5413
    \* -> 10413 states on the direct path and broke that check.
    /\ filled' = IF ALLOW_COMPOSE THEN [filled EXCEPT ![g] = TRUE] ELSE filled
    /\ UNCHANGED <<wstate, backed, serverRef, mapped, armed, attached, inblit,
                   inread, staleMapped, destroyReq>>

(***************************************************************************)
(* C-1: the composed present (Warp-C, GPU-DESIGN section 4.5).             *)
(*                                                                         *)
(* Attach confers the authority (P1b); ComposeBlit spends it, reading ONE  *)
(* named source slot; ComposeComplete is the fence retiring, which is what *)
(* SET_SCANOUT + RESOURCE_FLUSH ride. The compositor blits from the HOST   *)
(* resource, so it deliberately does NOT require mapped[g] -- the client's *)
(* guest mapping is irrelevant to composition, which is precisely why the  *)
(* composed path needs its own drain rather than inheriting the #847 one.  *)
(***************************************************************************)

Attach(g) ==
    /\ ALLOW_COMPOSE
    /\ ~attached[g]
    /\ serverRef[g]
    /\ wstate[g] \in {"woven", "live"}
    /\ attached' = [attached EXCEPT ![g] = TRUE]
    /\ UNCHANGED <<wstate, backed, serverRef, mapped, armed, slot, intransfer,
                   displayed, inblit, filled, inread, staleMapped, destroyReq>>

\* ctx_detach_resource. Never under an in-flight blit: the host would be
\* reading a resource it no longer has a reference to through this context.
Detach(g) ==
    /\ attached[g]
    /\ ~InBlit(g)
    /\ attached' = [attached EXCEPT ![g] = FALSE]
    /\ UNCHANGED <<wstate, backed, serverRef, mapped, armed, slot, intransfer,
                   displayed, inblit, filled, inread, staleMapped, destroyReq>>

\* The correct composition blit: the fill of g's host resource has LANDED --
\* the cross-context sync that transfer_from_3d_sync used to supply as a side
\* effect, now explicit -- and the resource has been populated at least once
\* (filled[g], NOT merely a zero in-flight count). No new blit once retiring,
\* which is what lets the drain terminate.
ComposeBlit(g) ==
    /\ ALLOW_COMPOSE
    /\ ~destroyReq
    /\ wstate[g] = "live"
    /\ attached[g]
    /\ ~InBlit(g)
    /\ filled[g]
    /\ FillLanded(g)
    /\ inblit' = [inblit EXCEPT ![g] = TRUE]
    /\ UNCHANGED <<wstate, backed, serverRef, mapped, armed, slot, intransfer,
                   displayed, attached, filled, inread, staleMapped, destroyReq>>

\* The composition fence retires -> SET_SCANOUT(screen) + RESOURCE_FLUSH. The
\* displayed update mirrors Complete's exactly (a retiring generation never
\* becomes the composed one).
ComposeComplete(g) ==
    /\ InBlit(g)
    /\ backed[g]
    /\ inblit' = [inblit EXCEPT ![g] = FALSE]
    /\ displayed' = IF wstate[g] # "live"
                    THEN displayed
                    ELSE IF displayed = "nothing"
                         THEN g
                         ELSE IF GenNo(g) > GenNo(displayed)
                              THEN g
                              ELSE displayed
    /\ UNCHANGED <<wstate, backed, serverRef, mapped, armed, slot, intransfer,
                   attached, filled, inread, staleMapped, destroyReq>>

(***************************************************************************)
(* C-6: the readback arm of the composed present (GPU-DESIGN 4.5.13).      *)
(*                                                                         *)
(* Issue = the fenced TRANSFER_FROM_HOST_3D of g's host resource INTO g's  *)
(* guest pages, at most one in flight per generation (a present arriving   *)
(* while one is pending is coalesced -- latest wins -- so a second Issue   *)
(* is not an action). Requires a populated resource (filled), like the     *)
(* blit; requires NEITHER attached (it runs under the client's own ctx --  *)
(* it is the arm for the un-imported BO) NOR FillLanded (the device        *)
(* serializes the read against the client's fill: the in-order controlq    *)
(* plus the synchronous host read, the very side effect P2 credits the     *)
(* sync readback with -- so no content leg exists to model). No new        *)
(* readback once retiring, which is what lets the drain terminate.         *)
(* Complete = the fence retiring: g's pages now hold the frame, the CPU    *)
(* compose runs, the screen shows g (the displayed update mirrors          *)
(* ComposeComplete's).                                                     *)
(***************************************************************************)

ComposeReadbackIssue(g) ==
    /\ ALLOW_COMPOSE
    /\ ~destroyReq
    /\ wstate[g] = "live"
    /\ ~InRead(g)
    /\ filled[g]
    /\ inread' = [inread EXCEPT ![g] = TRUE]
    /\ UNCHANGED <<wstate, backed, serverRef, mapped, armed, slot, intransfer,
                   displayed, attached, inblit, filled, staleMapped, destroyReq>>

ComposeReadbackComplete(g) ==
    /\ InRead(g)
    /\ backed[g]
    /\ inread' = [inread EXCEPT ![g] = FALSE]
    /\ displayed' = IF wstate[g] # "live"
                    THEN displayed
                    ELSE IF displayed = "nothing"
                         THEN g
                         ELSE IF GenNo(g) > GenNo(displayed)
                              THEN g
                              ELSE displayed
    /\ UNCHANGED <<wstate, backed, serverRef, mapped, armed, slot, intransfer,
                   attached, inblit, filled, staleMapped, destroyReq>>

(***************************************************************************)
(* Teardown: destroy / the reweave displacement / the free edge.           *)
(*                                                                         *)
(* The #847 dual-refcount: `serverRef` (handle_count) + `mapped`            *)
(* (mapping_count). A GRACEFUL retire drops serverRef via ServerRelease     *)
(* AFTER quiesce; a CRASH (ServerDeath) drops it AT ONCE, even with a       *)
(* transfer in flight -- the distinct state the round-2 R2-F2 fix exists to  *)
(* make checkable. Free requires BOTH refs dropped (~serverRef /\ ~mapped). *)
(***************************************************************************)

Destroy ==
    /\ ALLOW_DESTROY
    /\ ~destroyReq
    /\ wstate["g1"] # "none"
    /\ destroyReq' = TRUE
    /\ wstate' = [g \in Gens |->
                    IF wstate[g] \in {"woven", "live"} THEN "retiring"
                                                       ELSE wstate[g]]
    /\ UNCHANGED <<backed, serverRef, mapped, armed, slot, intransfer,
                   displayed, attached, inblit, filled, inread, staleMapped>>

RetireDisplaced ==
    /\ wstate["g1"] = "live"
    /\ displayed = "g2"
    /\ wstate' = [wstate EXCEPT !["g1"] = "retiring"]
    /\ UNCHANGED <<backed, serverRef, mapped, armed, slot, intransfer,
                   displayed, attached, inblit, filled, inread, staleMapped, destroyReq>>

\* The graceful server-side ref drop: tapestryd finishes quiescing a retiring
\* weave's in-flight presents (#898) AND its in-flight composition blits (C-1),
\* then releases its #847 handle_count ref. Requires intransfer = 0 -- the
\* graceful path NEVER drops the server ref with a host DMA-read in flight (that
\* is exactly what a crash does; ServerDeath) -- and DrainedOfBlits, the C-1
\* addition whose omission is BUGGY_DRAIN_SKIPPED.
ServerRelease(g) ==
    /\ wstate[g] = "retiring"
    /\ serverRef[g]
    /\ \A s \in Slots : intransfer[g][s] = 0
    /\ DrainedOfBlits(g)
    /\ DrainedOfReadbacks(g)
    /\ serverRef' = [serverRef EXCEPT ![g] = FALSE]
    /\ UNCHANGED <<wstate, backed, mapped, armed, slot, intransfer, displayed,
                   attached, inblit, filled, inread, staleMapped, destroyReq>>

\* F4: a tapestryd crash. Every live/woven generation snaps to "retiring", the
\* registry's claim tokens die (armed -> FALSE -- weft_share_release_owner), AND
\* the #847 SERVER ref drops AT ONCE (serverRef -> FALSE, even with a transfer in
\* flight -- the KObj_DMA + virtio-gpu resource die with the reaped Proc). The
\* CLIENT MAPPING stays (mapped unchanged): mapping_count alone keeps the pages
\* backed -- RefImpliesBacked must hold across it (the #847-across-crash check,
\* now NON-VACUOUS: serverRef=FALSE /\ mapped=TRUE /\ intransfer>0 is a state the
\* graceful path cannot reach). The client's ClunkMap -> Free drains to gone (the
\* reconnect contract's teardown leg). A terminal surface event (sets destroyReq)
\* so EventuallyRetired covers it too.
\*
\* C-1: inblit is left UNCHANGED for the same reason intransfer is -- the host
\* may still be executing a composition blit when the guest Proc is reaped, so
\* the crash must reach inblit = TRUE with the server gone. Clearing it here
\* would make NoTornCompose vacuous across exactly the path that most needs it.
\* C-6: inread likewise -- a readback the device is still landing outlives the
\* reaped server, and NoTornReadback must be checked across that state.
ServerDeath ==
    /\ ALLOW_SERVER_DEATH
    /\ ~destroyReq
    /\ \E g \in Gens : wstate[g] \in {"woven", "live"}
    /\ destroyReq' = TRUE
    /\ wstate' = [g \in Gens |->
                    IF wstate[g] \in {"woven", "live"} THEN "retiring"
                                                       ELSE wstate[g]]
    /\ serverRef' = [g \in Gens |->
                    IF wstate[g] \in {"woven", "live"} THEN FALSE
                                                       ELSE serverRef[g]]
    /\ armed'  = [g \in Gens |-> FALSE]
    /\ UNCHANGED <<backed, mapped, slot, intransfer, displayed, attached,
                   inblit, filled, inread, staleMapped>>

Free(g) ==
    /\ wstate[g] = "retiring"
    /\ ~serverRef[g]
    /\ ~mapped[g]
    /\ \A s \in Slots : intransfer[g][s] = 0
    /\ DrainedOfBlits(g)
    /\ DrainedOfReadbacks(g)
    /\ wstate'    = [wstate EXCEPT ![g] = "gone"]
    /\ backed'    = [backed EXCEPT ![g] = FALSE]
    /\ attached'  = [attached EXCEPT ![g] = FALSE]
    /\ displayed' = IF displayed = g THEN "nothing" ELSE displayed
    /\ UNCHANGED <<serverRef, mapped, armed, slot, intransfer, inblit, filled, inread,
                   staleMapped, destroyReq>>

FreeNoQuiesce(g) ==
    /\ BUGGY_RETIRE_NO_QUIESCE
    /\ wstate[g] = "retiring"
    /\ wstate' = [wstate EXCEPT ![g] = "gone"]
    /\ backed' = [backed EXCEPT ![g] = FALSE]
    /\ UNCHANGED <<serverRef, mapped, armed, slot, intransfer, displayed,
                   attached, inblit, filled, inread, staleMapped, destroyReq>>

ReweaveEagerFree ==
    /\ BUGGY_REWEAVE_NO_QUIESCE
    /\ wstate["g1"] = "live"
    /\ wstate["g2"] # "none"
    /\ wstate' = [wstate EXCEPT !["g1"] = "gone"]
    /\ backed' = [backed EXCEPT !["g1"] = FALSE]
    /\ UNCHANGED <<serverRef, mapped, armed, slot, intransfer, displayed,
                   attached, inblit, filled, inread, staleMapped, destroyReq>>

(***************************************************************************)
(* The next-state relation.                                                *)
(***************************************************************************)

Next ==
    \/ WeaveFirst
    \/ Reweave
    \/ Destroy
    \/ ServerDeath
    \/ RetireDisplaced
    \/ ReweaveEagerFree
    \/ \E g \in Gens :
         \/ Map(g) \/ MapStale(g) \/ ClunkMap(g)
         \/ ServerRelease(g) \/ Free(g) \/ FreeNoQuiesce(g)
         \/ Attach(g) \/ Detach(g)
         \/ ComposeBlit(g) \/ ComposeComplete(g)
         \/ ComposeReadbackIssue(g) \/ ComposeReadbackComplete(g)
    \/ \E g \in Gens, s \in Slots :
         \/ Draw(g, s) \/ Submit(g, s) \/ SubmitEarlyFree(g, s)
         \/ Complete(g, s)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Invariants (TAPESTRY.md section 18.8; GPU-DESIGN section 4.5 for C-1).  *)
(***************************************************************************)

\* T-1 proper: pages stay backed while any transfer is in flight on them.
NoTornScanout ==
    \A g \in Gens : (\E s \in Slots : intransfer[g][s] > 0) => backed[g]

\* T-1's scanout leg: the composed generation's pages are live.
DisplayedBacked ==
    displayed \in Gens => backed[displayed]

\* D1: a slot is never drawable while the host still reads it.
RecycleGate ==
    \A g \in Gens, s \in Slots :
        ~(slot[g][s] = "drawn" /\ intransfer[g][s] > 0)

\* #847 no-UAF (round-2 R2-F2): while EITHER ref is held -- the server's
\* handle_count OR the client's mapping_count -- the pages stay backed. The
\* crash-specific state (serverRef=FALSE /\ mapped=TRUE, the client mapping
\* outliving the reaped server) is checked here: the mapping alone keeps the
\* pages alive, no UAF. Generalizes MappedImpliesBacked to both refs.
RefImpliesBacked ==
    \A g \in Gens : (serverRef[g] \/ mapped[g]) => backed[g]

\* #847: the client's mapping keeps the pages alive, whatever the server does
\* (the mapping-side leg of RefImpliesBacked; kept for continuity).
MappedImpliesBacked ==
    \A g \in Gens : mapped[g] => backed[g]

\* GONE means BOTH #847 refs dropped + the pages freed.
GoneClean ==
    \A g \in Gens :
        wstate[g] = "gone" => (~serverRef[g] /\ ~mapped[g] /\ ~backed[g])

\* V2 consume-once: no claim ever resolved against a retiring/gone weave.
NoStaleMap == ~staleMapped

\* The reweave allocates strictly after (and because of) the first weave.
ReweaveOrdered == wstate["g2"] # "none" => wstate["g1"] # "none"

\* ---------------------------------------------------------------------
\* C-1: the composed path. The same LIFETIME / CONTENT split as T-1 above.
\* ---------------------------------------------------------------------

\* C-1 LIFETIME leg (the drain): a generation's pages stay backed while a host
\* composition blit is reading its resource. This is the composed-path twin of
\* NoTornScanout, and it is what BUGGY_DRAIN_SKIPPED breaks -- retiring on the
\* direct-path quiesce alone frees the pages with a blit still in flight.
NoTornCompose ==
    \A g \in Gens : InBlit(g) => backed[g]

\* C-1 CONTENT leg (the P2 ordering hazard): a blit and a fill of one host
\* resource never overlap. Stated per-GENERATION, not per-slot: all of a
\* surface's slots transfer into the same host resource at an offset, so a
\* fill of ANY slot collides with a blit. An earlier per-slot form of this
\* line was TLC-refuted -- it permitted exactly the trace where the client
\* refills slot s1 while the compositor blits it.
NoStaleCompose ==
    \A g \in Gens : InBlit(g) => (\A s \in Slots : intransfer[g][s] = 0)

\* I-45 / P1b: composition reads only what was explicitly attached to the
\* compositor context. STRUCTURAL under the actions above (ComposeBlit requires
\* attached[g]; Detach requires ~InBlit(g)), so it is stated as a regression
\* guard on those two guards -- NOT as evidence, since no modeled action can
\* falsify it. The measured evidence for the property itself is P1b's two-arm
\* probe on real virglrenderer, not this line.
ComposeNeedsAttach ==
    \A g \in Gens : InBlit(g) => attached[g]

\* C-6 LIFETIME leg: a generation's pages stay backed while a compositor
\* readback is landing in them. The readback is a host DMA WRITE into guest
\* memory, so this is the graver twin of NoTornCompose (a blit only READ the
\* host resource); BUGGY_READBACK_FREE breaks it -- retiring on the transfer +
\* blit drains alone frees the pages the device is still writing.
NoTornReadback ==
    \A g \in Gens : InRead(g) => backed[g]

Invariants ==
    /\ TypeOK
    /\ NoTornScanout
    /\ DisplayedBacked
    /\ RecycleGate
    /\ RefImpliesBacked
    /\ MappedImpliesBacked
    /\ GoneClean
    /\ NoStaleMap
    /\ ReweaveOrdered
    /\ NoTornCompose
    /\ NoStaleCompose
    /\ ComposeNeedsAttach
    /\ NoTornReadback

(***************************************************************************)
(* Liveness: a destroy always drains to full teardown (no stranded weave). *)
(***************************************************************************)

Fairness ==
    /\ \A g \in Gens : WF_vars(ClunkMap(g))
    /\ \A g \in Gens : WF_vars(ServerRelease(g))
    /\ \A g \in Gens : WF_vars(Free(g))
    /\ \A g \in Gens : WF_vars(ComposeComplete(g))
    /\ \A g \in Gens : WF_vars(ComposeReadbackComplete(g))
    /\ \A g \in Gens : \A s \in Slots : WF_vars(Complete(g, s))

Spec_Live == Spec /\ Fairness

EventuallyRetired ==
    destroyReq ~> (\A g \in Gens : wstate[g] \in {"none", "gone"})

====
