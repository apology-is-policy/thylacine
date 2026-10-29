------------------------------ MODULE mandate ------------------------------
EXTENDS Naturals, FiniteSets, TLC

(***************************************************************************
 UA-0: persistent authority, trusted transaction and live-revocation model.
 USER-AUTHORITY-DESIGN (ratified 4c889a13), I-35 / I-25 / I-27.

 One delegated grant from a founding envelope {net}; one transaction; two
 execution members. A second requested right fs is incomparable. The split
 CopyFork/PublishFork is deliberate: a copied child must not publish behind
 the revoke walk. PersistRestrict is a durable intent, distinct from closing
 admission; crash replay must finish that restriction before reopening.

 No implementation mapped yet. This model abstracts the policy backend,
 kernel locks, principal names, cryptography, byte codecs, storage guarantees,
 resource sessions and physical display as actions with specified commit points.
 It verifies their composition, NOT those lower-level implementations. The
 full hierarchy/selector checks additionally require policy-engine tests.
***************************************************************************)
CONSTANTS BUG_STALE_SUPPORT, BUG_FORK_ADMISSION, BUG_NO_CASCADE,
          BUG_PREVIEW_CHANGE, BUG_NO_RESTORE, BUG_REPLAY, BUG_NO_AUDIT

VARIABLES phase, request, shown, checkedRevision, sourceRevision,
          sourceLive, admission, grant, granted, grantRevision, audit,
          restored, commitWasRestored, alive, forkPending, copiedRevision,
          log, online, crashed, barrier, restrictionDone

vars == <<phase, request, shown, checkedRevision, sourceRevision,
          sourceLive, admission, grant, granted, grantRevision, audit,
          restored, commitWasRestored, alive, forkPending, copiedRevision,
          log, online, crashed, barrier, restrictionDone>>

Caps == {"net", "fs"}
Procs == {"root", "child"}
Init == /\ phase = "Idle" /\ request = {} /\ shown = {}
        /\ checkedRevision = 0 /\ sourceRevision = 1
        /\ sourceLive = TRUE /\ admission = TRUE /\ grant = FALSE /\ granted = {}
        /\ grantRevision = 0 /\ audit = FALSE /\ restored = FALSE /\ commitWasRestored = TRUE
        /\ alive = {} /\ forkPending = FALSE /\ copiedRevision = 0
        /\ log = "None" /\ online = TRUE /\ crashed = FALSE /\ barrier = FALSE
        /\ restrictionDone = FALSE

Prepare == /\ online /\ phase = "Idle" /\ sourceLive
           /\ phase' = "Prepared" /\ request' = {"net"}
           /\ UNCHANGED <<shown, checkedRevision, sourceRevision, sourceLive,
                admission, grant, granted, grantRevision, audit, restored,
                commitWasRestored, alive, forkPending, copiedRevision, log,
                online, crashed, barrier, restrictionDone>>

Show == /\ online /\ phase = "Prepared"
        /\ phase' = "Visible" /\ shown' = request
        /\ checkedRevision' = sourceRevision
        /\ UNCHANGED <<request, sourceRevision, sourceLive, admission, grant,
              granted, grantRevision, audit, restored, commitWasRestored,
              alive, forkPending, copiedRevision, log, online, crashed,
              barrier, restrictionDone>>

Authenticate == /\ online /\ phase = "Visible" /\ shown \subseteq {"net"}
                /\ phase' = "Authenticated"
                /\ UNCHANGED <<request, shown, checkedRevision, sourceRevision,
                   sourceLive, admission, grant, granted, grantRevision, audit,
                   restored, commitWasRestored, alive, forkPending,
                   copiedRevision, log, online, crashed, barrier, restrictionDone>>

\* Models a concurrent group/target policy edit after the trusted preview.
ChangeIntent == /\ online /\ phase \in {"Visible", "Authenticated", "Restored"}
                /\ request = {"net"} /\ request' = {"fs"}
                /\ UNCHANGED <<phase, shown, checkedRevision, sourceRevision,
                   sourceLive, admission, grant, granted, grantRevision, audit,
                   restored, commitWasRestored, alive, forkPending,
                   copiedRevision, log, online, crashed, barrier, restrictionDone>>

Restore == /\ online /\ phase = "Authenticated"
           /\ phase' = "Restored" /\ restored' = TRUE
           /\ UNCHANGED <<request, shown, checkedRevision, sourceRevision,
                sourceLive, admission, grant, granted, grantRevision, audit,
                commitWasRestored, alive, forkPending, copiedRevision, log,
                online, crashed, barrier, restrictionDone>>

