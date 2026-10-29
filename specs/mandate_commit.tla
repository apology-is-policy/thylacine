-------------------------- MODULE mandate_commit --------------------------
EXTENDS Naturals, FiniteSets, TLC

(***************************************************************************
 UA-0 trusted transaction detail, supplementing mandate.tla's execution-member
 teardown model. Admit and Publish are separate. Requester death/expiry after
 admission may not repurpose the immutable transaction, but may not prevent its
 already-authorized durable completion either. Durable source/group restriction
 still wins at materialization. A group-dependent preview must not approve an
 unseen authority increase. All values are bounded: one transaction, two rights,
 one group revision change and one crash. No liveness/crypto/storage proof.
***************************************************************************)
CONSTANTS BUG_NO_RECHECK, BUG_GROUP_DELTA, BUG_NO_RESTORE,
          BUG_FORK_ADMIN, BUG_REPURPOSE, BUG_NO_AUDIT, BUG_REPLAY
VARIABLE s
vars == <<s>>
Init == s = [phase |-> "Idle", request |-> {"net"}, group |-> {"net"},
             groupRevision |-> 1, shown |-> {}, shownGroup |-> 0,
             shownGeneration |-> 0, generation |-> 1, source |-> TRUE,
             account |-> "Active", admission |-> TRUE, issuer |-> TRUE,
             client |-> TRUE, scope |-> FALSE, restored |-> FALSE,
             admitted |-> {}, admittedGroup |-> 0, admittedGeneration |-> 0,
             admittedActor |-> "none", wasRestored |-> TRUE,
             record |-> FALSE, effective |-> FALSE, rights |-> {},
             audit |-> FALSE, restrictLog |-> "None", online |-> TRUE,
             crashed |-> FALSE, replayClosed |-> FALSE]
Prepare == /\ s.online /\ s.phase = "Idle" /\ s.client
           /\ s.source /\ s.issuer /\ s.admission
           /\ s' = [s EXCEPT !.phase = "Prepared"]
Show == /\ s.online /\ s.phase = "Prepared"
        /\ s' = [s EXCEPT !.phase = "Visible",
                  !.shown = s.request \cup s.group,
                  !.shownGroup = s.groupRevision,
                  !.shownGeneration = s.generation]
Authenticate == /\ s.online /\ s.phase = "Visible" /\ s.client /\ s.issuer
                /\ s.shown \subseteq {"net"}
                /\ s' = [s EXCEPT !.phase = "Authenticated", !.scope = TRUE]
Restore == /\ s.online /\ s.phase = "Authenticated"
           /\ s' = [s EXCEPT !.phase = "Restored", !.restored = TRUE]
ChangeIntent == /\ s.online /\ s.request = {"net"}
                /\ s.phase \in {"Visible", "Authenticated", "Restored", "Admitted"}
                /\ s' = [s EXCEPT !.request = {"fs"}]
ChangeGroup == /\ s.online /\ s.groupRevision = 1
               /\ s' = [s EXCEPT !.group = {"net", "fs"}, !.groupRevision = 2,
                         !.effective = FALSE]
Expire == /\ s.online /\ s.scope
          /\ s' = [s EXCEPT !.scope = FALSE]
ClientExit == /\ s.online /\ s.client
              /\ s' = [s EXCEPT !.client = FALSE, !.scope = FALSE]
IssuerExit == /\ s.online /\ s.issuer
              /\ s' = [s EXCEPT !.issuer = FALSE, !.scope = FALSE,
                        !.admission = FALSE, !.effective = FALSE]

Admit(actor) ==
    /\ s.online /\ s.phase \in {"Authenticated", "Restored"}
    /\ s.client /\ s.issuer /\ s.scope
    /\ (actor = "root" \/ BUG_FORK_ADMIN)
    /\ (s.restored \/ BUG_NO_RESTORE)
    /\ (s.source /\ s.admission /\ s.shownGeneration = s.generation
        /\ s.restrictLog = "None")
    /\ s.request \subseteq s.shown
    /\ (s.groupRevision = s.shownGroup \/ BUG_GROUP_DELTA)
    /\ s' = [s EXCEPT !.phase = "Admitted",
              !.admitted = s.request \cup s.group,
              !.admittedGroup = s.groupRevision,
              !.admittedGeneration = s.generation,
              !.admittedActor = actor, !.wasRestored = s.restored]

