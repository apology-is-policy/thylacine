---- MODULE poll ----
(***************************************************************************)
(* Thylacine `poll` — the multi-fd wait/wake primitive (P5-poll).          *)
(*                                                                         *)
(* `poll(fds, nfds, timeout_ms)` parks the calling thread until at least   *)
(* one of N file descriptors is ready, or a timeout elapses (ARCH §23.3,   *)
(* §28 I-9). Thylacine has no fd layer — an `fd` is a handle index — and   *)
(* a thread can wait on only ONE `Rendez` (single-waiter; rendez.h         *)
(* extincts on a second). poll therefore does NOT make `Rendez`            *)
(* multi-waiter: the poller sleeps on its OWN private `Rendez` via         *)
(* `tsleep`, and registers a lightweight `poll_waiter` hook on each polled *)
(* object's hook list. When an object becomes ready, its existing wakeup   *)
(* site also walks that hook list, sets each registered waiter's flag, and *)
(* signals that poller's private `Rendez`.                                 *)
(*                                                                         *)
(* WHAT THIS SPEC PINS                                                      *)
(*                                                                         *)
(*   The single-fd missed wakeup (cond check vs sleep, one Rendez) is      *)
(*   scheduler.tla's proof; the deadline race is tsleep.tla's. poll adds   *)
(*   what neither covers: ONE thread waiting on N readiness sources whose  *)
(*   state lives behind N DIFFERENT locks. The poller cannot observe all   *)
(*   N fds atomically under one lock; the `poll_waiter` flag is the        *)
(*   cross-lock hand-off — a producer sets it under fd f's object lock,    *)
(*   the poller reads it under its own `Rendez` lock. The load-bearing     *)
(*   discipline is REGISTER-THEN-OBSERVE: the hook is installed before     *)
(*   (atomically with) the fd's readiness is sampled, so no readiness      *)
(*   event between sample and sleep is lost. This spec proves a poller is  *)
(*   never left asleep while a registered fd is ready (NoMissedPoll — I-9  *)
(*   across N fds) and that a returned poll holds no stale hook.           *)
(*                                                                         *)
(*   Since 2026-09-21 it also pins the RE-ARM. A poll_waiter flag is a     *)
(*   HINT, not a verdict: one hook list serves every poller of an object   *)
(*   whatever each asked for (POLLIN and POLLOUT pollers share a list; the *)
(*   two endpoints of a SrvConn share one across four readiness edges),    *)
(*   and readiness is a LEVEL a competing reader can lower before the      *)
(*   poller looks. So a wake is followed by a re-sample, and a re-sample   *)
(*   that finds nothing is followed by ANOTHER sleep against the same      *)
(*   deadline -- never by a return of 0 (NoSpuriousZero). The re-arm has   *)
(*   exactly one sound order, clear-then-sample, and the spec fails the    *)
(*   other one.                                                             *)
(*                                                                         *)
(* THE BUGS THIS PINS                                                       *)
(*                                                                         *)
(*   BUGGY_CHECK_BEFORE_REGISTER — the poller samples each fd's readiness  *)
(*     BEFORE installing its hook (check, then register, then sleep). A    *)
(*     readiness event in the gap reaches no hook — the producer's         *)
(*     hook-list walk finds the poller absent — so the stale sample drives *)
(*     the poller to sleep on an fd that is already ready. The fix is the  *)
(*     register-then-observe order: `dev->poll` installs the hook and      *)
(*     returns the readiness in one locked step (NoMissedPoll              *)
(*     counterexample).                                                     *)
(*                                                                         *)
(*   BUGGY_NO_WAKE — a producer makes an fd ready and sets the registered  *)
(*     poll_waiter's flag but does not signal the poller's `Rendez`. A     *)
(*     sleeping poller is never re-scheduled to observe the flag. The fix: *)
(*     every readiness event walks the hook list AND wakes each registered *)
(*     poller (NoMissedPoll counterexample).                                *)
(*                                                                         *)
(*   BUGGY_LAZY_UNREGISTER — poll returns without removing its poll_waiter *)
(*     hooks from the polled objects' lists. The hooks are stack-allocated *)
(*     for the duration of the call; a leftover hook is a dangling pointer *)
(*     the next readiness event will walk. The fix: poll unregisters every *)
(*     hook before it returns (NoStaleHook counterexample).                 *)
(*                                                                         *)
(*   BUGGY_CLEAR_AFTER_SAMPLE — on a wake the poller re-samples, THEN      *)
(*     clears its flags. An event landing between the two is recorded only *)
(*     in a flag the clear then wipes; the next tsleep finds no flag and   *)
(*     sleeps on a ready fd (NoMissedPoll counterexample). The fix: clear  *)
(*     first, sample second.                                                *)
(*                                                                         *)
(*   BUGGY_RETURN_ON_WAKE — the pre-2026-09-21 sys_poll_for_proc: a wake   *)
(*     whose re-sample finds nothing returns 0. poll(fd, 10 s) reports a   *)
(*     timeout after microseconds because a competing reader won the       *)
(*     bytes, or because the list was walked for an event this poller did  *)
(*     not ask about; poll(-1) returns 0, which POSIX never permits        *)
(*     (NoSpuriousZero counterexample). The fix: sleep again.               *)
(*                                                                         *)
(* CFG MATRIX (executable documentation per CLAUDE.md spec-first policy)    *)
(*                                                                         *)
(*   poll.cfg                            all buggy flags FALSE,            *)
(*                                        HAS_TIMEOUT TRUE — every safety  *)
(*                                        invariant holds.                  *)
(*   poll_notimeout.cfg                  HAS_TIMEOUT FALSE — poll(-1), the *)
(*                                        infinite wait; safety holds.     *)
(*   poll_liveness.cfg                   Spec_Live, HAS_TIMEOUT TRUE —     *)
(*                                        PollTerminates (the timeout      *)
(*                                        backstop, against a producer     *)
(*                                        that walks the lists forever) +  *)
(*                                        StableReadyReturns.              *)
(*   poll_liveness_notimeout.cfg         Spec_Live, HAS_TIMEOUT FALSE —    *)
(*                                        StableReadyReturns alone: a      *)
(*                                        poll(-1) may block forever, but  *)
(*                                        not on an fd that stays ready.   *)
(*   poll_buggy_check_before_register.cfg BUGGY_CHECK_BEFORE_REGISTER —    *)
(*                                        NoMissedPoll counterexample.     *)
(*   poll_buggy_no_wake.cfg              BUGGY_NO_WAKE — NoMissedPoll      *)
(*                                        counterexample.                  *)
(*   poll_buggy_lazy_unregister.cfg      BUGGY_LAZY_UNREGISTER —          *)
(*                                        NoStaleHook counterexample.      *)
(*   poll_buggy_clear_after_sample.cfg   BUGGY_CLEAR_AFTER_SAMPLE —       *)
(*                                        NoMissedPoll counterexample.     *)
(*   poll_buggy_return_on_wake.cfg       BUGGY_RETURN_ON_WAKE —           *)
(*                                        NoSpuriousZero counterexample.   *)
(*                                                                         *)
(* MODELING ASSUMPTIONS                                                     *)
(*                                                                         *)
(*   One poller, N fds (Fds). The headline property — a single thread      *)
(*   waiting on N readiness sources — is fully exercised by one poller.    *)
(*   Multiple pollers on one fd's hook list compose: each has its own      *)
(*   private `Rendez` and its own `poll_waiter`; a producer's list walk    *)
(*   wakes each independently, with no mutable state shared between them.  *)
(*                                                                         *)
(*   Atomic actions. `Register` installs every hook AND samples every fd's *)
(*   readiness in one step — modeling the per-fd `dev->poll` call, which   *)
(*   holds fd f's object lock across hook-install + readiness-return. The  *)
(*   cross-fd loop is not separately interleaved: a readiness event during *)
(*   the loop either precedes a given fd's install (that install's sample *)
(*   then captures it) or follows it (the producer's hook-list walk then   *)
(*   captures it) — both are already in the state space.                   *)
(*                                                                         *)
(*   `ready` is a LEVEL, not an edge: MakeReady raises it, Retract lowers  *)
(*   it (a competing reader drained the bytes before this poller looked).  *)
(*   Until 2026-09-21 this spec modeled it as monotonic and called a       *)
(*   draining consumer "a separate concern outside one poll call". It is   *)
(*   not outside the call: it is what makes a post-wake re-sample come     *)
(*   back empty, and the code's answer to that (return 0) was wrong in a   *)
(*   way a monotonic model could not express. `ready` means "ready FOR     *)
(*   WHAT THIS POLLER ASKED"; OtherEvent is the list walk for anything     *)
(*   else.                                                                  *)
(*                                                                         *)
(*   The flag-vs-timeout precedence on resume (success has precedence — a  *)
(*   fd readied exactly at the deadline reports ready, not timeout) is     *)
(*   tsleep.tla's `TimeoutSound` proof; poll's `TSleepCommit` is a         *)
(*   `tsleep` call and checks the flag before the deadline. Not re-pinned. *)
(*   What IS pinned here is its consequence for the loop: because a flag   *)
(*   beats the deadline, the loop needs its own Expired test, or a         *)
(*   producer that never stops walking the list holds the poller past its  *)
(*   timeout forever (PollTerminates).                                      *)
(*                                                                         *)
(*   The death-interrupt (#811, TSLEEP_INTR) is not modeled: it skips the  *)
(*   re-sample and falls to the unregister sweep, which NoStaleHook's      *)
(*   return arms already cover.                                             *)
(*                                                                         *)
(* See ARCHITECTURE.md §23.3 (poll/select), §28 invariant I-9; tsleep.tla  *)
(* (the deadline-bounded `Rendez` sleep poll builds on); scheduler.tla     *)
(* (the single-`Rendez` wait/wake proof).                                  *)
(***************************************************************************)
EXTENDS FiniteSets

CONSTANTS
    Fds,                          \* the set of file descriptors polled.
    HAS_TIMEOUT,                  \* BOOLEAN — TRUE: the poll call carries a
                                  \*   finite timeout (timeout_ms >= 0).
                                  \*   FALSE: poll(-1), an unbounded wait.
    BUGGY_CHECK_BEFORE_REGISTER,  \* BOOLEAN — TRUE: the poller samples each
                                  \*   fd's readiness BEFORE installing its
                                  \*   hook (check, register, sleep).
    BUGGY_NO_WAKE,                \* BOOLEAN — TRUE: a readiness event sets
                                  \*   the poll_waiter flag but does NOT
                                  \*   signal the sleeping poller's Rendez.
    BUGGY_LAZY_UNREGISTER,        \* BOOLEAN — TRUE: poll returns without
                                  \*   unregistering its poll_waiter hooks.
    BUGGY_CLEAR_AFTER_SAMPLE,     \* BOOLEAN — TRUE: on a wake the poller
                                  \*   re-samples FIRST and clears its flags
                                  \*   SECOND, so an event between the two is
                                  \*   wiped with the flag that recorded it.
    BUGGY_RETURN_ON_WAKE          \* BOOLEAN — TRUE: a wake whose re-sample
                                  \*   finds nothing ready RETURNS 0 instead
                                  \*   of sleeping again (the pre-2026-09-21
                                  \*   sys_poll_for_proc).

ASSUME Fds # {}
ASSUME HAS_TIMEOUT                 \in BOOLEAN
ASSUME BUGGY_CHECK_BEFORE_REGISTER \in BOOLEAN
ASSUME BUGGY_NO_WAKE               \in BOOLEAN
ASSUME BUGGY_LAZY_UNREGISTER       \in BOOLEAN
ASSUME BUGGY_CLEAR_AFTER_SAMPLE    \in BOOLEAN
ASSUME BUGGY_RETURN_ON_WAKE        \in BOOLEAN

VARIABLES
    pc,               \* the poll call's lifecycle ∈ PCs (see below).
    ready,            \* [Fds -> BOOLEAN] — the kernel-side truth of each
                      \*   fd's readiness FOR THE EVENTS THIS POLLER ASKED
                      \*   ABOUT. A LEVEL: it rises (MakeReady) and falls
                      \*   (Retract).
    registered,       \* [Fds -> BOOLEAN] — the poller's poll_waiter hook is
                      \*   installed on fd f's hook list.
    flagged,          \* [Fds -> BOOLEAN] — fd f's poll_waiter (this
                      \*   poller's) has its `ready` flag set. A HINT that
                      \*   something happened on f, not a verdict: a flag is
                      \*   also set by an event the poller did not ask about.
    seen,             \* [Fds -> BOOLEAN] — what the poller's latest SAMPLE
                      \*   of each fd returned (the revents it would report).
    deadline_passed   \* BOOLEAN — monotonic time reached the poll timeout.

vars == <<pc, ready, registered, flagged, seen, deadline_passed>>

\* "start"         — poll() entered; no hook installed, nothing sampled.
\* "checked"       — BUGGY path only: readiness sampled, no hook installed.
\* "scanned"       — the first scan is done (hooks installed + sampled).
\* "armed"         — about to call tsleep: the commit point.
\* "sleeping"      — committed to sleep on the poller's private Rendez.
\* "woken"         — tsleep returned AWOKEN (some flag was set).
\* "cleared"       — the flags are cleared; the re-sample is pending.
\* "sampled_dirty" — BUGGY_CLEAR_AFTER_SAMPLE only: re-sampled, flags not
\*                   yet cleared.
\* "rescanned"     — the post-wake re-sample is done; evaluate it.
\* "timedout"      — tsleep returned TIMEDOUT; the final sample is pending.
\* "final"         — the final sample is done; evaluate it.
\* "done_ready"    — poll returned >= 1 ready fd.
\* "done_timeout"  — poll returned 0.
PCs      == {"start", "checked", "scanned", "armed", "sleeping", "woken",
             "cleared", "sampled_dirty", "rescanned", "timedout", "final",
             "done_ready", "done_timeout"}
Terminal == {"done_ready", "done_timeout"}

TypeOk ==
    /\ pc              \in PCs
    /\ ready           \in [Fds -> BOOLEAN]
    /\ registered      \in [Fds -> BOOLEAN]
    /\ flagged         \in [Fds -> BOOLEAN]
    /\ seen            \in [Fds -> BOOLEAN]
    /\ deadline_passed \in BOOLEAN

NoneSet == [f \in Fds |-> FALSE]
AllSet  == [f \in Fds |-> TRUE]

Init ==
    /\ pc              = "start"
    /\ ready           = NoneSet
    /\ registered      = NoneSet
    /\ flagged         = NoneSet
    /\ seen            = NoneSet
    /\ deadline_passed = FALSE

(***************************************************************************)
(* Expired — the deadline-reached predicate. FALSE whenever the modeled    *)
(* poll has no timeout (poll(-1)).                                          *)
(***************************************************************************)
Expired == HAS_TIMEOUT /\ deadline_passed

\* On any return the hooks come off; BUGGY_LAZY_UNREGISTER leaves them.
Unhook == IF BUGGY_LAZY_UNREGISTER THEN registered ELSE NoneSet

(***************************************************************************)
(* The hook-list walk every producer-side event performs: set this         *)
(* poller's flag if its hook is on f's list, and — the CORRECT path — wake *)
(* a sleeping poller. BUGGY_NO_WAKE sets the flag but skips the wake.      *)
(***************************************************************************)
Walk(f) ==
    /\ flagged' = IF registered[f]
                  THEN [flagged EXCEPT ![f] = TRUE]
                  ELSE flagged
    /\ pc' = IF pc = "sleeping" /\ registered[f] /\ ~BUGGY_NO_WAKE
             THEN "woken"
             ELSE pc

(***************************************************************************)
(* MakeReady — a producer makes fd f ready for what the poller asked       *)
(* (bytes arrive, the peer closes) and walks f's hook list.                 *)
(***************************************************************************)
MakeReady(f) ==
    /\ pc \notin Terminal
    /\ ~ready[f]
    /\ ready' = [ready EXCEPT ![f] = TRUE]
    /\ Walk(f)
    /\ UNCHANGED <<registered, seen, deadline_passed>>

(***************************************************************************)
(* Retract — readiness falls again before the poller looks: a competing    *)
(* reader drained the bytes. No hook-list walk (nothing became MORE        *)
(* ready); a flag an earlier MakeReady set stays set, now stale.            *)
(***************************************************************************)
Retract(f) ==
    /\ pc \notin Terminal
    /\ ready[f]
    /\ ready' = [ready EXCEPT ![f] = FALSE]
    /\ UNCHANGED <<pc, registered, flagged, seen, deadline_passed>>

(***************************************************************************)
(* OtherEvent — f's hook list is walked for an event this poller did NOT   *)
(* ask about. One list serves every poller of the object, whatever each    *)
(* asked: a poller waiting for POLLIN on a connection is on the same list  *)
(* as one waiting for POLLOUT, and the two endpoints of a SrvConn share    *)
(* one list between four readiness edges. `ready` is unchanged.             *)
(***************************************************************************)
OtherEvent(f) ==
    /\ pc \notin Terminal
    /\ Walk(f)
    /\ UNCHANGED <<ready, registered, seen, deadline_passed>>

(***************************************************************************)
(* AdvanceTime — the monotonic counter reaches the poll timeout.           *)
(***************************************************************************)
AdvanceTime ==
    /\ HAS_TIMEOUT
    /\ ~deadline_passed
    /\ pc \notin Terminal
    /\ deadline_passed' = TRUE
    /\ UNCHANGED <<pc, ready, registered, flagged, seen>>

(***************************************************************************)
(* Register — the CORRECT entry. For every fd, `dev->poll` installs the    *)
(* poll_waiter hook AND returns the fd's current readiness, in one step    *)
(* under fd f's object lock: register-then-observe. No readiness event can *)
(* slip between the sample and the hook being live.                         *)
(***************************************************************************)
Register ==
    /\ ~BUGGY_CHECK_BEFORE_REGISTER
    /\ pc = "start"
    /\ pc'         = "scanned"
    /\ registered' = AllSet
    /\ seen'       = ready
    /\ UNCHANGED <<ready, flagged, deadline_passed>>

(***************************************************************************)
(* BuggyCheck / BuggyRegisterLate — the BUGGY entry: sample, THEN install. *)
(* A readiness event between the two set `ready` but reached no hook, and  *)
(* is not re-captured.                                                      *)
(***************************************************************************)
BuggyCheck ==
    /\ BUGGY_CHECK_BEFORE_REGISTER
    /\ pc = "start"
    /\ pc'   = "checked"
    /\ seen' = ready
    /\ UNCHANGED <<ready, registered, flagged, deadline_passed>>

BuggyRegisterLate ==
    /\ BUGGY_CHECK_BEFORE_REGISTER
    /\ pc = "checked"
    /\ pc'         = "scanned"
    /\ registered' = AllSet
    /\ UNCHANGED <<ready, flagged, seen, deadline_passed>>

(***************************************************************************)
(* EvaluateFirst — the first scan's verdict. Anything seen ready returns;  *)
(* otherwise the poller goes to its tsleep. (timeout_ms == 0 is the run in *)
(* which the deadline has already passed: TSleepCommit sees it.)            *)
(***************************************************************************)
EvaluateFirst ==
    /\ pc = "scanned"
    /\ UNCHANGED <<ready, flagged, seen, deadline_passed>>
    /\ IF \E f \in Fds : seen[f]
       THEN /\ pc' = "done_ready"
            /\ registered' = Unhook
       ELSE /\ pc' = "armed"
            /\ registered' = registered

(***************************************************************************)
(* TSleepCommit — the `tsleep` call. The flag scan and the sleep           *)
(* transition are atomic under the poller's Rendez lock. A set flag has    *)
(* precedence over the deadline (tsleep.tla TimeoutSound).                  *)
(***************************************************************************)
TSleepCommit ==
    /\ pc = "armed"
    /\ pc' = IF \E f \in Fds : flagged[f] THEN "woken"
             ELSE IF Expired              THEN "timedout"
             ELSE "sleeping"
    /\ UNCHANGED <<ready, registered, flagged, seen, deadline_passed>>

(***************************************************************************)
(* Timeout — the tsleep deadline fires and wakes the sleeping poller.      *)
(***************************************************************************)
Timeout ==
    /\ pc = "sleeping"
    /\ deadline_passed
    /\ pc' = "timedout"
    /\ UNCHANGED <<ready, registered, flagged, seen, deadline_passed>>

(***************************************************************************)
(* ClearFlags THEN Resample — the re-arm, in the only sound order. The     *)
(* flags are cleared FIRST (each under its hook list's lock), the fds      *)
(* re-sampled SECOND. An event that lands between the two sets a flag that *)
(* survives into the next tsleep; one that landed before the clear is seen *)
(* by the sample, which runs under the object's lock after the producer    *)
(* released it.                                                             *)
(***************************************************************************)
ClearFlags ==
    /\ ~BUGGY_CLEAR_AFTER_SAMPLE
    /\ pc = "woken"
    /\ pc'      = "cleared"
    /\ flagged' = NoneSet
    /\ UNCHANGED <<ready, registered, seen, deadline_passed>>

Resample ==
    /\ pc = "cleared"
    /\ pc'   = "rescanned"
    /\ seen' = ready
    /\ UNCHANGED <<ready, registered, flagged, deadline_passed>>

\* The BUGGY order: sample, then clear.
BuggySampleFirst ==
    /\ BUGGY_CLEAR_AFTER_SAMPLE
    /\ pc = "woken"
    /\ pc'   = "sampled_dirty"
    /\ seen' = ready
    /\ UNCHANGED <<ready, registered, flagged, deadline_passed>>

BuggyClearLate ==
    /\ pc = "sampled_dirty"
    /\ pc'      = "rescanned"
    /\ flagged' = NoneSet
    /\ UNCHANGED <<ready, registered, seen, deadline_passed>>

(***************************************************************************)
(* EvaluateWake — the verdict after a wake. Ready returns. NOT ready is    *)
(* the case the pre-2026-09-21 code got wrong: it returned 0, so a poll    *)
(* with seconds left reported a timeout the moment anything at all         *)
(* happened on one of its objects, and poll(-1) returned 0, which POSIX    *)
(* never permits. The poller sleeps AGAIN, against the SAME deadline. The  *)
(* explicit Expired test bounds the loop: tsleep prefers a set flag to a   *)
(* passed deadline, so a producer that keeps walking the list would        *)
(* otherwise keep the poller circling past its timeout.                     *)
(***************************************************************************)
EvaluateWake ==
    /\ pc = "rescanned"
    /\ UNCHANGED <<ready, flagged, seen, deadline_passed>>
    /\ IF \E f \in Fds : seen[f]
       THEN /\ pc' = "done_ready"
            /\ registered' = Unhook
       ELSE IF Expired \/ BUGGY_RETURN_ON_WAKE
       THEN /\ pc' = "done_timeout"
            /\ registered' = Unhook
       ELSE /\ pc' = "armed"
            /\ registered' = registered

(***************************************************************************)
(* FinalSample / EvaluateFinal — tsleep returned TIMEDOUT. One last sample *)
(* (success has precedence: an fd readied as the deadline lapses reports   *)
(* ready), then return either way.                                          *)
(***************************************************************************)
FinalSample ==
    /\ pc = "timedout"
    /\ pc'   = "final"
    /\ seen' = ready
    /\ UNCHANGED <<ready, registered, flagged, deadline_passed>>

EvaluateFinal ==
    /\ pc = "final"
    /\ pc' = IF \E f \in Fds : seen[f] THEN "done_ready" ELSE "done_timeout"
    /\ registered' = Unhook
    /\ UNCHANGED <<ready, flagged, seen, deadline_passed>>

(***************************************************************************)
(* Done — terminal self-loop (keeps TLC's deadlock check quiet).            *)
(***************************************************************************)
Done == pc \in Terminal /\ UNCHANGED vars

PollerStep ==
    \/ Register
    \/ BuggyCheck
    \/ BuggyRegisterLate
    \/ EvaluateFirst
    \/ TSleepCommit
    \/ Timeout
    \/ ClearFlags
    \/ Resample
    \/ BuggySampleFirst
    \/ BuggyClearLate
    \/ EvaluateWake
    \/ FinalSample
    \/ EvaluateFinal

Next ==
    \/ PollerStep
    \/ \E f \in Fds : MakeReady(f)
    \/ \E f \in Fds : Retract(f)
    \/ \E f \in Fds : OtherEvent(f)
    \/ AdvanceTime
    \/ Done

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

\* HookedReadyIsFlagged — at the two points where it matters (about to
\* sleep, asleep) every hooked fd that IS ready has its flag set. This is
\* the register-then-observe discipline, and the clear-then-sample order,
\* stated as one fact. It is deliberately NOT claimed between the clear and
\* the evaluate: there the flag is down by design and the SAMPLE carries
\* the readiness instead.
HookedReadyIsFlagged ==
    (pc \in {"armed", "sleeping"}) =>
        (\A f \in Fds : (registered[f] /\ ready[f]) => flagged[f])

\* NoMissedPoll — ARCH §28 I-9 across N fds: a poller is never left asleep
\* while a registered fd is ready. The headline property. Violated by
\* BUGGY_CHECK_BEFORE_REGISTER (stale sample), BUGGY_NO_WAKE (the event
\* never wakes the sleeper) and BUGGY_CLEAR_AFTER_SAMPLE (the re-arm wipes
\* the flag of an event its own sample was too early to see).
NoMissedPoll ==
    ~(pc = "sleeping" /\ \E f \in Fds : ready[f] /\ registered[f])

\* NoStaleHook — a returned poll holds no poll_waiter hook. Violated by
\* BUGGY_LAZY_UNREGISTER.
NoStaleHook ==
    (pc \in Terminal) => (\A f \in Fds : ~registered[f])

\* ReadyResultSound — poll returns "ready" only on a sample that saw it. A
\* FLAG alone never produces a ready return: flags are hints.
ReadyResultSound == (pc = "done_ready") => (\E f \in Fds : seen[f])

\* TimeoutResultSound — poll returns 0 only when its last sample saw nothing.
TimeoutResultSound == (pc = "done_timeout") => (\A f \in Fds : ~seen[f])

\* NoSpuriousZero — poll returns 0 ONLY once its deadline has passed; a
\* poll(-1) never returns 0 at all. Violated by BUGGY_RETURN_ON_WAKE.
NoSpuriousZero == (pc = "done_timeout") => Expired

Invariants ==
    /\ TypeOk
    /\ HookedReadyIsFlagged
    /\ NoMissedPoll
    /\ NoStaleHook
    /\ ReadyResultSound
    /\ TimeoutResultSound
    /\ NoSpuriousZero

(***************************************************************************)
(* ============================== LIVENESS ================================ *)
(*                                                                         *)
(* PollTerminates — with a timeout, poll always eventually returns, even   *)
(* against a producer that walks the hook lists forever without ever       *)
(* leaving an fd ready (fairness grants the producer NOTHING). This is the *)
(* property EvaluateWake's explicit Expired test exists for.                *)
(*                                                                         *)
(* StableReadyReturns — once an fd is ready and STAYS ready, poll returns, *)
(* timeout or no timeout. (The old PollReturnsWhenReady — "a set flag      *)
(* leads to a return" — is false by design now: a flag is a hint.)          *)
(***************************************************************************)
PollTerminates == <>(pc \in Terminal)

StableReadyReturns ==
    \A f \in Fds : (<>[](ready[f])) => <>(pc \in Terminal)

Liveness ==
    /\ WF_vars(PollerStep)
    /\ WF_vars(AdvanceTime)

Spec_Live == Init /\ [][Next]_vars /\ Liveness

====
