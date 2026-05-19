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
(* CFG MATRIX (executable documentation per CLAUDE.md spec-first policy)    *)
(*                                                                         *)
(*   poll.cfg                            all buggy flags FALSE,            *)
(*                                        HAS_TIMEOUT TRUE — every safety  *)
(*                                        invariant holds.                  *)
(*   poll_notimeout.cfg                  HAS_TIMEOUT FALSE — poll(-1), the *)
(*                                        infinite wait; safety holds.     *)
(*   poll_liveness.cfg                   Spec_Live, HAS_TIMEOUT TRUE —     *)
(*                                        PollTerminates (the timeout      *)
(*                                        backstop) + PollReturnsWhenReady.*)
(*   poll_buggy_check_before_register.cfg BUGGY_CHECK_BEFORE_REGISTER —    *)
(*                                        NoMissedPoll counterexample.     *)
(*   poll_buggy_no_wake.cfg              BUGGY_NO_WAKE — NoMissedPoll      *)
(*                                        counterexample.                  *)
(*   poll_buggy_lazy_unregister.cfg      BUGGY_LAZY_UNREGISTER —          *)
(*                                        NoStaleHook counterexample.      *)
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
(*   `ready` is monotonic FALSE -> TRUE: a readiness edge, not a level     *)
(*   that can retract. The events poll waits on (POLLIN — bytes buffered;  *)
(*   POLLHUP — peer closed) latch; a consumer draining the bytes is a      *)
(*   separate concern outside one poll call.                               *)
(*                                                                         *)
(*   The flag-vs-timeout precedence on resume (success has precedence — a  *)
(*   fd readied exactly at the deadline reports ready, not timeout) is     *)
(*   tsleep.tla's `TimeoutSound` proof; poll's `CommitOrSleep` is a        *)
(*   `tsleep` call and checks the flag before the deadline. Not re-pinned. *)
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
    BUGGY_LAZY_UNREGISTER         \* BOOLEAN — TRUE: poll returns without
                                  \*   unregistering its poll_waiter hooks.

ASSUME Fds # {}
ASSUME HAS_TIMEOUT                 \in BOOLEAN
ASSUME BUGGY_CHECK_BEFORE_REGISTER \in BOOLEAN
ASSUME BUGGY_NO_WAKE               \in BOOLEAN
ASSUME BUGGY_LAZY_UNREGISTER       \in BOOLEAN

VARIABLES
    pc,               \* the poll call's lifecycle ∈ PCs (see below).
    ready,            \* [Fds -> BOOLEAN] — the kernel-side truth of each
                      \*   fd's readiness (bytes buffered / EOF latched).
    registered,       \* [Fds -> BOOLEAN] — the poller's poll_waiter hook is
                      \*   installed on fd f's hook list.
    flagged,          \* [Fds -> BOOLEAN] — fd f's poll_waiter (this
                      \*   poller's) has its `ready` flag set: the readiness
                      \*   the poller observes under its own Rendez lock.
    deadline_passed   \* BOOLEAN — monotonic time reached the poll timeout.

vars == <<pc, ready, registered, flagged, deadline_passed>>

\* "start"        — poll() entered; no hook installed, nothing sampled.
\* "checked"      — BUGGY path only: readiness sampled, no hook installed.
\* "registered"   — hooks installed; the evaluate point (first entry and
\*                  every resume from a wake re-evaluate here).
\* "sleeping"     — committed to sleep: tsleep on the poller's Rendez.
\* "done_ready"   — poll returned >= 1 ready fd.
\* "done_timeout" — poll returned 0 (the timeout elapsed, nothing ready).
PCs      == {"start", "checked", "registered", "sleeping",
             "done_ready", "done_timeout"}
Terminal == {"done_ready", "done_timeout"}

TypeOk ==
    /\ pc              \in PCs
    /\ ready           \in [Fds -> BOOLEAN]
    /\ registered      \in [Fds -> BOOLEAN]
    /\ flagged         \in [Fds -> BOOLEAN]
    /\ deadline_passed \in BOOLEAN

(***************************************************************************)
(* The poller has just called poll; no fd is ready, no hook is installed,  *)
(* no flag is set, time has not reached the deadline.                       *)
(***************************************************************************)
Init ==
    /\ pc              = "start"
    /\ ready           = [f \in Fds |-> FALSE]
    /\ registered      = [f \in Fds |-> FALSE]
    /\ flagged         = [f \in Fds |-> FALSE]
    /\ deadline_passed = FALSE

(***************************************************************************)
(* Expired — the deadline-reached predicate the resume path evaluates.     *)
(* FALSE whenever the modeled poll has no timeout (poll(-1)).               *)
(***************************************************************************)
Expired == HAS_TIMEOUT /\ deadline_passed

