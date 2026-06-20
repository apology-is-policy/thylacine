---- MODULE weft ----
(***************************************************************************)
(* Thylacine Weft -- the capability network dataplane (Weft-1).            *)
(*                                                                         *)
(* Weft is the per-flow, capability-scoped, zero-copy network dataplane     *)
(* (docs/NET-THROUGHPUT.md section 5; ARCH section 28 I-37). Granting a     *)
(* Proc its flow capability (a /net/<proto>/N/data fid) ALSO establishes a  *)
(* per-flow shared-page Burrow ring guest<->netd; the flow's payload bytes  *)
(* then travel through the shared page with NO per-operation mediation by   *)
(* netd; netd does the control-plane setup (the capability check, the flow  *)
(* grant, the policy) ONCE and drops out of the per-packet loop. Isolation  *)
(* is the capability grant; speed is the absence of per-op mediation.       *)
(*                                                                         *)
(* This module models the SHARED-MEMORY dataplane lifecycle and pins the    *)
(* ARCH section 28 invariant Weft reserves -- I-37 -- which GENERALIZES the *)
(* Loom I-29/I-30 pin to the cross-Proc shared buffer + the notification-   *)
(* terminal (F_NOTIF) multi-holder release. The famous io_uring ubuf_info   *)
(* buffer-lifetime race is the subtle class this exists to catch; spec-     *)
(* first is RE-ENABLED for THIS surface (NET-THROUGHPUT section 5.5, the    *)
(* fifth instance of re-enabling point (a)).                                *)
(*                                                                         *)
(* WHAT THIS SPEC PINS (the four legs of I-37; NET-THROUGHPUT section 5.5)   *)
(*                                                                         *)
(*   (1) Enforcement ENTIRELY at setup/grant, NEVER per-packet -- netd is    *)
(*       out of the per-packet path; no software toucher re-enters. The      *)
(*       flow capability is resolved + PINNED once at grant; every data op   *)
(*       proceeds under that pin, never a re-resolution of the live flow     *)
(*       binding. This is the reviewer attack of section 4.7 ("no per-op     *)
(*       mediation while per-flow in software") stated as a checkable        *)
(*       invariant: a per-op capability re-check is precisely the thing the  *)
(*       design eliminates, and (composed with a flow re-bind) it            *)
(*       reintroduces the I-30 credential-vs-work race. Pinned by            *)
(*       NoPerOpMediation + ActedUnderFlowPin.                               *)
(*                                                                         *)
(*   (2) The shared payload page's lifetime is correct under the MULTI-      *)
(*       HOLDER completion. A registered send buffer is not reused/freed     *)
(*       until the LAST of {netd stack done, NIC DMA done, peer ACK}         *)
(*       releases it -- the two-CQE F_NOTIF contract: the I-30 pin releases  *)
(*       at NOTIFICATION-terminal, NOT op-terminal (the result CQE = netd    *)
(*       done). Releasing at op-terminal reuses a page the NIC may still be  *)
(*       DMAing or TCP may still retransmit = an in-flight-page UAF. Pinned  *)
(*       by PinHeldWhileInFlight + NoInFlightReuse.                          *)
(*                                                                         *)
(*   (3) Ring TOCTOU. The descriptor ring fields ({burrow-relative addr,    *)
(*       len}) live in a Burrow the guest shares with the kernel and can     *)
(*       mutate CONCURRENTLY. The kernel COPIES each descriptor to kernel    *)
(*       memory at consume, VALIDATES the bounds against the registered      *)
(*       Burrow, and acts ONLY on that snapshot -- never re-reads the shared *)
(*       slot after the check, or a post-check mutation drives an unvalidated *)
(*       (out-of-bounds) descriptor into the in-place payload access. The     *)
(*       Loom I-30 ring-TOCTOU (loom.tla ArgPinnedToSnapshot) lifted to the  *)
(*       Weft descriptor ring. Pinned by DescPinnedToSnapshot +              *)
(*       ActedDescValidated.                                                  *)
(*                                                                         *)
(*   (4) The shared Burrow's lifetime is bounded by the flow + dual-         *)
(*       refcounted (#847, I-7). A confined Proc reaches only the flows its  *)
(*       namespace grants (I-1/I-28), the ring is per-flow, netd owns the    *)
(*       NIC (I-5). On flow teardown the share is fully dropped (both #847   *)
(*       refs -> the Burrow frees); the share NEVER outlives the flow (a     *)
(*       leaked mapping a later flow's page reuses would be a cross-flow     *)
(*       isolation break). Pinned by ShareBoundedByFlow + NoStaleShareAccess.*)
(*                                                                         *)
(*   (5) The descriptor ring is split-ring UNIDIRECTIONAL (one writer per    *)
(*       region) so SMP correctness holds without a per-op lock -- STRUCTURAL,*)
(*       not a reachable-state invariant (the loom.tla precedent: the SQ/CQ  *)
(*       split-ring is encoded in the action separation, not a checked       *)
(*       property). Here the guest is the SOLE writer of the descriptor slot *)
(*       (GuestPostDesc / GuestMutateDesc) and the kernel is the SOLE writer *)
(*       of the completion side (holders / the release); no shared region    *)
(*       has two concurrent writers. The snapshot discipline (Consume copies *)
(*       + DescPinnedToSnapshot) is exactly what makes the concurrent guest  *)
(*       mutation of the one-writer descriptor region safe to read lock-free.*)
(*                                                                         *)
(* THE BUFFER/FLOW LIFECYCLE                                                 *)
(*                                                                         *)
(* The flow (one /net data fid + its per-flow shared Burrow) is "active"     *)
(* from grant (Init) until Teardown moves it "torndown". share_refs models  *)
(* the #847 dual-refcount on the shared ring Burrow ({"guest","netd"} =      *)
(* mapping_count + handle_count); the Burrow is mapped while share_refs # {}.*)
(* flow_pin is the capability resolved + PINNED at grant (immutable);        *)
(* flow_cap_live is the live /net-fid binding, which a clunk + reuse / a     *)
(* revoke-rebind can repoint (RebindFlowCap -- the loom.tla UserRegister     *)
(* analog).                                                                  *)
(*                                                                         *)
(* Each send buffer b \in Bufs (a registered payload page within the share)  *)
(* runs the per-op lifecycle:                                                *)
(*                                                                         *)
(*   "free"       -- not registered.                                        *)
(*   "registered" -- the I-30 buffer pin is held; the guest may write the    *)
(*                   payload + the shared descriptor slot.                   *)
(*   "submitted"  -- the guest posted the descriptor into the shared ring    *)
(*                   and bumped the tail. The TOCTOU window: desc[b] is      *)
(*                   still guest-mutable.                                     *)
(*   "snapped"    -- the kernel CONSUMED the descriptor: copied it to kernel *)
(*                   memory (snap_desc), validated the bounds. ADMITTED.     *)
(*   "sending"    -- netd processed the payload IN PLACE (acted under the    *)
(*                   flow pin, no per-op mediation) and queued the send;     *)
(*                   the page now has the multi-holder set                    *)
(*                   holders = {"netd","nic","ack"} pending (the F_NOTIF      *)
(*                   in-flight window). The buffer pin is still HELD.         *)
(*   "released"   -- the LAST holder released; the I-30 pin is dropped; the  *)
(*                   page is reusable (the notification-terminal CQE).       *)
(*   "abandoned"  -- teardown quiesced this buffer (the #898 quiesce /        *)
(*                   #811 death-interruptible unwind); no reuse.             *)
(*                                                                         *)
(* THE BUGS THIS PINS (each a BUGGY_* flag, each its own cfg)               *)
(*                                                                         *)
(*   BUGGY_PREMATURE_RELEASE -- ReleasePremature drops the I-30 pin at       *)
(*     OP-terminal (netd's stack done = the result CQE) instead of at        *)
(*     NOTIFICATION-terminal (the LAST of {netd,nic,ack}). The page is       *)
(*     reusable while the NIC may still DMA from it / TCP may still          *)
(*     retransmit -> ReuseBuffer rewrites an in-flight page = the io_uring   *)
(*     ubuf_info UAF (PinHeldWhileInFlight + NoInFlightReuse counterexample).*)
(*                                                                         *)
(*   BUGGY_RECHECK_PER_OP -- NetdAct RE-RESOLVES the live flow capability    *)
(*     per data op instead of using the submit-time pin -- the reviewer      *)
(*     attack (a software toucher creeps back into the per-packet path). It  *)
(*     violates the no-mediation property directly, and (composed with a     *)
(*     RebindFlowCap) makes the op act under a DIFFERENT capability than the *)
(*     grant pinned (NoPerOpMediation + ActedUnderFlowPin counterexample).   *)
(*                                                                         *)
(*   BUGGY_RING_TOCTOU -- NetdAct re-reads the LIVE shared descriptor slot   *)
(*     instead of the kernel snapshot. A guest mutation between consume and  *)
(*     the in-place access drives an unvalidated (out-of-bounds) descriptor  *)
(*     into the op (DescPinnedToSnapshot + ActedDescValidated counterexample)*)
(*                                                                         *)
(*   BUGGY_SHARE_OUTLIVES_FLOW -- Teardown moves the flow "torndown" but     *)
(*     does NOT drop the shared Burrow's refs (the #847 dual-refcount does   *)
(*     not reach 0); the share outlives the flow, so a stale mapping is      *)
(*     reachable past teardown (ShareBoundedByFlow + NoStaleShareAccess      *)
(*     counterexample) -- the allowance.tla RevokedFullyCleared analog for   *)
(*     the shared-page lifetime.                                            *)
(*                                                                         *)
(* CFG MATRIX (executable documentation per CLAUDE.md spec-first policy)     *)
(*                                                                         *)
(*   weft.cfg                          all BUGGY_* FALSE, ALLOW_TEARDOWN     *)
(*                                      TRUE -- every safety invariant holds *)
(*                                      (incl. correct teardown).            *)
(*   weft_liveness.cfg                 Spec_Live, ALLOW_TEARDOWN FALSE --    *)
(*                                      EventuallyReleased (the F_NOTIF       *)
(*                                      multi-holder release never strands a *)
(*                                      buffer: an in-flight send always      *)
(*                                      eventually drops its pin).            *)
(*   weft_buggy_premature_release.cfg  BUGGY_PREMATURE_RELEASE --            *)
(*                                      PinHeldWhileInFlight counterexample.  *)
(*   weft_buggy_recheck_per_op.cfg     BUGGY_RECHECK_PER_OP --               *)
(*                                      NoPerOpMediation counterexample.      *)
(*   weft_buggy_ring_toctou.cfg        BUGGY_RING_TOCTOU --                  *)
(*                                      DescPinnedToSnapshot counterexample.  *)
(*   weft_buggy_share_outlives_flow.cfg BUGGY_SHARE_OUTLIVES_FLOW --         *)
(*                                      ShareBoundedByFlow counterexample.    *)
(*                                                                         *)
(* MODELING ASSUMPTIONS                                                      *)
(*                                                                         *)
(*   Bufs = the registered payload pages (2 in the clean/liveness cfgs:      *)
(*   exercises two concurrent sends + the page-reuse race; 1 in the buggy    *)
(*   cfgs is sufficient for the single-buffer counterexamples). ONE flow     *)
(*   suffices: the share leak (torndown /\ share_refs # {}) is a single-flow *)
(*   property; the cross-flow page-reuse it enables is the consequence.      *)
(*   Actions are atomic (the impl's per-client lock discipline -- the same   *)
(*   atomic-action choice loom.tla / 9p_client.tla make). The 9P control     *)
(*   framing under which the payload rides (tag uniqueness I-10, fid         *)
(*   stability I-11, out-of-order match) is 9p_client.tla's proof and is NOT *)
(*   re-pinned here; Weft optimizes only the byte-movement under the         *)
(*   Tread/Twrite (NET-THROUGHPUT section 4.8). The completion-ring          *)
(*   integrity (exactly-one CQE, CQ back-pressure, no stale-across-teardown) *)
(*   is loom.tla / loom_multishot.tla's proof; Weft adds the cross-Proc      *)
(*   SHARED-PAGE lifetime above that engine.                                 *)
(*                                                                         *)
(*   ENGINE: Weft rides the Loom async path (NET-DESIGN section 12). This    *)
(*   module models the DATAPLANE (the shared page + the flow cap + the       *)
(*   multi-holder release), not the SQ/CQ transport (loom.tla's proof).      *)
(*                                                                         *)
(* SPEC-TO-CODE (the impl this gates, OWED at Weft-2..7):                    *)
(*   RebindFlowCap     <-> a clunk + reuse / revoke-rebind of the /net data  *)
(*                         fid (the flow capability binding).                *)
(*   Register          <-> SYS_LOOM_REGISTER of a payload page within the    *)
(*                         per-flow shared Burrow (the I-30 buffer pin).     *)
(*   GuestPostDesc     <-> the guest writes a {addr,len} descriptor into the *)
(*                         shared split-ring + bumps the tail.               *)
(*   Consume           <-> the kernel copies + bounds-validates the          *)
(*                         descriptor against the registered Burrow.         *)
(*   NetdAct           <-> netd reads the payload IN PLACE under the flow     *)
(*                         pin (no per-op re-check) + queues the smoltcp send*)
(*   HolderRelease     <-> netd-stack-done / NIC-DMA-done / peer-ACK, each    *)
(*                         clearing one F_NOTIF holder.                       *)
(*   ReleaseClean      <-> the notification-terminal CQE: pin released when   *)
(*                         the LAST holder clears.                            *)
(*   Teardown          <-> the /net data fid clunk: quiesce in-flight + drop  *)
(*                         BOTH #847 share refs (the Burrow frees).          *)
(*   See specs/SPEC-TO-CODE.md (weft.tla) once the impl lands.               *)
(*                                                                         *)
(* See docs/NET-THROUGHPUT.md (the design), ARCHITECTURE.md section 28       *)
(* (I-37 reserved), loom.tla (the I-29/I-30 engine Weft generalizes),        *)
(* allowance.tla (the revoke-vs-create teardown-race lineage), burrow.tla    *)
(* (the #847 dual-refcount this share lifetime extends).                     *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Bufs,                          \* the registered payload pages (op identities).
    ALLOW_TEARDOWN,                \* BOOLEAN -- TRUE: the flow may tear down mid-flight
                                   \*   (safety cfgs); FALSE: no teardown (liveness cfg).
    BUGGY_PREMATURE_RELEASE,       \* BOOLEAN -- release the pin at op-terminal (F_NOTIF UAF).
    BUGGY_RECHECK_PER_OP,          \* BOOLEAN -- re-resolve the flow cap per op (reviewer attack).
    BUGGY_RING_TOCTOU,             \* BOOLEAN -- act on the live shared descriptor, not the snapshot.
    BUGGY_SHARE_OUTLIVES_FLOW      \* BOOLEAN -- teardown leaves the share mapped (the #847 leak).

ASSUME Bufs # {}
ASSUME ALLOW_TEARDOWN            \in BOOLEAN
ASSUME BUGGY_PREMATURE_RELEASE   \in BOOLEAN
ASSUME BUGGY_RECHECK_PER_OP      \in BOOLEAN
ASSUME BUGGY_RING_TOCTOU         \in BOOLEAN
ASSUME BUGGY_SHARE_OUTLIVES_FLOW \in BOOLEAN

\* The descriptor domain (the {addr,len} the guest writes into the shared
\* ring). ValidDescs is the bounds-check gate at consume; "desc_bad" models an
\* out-of-bounds descriptor a malicious guest writes into the shared slot AFTER
\* the kernel snapshots a good one.
Descs      == {"desc_ok", "desc_bad"}
ValidDescs == {"desc_ok"}
NONE_DESC  == "desc_none"          \* sentinel: no snapshot / no acted value yet.

\* The flow capability domain. flow_pin (resolved at grant) is "cap_granted";
\* "cap_other" models the object a clunk + reuse repoints the live /net fid to.
Caps     == {"cap_granted", "cap_other"}
NONE_CAP == "cap_none"             \* sentinel: no op cap acted yet.

\* The #847 dual-refcount on the shared ring Burrow: the guest mapping +
\* netd's mapping (mapping_count + handle_count). The Burrow is mapped while
\* this set is non-empty; it frees when both drop (teardown).
ShareRefHolders == {"guest", "netd"}

\* The F_NOTIF multi-holder set of an in-flight send page: netd's stack still
\* holds it, the NIC may still DMA from it, the peer has not yet ACKed (TCP may
\* retransmit). The page is reusable only when ALL THREE have released.
Holders == {"netd", "nic", "ack"}

Phases == {"free", "registered", "submitted", "snapped", "sending",
           "released", "abandoned"}

VARIABLES
    flow,          \* {"active","torndown"} -- the per-flow lifecycle.
    flow_pin,      \* Caps -- the capability PINNED at grant (immutable).
    flow_cap_live, \* Caps -- the live /net-fid binding (RebindFlowCap repoints it).
    share_refs,    \* SUBSET ShareRefHolders -- the #847 dual-refcount on the shared Burrow.
    stale_share,   \* BOOLEAN -- ghost: a share access happened after teardown (leak consequence).
    bphase,        \* [Bufs -> Phases] -- each buffer's lifecycle stage.
    pinned,        \* [Bufs -> BOOLEAN] -- the I-30 per-buffer pin is held.
    holders,       \* [Bufs -> SUBSET Holders] -- the in-flight F_NOTIF holders.
    desc,          \* [Bufs -> Descs] -- the SHARED descriptor slot (guest-mutable).
    snap_desc,     \* [Bufs -> Descs \cup {NONE_DESC}] -- kernel copy taken at consume.
    acted_desc,    \* [Bufs -> Descs \cup {NONE_DESC}] -- the descriptor the op ACTED on.
    acted_cap,     \* [Bufs -> Caps \cup {NONE_CAP}] -- the cap the op ACTED under.
    mediated,      \* [Bufs -> BOOLEAN] -- ghost: netd did a per-op capability mediation.
    uaf            \* [Bufs -> BOOLEAN] -- ghost: a reuse-while-in-flight happened (the UAF).

vars == <<flow, flow_pin, flow_cap_live, share_refs, stale_share, bphase,
          pinned, holders, desc, snap_desc, acted_desc, acted_cap, mediated, uaf>>

\* Buffers the teardown quiesce must reach (admitted, not yet released/free).
LiveBufs == {b \in Bufs : bphase[b] \in {"registered", "submitted", "snapped", "sending"}}

\* The shared ring Burrow is mapped while either #847 ref is held.
ShareMapped == share_refs # {}

TypeOk ==
    /\ flow          \in {"active", "torndown"}
    /\ flow_pin      \in Caps
    /\ flow_cap_live \in Caps
    /\ share_refs    \subseteq ShareRefHolders
    /\ stale_share   \in BOOLEAN
    /\ bphase        \in [Bufs -> Phases]
    /\ pinned        \in [Bufs -> BOOLEAN]
    /\ holders       \in [Bufs -> SUBSET Holders]
    /\ desc          \in [Bufs -> Descs]
    /\ snap_desc     \in [Bufs -> Descs \cup {NONE_DESC}]
    /\ acted_desc    \in [Bufs -> Descs \cup {NONE_DESC}]
    /\ acted_cap     \in [Bufs -> Caps \cup {NONE_CAP}]
    /\ mediated      \in [Bufs -> BOOLEAN]
    /\ uaf           \in [Bufs -> BOOLEAN]

(***************************************************************************)
(* Init: the flow is granted + active, its shared Burrow mapped into both   *)
(* guest and netd (#847 dual ref), the capability resolved + pinned. No      *)
(* buffer registered. (The grant -- the control-plane setup that resolves    *)
(* the cap + shares the Burrow -- is implicit in Init; the model focuses on  *)
(* the data plane + teardown, where the bugs live.)                          *)
(***************************************************************************)
Init ==
    /\ flow          = "active"
    /\ flow_pin      = "cap_granted"
    /\ flow_cap_live = "cap_granted"
    /\ share_refs    = ShareRefHolders
    /\ stale_share   = FALSE
    /\ bphase        = [b \in Bufs |-> "free"]
    /\ pinned        = [b \in Bufs |-> FALSE]
    /\ holders       = [b \in Bufs |-> {}]
    /\ desc          = [b \in Bufs |-> "desc_ok"]   \* slot irrelevant until posted
    /\ snap_desc     = [b \in Bufs |-> NONE_DESC]
    /\ acted_desc    = [b \in Bufs |-> NONE_DESC]
    /\ acted_cap     = [b \in Bufs |-> NONE_CAP]
    /\ mediated      = [b \in Bufs |-> FALSE]
    /\ uaf           = [b \in Bufs |-> FALSE]

(***************************************************************************)
(* RebindFlowCap -- the live /net data fid is repointed to a different       *)
(* capability (a clunk + reuse, or a revoke-rebind). The grant-time pin       *)
(* (flow_pin) is immutable; only the live binding moves. This is exactly the *)
(* mutation the setup-time pin protects an in-flight op against (loom.tla's  *)
(* UserRegister analog) -- a per-op re-resolution (BUGGY_RECHECK_PER_OP)     *)
(* loses to it.                                                              *)
(***************************************************************************)
RebindFlowCap(c) ==
    /\ flow = "active"
    /\ c \in Caps
    /\ c # flow_cap_live
    /\ flow_cap_live' = c
    /\ UNCHANGED <<flow, flow_pin, share_refs, stale_share, bphase, pinned,
                   holders, desc, snap_desc, acted_desc, acted_cap, mediated, uaf>>

(***************************************************************************)
(* Register(b) -- the guest registers a payload page within the per-flow     *)
(* shared Burrow (SYS_LOOM_REGISTER). The I-30 buffer pin is taken; the      *)
(* page is now writable by the guest. Requires the flow active + the share   *)
(* mapped (the ring exists).                                                 *)
(***************************************************************************)
Register(b) ==
    /\ flow = "active"
    /\ ShareMapped
    /\ bphase[b] = "free"
    /\ bphase'  = [bphase  EXCEPT ![b] = "registered"]
    /\ pinned'  = [pinned  EXCEPT ![b] = TRUE]
    /\ UNCHANGED <<flow, flow_pin, flow_cap_live, share_refs, stale_share,
                   holders, desc, snap_desc, acted_desc, acted_cap, mediated, uaf>>

(***************************************************************************)
(* GuestPostDesc(b, d) -- the guest writes a descriptor into the shared      *)
(* split-ring slot and bumps the tail. d may be valid OR out-of-bounds;      *)
(* admission (Consume) gates it. The TOCTOU window opens (desc[b] is shared  *)
(* and still guest-mutable until the kernel snapshots + acts).               *)
(***************************************************************************)
GuestPostDesc(b, d) ==
    /\ flow = "active"
    /\ bphase[b] = "registered"
    /\ d \in Descs
    /\ bphase' = [bphase EXCEPT ![b] = "submitted"]
    /\ desc'   = [desc   EXCEPT ![b] = d]
    /\ UNCHANGED <<flow, flow_pin, flow_cap_live, share_refs, stale_share,
                   pinned, holders, snap_desc, acted_desc, acted_cap, mediated, uaf>>

(***************************************************************************)
(* GuestMutateDesc(b, d) -- the adversary mutates the SHARED descriptor slot *)
(* concurrently with kernel processing (the ring TOCTOU). Legal while the    *)
(* slot is still live (submitted..sending): models a guest thread racing the *)
(* kernel's read of the ring. The CORRECT kernel already snapshotted at      *)
(* consume and is immune; a re-reading kernel (BUGGY_RING_TOCTOU) is not.    *)
(***************************************************************************)
GuestMutateDesc(b, d) ==
    /\ flow = "active"
    /\ bphase[b] \in {"submitted", "snapped", "sending"}
    /\ d \in Descs
    /\ d # desc[b]
    /\ desc' = [desc EXCEPT ![b] = d]
    /\ UNCHANGED <<flow, flow_pin, flow_cap_live, share_refs, stale_share,
                   bphase, pinned, holders, snap_desc, acted_desc, acted_cap, mediated, uaf>>

(***************************************************************************)
(* Consume(b) -- the kernel admits a submitted descriptor: COPIES it to      *)
(* kernel memory (snap_desc) and VALIDATES the bounds (snap must be in        *)
(* ValidDescs -- an out-of-bounds descriptor is rejected at the gate). From  *)
(* here the kernel acts only on the snapshot.                                *)
(***************************************************************************)
Consume(b) ==
    /\ flow = "active"
    /\ bphase[b] = "submitted"
    /\ desc[b] \in ValidDescs                       \* bounds-validate the snapshot (admission)
    /\ bphase'    = [bphase    EXCEPT ![b] = "snapped"]
    /\ snap_desc' = [snap_desc EXCEPT ![b] = desc[b]]    \* copy the descriptor field
    /\ UNCHANGED <<flow, flow_pin, flow_cap_live, share_refs, stale_share,
                   pinned, holders, desc, acted_desc, acted_cap, mediated, uaf>>

(***************************************************************************)
(* NetdAct(b) -- netd reads the payload IN PLACE and queues the smoltcp      *)
(* send; the page enters the F_NOTIF in-flight window with all three          *)
(* holders pending. CORRECT: acts on the descriptor SNAPSHOT (snap_desc) and *)
(* under the grant-time flow PIN (flow_pin), with NO per-op mediation.        *)
(*                                                                         *)
(* BUGGY_RING_TOCTOU: re-reads the live shared descriptor (desc) -- a guest  *)
(* mutation after consume drives an unvalidated value in. BUGGY_RECHECK_PER  *)
(* _OP: re-resolves the LIVE flow cap (flow_cap_live) per op AND records the *)
(* mediation -- the reviewer attack (a per-packet software toucher), which a *)
(* RebindFlowCap makes act under the wrong capability.                       *)
(***************************************************************************)
NetdAct(b) ==
    /\ flow = "active"
    /\ bphase[b] = "snapped"
    /\ bphase'     = [bphase     EXCEPT ![b] = "sending"]
    /\ holders'    = [holders    EXCEPT ![b] = Holders]          \* {netd, nic, ack} pending
    /\ acted_desc' = [acted_desc EXCEPT ![b] =
                        IF BUGGY_RING_TOCTOU THEN desc[b]        \* re-read shared descriptor
                        ELSE snap_desc[b]]                       \* act on the snapshot
    /\ acted_cap'  = [acted_cap  EXCEPT ![b] =
                        IF BUGGY_RECHECK_PER_OP THEN flow_cap_live  \* re-resolve the live cap
                        ELSE flow_pin]                           \* act under the grant pin
    /\ mediated'   = [mediated   EXCEPT ![b] = BUGGY_RECHECK_PER_OP]
    /\ UNCHANGED <<flow, flow_pin, flow_cap_live, share_refs, stale_share,
                   pinned, desc, snap_desc, uaf>>

(***************************************************************************)
(* HolderRelease(b, h) -- one F_NOTIF holder clears: netd's stack finished   *)
(* with the page / the NIC DMA completed / the peer ACKed. Any order, any    *)
(* interleaving. The page is reusable only once ALL THREE have cleared. Also *)
(* permitted in "released" (a late DMA/ACK finishing after a -- premature -- *)
(* pin drop): the model explores the DMA finishing before OR after a reuse.  *)
(***************************************************************************)
HolderRelease(b, h) ==
    /\ bphase[b] \in {"sending", "released"}
    /\ h \in holders[b]
    /\ holders' = [holders EXCEPT ![b] = holders[b] \ {h}]
    /\ UNCHANGED <<flow, flow_pin, flow_cap_live, share_refs, stale_share,
                   bphase, pinned, desc, snap_desc, acted_desc, acted_cap, mediated, uaf>>

(***************************************************************************)
(* ReleaseClean(b) -- the NOTIFICATION-terminal CQE: the I-30 buffer pin is  *)
(* released ONLY once the LAST holder clears (holders[b] = {}). The page is  *)
(* now reusable. This is the F_NOTIF contract done right.                    *)
(***************************************************************************)
ReleaseClean(b) ==
    /\ bphase[b] = "sending"
    /\ holders[b] = {}                              \* the last of {netd,nic,ack} cleared
    /\ bphase' = [bphase EXCEPT ![b] = "released"]
    /\ pinned' = [pinned EXCEPT ![b] = FALSE]
    /\ UNCHANGED <<flow, flow_pin, flow_cap_live, share_refs, stale_share,
                   holders, desc, snap_desc, acted_desc, acted_cap, mediated, uaf>>

(***************************************************************************)
(* ReleasePremature(b) -- THE F_NOTIF UAF bug class: drop the I-30 pin at    *)
(* OP-terminal (netd's stack done = the result CQE) regardless of the NIC    *)
(* DMA / peer ACK still pending. The page is marked reusable while a holder  *)
(* may still be reading it. Only reachable under BUGGY_PREMATURE_RELEASE.    *)
(* Caught by PinHeldWhileInFlight (the pin dropped with holders # {}) and,   *)
(* via a subsequent ReuseBuffer, NoInFlightReuse (the actual UAF).            *)
(***************************************************************************)
ReleasePremature(b) ==
    /\ BUGGY_PREMATURE_RELEASE
    /\ bphase[b] = "sending"
    /\ "netd" \notin holders[b]                     \* netd done (op-terminal) -- nic/ack may pend
    /\ bphase' = [bphase EXCEPT ![b] = "released"]
    /\ pinned' = [pinned EXCEPT ![b] = FALSE]       \* THE BUG: pin dropped before notification-terminal
    /\ UNCHANGED <<flow, flow_pin, flow_cap_live, share_refs, stale_share,
                   holders, desc, snap_desc, acted_desc, acted_cap, mediated, uaf>>

(***************************************************************************)
(* ReuseBuffer(b) -- the guest re-registers a released page to a NEW op,     *)
(* rewriting its bytes. CORRECT (after ReleaseClean): holders[b] = {} -- the  *)
(* data plane is done, the reuse is safe. After a PREMATURE release a holder *)
(* may still be pending: rewriting the page now corrupts the in-flight NIC   *)
(* DMA / TCP retransmit -- the UAF (uaf ghost). Requires the flow active +   *)
(* the share mapped.                                                         *)
(***************************************************************************)
ReuseBuffer(b) ==
    /\ flow = "active"
    /\ ShareMapped
    /\ bphase[b] = "released"
    /\ uaf'       = [uaf       EXCEPT ![b] = uaf[b] \/ (holders[b] # {})]   \* in-flight reuse = UAF
    /\ bphase'    = [bphase    EXCEPT ![b] = "registered"]                  \* re-registered, page rewritten
    /\ pinned'    = [pinned    EXCEPT ![b] = TRUE]
    /\ holders'   = [holders   EXCEPT ![b] = {}]                            \* the new op has no holders yet
    /\ snap_desc' = [snap_desc EXCEPT ![b] = NONE_DESC]
    /\ acted_desc'= [acted_desc EXCEPT ![b] = NONE_DESC]
    /\ acted_cap' = [acted_cap EXCEPT ![b] = NONE_CAP]
    /\ mediated'  = [mediated  EXCEPT ![b] = FALSE]
    /\ UNCHANGED <<flow, flow_pin, flow_cap_live, share_refs, stale_share, desc>>

(***************************************************************************)
(* Teardown -- the /net data fid is clunked: the flow ends. CORRECT:         *)
(* QUIESCE every live buffer (the #898 quiesce / #811 death-interruptible     *)
(* unwind -- move it "abandoned", drop its pin + holders) AND drop BOTH #847 *)
(* share refs so the shared Burrow frees (share_refs := {}). A released page  *)
(* is already reusable; a free page is untouched.                            *)
(*                                                                         *)
(* BUGGY_SHARE_OUTLIVES_FLOW quiesces the buffers the same way but does NOT  *)
(* drop the share refs (the #847 dual-refcount does not reach 0 -- a leaked   *)
(* netd mapping) -- so the shared Burrow outlives the flow.                  *)
(***************************************************************************)
Teardown ==
    /\ ALLOW_TEARDOWN
    /\ flow = "active"
    /\ flow'    = "torndown"
    /\ bphase'  = [b \in Bufs |->
                     IF bphase[b] \in {"registered", "submitted", "snapped", "sending"}
                     THEN "abandoned" ELSE bphase[b]]
    /\ pinned'  = [b \in Bufs |->
                     IF bphase[b] \in {"registered", "submitted", "snapped", "sending"}
                     THEN FALSE ELSE pinned[b]]
    /\ holders' = [b \in Bufs |->
                     IF bphase[b] \in {"registered", "submitted", "snapped", "sending"}
                     THEN {} ELSE holders[b]]
    /\ share_refs' = IF BUGGY_SHARE_OUTLIVES_FLOW
                     THEN share_refs                 \* THE BUG: the #847 refs not dropped
                     ELSE {}                          \* drop both refs -> the Burrow frees
    /\ UNCHANGED <<flow_pin, flow_cap_live, stale_share, desc, snap_desc,
                   acted_desc, acted_cap, mediated, uaf>>

(***************************************************************************)
(* StaleShareAccess -- the consequence of a leaked share: a mapping reachable *)
(* AFTER the flow tore down is accessed (a stale guest/netd read of a torn-  *)
(* down flow's ring, or -- once the page is reused by a later flow -- a       *)
(* cross-flow read = the I-1 isolation break). Only reachable when the share *)
(* outlived the flow (clean teardown dropped share_refs, disabling this).    *)
(* Caught by NoStaleShareAccess.                                             *)
(***************************************************************************)
StaleShareAccess ==
    /\ flow = "torndown"
    /\ ShareMapped                                  \* a leaked mapping is still reachable
    /\ stale_share' = TRUE
    /\ UNCHANGED <<flow, flow_pin, flow_cap_live, share_refs, bphase, pinned,
                   holders, desc, snap_desc, acted_desc, acted_cap, mediated, uaf>>

(***************************************************************************)
(* Done -- terminal self-loop. Once the flow is torn down, the share         *)
(* dropped, and no buffer remains in an advanceable phase, the model halts;  *)
(* the explicit stutter keeps the legitimate terminal state from reading as  *)
(* a deadlock.                                                               *)
(***************************************************************************)
Done ==
    /\ flow = "torndown"
    /\ ~ShareMapped
    /\ \A b \in Bufs : bphase[b] \notin {"registered", "submitted", "snapped", "sending"}
    /\ UNCHANGED vars

Next ==
    \/ \E c \in Caps : RebindFlowCap(c)
    \/ \E b \in Bufs : Register(b)
    \/ \E b \in Bufs, d \in Descs : GuestPostDesc(b, d)
    \/ \E b \in Bufs, d \in Descs : GuestMutateDesc(b, d)
    \/ \E b \in Bufs : Consume(b)
    \/ \E b \in Bufs : NetdAct(b)
    \/ \E b \in Bufs, h \in Holders : HolderRelease(b, h)
    \/ \E b \in Bufs : ReleaseClean(b)
    \/ \E b \in Bufs : ReleasePremature(b)
    \/ \E b \in Bufs : ReuseBuffer(b)
    \/ Teardown
    \/ StaleShareAccess
    \/ Done

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

\* I-37 (1) NoPerOpMediation -- netd never performs a per-op capability        \
\* mediation: enforcement is entirely at grant time, never per-packet. The   \
\* novel-engineering property of section 5.3 (the reviewer attack). Violated \
\* by BUGGY_RECHECK_PER_OP.
NoPerOpMediation ==
    \A b \in Bufs : ~mediated[b]

\* I-37 (1) ActedUnderFlowPin -- every data op acted under the capability      \
\* PINNED at grant, never a re-resolution of the live flow binding. The       \
\* soundness consequence of no-mediation: a per-op re-check, composed with a  \
\* RebindFlowCap, makes the op act under a different cap (the I-30 credential- \
\* vs-work race). Violated by BUGGY_RECHECK_PER_OP (with a rebind).
ActedUnderFlowPin ==
    \A b \in Bufs : acted_cap[b] # NONE_CAP => acted_cap[b] = flow_pin

\* I-37 (2) PinHeldWhileInFlight -- the I-30 buffer pin is held as long as     \
\* ANY F_NOTIF holder remains: the page is never released before the LAST of  \
\* {netd,nic,ack} (the notification-terminal). Violated by                    \
\* BUGGY_PREMATURE_RELEASE (op-terminal release with holders pending).
PinHeldWhileInFlight ==
    \A b \in Bufs : (holders[b] # {}) => pinned[b]

\* I-37 (2) NoInFlightReuse -- no page is ever reused/rewritten while the NIC  \
\* DMA or peer ACK still holds it (the io_uring ubuf_info UAF). The direct     \
\* corruption consequence of a premature release. Violated by                 \
\* BUGGY_PREMATURE_RELEASE (premature drop -> ReuseBuffer while holders # {}).
NoInFlightReuse ==
    \A b \in Bufs : ~uaf[b]

\* I-37 (3) DescPinnedToSnapshot -- the descriptor the op acted on equals the  \
\* kernel snapshot, never a later re-read of the shared ring. The Loom        \
\* ring-TOCTOU lifted to the Weft descriptor. Violated by BUGGY_RING_TOCTOU.
DescPinnedToSnapshot ==
    \A b \in Bufs : acted_desc[b] # NONE_DESC => acted_desc[b] = snap_desc[b]

\* I-37 (3) ActedDescValidated -- the kernel only ever acts on a descriptor    \
\* that passed the submit-time bounds gate. The security consequence of the   \
\* TOCTOU fix: an out-of-bounds descriptor never drives an in-place access.   \
\* Violated by BUGGY_RING_TOCTOU.
ActedDescValidated ==
    \A b \in Bufs : acted_desc[b] # NONE_DESC => acted_desc[b] \in ValidDescs

\* I-37 (4) ShareBoundedByFlow -- the shared ring Burrow never outlives the    \
\* flow: once torn down, both #847 refs are dropped (the Burrow frees). The   \
\* allowance.tla RevokedFullyCleared analog for the shared-page lifetime.     \
\* Violated by BUGGY_SHARE_OUTLIVES_FLOW.
ShareBoundedByFlow ==
    (flow = "torndown") => (share_refs = {})

\* I-37 (4) NoStaleShareAccess -- no access to the shared ring ever happens    \
\* after the flow tore down (the leaked-mapping consequence: a cross-flow     \
\* read once the page is reused = the I-1 isolation break). Violated by       \
\* BUGGY_SHARE_OUTLIVES_FLOW.
NoStaleShareAccess ==
    ~stale_share

\* DescSnapValidated -- whenever the kernel holds a descriptor snapshot, it    \
\* passed the bounds gate. The structural backing of ActedDescValidated.
DescSnapValidated ==
    \A b \in Bufs : snap_desc[b] # NONE_DESC => snap_desc[b] \in ValidDescs

\* PinTracksLifecycle -- the I-30 buffer pin is held exactly across the        \
\* registered..sending lifetime and dropped otherwise. The structural backing \
\* of PinHeldWhileInFlight (no stray pin on a free/released/abandoned buffer). \
PinTracksLifecycle ==
    \A b \in Bufs : pinned[b] <=> (bphase[b] \in {"registered", "submitted", "snapped", "sending"})

\* HoldersOnlyWhileSendingOrReleased -- the F_NOTIF holder set is non-empty    \
\* only on a page netd has queued (sending) or just released (a late          \
\* DMA/ACK). A free/registered/snapped/abandoned page holds none.
HoldersBounded ==
    \A b \in Bufs : (holders[b] # {}) => (bphase[b] \in {"sending", "released"})

\* QuiescedAfterTeardown -- after teardown no buffer remains admitted          \
\* (registered/submitted/snapped/sending): the quiesce reached them all, so    \
\* no late access posts into a freed share. The loom.tla NoStaleCompletion     \
\* analog for the dataplane.
QuiescedAfterTeardown ==
    (flow = "torndown") =>
        \A b \in Bufs : bphase[b] \notin {"registered", "submitted", "snapped", "sending"}

Invariants ==
    /\ TypeOk
    /\ NoPerOpMediation
    /\ ActedUnderFlowPin
    /\ PinHeldWhileInFlight
    /\ NoInFlightReuse
    /\ DescPinnedToSnapshot
    /\ ActedDescValidated
    /\ ShareBoundedByFlow
    /\ NoStaleShareAccess
    /\ DescSnapValidated
    /\ PinTracksLifecycle
    /\ HoldersBounded
    /\ QuiescedAfterTeardown

(***************************************************************************)
(* ============================== LIVENESS ================================ *)
(*                                                                         *)
(* EventuallyReleased -- the F_NOTIF multi-holder release never strands a    *)
(* buffer: once a page is in flight (reaches "sending"), it ALWAYS            *)
(* eventually drops its I-30 pin (the notification-terminal CQE fires). The  *)
(* temporal witness that holding the pin until the LAST holder does not      *)
(* deadlock -- every holder eventually clears and ReleaseClean fires.         *)
(* Checked with ALLOW_TEARDOWN FALSE (no teardown to legitimately abandon a  *)
(* send). Holds under weak fairness of the kernel's forward actions: each    *)
(* HolderRelease (the NIC/netd/peer eventually complete) + ReleaseClean.      *)
(***************************************************************************)
Liveness ==
    /\ \A b \in Bufs : WF_vars(Consume(b))
    /\ \A b \in Bufs : WF_vars(NetdAct(b))
    /\ \A b \in Bufs, h \in Holders : WF_vars(HolderRelease(b, h))
    /\ \A b \in Bufs : WF_vars(ReleaseClean(b))

Spec_Live == Init /\ [][Next]_vars /\ Liveness

EventuallyReleased ==
    \A b \in Bufs : (bphase[b] = "sending") ~> (~pinned[b])

====
