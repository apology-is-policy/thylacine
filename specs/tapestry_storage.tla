---- MODULE tapestry_storage ----
EXTENDS Naturals, FiniteSets, TLC
(***************************************************************************)
(* Cooperative hidden-pixel storage, TAPESTRY-STORAGE.md, October 4.        *)
(* A companion to tapestry_present, not a replacement for its slot/DMA      *)
(* model. Device refs abstract the existing transfer/import retirement      *)
(* proof; client maps remain independent refs. Two fresh generations and    *)
(* three visibility epochs include hide/show/hide and stale queued offers.  *)
(* Exhaustion disables transitions; symbols are never recycled.             *)
(* Client quiescence before Suspend is structural here: no concurrent       *)
(* writes action. The existing present model proves its drain obligation.   *)
(***************************************************************************)
CONSTANT Bug
VARIABLES visible, phase, gen, server, client, device, display, painted,
          freed, epoch, offer, pending, fid, badOffer, badFid, badResurrection
vars == <<visible, phase, gen, server, client, device, display, painted,
          freed, epoch, offer, pending, fid, badOffer, badFid, badResurrection>>
Gens == {1, 2}
None == <<0, "none">>
Resident == phase \in {"resident", "repainting"}
Init == /\ visible = TRUE /\ phase = "resident" /\ gen = 1
        /\ server = {1} /\ client = {1} /\ device = {1} /\ display = 1
        /\ painted = {1} /\ freed = {} /\ epoch = 1
        /\ offer = None /\ pending = None /\ fid = 1
        /\ badOffer = FALSE /\ badFid = FALSE /\ badResurrection = FALSE

Visibility == /\ phase # "dead" /\ epoch < 3
              /\ visible' = ~visible /\ epoch' = epoch + 1
              /\ offer' = None
              /\ UNCHANGED <<phase, gen, server, client, device, display,
                   painted, freed, pending, fid, badOffer, badFid, badResurrection>>
Offer == /\ phase # "dead" /\ offer = None
         /\ ((~visible /\ Resident) \/ (visible /\ phase = "dormant"))
         /\ offer' = <<epoch, IF visible THEN "resume" ELSE "suspend">>
         /\ UNCHANGED <<visible, phase, gen, server, client, device, display,
              painted, freed, epoch, pending, fid, badOffer, badFid, badResurrection>>
ReadOffer == /\ offer # None /\ pending = None /\ pending' = offer
             /\ UNCHANGED <<visible, phase, gen, server, client, device, display,
                  painted, freed, epoch, offer, fid, badOffer, badFid, badResurrection>>
DropOffer == /\ pending # None /\ pending' = None
             /\ UNCHANGED <<visible, phase, gen, server, client, device, display,
                  painted, freed, epoch, offer, fid, badOffer, badFid, badResurrection>>
Unbind == /\ display # 0 /\ (~visible \/ phase = "dead") /\ display' = 0
          /\ UNCHANGED <<visible, phase, gen, server, client, device, painted,
               freed, epoch, offer, pending, fid, badOffer, badFid, badResurrection>>
Suspend == /\ Resident /\ pending[2] = "suspend"
           /\ (Bug = "offer" \/ (~visible /\ pending = offer /\ pending[1] = epoch))
           /\ (Bug = "scanout" \/ display = 0)
           /\ phase' = "dormant" /\ server' = server \ {gen}
           /\ offer' = None /\ pending' = None
           /\ badOffer' = (badOffer \/ visible \/ pending # offer \/ pending[1] # epoch)
           /\ UNCHANGED <<visible, gen, client, device, display, painted, freed,
                epoch, fid, badFid, badResurrection>>
Resume == /\ (phase = "dormant" \/ (Bug = "dead" /\ phase = "dead"))
          /\ visible /\ gen < 2 /\ pending[2] = "resume"
          /\ (Bug = "dead" \/ (pending = offer /\ pending[1] = epoch))
          /\ gen' = gen + 1 /\ server' = server \cup {gen + 1}
          /\ phase' = "repainting" /\ offer' = None /\ pending' = None
          /\ badResurrection' = (badResurrection \/ phase = "dead")
          /\ UNCHANGED <<visible, client, device, display, painted, freed,
               epoch, fid, badOffer, badFid>>
