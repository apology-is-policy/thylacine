---- MODULE tapestry_present ----
(***************************************************************************)
(* Thylacine Tapestry -- the present/recycle/reweave lifecycle (G-2).      *)
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
(* names them distinctly.                                                  *)
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
    BUGGY_EARLY_FREE,          \* BOOLEAN -- recycle the slot at submit-ack (skip D1).
    BUGGY_RETIRE_NO_QUIESCE,   \* BOOLEAN -- free a retiring weave without quiesce.
    BUGGY_REWEAVE_NO_QUIESCE,  \* BOOLEAN -- free the old weave eagerly on reweave.
    BUGGY_STALE_MAP            \* BOOLEAN -- let a stale claim token resolve.

ASSUME ALLOW_DESTROY            \in BOOLEAN
ASSUME ALLOW_REWEAVE            \in BOOLEAN
ASSUME ALLOW_SERVER_DEATH       \in BOOLEAN
ASSUME BUGGY_EARLY_FREE         \in BOOLEAN
ASSUME BUGGY_RETIRE_NO_QUIESCE  \in BOOLEAN
ASSUME BUGGY_REWEAVE_NO_QUIESCE \in BOOLEAN
ASSUME BUGGY_STALE_MAP          \in BOOLEAN

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
    staleMapped, \* BOOLEAN -- history: a claim resolved against a retiring/gone weave
    destroyReq   \* BOOLEAN -- the surface destroy was requested

vars == <<wstate, backed, serverRef, mapped, armed, slot, intransfer, displayed,
          staleMapped, destroyReq>>

TypeOK ==
    /\ wstate      \in [Gens -> {"none", "woven", "live", "retiring", "gone"}]
    /\ backed      \in [Gens -> BOOLEAN]
    /\ serverRef   \in [Gens -> BOOLEAN]
    /\ mapped      \in [Gens -> BOOLEAN]
    /\ armed       \in [Gens -> BOOLEAN]
    /\ slot        \in [Gens -> [Slots -> {"free", "drawn", "pending"}]]
    /\ intransfer  \in [Gens -> [Slots -> 0..MaxInflight]]
    /\ displayed   \in Gens \cup {"nothing"}
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
    /\ staleMapped = FALSE
    /\ destroyReq  = FALSE

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
    /\ UNCHANGED <<mapped, slot, intransfer, displayed, staleMapped, destroyReq>>

Reweave ==
    /\ ALLOW_REWEAVE
    /\ ~destroyReq
    /\ wstate["g1"] = "live"
    /\ wstate["g2"] = "none"
    /\ wstate'    = [wstate    EXCEPT !["g2"] = "woven"]
    /\ backed'    = [backed    EXCEPT !["g2"] = TRUE]
    /\ serverRef' = [serverRef EXCEPT !["g2"] = TRUE]
    /\ armed'     = [armed     EXCEPT !["g2"] = TRUE]
    /\ UNCHANGED <<mapped, slot, intransfer, displayed, staleMapped, destroyReq>>

(***************************************************************************)
(* Client: the map claim (V2 grant-is-the-share; consume-once).            *)
(***************************************************************************)

Map(g) ==
    /\ armed[g]
    /\ wstate[g] \in {"woven", "live"}
    /\ mapped' = [mapped EXCEPT ![g] = TRUE]
    /\ armed'  = [armed  EXCEPT ![g] = FALSE]
    /\ wstate' = [wstate EXCEPT ![g] = "live"]
    /\ UNCHANGED <<backed, serverRef, slot, intransfer, displayed, staleMapped,
                   destroyReq>>

MapStale(g) ==
    /\ BUGGY_STALE_MAP
    /\ armed[g]
    /\ wstate[g] \in {"retiring", "gone"}
    /\ mapped'      = [mapped EXCEPT ![g] = TRUE]
    /\ armed'       = [armed  EXCEPT ![g] = FALSE]
    /\ staleMapped' = TRUE
    /\ UNCHANGED <<wstate, backed, serverRef, slot, intransfer, displayed,
                   destroyReq>>

ClunkMap(g) ==
    /\ mapped[g]
    /\ mapped' = [mapped EXCEPT ![g] = FALSE]
    /\ UNCHANGED <<wstate, backed, serverRef, armed, slot, intransfer, displayed,
                   staleMapped, destroyReq>>

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
                   displayed, staleMapped, destroyReq>>

