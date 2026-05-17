---- MODULE tsleep ----
(***************************************************************************)
(* Thylacine `tsleep` — the deadline-bounded Rendez sleep (P5-tsleep).      *)
(*                                                                         *)
(* `tsleep(r, cond, arg, deadline_ns)` is `sleep` with a deadline: the     *)
(* waiter wakes on `wakeup`, on its condition becoming true, or when the   *)
(* deadline passes — the return value distinguishes a timeout from a       *)
(* condition wake (ARCHITECTURE.md §8.8). It is the kernel primitive       *)
(* behind every bounded wait: a `/srv` client blocked on a possibly-hung   *)
(* 9P server (CORVUS-DESIGN.md §6.2), and the Phase-5 `poll` / `futex`     *)
(* timeouts.                                                                *)
(*                                                                         *)
(* WHAT THIS SPEC PINS                                                      *)
(*                                                                         *)
(*   `sleep` has TWO wake sources serialized by one Rendez lock: the       *)
(*   condition becoming true, and `wakeup`. Its missed-wakeup-freedom      *)
(*   (I-9) is proven by scheduler.tla (WaitOnCond / WakeAll atomic;        *)
(*   BuggyCheck + BuggySleep counterexample). `tsleep` adds a THIRD wake    *)
(*   source — the deadline, delivered off the periodic scheduler tick      *)
(*   (`sched_tick`) which scans a global timer-wait list. Three actors now *)
(*   race for one single-waiter Rendez:                                     *)
(*                                                                         *)
(*     - `wakeup(r)`            — the producer woke the waiter;            *)
(*     - the `sched_tick` scan  — the deadline expired;                    *)
(*     - the waiter itself      — resuming from sched() to re-evaluate.    *)
(*                                                                         *)
(*   The implementation serializes them with the per-Rendez lock plus a    *)
(*   global timer-wait lock (lock order: timerwait -> rendez). This spec   *)
(*   models the abstract mechanism — actions are atomic, which is exactly  *)
(*   what those locks buy — and proves the waiter is woken EXACTLY ONCE    *)
(*   per sleep episode, that the timer-wait list carries no stale entry,   *)
(*   and that the AWOKEN / TIMEDOUT return value is sound.                  *)
(*                                                                         *)
(*   The check-then-sleep atomicity (the classic missed wakeup) is NOT     *)
(*   re-modeled here: `tsleep`'s commit step holds the Rendez lock across  *)
(*   the cond check and the enqueue exactly as `sleep` does, and the       *)
(*   no-deadline path of `tsleep` IS `sleep`. scheduler.tla owns that      *)
(*   proof. This module is the focused sibling for the deadline surface,   *)
(*   on the sched_ctxsw.tla / pipe.tla precedent — folding the deadline    *)
(*   race into the 929-line scheduler model would multiply its state       *)
(*   space against no benefit.                                             *)
(*                                                                         *)
(* THE BUGS THIS PINS                                                       *)
(*                                                                         *)
(*   BUGGY_LAZY_UNLINK — `wakeup` wakes the waiter but does not unlink it  *)
(*     from the timer-wait list. The list now holds an entry for a thread  *)
(*     that is no longer sleeping (NoStaleTimerEntry counterexample). The  *)
(*     fix is the eager-unlink discipline: every wake — by `wakeup` OR by  *)
(*     the timeout scan — removes the thread from the list as it wakes it. *)
(*                                                                         *)
(*   BUGGY_TIMEOUT_STALE — the `sched_tick` scan times a thread out off    *)
(*     its mere presence on the list, without re-checking it is still      *)
(*     SLEEPING. Combined with BUGGY_LAZY_UNLINK's stale entry, this       *)
(*     wakes an already-woken thread a second time (NoDoubleWake           *)
(*     counterexample) — a double `ready()`, the runqueue-corruption       *)
(*     class. The fix: the scan re-validates SLEEPING under the lock.      *)
(*                                                                         *)
(*   BUGGY_RECHECK_ORDER — on resume, `tsleep` consults the timed-out      *)
(*     flag BEFORE re-checking `cond`. A wait satisfied right as the       *)
(*     deadline lapses then returns TIMEDOUT though the condition holds    *)
(*     (TimeoutSound counterexample). The fix: success has precedence —    *)
(*     re-check `cond` first; consult the timeout only if `cond` is false. *)
(*                                                                         *)
(* CFG MATRIX (executable documentation per CLAUDE.md spec-first policy)    *)
(*                                                                         *)
(*   tsleep.cfg                    all buggy flags FALSE, HAS_DEADLINE     *)
(*                                  TRUE — TLC proves every safety         *)
(*                                  invariant holds.                       *)
(*   tsleep_nodeadline.cfg         HAS_DEADLINE FALSE — the no-deadline    *)
(*                                  path (tsleep degrades to sleep);       *)
(*                                  safety invariants hold.                *)
(*   tsleep_liveness.cfg           Spec_Live, HAS_DEADLINE TRUE — TLC      *)
(*                                  proves TsleepTerminates: the waiter    *)
(*                                  finishes even if `wakeup` NEVER fires  *)
(*                                  and `cond` is NEVER satisfied. The     *)
(*                                  hung-corvus backstop.                  *)
(*   tsleep_buggy_lazy_unlink.cfg  BUGGY_LAZY_UNLINK — NoStaleTimerEntry   *)
(*                                  counterexample.                        *)
(*   tsleep_buggy_double_wake.cfg  BUGGY_LAZY_UNLINK + BUGGY_TIMEOUT_STALE *)
(*                                  — NoDoubleWake counterexample.         *)
(*   tsleep_buggy_recheck_order.cfg BUGGY_RECHECK_ORDER — TimeoutSound     *)
(*                                  counterexample.                        *)
(*   tsleep_buggy_wedge.cfg        Spec_Live, HAS_DEADLINE FALSE —         *)
(*                                  TsleepTerminates counterexample: with  *)
(*                                  no deadline, a producer that never     *)
(*                                  wakes the waiter wedges it forever.    *)
(*                                  The problem `tsleep` exists to solve.  *)
(*                                                                         *)
(* MODELING ASSUMPTIONS                                                     *)
(*                                                                         *)
(*   One waiter, one Rendez. `Rendez` is single-waiter by construction     *)
(*   (kernel/include/thylacine/rendez.h — `sleep` extincts on a second    *)
(*   waiter), so the deadline race is fully exercised by one sleeper.      *)
(*   Per-Rendez races compose: the timer-wait lock orders the global       *)
(*   scan against every Rendez uniformly.                                  *)
(*                                                                         *)
(*   Atomic actions. Each action is one TLA+ step; that models the         *)
(*   critical sections the timer-wait lock + the Rendez lock bracket. The  *)
(*   AB-BA hazard between them is excluded at the impl level by the        *)
(*   single lock order timerwait -> rendez (audit + code review); the     *)
(*   spec assumes the locks hold and proves what they then guarantee.      *)
(*                                                                         *)
(*   Monotonic time. `deadline_passed` is write-once FALSE -> TRUE; the    *)
(*   architectural counter never runs backward.                            *)
(*                                                                         *)
(* See ARCHITECTURE.md §8.8 (the Plan 9 idiom layer), §28 invariant I-9;   *)
(* CORVUS-DESIGN.md §6.2; scheduler.tla (the `sleep` proof this builds on).*)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    HAS_DEADLINE,          \* BOOLEAN — TRUE: the modeled tsleep carries a
                           \*   deadline (deadline_ns # 0). FALSE: no deadline
                           \*   — tsleep degrades to plain sleep (ARCH §8.8).
    BUGGY_LAZY_UNLINK,     \* BOOLEAN — TRUE: wakeup() wakes the waiter but
                           \*   does NOT unlink it from the timer-wait list.
    BUGGY_TIMEOUT_STALE,   \* BOOLEAN — TRUE: the sched_tick timeout scan does
                           \*   NOT re-check the thread is still SLEEPING
                           \*   before waking it.
    BUGGY_RECHECK_ORDER,   \* BOOLEAN — TRUE: tsleep, on resume, consults the
                           \*   timed-out flag BEFORE re-checking cond.
    MaxSleeps              \* Nat >= 1 — cap on sleep episodes (state bound).

ASSUME HAS_DEADLINE        \in BOOLEAN
ASSUME BUGGY_LAZY_UNLINK   \in BOOLEAN
ASSUME BUGGY_TIMEOUT_STALE \in BOOLEAN
ASSUME BUGGY_RECHECK_ORDER \in BOOLEAN
ASSUME MaxSleeps \in Nat /\ MaxSleeps >= 1

VARIABLES
    pc,              \* sleeper lifecycle ∈ PCs (see below)
    cond,            \* BOOLEAN — the wait condition cond(arg)
    r_waiter,        \* BOOLEAN — sleeper is r->waiter (enqueued on the Rendez)
    on_timerlist,    \* BOOLEAN — sleeper is on the global timer-wait list
    timedout_flag,   \* BOOLEAN — t->sleep_timedout (set by the timeout scan)
    deadline_passed, \* BOOLEAN — monotonic time has reached the deadline
    n_sleep,         \* Nat — count of running->sleeping transitions (episodes)
    n_wake           \* Nat — count of waker-effected sleeping->running

vars == <<pc, cond, r_waiter, on_timerlist, timedout_flag,
          deadline_passed, n_sleep, n_wake>>

\* "running"       — executing inside tsleep: first entry, or resuming from
\*                   sched() after a wake, about to (re-)evaluate.
\* "sleeping"      — committed to sleep: r->waiter set, on the timer-wait
\*                   list (if deadlined), thread state SLEEPING.
\* "done_woken"    — tsleep returned TSLEEP_AWOKEN  (condition satisfied).
\* "done_timedout" — tsleep returned TSLEEP_TIMEDOUT (deadline; cond false).
PCs      == {"running", "sleeping", "done_woken", "done_timedout"}
Terminal == {"done_woken", "done_timedout"}

TypeOk ==
    /\ pc              \in PCs
    /\ cond            \in BOOLEAN
    /\ r_waiter        \in BOOLEAN
    /\ on_timerlist    \in BOOLEAN
    /\ timedout_flag   \in BOOLEAN
    /\ deadline_passed \in BOOLEAN
    /\ n_sleep         \in 0..MaxSleeps
    /\ n_wake          \in 0..(MaxSleeps + 1)

(***************************************************************************)
(* The waiter has just called tsleep; nothing has happened yet. cond is    *)
(* FALSE (an already-true cond is the uninteresting fast path); the waiter *)
(* is on neither the Rendez nor the timer-wait list; time has not passed   *)
(* the deadline.                                                            *)
(***************************************************************************)
Init ==
    /\ pc              = "running"
    /\ cond            = FALSE
    /\ r_waiter        = FALSE
    /\ on_timerlist    = FALSE
    /\ timedout_flag   = FALSE
    /\ deadline_passed = FALSE
    /\ n_sleep         = 0
    /\ n_wake          = 0

(***************************************************************************)
(* SetCond — a producer satisfies the wait condition. Monotonic (cond is   *)
(* never cleared). Gated to the pre-terminal space: once tsleep has        *)
(* returned, no one observes cond, so freezing it keeps WokenSound /       *)
(* TimeoutSound stable statements about the value at the decision point.   *)
(*                                                                         *)
(* NOTE cond becoming true does NOT itself wake the sleeper — the producer *)
(* must still call `wakeup` (the Wakeup action). SetCond and Wakeup are    *)
(* separate steps precisely because the impl producer does both in order.  *)
(***************************************************************************)
SetCond ==
    /\ pc \notin Terminal
    /\ ~cond
    /\ cond' = TRUE
    /\ UNCHANGED <<pc, r_waiter, on_timerlist, timedout_flag,
                   deadline_passed, n_sleep, n_wake>>

(***************************************************************************)
(* AdvanceTime — the monotonic counter reaches the deadline. Write-once.   *)
(* Only meaningful when the modeled tsleep carries a deadline.             *)
(***************************************************************************)
AdvanceTime ==
    /\ HAS_DEADLINE
    /\ ~deadline_passed
    /\ deadline_passed' = TRUE
    /\ UNCHANGED <<pc, cond, r_waiter, on_timerlist, timedout_flag,
                   n_sleep, n_wake>>

(***************************************************************************)
(* Expired — the deadline-reached predicate the resume path evaluates:     *)
(* either the timer scan set t->sleep_timedout, or (the entry case) the    *)
(* absolute deadline already lay in the past before the thread enqueued.   *)
(* FALSE whenever the modeled tsleep has no deadline.                       *)
(***************************************************************************)
Expired == HAS_DEADLINE /\ (timedout_flag \/ deadline_passed)

(***************************************************************************)
(* Commit — the waiter, in "running", evaluates whether to sleep. This is  *)
(* both the first entry into tsleep and every resume from sched() after a  *)
(* wake; the impl loop is identical at both (cond re-checked under the     *)
(* Rendez lock).                                                            *)
(*                                                                         *)
(*   cond true     -> return AWOKEN  (the fast path on entry; success on   *)
(*                    a resume).                                            *)
(*   else Expired  -> return TIMEDOUT.                                      *)
(*   else          -> sleep: enqueue on the Rendez, and — if deadlined —   *)
(*                    onto the timer-wait list; thread state -> SLEEPING.  *)
(*                                                                         *)
(* CORRECT order checks `cond` FIRST: a wait satisfied at the deadline     *)
(* succeeds. BUGGY_RECHECK_ORDER checks Expired first and loses that race. *)
(***************************************************************************)
Commit ==
    /\ pc = "running"
    /\ LET sleepBranch ==
              /\ n_sleep < MaxSleeps
              /\ pc'           = "sleeping"
              /\ r_waiter'     = TRUE
              /\ on_timerlist' = HAS_DEADLINE
              /\ n_sleep'      = n_sleep + 1
              /\ UNCHANGED <<cond, timedout_flag, deadline_passed, n_wake>>
           wokenBranch ==
              /\ pc' = "done_woken"
              /\ UNCHANGED <<cond, r_waiter, on_timerlist, timedout_flag,
                             deadline_passed, n_sleep, n_wake>>
           timedoutBranch ==
              /\ pc' = "done_timedout"
              /\ UNCHANGED <<cond, r_waiter, on_timerlist, timedout_flag,
                             deadline_passed, n_sleep, n_wake>>
       IN  IF BUGGY_RECHECK_ORDER
           THEN \* BUG: the timeout is consulted before cond.
                IF Expired   THEN timedoutBranch
                ELSE IF cond THEN wokenBranch
                ELSE              sleepBranch
           ELSE \* CORRECT: cond first (success has precedence).
                IF cond      THEN wokenBranch
                ELSE IF Expired THEN timedoutBranch
                ELSE              sleepBranch

(***************************************************************************)
(* Wakeup — the producer calls wakeup(r). It wakes the waiter iff one is   *)
(* enqueued on the Rendez (r->waiter # NULL); after a Timeout has already  *)
(* cleared r_waiter, Wakeup finds no waiter and is a no-op (disabled here).*)
(* That single-consumer check is what makes the wakeup / timeout race      *)
(* resolve to exactly one wake.                                            *)
(*                                                                         *)
(* CORRECT: the wake eagerly unlinks the thread from the timer-wait list.  *)
(* BUGGY_LAZY_UNLINK leaves it linked — the stale-entry hazard.            *)
(***************************************************************************)
Wakeup ==
    /\ r_waiter
    /\ pc = "sleeping"
    /\ pc'           = "running"
    /\ r_waiter'     = FALSE
    /\ on_timerlist' = IF BUGGY_LAZY_UNLINK THEN on_timerlist ELSE FALSE
    /\ n_wake'       = n_wake + 1
    /\ UNCHANGED <<cond, timedout_flag, deadline_passed, n_sleep>>

(***************************************************************************)
(* Timeout — a sched_tick scan finds the deadline expired and wakes the    *)
(* waiter, recording the timeout in t->sleep_timedout. The wake unlinks    *)
(* the thread and clears r->waiter, so a subsequent Wakeup no-ops.         *)
(*                                                                         *)
(* CORRECT: the scan re-validates the thread is still "sleeping" before    *)
(* waking it (defence in depth beside Wakeup's eager unlink — both true   *)
(* of the impl). BUGGY_TIMEOUT_STALE drops that re-check, so on a stale    *)
(* list entry (BUGGY_LAZY_UNLINK) it wakes an already-woken thread.        *)
(***************************************************************************)
Timeout ==
    /\ on_timerlist
    /\ deadline_passed
    /\ (BUGGY_TIMEOUT_STALE \/ pc = "sleeping")
    /\ pc'            = "running"
    /\ r_waiter'      = FALSE
    /\ on_timerlist'  = FALSE
    /\ timedout_flag' = TRUE
    /\ n_wake'        = n_wake + 1
    /\ UNCHANGED <<cond, deadline_passed, n_sleep>>

(***************************************************************************)
(* Done — terminal self-loop. Once tsleep has returned the model halts;    *)
(* the explicit stutter keeps a legitimate terminal state from tripping    *)
(* TLC's deadlock check, which then remains meaningful for the pre-        *)
(* terminal space.                                                          *)
(***************************************************************************)
Done == pc \in Terminal /\ UNCHANGED vars

Next ==
    \/ SetCond
    \/ AdvanceTime
    \/ Commit
    \/ Wakeup
    \/ Timeout
    \/ Done

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

\* NoStaleTimerEntry — a thread is on the timer-wait list only while it is
\* actually in a timed sleep. Pins the eager-unlink discipline: every wake
\* — Wakeup AND Timeout — removes the thread from the list as it wakes it.
\* Violated by BUGGY_LAZY_UNLINK.
NoStaleTimerEntry == on_timerlist => pc = "sleeping"

\* SleepingHasWaiter — a sleeping thread is always enqueued on its Rendez.
\* The Rendez waiter and the sleep state are set and cleared together.
SleepingHasWaiter == (pc = "sleeping") => r_waiter

\* NoDoubleWake — the waiter is transitioned out of "sleeping" by at most
\* one waker per sleep episode: the running count of waker-effected wakes
\* never exceeds the running count of sleep episodes. A second wake of an
\* already-woken thread (a double ready(), the runqueue-corruption class)
\* drives n_wake past n_sleep. Violated by BUGGY_LAZY_UNLINK +
\* BUGGY_TIMEOUT_STALE together.
NoDoubleWake == n_wake <= n_sleep

\* WokenSound — tsleep returns AWOKEN only when the condition holds.
WokenSound == (pc = "done_woken") => cond

\* TimeoutSound — tsleep returns TIMEDOUT only when the condition does NOT
\* hold. Success has precedence: a wait satisfied as the deadline lapses
\* reports AWOKEN. Violated by BUGGY_RECHECK_ORDER.
TimeoutSound == (pc = "done_timedout") => ~cond

Invariants ==
    /\ TypeOk
    /\ NoStaleTimerEntry
    /\ SleepingHasWaiter
    /\ NoDoubleWake
    /\ WokenSound
    /\ TimeoutSound

(***************************************************************************)
(* ============================== LIVENESS ================================ *)
(*                                                                         *)
(* TsleepTerminates — tsleep always eventually returns. This is the whole  *)
(* reason the primitive exists: a corvus that hangs (CORVUS-DESIGN §6.2)   *)
(* must not wedge its clients. The fairness below grants NOTHING to the    *)
(* producer — neither Wakeup nor SetCond is fair — so termination must     *)
(* hold against a producer that never wakes the waiter and never satisfies *)
(* the condition. It is the deadline that carries progress:                *)
(*                                                                         *)
(*   WF(AdvanceTime) — monotonic time always advances to the deadline.     *)
(*   WF(Timeout)     — the periodic sched_tick scan always eventually      *)
(*                     fires once the deadline is reached.                  *)
(*   WF(Commit)      — the waiter is eventually scheduled to re-evaluate.  *)
(*                                                                         *)
(* With a deadline (HAS_DEADLINE TRUE) these suffice: AdvanceTime ->       *)
(* Timeout -> Commit drives the waiter to done_timedout. With no deadline  *)
(* (tsleep_buggy_wedge.cfg) AdvanceTime and Timeout are disabled, and an   *)
(* unfair producer leaves the waiter in "sleeping" forever — the           *)
(* counterexample that motivates the deadline.                              *)
(***************************************************************************)
TsleepTerminates == <>(pc \in Terminal)

Liveness ==
    /\ WF_vars(Commit)
    /\ WF_vars(AdvanceTime)
    /\ WF_vars(Timeout)

Spec_Live == Init /\ [][Next]_vars /\ Liveness

====