(* Refusal does not allocate/destroy. Abort only an unpresented fresh weave. *)
Abort == /\ phase = "repainting" /\ gen \notin painted
         /\ phase' = "dormant" /\ server' = server \ {gen}
         /\ UNCHANGED <<visible, gen, client, device, display, painted, freed,
              epoch, offer, pending, fid, badOffer, badFid, badResurrection>>
OpenFresh == /\ Resident /\ fid' = gen
             /\ UNCHANGED <<visible, phase, gen, server, client, device, display,
                  painted, freed, epoch, offer, pending, badOffer, badFid, badResurrection>>
Map == /\ Resident /\ (Bug = "fid" \/ fid = gen)
       /\ client' = client \cup {gen}
       /\ badFid' = (badFid \/ fid # gen)
       /\ UNCHANGED <<visible, phase, gen, server, device, display, painted,
            freed, epoch, offer, pending, fid, badOffer, badResurrection>>
Clunk(g) == /\ g \in client /\ client' = client \ {g}
            /\ UNCHANGED <<visible, phase, gen, server, device, display, painted,
                 freed, epoch, offer, pending, fid, badOffer, badFid, badResurrection>>
Paint == /\ Resident /\ gen \in client /\ fid = gen
         /\ painted' = painted \cup {gen}
         /\ UNCHANGED <<visible, phase, gen, server, client, device, display,
              freed, epoch, offer, pending, fid, badOffer, badFid, badResurrection>>
Present == /\ Resident /\ visible /\ gen \in client /\ fid = gen
           /\ (Bug = "partial" \/ gen \in painted)
           /\ display' = gen /\ device' = device \cup {gen} /\ phase' = "resident"
           /\ UNCHANGED <<visible, gen, server, client, painted, freed,
                epoch, offer, pending, fid, badOffer, badFid, badResurrection>>
Complete(g) == /\ g \in device /\ display # g /\ device' = device \ {g}
               /\ UNCHANGED <<visible, phase, gen, server, client, display, painted,
                    freed, epoch, offer, pending, fid, badOffer, badFid, badResurrection>>
Free(g) == /\ g <= gen /\ g \notin server /\ g \notin freed
           /\ (Bug = "mapping" \/ g \notin client)
           /\ (Bug = "device" \/ g \notin device)
           /\ (Bug = "scanout" \/ display # g)
           /\ freed' = freed \cup {g}
           /\ UNCHANGED <<visible, phase, gen, server, client, device, display,
                painted, epoch, offer, pending, fid, badOffer, badFid, badResurrection>>
Destroy == /\ phase # "dead" /\ phase' = "dead" /\ server' = {}
           /\ offer' = None
           /\ UNCHANGED <<visible, gen, client, device, display, painted, freed,
                epoch, pending, fid, badOffer, badFid, badResurrection>>
Next == Visibility \/ Offer \/ ReadOffer \/ DropOffer \/ Unbind \/ Suspend \/ Resume
        \/ Abort \/ OpenFresh \/ Map \/ Paint \/ Present \/ Destroy
        \/ (\E g \in Gens : Clunk(g) \/ Complete(g) \/ Free(g))
Spec == Init /\ [][Next]_vars
TypeOK == /\ phase \in {"resident", "dormant", "repainting", "dead"}
          /\ gen \in Gens /\ epoch \in 1..3 /\ display \in 0..2
          /\ server \subseteq Gens /\ client \subseteq Gens /\ device \subseteq Gens
          /\ freed \subseteq Gens /\ painted \subseteq Gens
SuspendedUnbound == phase # "dormant" \/ display # gen
OfferFresh == ~badOffer
FidFresh == ~badFid
NoResurrection == ~badResurrection
ClientBacked == client \cap freed = {}
DeviceBacked == device \cap freed = {}
DisplayBacked == display = 0 \/ (display \notin freed /\ display \in server \cup device \cup client)
FirstFrameComplete == display = 0 \/ display \in painted
ResidentBacked == ~Resident \/ (gen \in server /\ gen \notin freed)
=============================================================================
