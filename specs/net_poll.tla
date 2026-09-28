---- MODULE net_poll ----
(***************************************************************************)
(* Thylacine `dev9p.poll` -- poll readiness over a 9P fd: the SAMPLE/ARM   *)
(* split (NET-DESIGN.md section 12.2 and its #98 amendment;                *)
(* dec-2026-09-28-poll-sample-arm-split).                                  *)
(*                                                                         *)
(* A `poll()` over a `/net` socket or a pty reaches the kernel's dev9p.    *)
(* The readiness lives in the server that holds the file (netd, ptyfs),    *)
(* and the kernel learns it only by asking, with a readiness READ: a 9P    *)
(* Tread on the file's `ready` fid, its offset carrying the requested      *)
(* event mask. There are two such reads, one for each job:                 *)
(*                                                                         *)
(*   the SNAPSHOT (offset = mask | P9_POLL_SNAPSHOT) -- the server answers *)
(*     AT ONCE with the current readiness, 0 included, and never defers    *)
(*     it. It is the only SAMPLE a verdict rests on.                       *)
(*                                                                         *)
(*   the ARM (offset = mask) -- the server holds it until the file is      *)
(*     ready, evaluating the level when the read ARRIVES and again on      *)
(*     every change, then answers. A poller sends it only when it is about *)
(*     to PARK, after its hook is on the list, and the answer is only a    *)
(*     WAKE: the woken poller samples again.                               *)
(*                                                                         *)
(* Every pass of a poll call SCANS (sends the snapshot), SETTLES (waits    *)
(* for the answer) and only then gives its VERDICT. Ready returns. Not     *)
(* ready returns 0 if the call's deadline has passed; otherwise the poller *)
(* hooks itself on the file's list, makes sure an arm is outstanding, and  *)
(* parks.                                                                  *)
(* The arm's answer is demuxed by the kernel 9P client's elected reader -- *)
(* the per-client poll-pump kthread, because a parked poller pumps nothing *)
(* -- whose `on_complete` runs under `c->lock` and so only sets a relay    *)
(* flag; the kthread walks the hook list afterwards, in process context    *)
(* (the LS-8a deferred wake, cons_poll.tla). A call with timeout 0 is one  *)
(* pass with no hook and no arm.                                           *)
(*                                                                         *)
(* WHAT THIS SPEC PINS                                                     *)
(*                                                                         *)
(*   poll.tla owns the N-fd loop and where the settle sits in it;          *)
(*   cons_poll.tla owns the kthread's own sleep. This module owns the      *)
(*   protocol with the server, and three properties follow from it.        *)
(*                                                                         *)
(*   A VERDICT IS THE SERVER'S OWN ANSWER, given inside the pass that      *)
(*   reports it. A socket reported not ready was not ready at some instant *)
(*   of that pass (NoFalseNotReady), and one reported ready was ready at   *)
(*   some instant of it (NoFalseReady). The one exception is the fail-safe *)
(*   below, and it is counted.                                             *)
(*                                                                         *)
(*   A PARKED POLLER ALWAYS HAS A WAKE COMING. It parks hooked, with an    *)
(*   arm outstanding, the arm's answer in the relay, or its flag already   *)
(*   set (ArmBeforePark), so it is never asleep on a ready socket with     *)
(*   nothing left to deliver (NoMissedNetPoll -- I-9 across the relay). A  *)
(*   poller whose socket becomes ready and stays ready returns             *)
(*   (PollerEventuallyServed).                                             *)
(*                                                                         *)
(*   THE SETTLE IS BOUNDED BY THE SERVER, NOT BY THE CALL. A snapshot the  *)
(*   server has not answered a fixed 1 s after it was sent is flushed and  *)
(*   reported not ready, and the fail-safe counter records it. The call's  *)
(*   own timeout never cuts that interval short, so a server that is       *)
(*   answering is never guessed about: a poll may return one round trip    *)
(*   late, never early on a guess.                                         *)
(*                                                                         *)
(* THE BUGS THIS PINS                                                      *)
(*                                                                         *)
(*   BUGGY_CACHE_ONLY_SAMPLE -- the design before #98. The only readiness  *)
(*     read was the deferred one, so the kernel heard "ready" and never    *)
(*     "not ready", and a pass that had to decide read a cache of whatever *)
(*     the relay had delivered so far. A zero-timeout poll of a socket     *)
(*     that was ready before the call returned 0 off an empty cache        *)
(*     (NoFalseNotReady); the 10 ms budget that widened a literal 0 only   *)
(*     moved the deadline the relay raced. The same cache reported a level *)
(*     a competing reader had already lowered (NoFalseReady, in            *)
(*     net_poll_buggy_stale_cache.cfg). The fix is the snapshot.           *)
(*                                                                         *)
(*   BUGGY_SETTLE_CUT_BY_DEADLINE -- the settle gives up when the call's   *)
(*     deadline passes. For timeout 0 the deadline has passed before the   *)
(*     snapshot is sent, so a zero-timeout poll of a ready socket on a     *)
(*     healthy server returns 0: #98 again, one layer down                 *)
(*     (NoFalseNotReady). The fix is the fixed interval.                   *)
(*                                                                         *)
(*   BUGGY_GC_SNAPSHOT -- the kthread's collector of stranded ops takes a  *)
(*     snapshot for one. A snapshot has no hook by design (it is a sample, *)
(*     not a wait), so the collector flushes it and the pass reports a     *)
(*     ready socket not ready (NoFalseNotReady). The fix: the collector    *)
(*     takes arms only.                                                    *)
(*                                                                         *)
(*   BUGGY_LOST_READY -- the poller parks with no arm outstanding (none    *)
(*     was ensured, or the collector took the last one). The readiness     *)
(*     edge fires in the server with no request to answer, and the poller  *)
(*     sleeps on a ready socket (NoMissedNetPoll). The fix: hook, then     *)
(*     ensure an arm, then park.                                           *)
(*                                                                         *)
(*   BUGGY_EDGE_ARM -- the server answers an arm only for a rise AFTER the *)
(*     arm arrived. A socket that became ready between the snapshot and    *)
(*     the arm, and stays ready, never answers it, and a poll(-1) sleeps   *)
(*     forever (PollerEventuallyServed). No safety invariant sees this --  *)
(*     an arm IS outstanding -- which is why the liveness property is      *)
(*     checked. The fix is the server's: evaluate the level on arrival.    *)
(*                                                                         *)
(* CFG MATRIX (specs/check-net-poll.sh runs it and judges each red cfg by  *)
(* the NAME of the property it violates)                                   *)
(*                                                                         *)
(*   net_poll.cfg             timed; Invariants + FailSafeSilent.          *)
(*   net_poll_notimeout.cfg   poll(-1); the same.                          *)
(*   net_poll_liveness.cfg    Spec_Live, poll(-1): PollerEventuallyServed. *)
(*   net_poll_liveness_timeout.cfg                                         *)
(*                            Spec_Live, timed: PollTerminates +           *)
(*                            PollerEventuallyServed.                      *)
(*   net_poll_hung.cfg        HUNG_SERVER, Spec_Live, timed: Invariants +  *)
(*                            PollTerminates (the fail-safe ends it).      *)
(*   net_poll_failsafe_fires.cfg                                           *)
(*                            HUNG_SERVER: FailSafeSilent VIOLATED -- the  *)
(*                            fail-safe is reachable (the sabotage server  *)
(*                            the runtime test also uses).                 *)
(*   net_poll_buggy_cache_only_sample.cfg          NoFalseNotReady.        *)
(*   net_poll_buggy_stale_cache.cfg                NoFalseReady.           *)
(*   net_poll_buggy_settle_cut_by_deadline.cfg     NoFalseNotReady.        *)
(*   net_poll_buggy_gc_snapshot.cfg                NoFalseNotReady.        *)
(*   net_poll_buggy_lost_ready.cfg                 NoMissedNetPoll.        *)
(*   net_poll_buggy_edge_arm.cfg   Spec_Live, poll(-1):                    *)
(*                                 PollerEventuallyServed.                 *)
(*                                                                         *)
(* MODELING ASSUMPTIONS                                                    *)
(*                                                                         *)
(*   One poller, one socket. The N-fd fan, the local fds beside a socket,  *)
(*   death and stops are poll.tla's, and so is the snapshot's lifetime     *)
(*   across the settle's death unwind. This module's subject is the        *)
(*   protocol, and one poller exercises all of it.                         *)
(*                                                                         *)
(*   `ready` is a LEVEL for the requested mask: it rises and it falls (a   *)
(*   competing reader drains the bytes). It changes only while the call is *)
(*   live; once the call returns, nothing here observes it.                *)
(*                                                                         *)
(*   A snapshot is answered from the server's state at the instant it is   *)
(*   served, so its evaluation is one atomic step (SnapshotReply) inside   *)
(*   the pass. The ghosts `pass_ready` and `pass_notready` record whether  *)
(*   `ready` held, or failed, at some instant between the scan and the     *)
(*   verdict; a verdict is sound when it agrees with one of them.          *)
(*                                                                         *)
(*   The arm is level-evaluated: ArmReply is enabled while an arm is       *)
(*   outstanding and the socket is ready. Its answer and the reader's      *)
(*   demux are one step, as before the split; the kthread's walk is        *)
(*   another (KthreadWalk).                                                *)
(*                                                                         *)
(*   HUNG_SERVER lets the server stop answering at any step (Hang). The    *)
(*   fail-safe (SnapshotFailSafe) is enabled only then, because a healthy  *)
(*   server answers a snapshot within 1 s. That is a TIMING assumption no  *)
(*   model of this shape can check, and the runtime owns it: the counter   *)
(*   is printed, the boot gates require it to stay zero, and a test server *)
(*   that defers the snapshot must make it fire.                           *)
(*                                                                         *)
(*   The collector's teardown of a stranded arm, and the Tclunk that frees *)
(*   the server's slot, are net_poll_teardown.tla's.                       *)
(*                                                                         *)
(*   HAS_TIMEOUT = FALSE is poll(-1). With HAS_TIMEOUT the deadline may    *)
(*   pass at any step, the first scan included: that behaviour is timeout  *)
(*   0.                                                                    *)
(*                                                                         *)
(* See NET-DESIGN.md section 12.2; ARCHITECTURE.md section 23.3 and        *)
(* section 28 invariant I-9; poll.tla; cons_poll.tla;                      *)
(* net_poll_teardown.tla; kernel/dev9p_poll.c, kernel/poll.c,              *)
(* kernel/9p_client.c; the `ready` file in usr/netd/src/server.rs and      *)
(* usr/ptyfs/src/server.rs.                                                *)
(***************************************************************************)

CONSTANTS
    HAS_TIMEOUT,                  \* BOOLEAN -- TRUE: the call carries a finite
                                  \*   timeout (0 included). FALSE: poll(-1).
    HUNG_SERVER,                  \* BOOLEAN -- TRUE: the server may stop
                                  \*   answering, so the fail-safe is reachable.
    BUGGY_CACHE_ONLY_SAMPLE,      \* BOOLEAN -- TRUE: the sample reads what the
                                  \*   last answered arm recorded.
    BUGGY_SETTLE_CUT_BY_DEADLINE, \* BOOLEAN -- TRUE: the settle gives up when
                                  \*   the call's deadline passes.
    BUGGY_GC_SNAPSHOT,            \* BOOLEAN -- TRUE: the stranded-op collector
                                  \*   takes a snapshot too.
    BUGGY_LOST_READY,             \* BOOLEAN -- TRUE: the poller parks without
                                  \*   ensuring an arm is outstanding.
    BUGGY_EDGE_ARM                \* BOOLEAN -- TRUE: the server answers an arm
                                  \*   only for a rise after it arrived.

ASSUME HAS_TIMEOUT                  \in BOOLEAN
ASSUME HUNG_SERVER                  \in BOOLEAN
ASSUME BUGGY_CACHE_ONLY_SAMPLE      \in BOOLEAN
ASSUME BUGGY_SETTLE_CUT_BY_DEADLINE \in BOOLEAN
ASSUME BUGGY_GC_SNAPSHOT            \in BOOLEAN
ASSUME BUGGY_LOST_READY             \in BOOLEAN
ASSUME BUGGY_EDGE_ARM               \in BOOLEAN

VARIABLES
    ready,           \* BOOLEAN -- the server's truth: the socket is ready for
                     \*   the requested mask. A LEVEL.
    hung,            \* BOOLEAN -- the server has stopped answering. Monotonic.
    snap,            \* this pass's sample (see SnapStates): the snapshot, or
                     \*   under BUGGY_CACHE_ONLY_SAMPLE the cache read.
    arm,             \* BOOLEAN -- an arm is outstanding at the server.
    rose,            \* BOOLEAN -- BUGGY_EDGE_ARM only: the level rose after
                     \*   the outstanding arm arrived.
    pending,         \* BOOLEAN -- an arm's answer was demuxed and the
                     \*   kthread's walk of the hook list is owed.
    cache,           \* BOOLEAN -- BUGGY_CACHE_ONLY_SAMPLE only: the readiness
                     \*   the last answered arm recorded.
    hooked,          \* BOOLEAN -- the poller's hook is on the socket's list.
    flagged,         \* BOOLEAN -- the hook's flag, set by the kthread's walk.
    pc,              \* the poll call's lifecycle (see PCs).
    deadline_passed, \* BOOLEAN -- the call's deadline has passed. Monotonic.
    failsafe,        \* BOOLEAN -- the fail-safe counter is nonzero.
    pass_ready,      \* BOOLEAN -- ghost: `ready` held at some instant of the
                     \*   current pass.
    pass_notready    \* BOOLEAN -- ghost: `ready` failed at some instant of
                     \*   the current pass.

vars == <<ready, hung, snap, arm, rose, pending, cache, hooked, flagged, pc,
          deadline_passed, failsafe, pass_ready, pass_notready>>

\* "none"     -- no pass has scanned yet.
\* "sent"     -- the snapshot is in flight: the pass is settling.
\* "ready"    -- the server answered ready (or the cache said so).
\* "notready" -- the server answered not ready (or the cache said so).
\* "failsafe" -- unanswered 1 s after it was sent: flushed, counted.
\* "cut"      -- BUGGY_SETTLE_CUT_BY_DEADLINE: abandoned at the deadline.
\* "gc"       -- BUGGY_GC_SNAPSHOT: flushed by the stranded-op collector.
SnapStates == {"none", "sent", "ready", "notready", "failsafe", "cut", "gc"}

\* "start"        -- a pass is about to scan (poll() entered, or re-looping).
\* "settling"     -- scanned; the verdict waits for the snapshot.
\* "arming"       -- the verdict was not ready and the call will park.
\* "armed"        -- hooked and armed: the tsleep commit point.
\* "sleeping"     -- parked on the poller's private Rendez.
\* "woken"        -- tsleep returned (a flag, or the deadline).
\* "done_ready"   -- poll returned the socket ready.
\* "done_timeout" -- poll returned 0.
PCs      == {"start", "settling", "arming", "armed", "sleeping", "woken",
             "done_ready", "done_timeout"}
Terminal == {"done_ready", "done_timeout"}

TypeOk ==
    /\ ready           \in BOOLEAN
    /\ hung            \in BOOLEAN
    /\ snap            \in SnapStates
    /\ arm             \in BOOLEAN
    /\ rose            \in BOOLEAN
    /\ pending         \in BOOLEAN
    /\ cache           \in BOOLEAN
    /\ hooked          \in BOOLEAN
    /\ flagged         \in BOOLEAN
    /\ pc              \in PCs
    /\ deadline_passed \in BOOLEAN
    /\ failsafe        \in BOOLEAN
    /\ pass_ready      \in BOOLEAN
    /\ pass_notready   \in BOOLEAN

Init ==
    /\ ready           = FALSE
    /\ hung            = FALSE
    /\ snap            = "none"
    /\ arm             = FALSE
    /\ rose            = FALSE
    /\ pending         = FALSE
    /\ cache           = FALSE
    /\ hooked          = FALSE
    /\ flagged         = FALSE
    /\ pc              = "start"
    /\ deadline_passed = FALSE
    /\ failsafe        = FALSE
    /\ pass_ready      = FALSE
    /\ pass_notready   = FALSE

Live    == pc \notin Terminal
Expired == HAS_TIMEOUT /\ deadline_passed

(***************************************************************************)
(* SocketReady / SocketRetract -- the server's level rises (bytes arrive,  *)
(* the peer closes, the send buffer drains) or falls (a competing reader   *)
(* drains the bytes). Neither reaches the kernel by itself: only a         *)
(* snapshot's answer or an arm's does.                                     *)
(***************************************************************************)
SocketReady ==
    /\ Live
    /\ ~ready
    /\ ready'      = TRUE
    /\ pass_ready' = TRUE
    /\ rose'       = (rose \/ (BUGGY_EDGE_ARM /\ arm))
    /\ UNCHANGED <<hung, snap, arm, pending, cache, hooked, flagged, pc,
                   deadline_passed, failsafe, pass_notready>>

SocketRetract ==
    /\ Live
    /\ ready
    /\ ready'         = FALSE
    /\ pass_notready' = TRUE
    /\ UNCHANGED <<hung, snap, arm, rose, pending, cache, hooked, flagged, pc,
                   deadline_passed, failsafe, pass_ready>>

\* Hang -- the server stops answering reads (HUNG_SERVER only).
Hang ==
    /\ HUNG_SERVER
    /\ Live
    /\ ~hung
    /\ hung' = TRUE
    /\ UNCHANGED <<ready, snap, arm, rose, pending, cache, hooked, flagged, pc,
                   deadline_passed, failsafe, pass_ready, pass_notready>>

\* AdvanceTime -- the monotonic clock reaches the call's deadline.
AdvanceTime ==
    /\ HAS_TIMEOUT
    /\ Live
    /\ ~deadline_passed
    /\ deadline_passed' = TRUE
    /\ UNCHANGED <<ready, hung, snap, arm, rose, pending, cache, hooked,
                   flagged, pc, failsafe, pass_ready, pass_notready>>

(***************************************************************************)
(* Scan -- a pass begins: the poller sends the snapshot. No hook is        *)
(* installed (a snapshot is a sample, not a wait) and no arm is sent. The  *)
(* pass ghosts start from the level at this instant.                       *)
(*                                                                         *)
(* BUGGY_CACHE_ONLY_SAMPLE replaces the snapshot with a read of the cache, *)
(* settled at once: the design before #98.                                 *)
(***************************************************************************)
Scan ==
    /\ pc = "start"
    /\ pc'            = "settling"
    /\ snap'          = IF BUGGY_CACHE_ONLY_SAMPLE
                        THEN IF cache THEN "ready" ELSE "notready"
                        ELSE "sent"
    /\ pass_ready'    = ready
    /\ pass_notready' = ~ready
    /\ UNCHANGED <<ready, hung, arm, rose, pending, cache, hooked, flagged,
                   deadline_passed, failsafe>>

(***************************************************************************)
(* SnapshotReply -- the server serves the snapshot: it evaluates the level *)
(* at this instant and answers at once, 0 included. A hung server answers  *)
(* nothing.                                                                *)
(***************************************************************************)
SnapshotReply ==
    /\ snap = "sent"
    /\ ~hung
    /\ snap' = IF ready THEN "ready" ELSE "notready"
    /\ UNCHANGED <<ready, hung, arm, rose, pending, cache, hooked, flagged,
                   pc, deadline_passed, failsafe, pass_ready, pass_notready>>

(***************************************************************************)
(* SnapshotFailSafe -- the snapshot is still unanswered a fixed 1 s after  *)
(* it was sent: the kernel flushes it, reports the socket not ready, and   *)
(* counts the expiry. Enabled only against a hung server -- see MODELING   *)
(* ASSUMPTIONS for why that is the model's timing assumption and not a     *)
(* theorem.                                                                *)
(*                                                                         *)
(* SnapshotCut -- BUGGY_SETTLE_CUT_BY_DEADLINE: the settle gives up at the *)
(* call's deadline instead, whether or not the server is answering. It is  *)
(* counted too, which is why the counter alone cannot tell the two apart   *)
(* and NoFalseNotReady asks whether the server had hung.                   *)
(***************************************************************************)
SnapshotFailSafe ==
    /\ snap = "sent"
    /\ hung
    /\ snap'     = "failsafe"
    /\ failsafe' = TRUE
    /\ UNCHANGED <<ready, hung, arm, rose, pending, cache, hooked, flagged,
                   pc, deadline_passed, pass_ready, pass_notready>>

SnapshotCut ==
    /\ BUGGY_SETTLE_CUT_BY_DEADLINE
    /\ snap = "sent"
    /\ Expired
    /\ snap'     = "cut"
    /\ failsafe' = TRUE
    /\ UNCHANGED <<ready, hung, arm, rose, pending, cache, hooked, flagged,
                   pc, deadline_passed, pass_ready, pass_notready>>

(***************************************************************************)
(* Verdict -- the settle is over (the snapshot is terminal). Ready         *)
(* returns. Not ready returns 0 once the deadline has passed -- whether it *)
(* passed before the scan (timeout 0, a sample-only pass) or during the    *)
(* settle, the snapshot's answer IS the answer -- and otherwise the call   *)
(* goes on to park. The pass installed no hook, so none needs removing.    *)
(***************************************************************************)
Verdict ==
    /\ pc = "settling"
    /\ snap # "sent"
    /\ pc' = IF snap = "ready" THEN "done_ready"
             ELSE IF Expired  THEN "done_timeout"
             ELSE                  "arming"
    /\ UNCHANGED <<ready, hung, snap, arm, rose, pending, cache, hooked,
                   flagged, deadline_passed, failsafe, pass_ready,
                   pass_notready>>

(***************************************************************************)
(* PollerArm -- the call will park: install the hook, then ENSURE an arm   *)
(* is outstanding (send one unless one already is), in one step under the  *)
(* poll-state lock. The hook goes on first, so the arm's answer, whenever  *)
(* it comes, has a hook to walk. An arm already outstanding keeps its      *)
(* history; a fresh one has seen no rise.                                  *)
(*                                                                         *)
(* BUGGY_LOST_READY skips the ensure: the poller parks with whatever arm   *)
(* happens to be outstanding, which may be none.                           *)
(***************************************************************************)
PollerArm ==
    /\ pc = "arming"
    /\ pc'     = "armed"
    /\ hooked' = TRUE
    /\ arm'    = (arm \/ ~BUGGY_LOST_READY)
    /\ rose'   = (arm /\ rose)
    /\ UNCHANGED <<ready, hung, snap, pending, cache, flagged,
                   deadline_passed, failsafe, pass_ready, pass_notready>>

(***************************************************************************)
(* ArmReply -- the server answers an outstanding arm because the socket is *)
(* ready, and the poll-pump kthread, as the elected reader, demuxes the    *)
(* answer: `on_complete` runs under `c->lock` and only sets the relay      *)
(* flag. The arm is spent. Under BUGGY_CACHE_ONLY_SAMPLE the answer also   *)
(* records the cache the old design sampled.                               *)
(*                                                                         *)
(* BUGGY_EDGE_ARM answers only a rise that came after the arm arrived.     *)
(***************************************************************************)
ArmReply ==
    /\ Live
    /\ arm
    /\ ready
    /\ ~hung
    /\ (~BUGGY_EDGE_ARM \/ rose)
    /\ arm'     = FALSE
    /\ rose'    = FALSE
    /\ pending' = TRUE
    /\ cache'   = (cache \/ BUGGY_CACHE_ONLY_SAMPLE)
    /\ UNCHANGED <<ready, hung, snap, hooked, flagged, pc, deadline_passed,
                   failsafe, pass_ready, pass_notready>>

(***************************************************************************)
(* KthreadWalk -- the kthread, after the pump and with `c->lock` released, *)
(* walks the hook list in process context: a hooked poller's flag is set   *)
(* and a sleeping one is woken. A walk that finds no hook (the poller is   *)
(* between passes) wakes nobody; the arm it answered is spent, and the     *)
(* next park ensures a fresh one.                                          *)
(***************************************************************************)
KthreadWalk ==
    /\ Live
    /\ pending
    /\ pending' = FALSE
    /\ flagged' = (flagged \/ hooked)
    /\ pc'      = IF hooked /\ pc = "sleeping" THEN "woken" ELSE pc
    /\ UNCHANGED <<ready, hung, snap, arm, rose, cache, hooked,
                   deadline_passed, failsafe, pass_ready, pass_notready>>

(***************************************************************************)
(* GcArm -- the kthread collects a STRANDED arm (no hook on the list:      *)
(* every poller that wanted it has moved on) and flushes it. Harmless to a *)
(* poller in a pass: the next park ensures a fresh arm. It can never take  *)
(* the arm of a parked poller, whose hook is on the list.                  *)
(*                                                                         *)
(* GcSnapshot -- BUGGY_GC_SNAPSHOT: the collector takes the snapshot too,  *)
(* because a snapshot has no hook either.                                  *)
(***************************************************************************)
GcArm ==
    /\ Live
    /\ arm
    /\ ~hooked
    /\ arm'  = FALSE
    /\ rose' = FALSE
    /\ UNCHANGED <<ready, hung, snap, pending, cache, hooked, flagged, pc,
                   deadline_passed, failsafe, pass_ready, pass_notready>>

GcSnapshot ==
    /\ BUGGY_GC_SNAPSHOT
    /\ snap = "sent"
    /\ ~hooked
    /\ snap' = "gc"
    /\ UNCHANGED <<ready, hung, arm, rose, pending, cache, hooked, flagged,
                   pc, deadline_passed, failsafe, pass_ready, pass_notready>>

(***************************************************************************)
(* TSleepCommit -- the `tsleep` call: the flag scan and the sleep are      *)
(* atomic under the poller's Rendez lock, and a set flag beats the         *)
(* deadline (tsleep.tla TimeoutSound). A flag or a passed deadline goes    *)
(* round to another pass; otherwise the poller sleeps.                     *)
(*                                                                         *)
(* Timeout -- the deadline wakes the sleeping poller.                      *)
(***************************************************************************)
TSleepCommit ==
    /\ pc = "armed"
    /\ pc' = IF flagged \/ Expired THEN "woken" ELSE "sleeping"
    /\ UNCHANGED <<ready, hung, snap, arm, rose, pending, cache, hooked,
                   flagged, deadline_passed, failsafe, pass_ready,
                   pass_notready>>

Timeout ==
    /\ pc = "sleeping"
    /\ Expired
    /\ pc' = "woken"
    /\ UNCHANGED <<ready, hung, snap, arm, rose, pending, cache, hooked,
                   flagged, deadline_passed, failsafe, pass_ready,
                   pass_notready>>

(***************************************************************************)
(* Rearm -- the woken poller takes its hook off the list (clearing it) and *)
(* goes round to a new pass. A flag is a hint, never a verdict: the pass   *)
(* samples again. The arm, if still outstanding, stays -- another poller   *)
(* may want it, and the collector takes it if nobody does.                 *)
(***************************************************************************)
Rearm ==
    /\ pc = "woken"
    /\ pc'      = "start"
    /\ hooked'  = FALSE
    /\ flagged' = FALSE
    /\ UNCHANGED <<ready, hung, snap, arm, rose, pending, cache,
                   deadline_passed, failsafe, pass_ready, pass_notready>>

\* Done -- terminal self-loop, so -deadlock stays meaningful before it.
Done == pc \in Terminal /\ UNCHANGED vars

PollerStep ==
    \/ Scan
    \/ Verdict
    \/ PollerArm
    \/ TSleepCommit
    \/ Timeout
    \/ Rearm

Next ==
    \/ PollerStep
    \/ SocketReady
    \/ SocketRetract
    \/ Hang
    \/ AdvanceTime
    \/ SnapshotReply
    \/ SnapshotFailSafe
    \/ SnapshotCut
    \/ ArmReply
    \/ KthreadWalk
    \/ GcArm
    \/ GcSnapshot
    \/ Done

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================= INVARIANTS =============================  *)
(***************************************************************************)

\* NoFalseNotReady -- a poll that returns 0 reports the socket not ready
\* only if it was not ready at some instant of the pass that decided, or
\* the server had stopped answering and the fail-safe (counted) decided.
\* Violated by BUGGY_CACHE_ONLY_SAMPLE (an empty cache), by
\* BUGGY_SETTLE_CUT_BY_DEADLINE (a healthy server cut off at the deadline)
\* and by BUGGY_GC_SNAPSHOT (a flushed snapshot).
NoFalseNotReady ==
    (pc = "done_timeout") => (pass_notready \/ (hung /\ failsafe))

\* NoFalseReady -- a poll reports the socket ready only if it was ready at
\* some instant of the pass that decided. Violated by
\* BUGGY_CACHE_ONLY_SAMPLE: an answered arm's cache outlives the level a
\* competing reader then lowered.
NoFalseReady == (pc = "done_ready") => pass_ready

\* ArmBeforePark -- a poller at or past its tsleep commit is hooked, and a
\* wake is on its way: an arm outstanding, its answer in the relay, or the
\* flag already set.
ArmBeforePark ==
    (pc \in {"armed", "sleeping"}) =>
        (hooked /\ (arm \/ pending \/ flagged))

\* NoMissedNetPoll -- ARCH section 28 I-9 across the relay: the poller is
\* never asleep on a ready socket, hooked, with no flag, nothing in the
\* relay and no arm outstanding -- nothing that will ever deliver.
\* Violated by BUGGY_LOST_READY.
NoMissedNetPoll ==
    ~( pc = "sleeping" /\ hooked /\ ready
       /\ ~flagged /\ ~pending /\ ~arm )

\* NoStaleHook -- a returned poll holds no hook.
NoStaleHook == (pc \in Terminal) => ~hooked

\* FailSafeSilent -- the fail-safe never fired. Holds against a server
\* that answers (the runtime counter the boot gates require to be zero);
\* net_poll_failsafe_fires.cfg shows it FAILS against one that hangs, so
\* the fail-safe is reachable and the counter is not decorative.
FailSafeSilent == ~failsafe

Invariants ==
    /\ TypeOk
    /\ NoFalseNotReady
    /\ NoFalseReady
    /\ ArmBeforePark
    /\ NoMissedNetPoll
    /\ NoStaleHook

(***************************************************************************)
(* ============================== LIVENESS ==============================  *)
(*                                                                         *)
(* PollTerminates -- a timed poll returns, even against a server that      *)
(* hangs: the fail-safe bounds the settle and the deadline bounds the      *)
(* park.                                                                   *)
(*                                                                         *)
(* PollerEventuallyServed -- a poll whose socket becomes ready and STAYS   *)
(* ready returns, timeout or no timeout. (Before the split, `ready` was    *)
(* monotonic and this read "ready and registered leads to a return"; a     *)
(* level that can fall needs the stable form.) Checked against a server    *)
(* that answers: a hung server may strand a poll(-1), as it would strand a *)
(* blocking read. Violated by BUGGY_EDGE_ARM.                              *)
(*                                                                         *)
(* Fairness grants the server nothing but its answers: WF on SnapshotReply *)
(* and ArmReply (a request it holds is eventually served while it can      *)
(* be), on SnapshotFailSafe (the 1 s timer fires), on the kthread's walk,  *)
(* on the clock, and on the poller's own steps. The socket's level, the    *)
(* hang and the collector get none.                                        *)
(***************************************************************************)
PollTerminates == <>(pc \in Terminal)

PollerEventuallyServed == (<>[]ready) => <>(pc \in Terminal)

Liveness ==
    /\ WF_vars(PollerStep)
    /\ WF_vars(SnapshotReply)
    /\ WF_vars(SnapshotFailSafe)
    /\ WF_vars(ArmReply)
    /\ WF_vars(KthreadWalk)
    /\ WF_vars(AdvanceTime)

Spec_Live == Init /\ [][Next]_vars /\ Liveness

====
