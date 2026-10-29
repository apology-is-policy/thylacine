---- MODULE loom_service ----
EXTENDS Naturals, FiniteSets, TLC
(***************************************************************************)
(* Approved AS lifecycle, docs/ASYNC-SERVICE-LIFECYCLE.md. One reused slot, *)
(* two incarnations, connect + data request per incarnation, CQ capacity1. *)
(* Completion COMMIT is the linearization point even if CQ delivery waits. *)
(* Local borrowers, completion obligations and peer-retained credits are   *)
(* separate. Peer may withhold every byte forever; Abort/Finish need none. *)
(* Model is blind to C pointers, DAC checks, byte parsing and actual locks.*)
(***************************************************************************)
CONSTANT Bug
VARIABLES phase, gen, accepted, borrows, pins, terminal, success, cq, delivered,
          observed, localRefs, peerRefs, charged, retired, cancelled,
          ringClosed, lateSuccess, doubleTerminal, staleUse
vars == <<phase, gen, accepted, borrows, pins, terminal, success, cq, delivered,
          observed, localRefs, peerRefs, charged, retired, cancelled,
          ringClosed, lateSuccess, doubleTerminal, staleUse>>
Gens == {1, 2}
Reqs == 1..4
Owner(r) == (r + 1) \div 2
Current == {r \in accepted : Owner(r) = gen}
Init == /\ phase = "empty" /\ gen = 0
        /\ accepted = {} /\ borrows = {} /\ pins = {} /\ terminal = {}
        /\ success = {} /\ cq = {} /\ delivered = {} /\ observed = {}
        /\ localRefs = {} /\ peerRefs = {} /\ charged = {} /\ retired = {}
        /\ cancelled = {} /\ ringClosed = FALSE /\ lateSuccess = FALSE
        /\ doubleTerminal = FALSE /\ staleUse = FALSE

Connect == /\ phase = "empty" /\ gen < 2 /\ ~ringClosed
           /\ gen' = gen + 1 /\ phase' = "connecting"
           /\ accepted' = accepted \cup {2 * gen + 1}
           /\ borrows' = borrows \cup {2 * gen + 1}
           /\ pins' = pins \cup {2 * gen + 1}
           /\ localRefs' = localRefs \cup {gen + 1}
           /\ peerRefs' = peerRefs \cup {gen + 1}
           /\ charged' = charged \cup {gen + 1}
           /\ UNCHANGED <<terminal, success, cq, delivered, observed, retired,
                  cancelled, ringClosed, lateSuccess, doubleTerminal, staleUse>>

Data == /\ phase = "ready" /\ ~ringClosed /\ 2 * gen \notin accepted
        /\ accepted' = accepted \cup {2 * gen}
        /\ borrows' = borrows \cup {2 * gen}
        /\ pins' = pins \cup {2 * gen}
        /\ UNCHANGED <<phase, gen, terminal, success, cq, delivered, observed,
             localRefs, peerRefs, charged, retired, cancelled, ringClosed,
             lateSuccess, doubleTerminal, staleUse>>

Reply(r) == /\ r \in borrows
            /\ (phase \in {"connecting", "ready"} \/ Bug = "late")
            /\ borrows' = borrows \ {r} /\ pins' = pins \ {r}
            /\ terminal' = terminal \cup {r} /\ success' = success \cup {r}
            /\ phase' = IF phase = "connecting" THEN "ready" ELSE phase
            /\ lateSuccess' = (lateSuccess \/ Owner(r) \in cancelled)
            /\ UNCHANGED <<gen, accepted, cq, delivered, observed, localRefs,
                   peerRefs, charged, retired, cancelled, ringClosed,
                   doubleTerminal, staleUse>>

Abort == /\ phase \in {"connecting", "ready"}
         /\ phase' = "aborting" /\ cancelled' = cancelled \cup {gen}
         /\ UNCHANGED <<gen, accepted, borrows, pins, terminal, success, cq,
               delivered, observed, localRefs, peerRefs, charged, retired,
               ringClosed, lateSuccess, doubleTerminal, staleUse>>