Publish == /\ s.online /\ s.phase = "Admitted"
           /\ s' = [s EXCEPT !.phase = "Committed", !.record = TRUE,
                     !.effective = (BUG_NO_RECHECK \/
                       (s.source /\ s.issuer /\ s.admission
                        /\ s.restrictLog = "None"
                        /\ s.admittedGeneration = s.generation
                        /\ s.admittedGroup = s.groupRevision)),
                     !.rights = IF BUG_REPURPOSE THEN s.request \cup s.group
                                ELSE s.admitted,
                     !.audit = ~BUG_NO_AUDIT]

PersistRestrict(kind) ==
    /\ s.online /\ s.restrictLog = "None" /\ s.admission
    /\ s' = [s EXCEPT !.restrictLog = kind]
CloseAdmission ==
    /\ s.online /\ s.restrictLog # "None" /\ s.admission
    /\ s' = [s EXCEPT !.admission = FALSE, !.generation = 2,
              !.source = IF s.restrictLog = "Revoke" THEN FALSE ELSE s.source,
              !.account = IF s.restrictLog = "Suspend" THEN "Suspended" ELSE s.account,
              !.effective = FALSE, !.scope = FALSE]
\* Resume is a separate authorized policy operation. It does not revive the
\* old grant or scope, and cannot restore a revoked source. Session teardown
\* acknowledgements are abstracted here and detailed in mandate.tla.
Resume == /\ s.online /\ s.account = "Suspended" /\ s.issuer /\ s.source
          /\ ~s.admission
          /\ s' = [s EXCEPT !.account = "Active", !.admission = TRUE,
                    !.restrictLog = "None", !.replayClosed = FALSE]
Crash == /\ s.online /\ ~s.crashed
         /\ s' = [s EXCEPT !.online = FALSE, !.crashed = TRUE,
                   !.admission = FALSE, !.effective = FALSE, !.scope = FALSE,
                   !.phase = "Abandoned"]
Replay == /\ ~s.online /\ s.crashed
          /\ s' = [s EXCEPT !.online = TRUE, !.replayClosed = (s.restrictLog # "None"),
                    !.generation = IF s.restrictLog # "None" THEN 2 ELSE s.generation,
                    !.source = IF s.restrictLog = "Revoke" /\ ~BUG_REPLAY
                               THEN FALSE ELSE s.source,
                    !.account = IF s.restrictLog = "Suspend" /\ ~BUG_REPLAY
                                THEN "Suspended" ELSE s.account,
                    !.admission = s.issuer /\ s.source /\
                        (s.restrictLog = "None" \/ BUG_REPLAY),
                    !.effective = s.record /\ s.audit /\ s.issuer /\ s.source
                        /\ s.admittedGeneration = s.generation
                        /\ s.admittedGroup = s.groupRevision
                        /\ (s.restrictLog = "None" \/ BUG_REPLAY)]

Next == Prepare \/ Show \/ Authenticate \/ Restore \/ ChangeIntent \/ ChangeGroup
     \/ Expire \/ ClientExit \/ IssuerExit
     \/ (\E a \in {"root", "child"}: Admit(a)) \/ Publish
     \/ (\E kind \in {"Revoke", "Suspend"}: PersistRestrict(kind))
     \/ CloseAdmission \/ Resume \/ Crash \/ Replay

Bounded == s.record => (s.rights \subseteq {"net"} /\ s.rights = s.shown)
Supported == s.effective => (s.source /\ s.issuer /\ s.admission
              /\ s.admittedGeneration = s.generation
              /\ s.admittedGroup = s.groupRevision)
TrustedActor == s.record => s.admittedActor = "root"
RestorationBeforeAdmission == s.wasRestored
AtomicAudit == s.record => s.audit
NoReplayReopen == s.replayClosed => ~s.admission
FrozenIntent == s.record => s.rights = s.admitted
Safety == Bounded /\ Supported /\ TrustedActor /\ RestorationBeforeAdmission
          /\ AtomicAudit /\ NoReplayReopen /\ FrozenIntent
Spec == Init /\ [][Next]_vars
=============================================================================