Submit(g, s) ==
    /\ ~destroyReq
    /\ wstate[g] = "live"
    /\ mapped[g]
    /\ slot[g][s] = "drawn"
    /\ intransfer[g][s] = 0
    /\ slot'       = [slot       EXCEPT ![g][s] = "pending"]
    /\ intransfer' = [intransfer EXCEPT ![g][s] = 1]
    /\ UNCHANGED <<wstate, backed, serverRef, mapped, armed, displayed,
                   staleMapped, destroyReq>>

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
                   staleMapped, destroyReq>>

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
    /\ UNCHANGED <<wstate, backed, serverRef, mapped, armed, staleMapped,
                   destroyReq>>

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
                   displayed, staleMapped>>

RetireDisplaced ==
    /\ wstate["g1"] = "live"
    /\ displayed = "g2"
    /\ wstate' = [wstate EXCEPT !["g1"] = "retiring"]
    /\ UNCHANGED <<backed, serverRef, mapped, armed, slot, intransfer,
                   displayed, staleMapped, destroyReq>>

\* The graceful server-side ref drop: tapestryd finishes quiescing a retiring
\* weave's in-flight presents (#898), then releases its #847 handle_count ref.
\* Requires intransfer = 0 -- the graceful path NEVER drops the server ref with a
\* host DMA-read in flight (that is exactly what a crash does; ServerDeath).
ServerRelease(g) ==
    /\ wstate[g] = "retiring"
    /\ serverRef[g]
    /\ \A s \in Slots : intransfer[g][s] = 0
    /\ serverRef' = [serverRef EXCEPT ![g] = FALSE]
    /\ UNCHANGED <<wstate, backed, mapped, armed, slot, intransfer, displayed,
                   staleMapped, destroyReq>>

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
    /\ UNCHANGED <<backed, mapped, slot, intransfer, displayed, staleMapped>>

Free(g) ==
    /\ wstate[g] = "retiring"
    /\ ~serverRef[g]
    /\ ~mapped[g]
    /\ \A s \in Slots : intransfer[g][s] = 0
    /\ wstate'    = [wstate EXCEPT ![g] = "gone"]
    /\ backed'    = [backed EXCEPT ![g] = FALSE]
    /\ displayed' = IF displayed = g THEN "nothing" ELSE displayed
    /\ UNCHANGED <<serverRef, mapped, armed, slot, intransfer, staleMapped,
                   destroyReq>>

FreeNoQuiesce(g) ==
    /\ BUGGY_RETIRE_NO_QUIESCE
    /\ wstate[g] = "retiring"
    /\ wstate' = [wstate EXCEPT ![g] = "gone"]
    /\ backed' = [backed EXCEPT ![g] = FALSE]
    /\ UNCHANGED <<serverRef, mapped, armed, slot, intransfer, displayed,
                   staleMapped, destroyReq>>

ReweaveEagerFree ==
    /\ BUGGY_REWEAVE_NO_QUIESCE
    /\ wstate["g1"] = "live"
    /\ wstate["g2"] # "none"
    /\ wstate' = [wstate EXCEPT !["g1"] = "gone"]
    /\ backed' = [backed EXCEPT !["g1"] = FALSE]
    /\ UNCHANGED <<serverRef, mapped, armed, slot, intransfer, displayed,
                   staleMapped, destroyReq>>

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
    \/ \E g \in Gens, s \in Slots :
         \/ Draw(g, s) \/ Submit(g, s) \/ SubmitEarlyFree(g, s)
         \/ Complete(g, s)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Invariants (TAPESTRY.md section 18.8).                                  *)
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

(***************************************************************************)
(* Liveness: a destroy always drains to full teardown (no stranded weave). *)
(***************************************************************************)

Fairness ==
    /\ \A g \in Gens : WF_vars(ClunkMap(g))
    /\ \A g \in Gens : WF_vars(ServerRelease(g))
    /\ \A g \in Gens : WF_vars(Free(g))
    /\ \A g \in Gens : \A s \in Slots : WF_vars(Complete(g, s))

Spec_Live == Spec /\ Fairness

EventuallyRetired ==
    destroyReq ~> (\A g \in Gens : wstate[g] \in {"none", "gone"})

====
