---- MODULE loom_service_close ----
EXTENDS Naturals, TLC
CONSTANT Bug
VARIABLE s
vars == <<s>>
Init == s = [phase |-> "new", roots |-> 0, linked |-> FALSE,
             blocked |-> TRUE, work |-> 2, dirty |-> FALSE,
             observed |-> FALSE, more |-> FALSE, done |-> FALSE,
             internalWake |-> FALSE, restarted |-> FALSE]
Start == /\ s.phase="new" \/ (Bug="restart" /\ s.phase="done")
         /\ s'=[s EXCEPT !.phase="ready", !.roots=@+1, !.linked=TRUE,
                 !.restarted=@ \/ s.phase="done"]
Duplicate == /\ Bug="duplicate" /\ s.phase="ready" /\ s.roots=1
             /\ s'=[s EXCEPT !.roots=@+1]
Take == /\ s.phase="ready"
        /\ s'=[s EXCEPT !.phase="running", !.linked=FALSE,
                        !.dirty=FALSE, !.observed=FALSE,
                        !.roots=IF Bug="early" THEN 0 ELSE @]
Observe == /\ s.phase="running" /\ ~s.observed
           /\ LET remaining == IF s.blocked THEN s.work ELSE IF s.work>0 THEN s.work-1 ELSE 0
              IN s'=[s EXCEPT !.observed=TRUE, !.work=remaining,
                     !.more=~s.blocked /\ remaining>0,
                     !.done=~s.blocked /\ remaining=0,
                     !.dirty=IF Bug="selfwake" THEN TRUE ELSE @,
                     !.internalWake=Bug="selfwake"]
Release == /\ s.blocked /\ s.phase \in {"ready","running","parked"}
           /\ s'=[s EXCEPT !.blocked=FALSE,
                  !.phase=IF s.phase="parked" /\ Bug#"park" THEN "ready" ELSE @,
                  !.linked=IF s.phase="parked" /\ Bug#"park" THEN TRUE ELSE @,
                  !.dirty=IF s.phase="running" /\ Bug#"lost" THEN TRUE ELSE @]
Finish == /\ s.phase="running" /\ s.observed
          /\ LET again == s.more \/ s.dirty
             IN s'=[s EXCEPT !.phase=IF s.done THEN "done" ELSE IF again THEN "ready" ELSE "parked",
                    !.linked=~s.done /\ again,
                    !.roots=IF s.done /\ Bug#"leak" THEN 0 ELSE @]
Next == Start \/ Duplicate \/ Take \/ Observe \/ Release \/ Finish
TypeOK == /\ s.phase \in {"new","ready","running","parked","done"}
          /\ s.roots \in 0..2 /\ s.work \in 0..2
QueueRoot == s.roots = IF s.phase \in {"ready","running","parked"} THEN 1 ELSE 0
ExactLink == s.linked = (s.phase="ready")
NoLostWake == s.phase="parked" => s.blocked
NoInternalWake == ~s.internalWake
NoRestart == ~s.restarted
Completed == s.phase="done" => (s.work=0 /\ ~s.blocked)
Spec == Init /\ [][Next]_vars
====