(***************************************************************************)
(* MakeReady — a producer makes fd f ready (bytes arrive, peer closes).    *)
(* Monotonic: readiness is an edge, fired once. Gated to the pre-terminal  *)
(* space — once poll has returned no one observes the fds.                  *)
(*                                                                         *)
(* If the poller's hook is installed on f, the producer's hook-list walk   *)
(* sets that poll_waiter's flag AND — the CORRECT path — wakes a sleeping  *)
(* poller (transitions it back to the evaluate point). BUGGY_NO_WAKE sets  *)
(* the flag but skips the wake, so a sleeping poller never re-evaluates.   *)
(* A producer on an UNregistered fd reaches no hook: it sets `ready` only. *)
(***************************************************************************)
MakeReady(f) ==
    /\ pc \notin Terminal
    /\ ~ready[f]
    /\ ready' = [ready EXCEPT ![f] = TRUE]
    /\ flagged' = IF registered[f]
                  THEN [flagged EXCEPT ![f] = TRUE]
                  ELSE flagged
    /\ pc' = IF pc = "sleeping" /\ registered[f] /\ ~BUGGY_NO_WAKE
             THEN "registered"
             ELSE pc
    /\ UNCHANGED <<registered, deadline_passed>>

(***************************************************************************)
(* AdvanceTime — the monotonic counter reaches the poll timeout.           *)
(* Write-once; meaningful only for a poll that carries a finite timeout.   *)
(***************************************************************************)
AdvanceTime ==
    /\ HAS_TIMEOUT
    /\ ~deadline_passed
    /\ pc \notin Terminal
    /\ deadline_passed' = TRUE
    /\ UNCHANGED <<pc, ready, registered, flagged>>

(***************************************************************************)
(* Register — the CORRECT entry. For every fd, `dev->poll` installs the    *)
(* poll_waiter hook AND returns the fd's current readiness, in one step    *)
(* under fd f's object lock. The readiness sampled at install becomes the  *)
(* initial flag set. This is register-then-observe: no readiness event can *)
(* slip between the sample and the hook being live.                         *)
(***************************************************************************)
Register ==
    /\ ~BUGGY_CHECK_BEFORE_REGISTER
    /\ pc = "start"
    /\ pc'         = "registered"
    /\ registered' = [f \in Fds |-> TRUE]
    /\ flagged'    = [f \in Fds |-> ready[f]]
    /\ UNCHANGED <<ready, deadline_passed>>

(***************************************************************************)
(* BuggyCheck — the BUGGY path's first half: the poller samples each fd's  *)
(* readiness into its flags but installs NO hook. A readiness event after  *)
(* this point reaches no hook list.                                         *)
(***************************************************************************)
BuggyCheck ==
    /\ BUGGY_CHECK_BEFORE_REGISTER
    /\ pc = "start"
    /\ pc'      = "checked"
    /\ flagged' = [f \in Fds |-> ready[f]]
    /\ UNCHANGED <<ready, registered, deadline_passed>>

(***************************************************************************)
(* BuggyRegisterLate — the BUGGY path's second half: the hooks are         *)
(* installed now, AFTER the readiness was sampled, and `flagged` is NOT    *)
(* re-sampled. Any readiness event between BuggyCheck and here was missed: *)
(* it set `ready` but reached no hook, and is not re-captured.              *)
(***************************************************************************)
BuggyRegisterLate ==
    /\ BUGGY_CHECK_BEFORE_REGISTER
    /\ pc = "checked"
    /\ pc'         = "registered"
    /\ registered' = [f \in Fds |-> TRUE]
    /\ UNCHANGED <<ready, flagged, deadline_passed>>

(***************************************************************************)
(* CommitOrSleep — the poller, at the evaluate point, decides. This is     *)
(* both the first evaluation (after Register / BuggyRegisterLate) and      *)
(* every resume from a wake; the impl re-checks identically at both. It    *)
(* is a `tsleep` call: the flag scan and the sleep transition are atomic   *)
(* under the poller's Rendez lock.                                          *)
(*                                                                         *)
(*   any flag set -> return AWOKEN: poll returns the ready fds.            *)
(*   else Expired -> return TIMEDOUT: poll returns 0.                       *)
(*   else         -> sleep on the poller's private Rendez.                  *)
(*                                                                         *)
(* The flag is checked before the deadline (success has precedence). On a  *)
(* return the poll_waiter hooks are unregistered; BUGGY_LAZY_UNREGISTER    *)
(* leaves them installed.                                                   *)
(***************************************************************************)
CommitOrSleep ==
    /\ pc = "registered"
    /\ UNCHANGED <<ready, flagged, deadline_passed>>
    /\ LET anyFlagged == \E f \in Fds : flagged[f]
           cleared    == [f \in Fds |-> FALSE]
           unhook     == IF BUGGY_LAZY_UNREGISTER THEN registered ELSE cleared
       IN  IF anyFlagged
           THEN /\ pc'         = "done_ready"
                /\ registered' = unhook
           ELSE IF Expired
           THEN /\ pc'         = "done_timeout"
                /\ registered' = unhook
           ELSE /\ pc'         = "sleeping"
                /\ registered' = registered

(***************************************************************************)
(* Timeout — the `tsleep` deadline fires off the sched_tick scan and wakes *)
(* the sleeping poller back to the evaluate point. CommitOrSleep then sees *)
(* Expired and returns TIMEDOUT — unless a flag was set meanwhile, in      *)
(* which case the flag check (first) returns AWOKEN. The tsleep timeout    *)
(* mechanics themselves are tsleep.tla's proof.                             *)
(***************************************************************************)
Timeout ==
    /\ pc = "sleeping"
    /\ deadline_passed
    /\ pc' = "registered"
    /\ UNCHANGED <<ready, registered, flagged, deadline_passed>>

(***************************************************************************)
(* Done — terminal self-loop. Once poll has returned the model halts; the  *)
(* explicit stutter keeps a legitimate terminal state from tripping TLC's  *)
(* deadlock check.                                                          *)
(***************************************************************************)
Done == pc \in Terminal /\ UNCHANGED vars

Next ==
    \/ Register
    \/ BuggyCheck
    \/ BuggyRegisterLate
    \/ CommitOrSleep
    \/ \E f \in Fds : MakeReady(f)
    \/ AdvanceTime
    \/ Timeout
    \/ Done

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

\* FlagImpliesReady — a poll_waiter flag is set only for a genuinely ready
\* fd. poll never reports a revents bit for an fd that was not ready (no
\* spurious wakeup return).
FlagImpliesReady == \A f \in Fds : flagged[f] => ready[f]

\* FlagTracksReady — once the hook is installed, a ready fd has its flag
\* set. This is the register-then-observe discipline: the mechanism behind
\* NoMissedPoll. Violated by BUGGY_CHECK_BEFORE_REGISTER, which installs
\* the hook without (re-)sampling readiness.
FlagTracksReady ==
    \A f \in Fds : (registered[f] /\ ready[f]) => flagged[f]

\* NoMissedPoll — ARCH §28 I-9 across N fds: a poller is never left asleep
\* while a registered fd is ready. The headline property. Violated by
\* BUGGY_CHECK_BEFORE_REGISTER (stale sample) and BUGGY_NO_WAKE (the
\* readiness event never wakes the sleeper).
NoMissedPoll ==
    ~(pc = "sleeping" /\ \E f \in Fds : ready[f] /\ registered[f])

\* NoStaleHook — a returned poll holds no poll_waiter hook. The hooks are
\* stack-allocated for the call; a leftover one is a dangling pointer a
\* later readiness event would walk. Violated by BUGGY_LAZY_UNREGISTER.
NoStaleHook ==
    (pc \in Terminal) => (\A f \in Fds : ~registered[f])

\* ReadyResultSound — poll returns "ready" only with a flag actually set.
ReadyResultSound == (pc = "done_ready") => (\E f \in Fds : flagged[f])

\* TimeoutResultSound — poll returns "timeout" (0 fds) only when no flag is
\* set. Success has precedence: a fd readied as the deadline lapses returns
\* ready, not timeout (the flag is checked before the deadline).
TimeoutResultSound == (pc = "done_timeout") => (\A f \in Fds : ~flagged[f])

Invariants ==
    /\ TypeOk
    /\ FlagImpliesReady
    /\ FlagTracksReady
    /\ NoMissedPoll
    /\ NoStaleHook
    /\ ReadyResultSound
    /\ TimeoutResultSound

(***************************************************************************)
(* ============================== LIVENESS ================================ *)
(*                                                                         *)
(* PollTerminates — poll always eventually returns. Under HAS_TIMEOUT the  *)
(* `tsleep` deadline is the backstop: it holds against a producer that     *)
(* never makes any fd ready (fairness grants the producer NOTHING —        *)
(* neither MakeReady is fair). AdvanceTime -> Timeout -> CommitOrSleep     *)
(* drives the poller to done_timeout. (poll(-1), HAS_TIMEOUT FALSE, may    *)
(* legitimately block forever — it is not checked for termination.)        *)
(*                                                                         *)
(* PollReturnsWhenReady — once any registered fd is flagged ready, poll    *)
(* eventually returns. A flag is set only together with the wake (the      *)
(* CORRECT MakeReady), so a flagged fd implies the poller is at the        *)
(* evaluate point or already returned — never left asleep.                 *)
(***************************************************************************)
PollTerminates == <>(pc \in Terminal)

PollReturnsWhenReady == (\E f \in Fds : flagged[f]) ~> (pc \in Terminal)

Liveness ==
    /\ WF_vars(Register)
    /\ WF_vars(CommitOrSleep)
    /\ WF_vars(AdvanceTime)
    /\ WF_vars(Timeout)

Spec_Live == Init /\ [][Next]_vars /\ Liveness

====