Commit == /\ online /\ phase \in {"Authenticated", "Restored"}
          /\ (restored \/ BUG_NO_RESTORE)
          /\ ((sourceLive /\ checkedRevision = sourceRevision /\ admission
               /\ log # "Restrict") \/ BUG_STALE_SUPPORT)
          /\ (request = shown \/ BUG_PREVIEW_CHANGE)
          /\ phase' = "Committed" /\ grant' = TRUE /\ granted' = request
          /\ grantRevision' = checkedRevision /\ audit' = ~BUG_NO_AUDIT
          /\ commitWasRestored' = restored
          /\ log' = IF log = "Restrict" THEN "Restrict" ELSE "Grant"
          /\ UNCHANGED <<request, shown, checkedRevision, sourceRevision,
                sourceLive, admission, restored, alive, forkPending,
                copiedRevision, online, crashed, barrier, restrictionDone>>

Start == /\ online /\ grant /\ admission /\ sourceLive
         /\ grantRevision = sourceRevision /\ alive = {} /\ ~restrictionDone
         /\ alive' = {"root"}
         /\ UNCHANGED <<phase, request, shown, checkedRevision, sourceRevision,
                sourceLive, admission, grant, granted, grantRevision, audit,
                restored, commitWasRestored, forkPending, copiedRevision, log,
                online, crashed, barrier, restrictionDone>>

CopyFork == /\ online /\ "root" \in alive /\ "child" \notin alive
            /\ admission /\ ~forkPending
            /\ forkPending' = TRUE /\ copiedRevision' = sourceRevision
            /\ UNCHANGED <<phase, request, shown, checkedRevision, sourceRevision,
                sourceLive, admission, grant, granted, grantRevision, audit,
                restored, commitWasRestored, alive, log, online, crashed,
                barrier, restrictionDone>>

PublishFork == /\ online /\ forkPending
               /\ (BUG_FORK_ADMISSION \/ (admission /\ sourceLive
                    /\ copiedRevision = sourceRevision /\ "root" \in alive))
               /\ alive' = alive \cup {"child"} /\ forkPending' = FALSE
               /\ UNCHANGED <<phase, request, shown, checkedRevision,
                    sourceRevision, sourceLive, admission, grant, granted,
                    grantRevision, audit, restored, commitWasRestored,
                    copiedRevision, log, online, crashed, barrier, restrictionDone>>

PersistRestrict == /\ online /\ log # "Restrict" /\ ~restrictionDone
                   /\ log' = "Restrict"
                   /\ UNCHANGED <<phase, request, shown, checkedRevision,
                        sourceRevision, sourceLive, admission, grant, granted,
                        grantRevision, audit, restored, commitWasRestored, alive,
                        forkPending, copiedRevision, online, crashed, barrier,
                        restrictionDone>>

CloseAdmission == /\ online /\ log = "Restrict" /\ sourceLive
                  /\ sourceLive' = FALSE /\ sourceRevision' = 2 /\ admission' = FALSE
                  /\ grant' = IF BUG_NO_CASCADE THEN grant ELSE FALSE
                  /\ barrier' = TRUE
                  /\ UNCHANGED <<phase, request, shown, checkedRevision, granted,
                        grantRevision, audit, restored, commitWasRestored, alive,
                        forkPending, copiedRevision, log, online, crashed,
                        restrictionDone>>

Drain(p) == /\ online /\ barrier /\ p \in alive
            /\ alive' = alive \ {p}
            /\ UNCHANGED <<phase, request, shown, checkedRevision, sourceRevision,
                 sourceLive, admission, grant, granted, grantRevision, audit,
                 restored, commitWasRestored, forkPending, copiedRevision, log,
                 online, crashed, barrier, restrictionDone>>

Complete == /\ online /\ barrier /\ alive = {} /\ ~restrictionDone
            /\ restrictionDone' = TRUE /\ barrier' = FALSE
            /\ UNCHANGED <<phase, request, shown, checkedRevision, sourceRevision,
                 sourceLive, admission, grant, granted, grantRevision, audit,
                 restored, commitWasRestored, alive, forkPending, copiedRevision,
                 log, online, crashed>>

Crash == /\ online /\ ~crashed
         /\ online' = FALSE /\ crashed' = TRUE /\ admission' = FALSE /\ grant' = FALSE
         /\ alive' = {} /\ forkPending' = FALSE /\ barrier' = FALSE
         /\ phase' = "Abandoned"
         /\ UNCHANGED <<request, shown, checkedRevision, sourceRevision,
               sourceLive, granted, grantRevision, audit, restored,
               commitWasRestored, copiedRevision, log, restrictionDone>>

Replay == /\ ~online /\ crashed /\ online' = TRUE
          /\ admission' = IF log = "Restrict" THEN BUG_REPLAY ELSE sourceLive
          /\ sourceLive' = IF log = "Restrict" THEN FALSE ELSE sourceLive
          /\ sourceRevision' = IF log = "Restrict" THEN 2 ELSE sourceRevision
          /\ grant' = (log = "Grant" /\ sourceLive /\ grantRevision = sourceRevision)
          /\ restrictionDone' = (log = "Restrict")
          /\ UNCHANGED <<phase, request, shown, checkedRevision, granted,
                grantRevision, audit, restored, commitWasRestored, alive,
                forkPending, copiedRevision, log, crashed, barrier>>

Next == Prepare \/ Show \/ Authenticate \/ ChangeIntent \/ Restore \/ Commit
     \/ Start \/ CopyFork \/ PublishFork \/ PersistRestrict \/ CloseAdmission
     \/ (\E p \in Procs: Drain(p)) \/ Complete \/ Crash \/ Replay

TypeOK == /\ request \subseteq Caps /\ shown \subseteq Caps
          /\ granted \subseteq Caps /\ alive \subseteq Procs
          /\ sourceRevision \in 1..2 /\ grantRevision \in 0..2
          /\ log \in {"None", "Grant", "Restrict"}
          /\ phase \in {"Idle", "Prepared", "Visible", "Authenticated",
                        "Restored", "Committed", "Abandoned"}
          /\ <<grant, admission, sourceLive, audit, restored, commitWasRestored,
                forkPending, online, crashed, barrier, restrictionDone>>
                \in [1..11 -> BOOLEAN]
Supported == grant => (sourceLive /\ grantRevision = sourceRevision)
Bounded == grant => (granted \subseteq {"net"} /\ granted = shown)
RestorationBeforeCommit == commitWasRestored
AtomicAudit == grant => audit
RevocationComplete == restrictionDone => (alive = {} /\ ~admission /\ ~grant)
NoReplayReopen == (~sourceLive) => ~admission
Safety == TypeOK /\ Supported /\ Bounded /\ RestorationBeforeCommit
          /\ AtomicAudit /\ RevocationComplete /\ NoReplayReopen
Spec == Init /\ [][Next]_vars
=============================================================================
