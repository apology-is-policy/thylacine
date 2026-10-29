---- MODULE loom_service_buffers ----
EXTENDS Naturals, Sequences, FiniteSets, TLC
(* One source stream; pool reuse and payload leases outlive source retirement.
   Distinct nonce values stand for distinguishable immutable payload bytes.
   No fairness on peer reply, CQ acknowledgement or payload return. Local
   abort cleanup alone is fair. Bounds are parameters, never state constraints.
   Does not model parser bytes, memory ordering, concurrent streams, DAC or C. *)
CONSTANTS Bug, Members, Shots, Generations
VARIABLE s
vars == <<s>>
M == 1..Members
Ticket(m,n,g) == [member |-> m, nonce |-> n, generation |-> g]
Zero == Ticket(0,0,0)
Items(q) == {q[i] : i \in 1..Len(q)}
Busy == {m \in M : s.state[m] = "BUSY"}
Held == s.held
Pending == {t \in Items(s.pending) : t.nonce # 0}
Init == s = [state |-> [m \in M |-> "AVAILABLE"],
             id |-> [m \in M |-> 0], bytes |-> [m \in M |-> 0],
             nonce |-> 0, generation |-> 1, abort |-> FALSE,
             retired |-> FALSE, final |-> FALSE, peer |-> TRUE,
             pending |-> <<>>, cq |-> <<>>, receipts |-> <<>>,
             held |-> {}, history |-> {}, published |-> <<>>,
             late |-> FALSE, badReturn |-> FALSE]
Claim(m) == /\ ~s.abort /\ ~s.retired /\ s.nonce < Shots
            /\ Busy = {} /\ s.pending = <<>>
            /\ s.state[m] = "AVAILABLE"
            /\ s' = [s EXCEPT !.state[m] = "BUSY", !.id[m] = s.nonce+1,
                                  !.nonce = s.nonce+1]
Reply(m) == /\ m \in Busy /\ (~s.abort \/ Bug = "late")
            /\ s' = [s EXCEPT !.state[m] = "PENDING",
                      !.bytes[m] = s.id[m],
                      !.pending = Append(@,Ticket(m,s.id[m],s.generation)),
                      !.late = @ \/ s.abort]
Abort == /\ ~s.abort /\ s' = [s EXCEPT !.abort = TRUE]
Stop(m) == /\ s.abort /\ m \in Busy
           /\ (Bug # "peerwait" \/ ~s.peer)
           /\ s' = [s EXCEPT !.state[m] = "AVAILABLE"]
PeerClose == /\ s.peer /\ s' = [s EXCEPT !.peer = FALSE]
Finalize == /\ s.abort /\ Busy = {} /\ ~s.final
            /\ s' = [s EXCEPT !.final = TRUE,
                 !.pending = IF Bug = "order"
                     THEN <<Ticket(0,0,s.generation)>> \o @
                     ELSE Append(@,Ticket(0,0,s.generation))]
Retire == /\ s.abort /\ s.final /\ Busy = {} /\ ~s.retired
          /\ (Bug # "returnwait" \/ s.held = {})
          /\ (Bug # "cqwait" \/ s.pending = <<>>)
          /\ s' = [s EXCEPT !.retired = TRUE,
               !.state = IF Bug = "recycle"
                   THEN [m \in M |-> "AVAILABLE"] ELSE @]
Deliver == /\ s.pending # <<>>
           /\ (Len(s.cq) < 1 \/ Bug = "cqfull")
           /\ LET t == Head(s.pending) IN
              s' = [s EXCEPT !.pending = Tail(@), !.cq = Append(@,t),
                   !.receipts = Append(@, IF Bug = "pair" THEN Zero ELSE t),
                   !.state = IF t.nonce # 0
                        THEN [@ EXCEPT ![t.member] = "LEASED"] ELSE @,
                   !.published = Append(@,t)]
Ack == /\ s.cq # <<>>
       /\ LET t == Head(s.cq) IN
          s' = [s EXCEPT !.cq = Tail(@), !.receipts = Tail(@),
                !.held = IF t.nonce # 0 THEN @ \cup {t} ELSE @,
                !.history = IF t.nonce # 0 THEN @ \cup {t} ELSE @,
                !.state = IF Bug = "ackfree" /\ t.nonce # 0
                    THEN [@ EXCEPT ![t.member] = "AVAILABLE"] ELSE @]
Return(t) == /\ t \in s.held
             /\ s' = [s EXCEPT !.held = @ \ {t},
                                   !.state[t.member] = "AVAILABLE"]
StaleReturn(t) == /\ Bug = "stale" /\ t \in s.history \ s.held
                  /\ s.state[t.member] = "LEASED"
                  /\ (s.id[t.member] # t.nonce \/ s.generation # t.generation)
                  /\ s' = [s EXCEPT !.state[t.member] = "AVAILABLE",
                                               !.badReturn = TRUE]
DuplicateFinal == /\ Bug = "double" /\ s.final /\ s.pending = <<>>
                  /\ s' = [s EXCEPT !.pending = <<Ticket(0,0,s.generation)>>]
Reuse == /\ s.retired /\ s.generation < Generations
         /\ s.pending = <<>> /\ s.cq = <<>> /\ s.held = {}
         /\ \A m \in M : s.state[m] = "AVAILABLE"
         /\ s' = [s EXCEPT !.generation = @+1, !.abort = FALSE,
                   !.retired = FALSE, !.final = FALSE, !.peer = TRUE]
StopAny == \E m \in M : Stop(m)
Next == Abort \/ Finalize \/ Retire \/ Deliver \/ Ack \/ Reuse \/ PeerClose
        \/ DuplicateFinal \/ StopAny
        \/ (\E m \in M : Claim(m) \/ Reply(m))
        \/ (\E t \in s.history : Return(t) \/ StaleReturn(t))
TypeOK == /\ s.state \in [M -> {"AVAILABLE","BUSY","PENDING","LEASED"}]
          /\ s.nonce \in 0..Shots /\ s.generation \in 1..Generations
          /\ s.id \in [M -> 0..Shots] /\ s.bytes \in [M -> 0..Shots]
          /\ Cardinality(Busy) <= 1
Payloads == Pending \cup Held \cup {t \in Items(s.cq) : t.nonce # 0}
PayloadIntact == \A t \in Payloads :
     /\ s.state[t.member] \in {"PENDING","LEASED"}
     /\ s.id[t.member] = t.nonce /\ s.bytes[t.member] = t.nonce
     /\ s.generation = t.generation
ExactReturn == ~s.badReturn
PairedPublication == s.cq = s.receipts
CqBounded == Len(s.cq) <= 1
PendingBounded == Len(s.pending) <= Members+1
NoLateSuccess == ~s.late
NoRetiredWriter == s.retired => Busy = {}
NoDuplicate == Cardinality(Items(s.published)) = Len(s.published)
MoreBeforeFinal == \A i,j \in 1..Len(s.published) :
   (s.published[i].nonce = 0 /\ s.published[j].nonce # 0 /\
    s.published[i].generation = s.published[j].generation) => j < i
(* A new generation may be admitted only after the old one retired. Count the
   later generation as a witness even if the observer misses that short state. *)
RetirementProgress == \A g \in 1..Generations :
   (s.abort /\ s.generation = g) ~> (s.retired \/ s.generation > g)
Spec == Init /\ [][Next]_vars /\ WF_vars(StopAny)
        /\ WF_vars(Finalize) /\ WF_vars(Retire)
====
