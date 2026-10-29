---- MODULE loom_service_index ----
EXTENDS Naturals, TLC
CONSTANT Bug
VARIABLE s
vars == <<s>>
Init == s = [phase |-> "empty", gen |-> 0, pins |-> 0, allocated |-> FALSE,
             aborted |-> FALSE, published |-> FALSE, terminal |-> FALSE,
             delivered |-> FALSE, local |-> FALSE, installer |-> FALSE,
             exposed |-> FALSE, late |-> FALSE, stale |-> FALSE]
Prepare == /\ s.phase = "empty" /\ s.gen < 2
           /\ s' = [s EXCEPT !.phase="prepared", !.gen=@+1, !.allocated=TRUE,
                    !.aborted=FALSE, !.published=FALSE, !.terminal=FALSE,
                    !.delivered=FALSE, !.local=FALSE]
Install == /\ s.phase="prepared"
           /\ s'=[s EXCEPT !.phase="installing", !.installer=TRUE,
                          !.pins=IF Bug="installer" THEN 0 ELSE 1]
Abort == /\ s.phase \in {"installing","live"} /\ ~s.aborted
         /\ s'=[s EXCEPT !.aborted=TRUE]
Publish == /\ s.phase="installing" /\ ~s.published
           /\ (~s.aborted \/ Bug="late")
           /\ s'=[s EXCEPT !.published=TRUE, !.late=@ \/ s.aborted]
WakeDone == /\ s.phase="installing" /\ (s.published \/ s.aborted)
            /\ s'=[s EXCEPT !.phase="live", !.installer=FALSE,
                           !.pins=IF s.pins>0 THEN @-1 ELSE 0]
Get == /\ s.pins < 2
       /\ (s.phase="live" \/ (Bug="expose" /\ s.phase="installing"))
       /\ s'=[s EXCEPT !.pins=@+1, !.exposed=@ \/ s.phase="installing"]
Put == /\ s.pins > (IF s.installer THEN 1 ELSE 0)
       /\ s'=[s EXCEPT !.pins=@-1]
Success == /\ s.phase="live" /\ s.published /\ ~s.aborted /\ ~s.terminal
           /\ s'=[s EXCEPT !.terminal=TRUE]
LocalAbort == /\ s.phase="live" /\ s.aborted /\ ~s.local
              /\ s'=[s EXCEPT !.local=TRUE, !.terminal=TRUE]
Deliver == /\ s.terminal /\ ~s.delivered
           /\ s'=[s EXCEPT !.delivered=TRUE]
Collect == /\ s.phase="live" /\ s.local
           /\ (s.pins=0 \/ Bug="pin")
           /\ (s.delivered \/ Bug="terminal")
           /\ s'=[s EXCEPT !.phase="removed"]
Free == /\ s.phase="removed" /\ s.allocated
        /\ s'=[s EXCEPT !.allocated=FALSE]
Reap == /\ s.phase="removed" /\ ~s.allocated /\ s.pins=0
        /\ s'=[s EXCEPT !.phase="empty"]
StaleGet == /\ Bug="stale" /\ s.gen=2 /\ s.phase="live"
            /\ s'=[s EXCEPT !.stale=TRUE]
Next == Prepare \/ Install \/ Abort \/ Publish \/ WakeDone \/ Get \/ Put
        \/ Success \/ LocalAbort \/ Deliver \/ Collect \/ Free \/ Reap \/ StaleGet
TypeOK == /\ s.gen \in 0..2 /\ s.pins \in 0..2
          /\ s.phase \in {"empty","prepared","installing","live","removed"}
InstallerPinned == s.installer => (s.pins>0 /\ s.allocated)
NoHalfPublished == ~s.exposed
NoLatePublication == ~s.late
EntryRetainsStorage == s.pins>0 => (s.allocated /\ s.phase # "removed")
TerminalRetained == s.phase="removed" => (s.local /\ s.terminal /\ s.delivered)
NoStaleEntry == ~s.stale
Spec == Init /\ [][Next]_vars
====