Finish(r) == /\ phase = "aborting" /\ r \in borrows
             /\ (Bug # "peerwait" \/ gen \notin peerRefs)
             /\ borrows' = borrows \ {r} /\ pins' = pins \ {r}
             /\ terminal' = terminal \cup {r}
             /\ UNCHANGED <<phase, gen, accepted, success, cq, delivered,
                    observed, localRefs, peerRefs, charged, retired, cancelled,
                    ringClosed, lateSuccess, doubleTerminal, staleUse>>
Deliver(r) == /\ ~ringClosed /\ r \in terminal \ delivered
              /\ (Cardinality(cq) < 1 \/ Bug = "cqfull")
              /\ cq' = cq \cup {r} /\ delivered' = delivered \cup {r}
              /\ UNCHANGED <<phase, gen, accepted, borrows, pins, terminal,
                  success, observed, localRefs, peerRefs, charged, retired,
                  cancelled, ringClosed, lateSuccess, doubleTerminal, staleUse>>
Drain(r) == /\ r \in cq /\ cq' = cq \ {r}
            /\ observed' = observed \cup {r}
            /\ UNCHANGED <<phase, gen, accepted, borrows, pins, terminal,
                success, delivered, localRefs, peerRefs, charged, retired,
                cancelled, ringClosed, lateSuccess, doubleTerminal, staleUse>>
Close == /\ ~ringClosed /\ ringClosed' = TRUE
         /\ phase' = IF phase \in {"connecting", "ready"} THEN "aborting" ELSE phase
         /\ cancelled' = IF gen \in localRefs THEN cancelled \cup {gen} ELSE cancelled
         /\ UNCHANGED <<gen, accepted, borrows, pins, terminal, success, cq,
               delivered, observed, localRefs, peerRefs, charged, retired,
               lateSuccess, doubleTerminal, staleUse>>
Discard == /\ ringClosed /\ terminal \ delivered # {}
           /\ delivered' = terminal
           /\ UNCHANGED <<phase, gen, accepted, borrows, pins, terminal, success,
               cq, observed, localRefs, peerRefs, charged, retired, cancelled,
               ringClosed, lateSuccess, doubleTerminal, staleUse>>
Retire == /\ phase = "aborting"
          /\ ((Current \cap borrows = {} /\ Current \subseteq delivered) \/ Bug = "earlyfree")
          /\ phase' = "retired" /\ retired' = retired \cup {gen}
          /\ localRefs' = localRefs \ {gen}
          /\ pins' = IF Bug = "earlyfree" THEN pins \ Current ELSE pins
          /\ UNCHANGED <<gen, accepted, borrows, terminal, success, cq,
              delivered, observed, peerRefs, charged, cancelled, ringClosed,
              lateSuccess, doubleTerminal, staleUse>>
Reap == /\ phase = "retired" /\ phase' = "empty"
        /\ UNCHANGED <<gen, accepted, borrows, pins, terminal, success, cq,
             delivered, observed, localRefs, peerRefs, charged, retired,
             cancelled, ringClosed, lateSuccess, doubleTerminal, staleUse>>
PeerClose(g) == /\ g \in peerRefs /\ peerRefs' = peerRefs \ {g}
                /\ UNCHANGED <<phase, gen, accepted, borrows, pins, terminal,
                    success, cq, delivered, observed, localRefs, charged,
                    retired, cancelled, ringClosed, lateSuccess,
                    doubleTerminal, staleUse>>
Refund(g) == /\ g \in charged /\ g \in retired
             /\ (g \notin peerRefs \/ Bug = "refund")
             /\ charged' = charged \ {g}
             /\ UNCHANGED <<phase, gen, accepted, borrows, pins, terminal,
                 success, cq, delivered, observed, localRefs, peerRefs, retired,
                 cancelled, ringClosed, lateSuccess, doubleTerminal, staleUse>>
Duplicate == /\ Bug = "double" /\ terminal # {} /\ ~doubleTerminal
             /\ doubleTerminal' = TRUE
             /\ UNCHANGED <<phase, gen, accepted, borrows, pins, terminal,
                 success, cq, delivered, observed, localRefs, peerRefs, charged,
                 retired, cancelled, ringClosed, lateSuccess, staleUse>>
Stale == /\ Bug = "reuse" /\ gen = 2 /\ 1 \in retired /\ ~staleUse
         /\ staleUse' = TRUE
         /\ UNCHANGED <<phase, gen, accepted, borrows, pins, terminal, success,
             cq, delivered, observed, localRefs, peerRefs, charged, retired,
             cancelled, ringClosed, lateSuccess, doubleTerminal>>

FinishAny == \E r \in Reqs : Finish(r)
DeliverAny == \E r \in Reqs : Deliver(r)
DrainAny == \E r \in Reqs : Drain(r)
Next == Connect \/ Data \/ Abort \/ Close \/ Discard \/ Retire \/ Reap
        \/ FinishAny \/ DeliverAny \/ DrainAny \/ Duplicate \/ Stale
        \/ (\E r \in Reqs : Reply(r))
        \/ (\E g \in Gens : PeerClose(g) \/ Refund(g))
TypeOK == /\ gen \in 0..2 /\ accepted \subseteq Reqs
          /\ borrows \subseteq accepted /\ pins \subseteq accepted
          /\ terminal \subseteq accepted /\ success \subseteq terminal
          /\ delivered \subseteq terminal /\ cq \subseteq delivered
          /\ charged \subseteq Gens /\ retired \subseteq Gens
NoEarlyFree == borrows \subseteq pins
NoLateSuccess == ~lateSuccess
NoDoubleTerminal == ~doubleTerminal
NoStaleSlot == ~staleUse
CqBounded == Cardinality(cq) <= 1
CreditsRetained == localRefs \cup peerRefs \subseteq charged
CompletionConservation == accepted = borrows \cup terminal
NoRetiredBorrow == \A r \in borrows : Owner(r) \notin retired
RetirementProgress == (phase = "aborting") ~> (phase \in {"retired", "empty"})
Spec == Init /\ [][Next]_vars /\ WF_vars(FinishAny) /\ WF_vars(DeliverAny)
        /\ WF_vars(DrainAny) /\ WF_vars(Discard) /\ WF_vars(Retire)
====
