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
(*   Since round 4 of that audit the re-arm is a full RE-REGISTRATION:     *)
(*   every pass takes every hook off its list (which clears it), then      *)
(*   calls each fd's `dev->poll` WITH the hook again -- the same atomic    *)
(*   install-and-sample as the first scan. Hooks that persisted across the *)
(*   loop were sound for an object with one list, and wrong for a Dev that *)
(*   CHOOSES its list by state: the console files a frozen poller on the   *)
(*   episode list, and a hook left there after the episode ended never saw *)
(*   another keystroke. That half is pinned where the choosing Dev is      *)
(*   modeled (cons_poll.tla BUGGY_NO_REREGISTER); here the re-registration *)
(*   is the as-built re-arm the other properties are checked against.      *)
(*                                                                         *)
(*   And the loop now owns DEATH and STOP. tsleep's own die-check and stop *)
(*   detour sit behind its cond test, so a flag set in every re-sample     *)
(*   window -- a producer that keeps walking a list -- makes each tsleep   *)
(*   return AWOKEN without reaching either: poll(-1) became unkillable and *)
(*   unstoppable. Each pass therefore checks both itself, with its hooks   *)
(*   off (DeathTerminates, StopHonoured).                                   *)
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
(*   BUGGY_CLEAR_AFTER_SAMPLE — on a wake the poller re-registers and     *)
(*     re-samples, THEN clears its flags. An event landing between the two *)
(*     is recorded only in a flag the clear then wipes; the next tsleep    *)
(*     finds no flag and sleeps on a ready fd (NoMissedPoll                *)
(*     counterexample). The fix: a hook comes off its list clear and goes  *)
(*     back on clear -- clear first, sample second.                        *)
(*                                                                         *)
(*   BUGGY_RETURN_ON_WAKE — the pre-2026-09-21 sys_poll_for_proc: a wake   *)
(*     whose re-sample finds nothing returns 0. poll(fd, 10 s) reports a   *)
(*     timeout after microseconds because a competing reader won the       *)
(*     bytes, or because the list was walked for an event this poller did  *)
(*     not ask about; poll(-1) returns 0, which POSIX never permits        *)
(*     (NoSpuriousZero counterexample). The fix: sleep again.               *)
(*                                                                         *)
(*   BUGGY_NO_LOOP_DIE_CHECK — the round-4 F2 loop: the re-arm relies on   *)
(*     tsleep's die-check alone. A producer that walks a list inside every *)
(*     re-sample window keeps a flag set at every tsleep entry, tsleep     *)
(*     returns AWOKEN before its die-check, and a dying poll(-1) circles   *)
(*     forever (DeathTerminates counterexample). The fix: the loop checks  *)
(*     thread_die_pending itself on every pass.                             *)
(*                                                                         *)
(*   BUGGY_NO_LOOP_STOP_CHECK — the same shape for a debugger / job stop:  *)
(*     tsleep's stop detour sits behind the same cond test, so the stop is *)
(*     never honoured (StopHonoured counterexample). The fix: the loop     *)
(*     parks on proc_stop_sleeper_park itself when a stop is pending.       *)
(*                                                                         *)
(*   BUGGY_NO_BACKSTOP — the round-5 F1 loop: nothing bounds how long a    *)
(*     poller goes without really sleeping. Syscalls run IRQ-masked, and a *)
(*     producer that walks a list inside every re-sample window keeps      *)
(*     every tsleep returning AWOKEN, so a poll(-1) holds its CPU IRQ-     *)
(*     masked for as long as the noise lasts: every device interrupt, the  *)
(*     SAK included, stays pending. Any unprivileged program can make that *)
(*     noise -- a pipe, a writer, a reader, and a poller asking for        *)
(*     nothing. The fix: when a spin budget lapses with no real sleep in   *)
(*     it, the loop unhooks every fd and sleeps a short bounded interval   *)
(*     no producer can cut short, then re-registers (SpinBounded           *)
(*     counterexample).                                                    *)
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
(*                                        StableReadyReturns +             *)
(*                                        DeathTerminates + StopHonoured + *)
(*                                        SpinBounded.                     *)
(*   poll_liveness_notimeout.cfg         Spec_Live, HAS_TIMEOUT FALSE —    *)
(*                                        StableReadyReturns: a poll(-1)   *)
(*                                        may block forever, but not on an *)
(*                                        fd that stays ready -- nor once  *)
(*                                        its Proc is dying (DeathTermin-  *)
(*                                        ates) or stopped (StopHonoured); *)
(*                                        and noise never keeps it from    *)
(*                                        sleeping (SpinBounded).          *)
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
(*   poll_buggy_no_loop_die_check.cfg    BUGGY_NO_LOOP_DIE_CHECK +         *)
(*                                       BUGGY_NO_BACKSTOP, poll(-1)       *)
(*                                       — DeathTerminates counterexample. *)
(*   poll_buggy_no_loop_stop_check.cfg   BUGGY_NO_LOOP_STOP_CHECK +        *)
(*                                       BUGGY_NO_BACKSTOP, poll(-1)       *)
(*                                       — StopHonoured counterexample.    *)
(*   poll_buggy_no_backstop.cfg          BUGGY_NO_BACKSTOP, poll(-1)       *)
(*                                       — SpinBounded counterexample.     *)
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
(*   DEATH and STOP are modeled since round 4 (`dying`, `stop_req`). The   *)
(*   tsleep call keeps its real order -- cond, then deadline, then the     *)
(*   stop detour, then the die-check -- because that order is the bug: a  *)
(*   set flag short-circuits the two checks behind it. Die and a stop      *)
(*   request wake a sleeping poller (the #811 death cascade; the stop      *)
(*   delivery's sleeper wake); a stop park ends on resume or, death        *)
(*   winning, on death. A resume is modeled only at a settled park (the    *)
(*   debugger continues a STOPPED target), with fairness: without it       *)
(*   StopHonoured would hold vacuously and PollTerminates would fail for a *)
(*   stop nobody ever lifts. ONE stop per behavior: a debugger that        *)
(*   re-stops forever can hold even a dying thread in tsleep's detour,     *)
(*   whose stop test precedes its die-check -- a race the debugger must    *)
(*   win every time, owned by debug_stop.tla (DeathWinsOverStop), not by   *)
(*   the poll loop. The loop's sched_yield_hint on a noise pass is a     *)
(*   stutter here -- it moves the CPU, not the poll's state. A pass that   *)
(*   follows the flag-sensitive tsleep's TIMEDOUT runs the same            *)
(*   unhook/check/re-register in the code; the model folds it into         *)
(*   FinalSample. Every exit from it is terminal except one: its stop      *)
(*   check can PARK first, and resume into the same terminal exits. That   *)
(*   park is LoopCheck's, already checked on the other passes, so the fold *)
(*   loses a stutter-equivalent detour, not a behavior. (The backoff's     *)
(*   TIMEDOUT is not folded: BackoffTimeout loops back through LoopCheck.) *)
(*                                                                         *)
(*   THE SPIN BUDGET (round 5). `spun` says the budget has lapsed since    *)
(*   the poller last really slept. SpinLapse raises it, fairly, whenever   *)
(*   the poller is awake -- time passes -- and entering any real sleep     *)
(*   (the tsleep, a park, the backoff) lowers it: the code's comparison of *)
(*   the thread's nsleeps counter, which a park moves too (it sleeps on    *)
(*   debug_rendez). The code tests the budget only on a noise pass, as     *)
(*   EvaluateWake does. The backoff's own short deadline is not modeled as *)
(*   lapsing before its tsleep commits: it is computed one pass earlier,   *)
(*   and a backoff that returned without sleeping would only leave `spun`  *)
(*   up and back off again. The loop's die-check and stop park are no      *)
(*   longer the ONLY way out of a noise loop -- the backoff's tsleep has   *)
(*   its own -- so their buggy cfgs turn the backstop off to isolate them. *)
(*   In the code they keep death and a stop prompt (one pass), where the   *)
(*   backstop alone would take a budget; the kernel tests pin that with    *)
(*   the backoff counter.                                                  *)
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
    BUGGY_RETURN_ON_WAKE,         \* BOOLEAN — TRUE: a wake whose re-sample
                                  \*   finds nothing ready RETURNS 0 instead
                                  \*   of sleeping again (the pre-2026-09-21
                                  \*   sys_poll_for_proc).
    BUGGY_NO_LOOP_DIE_CHECK,      \* BOOLEAN — TRUE: the re-arm loop leaves
                                  \*   death to tsleep's die-check alone.
    BUGGY_NO_LOOP_STOP_CHECK,     \* BOOLEAN — TRUE: the re-arm loop leaves
                                  \*   a stop to tsleep's detour alone.
    BUGGY_NO_BACKSTOP             \* BOOLEAN — TRUE: no spin budget; a noise
                                  \*   pass always goes straight back to the
                                  \*   flag-sensitive tsleep.

ASSUME Fds # {}
ASSUME HAS_TIMEOUT                 \in BOOLEAN
ASSUME BUGGY_CHECK_BEFORE_REGISTER \in BOOLEAN
ASSUME BUGGY_NO_WAKE               \in BOOLEAN
ASSUME BUGGY_LAZY_UNREGISTER       \in BOOLEAN
ASSUME BUGGY_CLEAR_AFTER_SAMPLE    \in BOOLEAN
ASSUME BUGGY_RETURN_ON_WAKE        \in BOOLEAN
ASSUME BUGGY_NO_LOOP_DIE_CHECK     \in BOOLEAN
ASSUME BUGGY_NO_LOOP_STOP_CHECK    \in BOOLEAN
ASSUME BUGGY_NO_BACKSTOP           \in BOOLEAN

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
    deadline_passed,  \* BOOLEAN — monotonic time reached the poll timeout.
    dying,            \* BOOLEAN — the poller's Proc is group-terminating
                      \*   (thread_die_pending). Monotonic.
    stop_req,         \* BOOLEAN — a debugger or job-control stop is pending
                      \*   (proc_stop_requested).
    stop_used,        \* BOOLEAN — the one stop request of a behavior has been
                      \*   made (see MODELING ASSUMPTIONS).
    spun              \* BOOLEAN — the spin budget has lapsed since the poller
                      \*   last really slept (see MODELING ASSUMPTIONS).

vars == <<pc, ready, registered, flagged, seen, deadline_passed, dying, stop_req,
          stop_used, spun>>

\* "start"         — poll() entered; no hook installed, nothing sampled.
\* "checked"       — BUGGY path only: readiness sampled, no hook installed.
\* "scanned"       — the first scan is done (hooks installed + sampled).
\* "armed"         — about to call tsleep: the commit point.
\* "sleeping"      — committed to sleep on the poller's private Rendez.
\* "tsparked"      — tsleep's own stop detour: parked, hooks STILL listed.
\* "woken"         — tsleep returned AWOKEN (some flag was set).
\* "unhooked"      — every hook is off its list; the loop's own death and
\*                   stop checks are next.
\* "loopparked"    — the loop's stop park: parked with NO hook listed.
\* "cleared"       — the checks passed; the re-register + re-sample is next.
\* "sampled_dirty" — BUGGY_CLEAR_AFTER_SAMPLE only: re-registered and
\*                   re-sampled, flags not yet cleared.
\* "rescanned"     — the post-wake re-sample is done; evaluate it.
\* "timedout"      — tsleep returned TIMEDOUT; the final sample is pending.
\* "final"         — the final sample is done; evaluate it.
\* "done_ready"    — poll returned >= 1 ready fd.
\* "done_timeout"  — poll returned 0.
\* "backoff"       — the backstop: the budget lapsed on a noise pass; every
\*                   hook is off its list, the bounded never-true tsleep is
\*                   next.
\* "backingoff"    — asleep in the backoff. No hook is listed, so no producer
\*                   can wake it: only its deadline, death, or a stop.
\* "bparked"       — the backoff tsleep's own stop detour (no hook listed).
\* "done_intr"     — poll unwound for death (the result is immaterial: the
\*                   thread dies at its EL0-return tail).
PCs      == {"start", "checked", "scanned", "armed", "sleeping", "tsparked",
             "woken", "unhooked", "loopparked", "cleared", "sampled_dirty",
             "rescanned", "timedout", "final", "backoff", "backingoff",
             "bparked", "done_ready", "done_timeout", "done_intr"}
Terminal == {"done_ready", "done_timeout", "done_intr"}
Parked   == {"tsparked", "loopparked", "bparked"}
\* Every state in which the poller has given up its CPU (the code's nsleeps
\* moves): the flag-sensitive tsleep, the backoff, and all three parks.
RealSleep == {"sleeping", "backingoff"} \cup Parked

TypeOk ==
    /\ pc              \in PCs
    /\ ready           \in [Fds -> BOOLEAN]
    /\ registered      \in [Fds -> BOOLEAN]
    /\ flagged         \in [Fds -> BOOLEAN]
    /\ seen            \in [Fds -> BOOLEAN]
    /\ deadline_passed \in BOOLEAN
    /\ dying           \in BOOLEAN
    /\ stop_req        \in BOOLEAN
    /\ stop_used       \in BOOLEAN
    /\ spun            \in BOOLEAN

NoneSet == [f \in Fds |-> FALSE]
AllSet  == [f \in Fds |-> TRUE]

Init ==
    /\ pc              = "start"
    /\ ready           = NoneSet
    /\ registered      = NoneSet
    /\ flagged         = NoneSet
    /\ seen            = NoneSet
    /\ deadline_passed = FALSE
    /\ dying           = FALSE
    /\ stop_req        = FALSE
    /\ stop_used       = FALSE
    /\ spun            = FALSE

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
    /\ UNCHANGED <<registered, seen, deadline_passed, dying, stop_req, stop_used, spun>>

(***************************************************************************)
(* Retract — readiness falls again before the poller looks: a competing    *)
(* reader drained the bytes. No hook-list walk (nothing became MORE        *)
(* ready); a flag an earlier MakeReady set stays set, now stale.            *)
(***************************************************************************)
Retract(f) ==
    /\ pc \notin Terminal
    /\ ready[f]
    /\ ready' = [ready EXCEPT ![f] = FALSE]
    /\ UNCHANGED <<pc, registered, flagged, seen, deadline_passed, dying, stop_req, stop_used, spun>>

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
    /\ UNCHANGED <<ready, registered, seen, deadline_passed, dying, stop_req, stop_used, spun>>

(***************************************************************************)
(* AdvanceTime — the monotonic counter reaches the poll timeout.           *)
(***************************************************************************)
AdvanceTime ==
    /\ HAS_TIMEOUT
    /\ ~deadline_passed
    /\ pc \notin Terminal
    /\ deadline_passed' = TRUE
    /\ UNCHANGED <<pc, ready, registered, flagged, seen, dying, stop_req, stop_used, spun>>

(***************************************************************************)
(* Die — the Proc starts group-terminating. The #811 death cascade wakes a *)
(* sleeping poller: tsleep re-loops, and re-checks cond BEFORE its         *)
(* die-check (TSleepCommit). A parked poller wakes through ParkDeath.       *)
(***************************************************************************)
Die ==
    /\ pc \notin Terminal
    /\ ~dying
    /\ dying' = TRUE
    /\ pc' = IF pc = "sleeping" THEN "armed"
             ELSE IF pc = "backingoff" THEN "backoff" ELSE pc
    /\ UNCHANGED <<ready, registered, flagged, seen, deadline_passed, stop_req, stop_used, spun>>

(***************************************************************************)
(* StopRequest — a debugger `stop` or a job-control suspend. The delivery  *)
(* wakes every sleeping thread of the Proc so it can re-observe the flag   *)
(* and park (proc_stop_wake_sleepers_locked).                               *)
(***************************************************************************)
StopRequest ==
    /\ pc \notin Terminal
    /\ ~stop_used
    /\ stop_req'  = TRUE
    /\ stop_used' = TRUE
    /\ pc' = IF pc = "sleeping" THEN "armed"
             ELSE IF pc = "backingoff" THEN "backoff" ELSE pc
    /\ UNCHANGED <<ready, registered, flagged, seen, deadline_passed, dying, spun>>

(***************************************************************************)
(* StopResume — the stop is lifted and the parked poller resumes. From     *)
(* tsleep's detour it re-loops tsleep (`continue`), the backoff's tsleep   *)
(* included; from the loop's own park it goes on to re-register. Modeled  *)
(* only at a settled park -- see                                           *)
(* MODELING ASSUMPTIONS.                                                    *)
(***************************************************************************)
StopResume ==
    /\ stop_req
    /\ pc \in Parked
    /\ stop_req' = FALSE
    /\ pc' = CASE pc = "tsparked"   -> "armed"
                [] pc = "loopparked" -> "cleared"
                [] pc = "bparked"    -> "backoff"
    /\ UNCHANGED <<ready, registered, flagged, seen, deadline_passed, dying, stop_used, spun>>

(***************************************************************************)
(* ParkDeath — DEATH WINS over a stop: proc_stop_sleeper_park returns      *)
(* SLEEP_INTR, the caller unwinds to the sweep.                             *)
(***************************************************************************)
ParkDeath ==
    /\ pc \in Parked
    /\ dying
    /\ pc'         = "done_intr"
    /\ registered' = Unhook
    /\ UNCHANGED <<ready, flagged, seen, deadline_passed, dying, stop_req, stop_used, spun>>

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
    /\ UNCHANGED <<ready, flagged, deadline_passed, dying, stop_req, stop_used, spun>>

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
    /\ UNCHANGED <<ready, registered, flagged, deadline_passed, dying, stop_req, stop_used, spun>>

BuggyRegisterLate ==
    /\ BUGGY_CHECK_BEFORE_REGISTER
    /\ pc = "checked"
    /\ pc'         = "scanned"
    /\ registered' = AllSet
    /\ UNCHANGED <<ready, flagged, seen, deadline_passed, dying, stop_req, stop_used, spun>>

(***************************************************************************)
(* EvaluateFirst — the first scan's verdict. Anything seen ready returns;  *)
(* otherwise the poller goes to its tsleep. (timeout_ms == 0 is the run in *)
(* which the deadline has already passed: TSleepCommit sees it.)            *)
(***************************************************************************)
EvaluateFirst ==
    /\ pc = "scanned"
    /\ UNCHANGED <<ready, flagged, seen, deadline_passed, dying, stop_req, stop_used, spun>>
    /\ IF \E f \in Fds : seen[f]
       THEN /\ pc' = "done_ready"
            /\ registered' = Unhook
       ELSE /\ pc' = "armed"
            /\ registered' = registered

(***************************************************************************)
(* TSleepCommit — the `tsleep` call, in the code's order: the flag scan    *)
(* (success has precedence -- tsleep.tla TimeoutSound), then the deadline, *)
(* then the 8c-2 stop detour, then the #811 die-check, then the sleep, all *)
(* atomic under the poller's locks. A set flag short-circuits BOTH checks  *)
(* behind it -- which is why the loop must make them itself.                *)
(***************************************************************************)
TSleepCommit ==
    /\ pc = "armed"
    /\ UNCHANGED <<ready, flagged, seen, deadline_passed, dying, stop_req, stop_used>>
    /\ IF \E f \in Fds : flagged[f] THEN /\ pc' = "woken"
                                         /\ registered' = registered
       ELSE IF Expired                THEN /\ pc' = "timedout"
                                         /\ registered' = registered
       ELSE IF stop_req               THEN /\ pc' = "tsparked"
                                         /\ registered' = registered
       ELSE IF dying                  THEN /\ pc' = "done_intr"
                                         /\ registered' = Unhook
       ELSE                                /\ pc' = "sleeping"
                                         /\ registered' = registered
    /\ spun' = IF pc' \in RealSleep THEN FALSE ELSE spun

(***************************************************************************)
(* Timeout — the tsleep deadline fires and wakes the sleeping poller.      *)
(***************************************************************************)
Timeout ==
    /\ pc = "sleeping"
    /\ deadline_passed
    /\ pc' = "timedout"
    /\ UNCHANGED <<ready, registered, flagged, seen, deadline_passed, dying, stop_req, stop_used, spun>>

(***************************************************************************)
(* Rearm -- every hook comes off its list. Off the list no producer can    *)
(* reach it, so it is cleared there: a hook goes back on clear. The        *)
(* BUGGY_CLEAR_AFTER_SAMPLE variant carries the stale flags forward and    *)
(* clears them after the re-sample instead (BuggyClearLate).                *)
(***************************************************************************)
Rearm ==
    /\ pc = "woken"
    /\ pc'         = "unhooked"
    /\ registered' = NoneSet
    /\ flagged'    = IF BUGGY_CLEAR_AFTER_SAMPLE THEN flagged ELSE NoneSet
    /\ UNCHANGED <<ready, seen, deadline_passed, dying, stop_req, stop_used, spun>>

(***************************************************************************)
(* LoopCheck -- the loop's own death and stop checks, with no hook listed. *)
(* Death unwinds to the sweep; a stop parks on proc_stop_sleeper_park.      *)
(***************************************************************************)
LoopCheck ==
    /\ pc = "unhooked"
    /\ pc' = IF dying /\ ~BUGGY_NO_LOOP_DIE_CHECK THEN "done_intr"
             ELSE IF stop_req /\ ~BUGGY_NO_LOOP_STOP_CHECK THEN "loopparked"
             ELSE "cleared"
    /\ spun' = IF pc' \in RealSleep THEN FALSE ELSE spun
    /\ UNCHANGED <<ready, registered, flagged, seen, deadline_passed, dying, stop_req, stop_used>>

(***************************************************************************)
(* Resample -- each fd's `dev->poll` WITH the hook: the first scan's       *)
(* atomic install-and-sample again, so the Dev re-chooses the list and the *)
(* fd re-resolves. An event before an fd's install is seen by its sample;  *)
(* one after reaches the fresh hook.                                        *)
(***************************************************************************)
Resample ==
    /\ pc = "cleared"
    /\ pc'         = IF BUGGY_CLEAR_AFTER_SAMPLE THEN "sampled_dirty" ELSE "rescanned"
    /\ registered' = AllSet
    /\ seen'       = ready
    /\ UNCHANGED <<ready, flagged, deadline_passed, dying, stop_req, stop_used, spun>>

\* The BUGGY order's second half: the flags are cleared after the sample.
BuggyClearLate ==
    /\ pc = "sampled_dirty"
    /\ pc'      = "rescanned"
    /\ flagged' = NoneSet
    /\ UNCHANGED <<ready, registered, seen, deadline_passed, dying, stop_req, stop_used, spun>>

(***************************************************************************)
(* EvaluateWake — the verdict after a wake. Ready returns. NOT ready is    *)
(* the case the pre-2026-09-21 code got wrong: it returned 0, so a poll    *)
(* with seconds left reported a timeout the moment anything at all         *)
(* happened on one of its objects, and poll(-1) returned 0, which POSIX    *)
(* never permits. The poller sleeps AGAIN, against the SAME deadline. The  *)
(* explicit Expired test bounds the loop: tsleep prefers a set flag to a   *)
(* passed deadline, so a producer that keeps walking the list would        *)
(* otherwise keep the poller circling past its timeout. And once the spin *)
(* budget has lapsed the next sleep is the backoff, which no producer can *)
(* shorten: the bound a poll(-1) has no deadline to supply (SpinBounded). *)
(***************************************************************************)
EvaluateWake ==
    /\ pc = "rescanned"
    /\ UNCHANGED <<ready, seen, deadline_passed, dying, stop_req, stop_used, spun>>
    /\ IF \E f \in Fds : seen[f]
       THEN /\ pc' = "done_ready"
            /\ registered' = Unhook
            /\ flagged' = flagged
       ELSE IF Expired \/ BUGGY_RETURN_ON_WAKE
       THEN /\ pc' = "done_timeout"
            /\ registered' = Unhook
            /\ flagged' = flagged
       ELSE IF spun /\ ~BUGGY_NO_BACKSTOP
       THEN /\ pc' = "backoff"
            /\ registered' = NoneSet
            /\ flagged' = NoneSet
       ELSE /\ pc' = "armed"
            /\ registered' = registered
            /\ flagged' = flagged

(***************************************************************************)
(* BackoffCommit — the backstop's tsleep: a never-true cond and a deadline *)
(* one budget out, capped at the poll's own, in tsleep's order: the        *)
(* deadline, the stop detour, the die-check, the sleep. With no hook       *)
(* listed nothing a producer does can end it. A poll deadline already past *)
(* times it out at once, into the loop's checks and the re-register.       *)
(***************************************************************************)
BackoffCommit ==
    /\ pc = "backoff"
    /\ pc' = IF Expired  THEN "unhooked"
             ELSE IF stop_req THEN "bparked"
             ELSE IF dying    THEN "done_intr"
             ELSE "backingoff"
    /\ spun' = IF pc' \in RealSleep THEN FALSE ELSE spun
    /\ UNCHANGED <<ready, registered, flagged, seen, deadline_passed, dying, stop_req, stop_used>>

\* BackoffTimeout — the backoff's deadline fires: tsleep returns TIMEDOUT, and
\* the loop runs its checks and re-registers as after any other pass.
BackoffTimeout ==
    /\ pc = "backingoff"
    /\ pc' = "unhooked"
    /\ UNCHANGED <<ready, registered, flagged, seen, deadline_passed, dying, stop_req, stop_used, spun>>

(***************************************************************************)
(* SpinLapse — time passes while the poller is awake, and the spin budget  *)
(* since its last real sleep runs out.                                      *)
(***************************************************************************)
SpinLapse ==
    /\ pc \notin Terminal \cup RealSleep
    /\ ~spun
    /\ spun' = TRUE
    /\ UNCHANGED <<pc, ready, registered, flagged, seen, deadline_passed, dying, stop_req, stop_used>>

(***************************************************************************)
(* FinalSample / EvaluateFinal — tsleep returned TIMEDOUT. One last sample *)
(* (success has precedence: an fd readied as the deadline lapses reports   *)
(* ready), then return either way.                                          *)
(***************************************************************************)
FinalSample ==
    /\ pc = "timedout"
    /\ pc'   = "final"
    /\ seen' = ready
    /\ UNCHANGED <<ready, registered, flagged, deadline_passed, dying, stop_req, stop_used, spun>>

EvaluateFinal ==
    /\ pc = "final"
    /\ pc' = IF \E f \in Fds : seen[f] THEN "done_ready" ELSE "done_timeout"
    /\ registered' = Unhook
    /\ UNCHANGED <<ready, flagged, seen, deadline_passed, dying, stop_req, stop_used, spun>>

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
    \/ Rearm
    \/ LoopCheck
    \/ Resample
    \/ BuggyClearLate
    \/ EvaluateWake
    \/ FinalSample
    \/ EvaluateFinal
    \/ ParkDeath
    \/ BackoffCommit
    \/ BackoffTimeout

Next ==
    \/ PollerStep
    \/ \E f \in Fds : MakeReady(f)
    \/ \E f \in Fds : Retract(f)
    \/ \E f \in Fds : OtherEvent(f)
    \/ AdvanceTime
    \/ SpinLapse
    \/ Die
    \/ StopRequest
    \/ StopResume
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
\* while a registered fd is ready. (The backoff is not "sleeping": it holds no
\* hook, is bounded, and re-registers when it ends -- StableReadyReturns.) The headline property. Violated by
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

\* ParkedLoopHoldsNoHook — the loop's own stop park happens with every hook
\* off its list, so no producer walks to a parked poller for as long as a
\* debugger holds it (tsleep's detour, by contrast, parks listed).
ParkedLoopHoldsNoHook == (pc = "loopparked") => (\A f \in Fds : ~registered[f])

\* BackoffHoldsNoHook — the backoff runs, sleeps and parks with every hook off
\* its list, and so holds no object ref either: that is what makes it a sleep
\* no producer can shorten. The fds are re-resolved when it ends.
BackoffHoldsNoHook ==
    (pc \in {"backoff", "backingoff", "bparked"}) => (\A f \in Fds : ~registered[f])

\* IntrOnlyWhenDying — poll unwinds for death only when its Proc is dying.
IntrOnlyWhenDying == (pc = "done_intr") => dying

Invariants ==
    /\ TypeOk
    /\ HookedReadyIsFlagged
    /\ NoMissedPoll
    /\ NoStaleHook
    /\ ReadyResultSound
    /\ TimeoutResultSound
    /\ NoSpuriousZero
    /\ ParkedLoopHoldsNoHook
    /\ BackoffHoldsNoHook
    /\ IntrOnlyWhenDying

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
(*                                                                         *)
(* DeathTerminates — a dying poller returns, whatever the producers do and *)
(* whatever its timeout. With poll(-1) this is the property the loop's own *)
(* die-check exists for (BUGGY_NO_LOOP_DIE_CHECK): without it a producer   *)
(* that keeps a flag set keeps the dying poller circling forever.           *)
(*                                                                         *)
(* StopHonoured — a pending stop is eventually honoured: the poller parks, *)
(* returns, or the stop is lifted (which here happens only at a park).     *)
(* Violated by BUGGY_NO_LOOP_STOP_CHECK.                                    *)
(*                                                                         *)
(* SpinBounded — the poller really sleeps, or returns, again and again:   *)
(* no producer can keep it awake. Awake means IRQ-masked here -- a        *)
(* syscall runs so end to end -- so this is the CPU's interrupt latency.  *)
(* A timeout does not supply it: a producer can hold a poll with ten      *)
(* seconds left for all ten. Violated by BUGGY_NO_BACKSTOP.               *)
(***************************************************************************)
PollTerminates == <>(pc \in Terminal)

StableReadyReturns ==
    \A f \in Fds : (<>[](ready[f])) => <>(pc \in Terminal)

DeathTerminates == dying ~> (pc \in Terminal)

StopHonoured == stop_req ~> (~stop_req \/ pc \in Parked \/ pc \in Terminal)

SpinBounded == []<>(pc \in RealSleep \cup Terminal)

Liveness ==
    /\ WF_vars(PollerStep)
    /\ WF_vars(AdvanceTime)
    /\ WF_vars(SpinLapse)
    /\ WF_vars(StopResume)

Spec_Live == Init /\ [][Next]_vars /\ Liveness

====
