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
(*   (`sched_tick`) which expires the head of its CPU's timer base. Three  *)
(*   actors now                                                            *)
(*   race for one single-waiter Rendez:                                     *)
(*                                                                         *)
(*     - `wakeup(r)`            — the producer woke the waiter;            *)
(*     - the `sched_tick` scan  — the deadline expired;                    *)
(*     - the waiter itself      — resuming from sched() to re-evaluate.    *)
(*                                                                         *)
(*   The Rendez lock serializes the wakes. `wakeup` and the death and note *)
(*   wakers take that lock alone, so a wake LEAVES the sleeper's entry on  *)
(*   its timer base, and the SLEEPER unlinks it on its way out of tsleep.  *)
(*   That is what makes an entry's Rendez valid under the base lock: the   *)
(*   owner cannot leave tsleep, and so cannot end the Rendez's life,       *)
(*   without passing through that lock or the Rendez lock. An entry whose  *)
(*   owner has been woken is stale and harmless: the tick takes it off and *)
(*   wakes nothing.                                                        *)
(*                                                                         *)
(*   This spec proves the waiter is woken EXACTLY ONCE per sleep episode,  *)
(*   that a timer entry exists only from the commit to the owner's leaving *)
(*   and always names a Rendez, that the tick never outlives the Rendez it *)
(*   reads, and that the AWOKEN / TIMEDOUT return value is sound.          *)
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
(*   BUGGY_OWNER_KEEPS — the owner leaves tsleep with its entry still      *)
(*     linked. The base now holds a thread out of its sleep, and the tick  *)
(*     would read a Rendez the caller may have ended (NoStaleTimerEntry    *)
(*     counterexample). The fix: timer_base_leave on every way out.        *)
(*                                                                         *)
(*   BUGGY_CLEAR_FIRST — the owner clears rendez_blocked_on before it      *)
(*     unlinks. The tick can find a linked entry with no Rendez            *)
(*     (TimerEntryHasRendez counterexample). The fix: unlink, then clear.  *)
(*                                                                         *)
(*   BUGGY_UNLINK_EARLY — the tick unlinks under the base lock alone,      *)
(*     before it takes the Rendez lock. An owner that reads its mark clear *)
(*     skips the base lock, takes the Rendez lock and leaves tsleep while  *)
(*     the tick still holds that Rendez's address (RendezOutlivesTick      *)
(*     counterexample). The fix: unlink under the Rendez lock, which the   *)
(*     owner must take before it can leave.                                *)
(*                                                                         *)
(*   BUGGY_TIMEOUT_STALE — the tick times a thread out off its mere        *)
(*     presence on the base, without re-checking it is still SLEEPING. On  *)
(*     a stale entry, which the design permits, this wakes an already-     *)
(*     woken thread a second time (NoDoubleWake counterexample) — a double *)
(*     `ready()`, the runqueue-corruption class. The fix: the tick         *)
(*     re-validates SLEEPING under the Rendez lock.                        *)
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
(*   tsleep_buggy_owner_keeps.cfg  BUGGY_OWNER_KEEPS — NoStaleTimerEntry   *)
(*                                  counterexample.                        *)
(*   tsleep_buggy_clear_first.cfg  BUGGY_CLEAR_FIRST — TimerEntryHasRendez *)
(*                                  counterexample.                        *)
(*   tsleep_buggy_unlink_early.cfg BUGGY_UNLINK_EARLY — RendezOutlivesTick *)
(*                                  counterexample.                        *)
(*   tsleep_buggy_double_wake.cfg  BUGGY_TIMEOUT_STALE — NoDoubleWake      *)
(*                                  counterexample.                        *)
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
(*   One base is modelled. Commit is blocked by a tick holding the Rendez  *)
(*   lock and not by one holding only the base's, since the owner may be   *)
(*   on another CPU by then. The correct cfgs never reach that freedom: a  *)
(*   tick in "base" means the entry is linked, so the owner has not left.  *)
(*   BUGGY_UNLINK_EARLY does.                                              *)
(*                                                                         *)
(*   Atomic actions. Each action is one TLA+ step, one critical section.   *)
(*   The tick's section is not atomic against the owner, since the owner   *)
(*   may read its mark without the base lock, so it is split at the points *)
(*   where that matters: TickPick, under the base lock; TickLock, which    *)
(*   takes the Rendez lock and unlinks; TickFire, which re-checks, wakes   *)
(*   and releases. Lock order, wait_lock -> base -> rendez -> CpuSched,    *)
(*   excludes AB-BA at the impl level (audit + code review).               *)
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
    BUGGY_OWNER_KEEPS,     \* BOOLEAN — TRUE: the owner leaves tsleep with its
                           \*   timer entry still linked.
    BUGGY_CLEAR_FIRST,     \* BOOLEAN — TRUE: the owner clears rendez_blocked_on
                           \*   a step before it unlinks.
    BUGGY_UNLINK_EARLY,    \* BOOLEAN — TRUE: the tick unlinks under the base
                           \*   lock alone, before it takes the Rendez lock.
    BUGGY_TIMEOUT_STALE,   \* BOOLEAN — TRUE: the tick does NOT re-check the
                           \*   thread is still SLEEPING before waking it.
    BUGGY_RECHECK_ORDER,   \* BOOLEAN — TRUE: tsleep, on resume, consults the
                           \*   timed-out flag BEFORE re-checking cond.
    MaxSleeps              \* Nat >= 1 — cap on sleep episodes (state bound).

ASSUME HAS_DEADLINE        \in BOOLEAN
ASSUME BUGGY_OWNER_KEEPS   \in BOOLEAN
ASSUME BUGGY_CLEAR_FIRST   \in BOOLEAN
ASSUME BUGGY_UNLINK_EARLY  \in BOOLEAN
ASSUME BUGGY_TIMEOUT_STALE \in BOOLEAN
ASSUME BUGGY_RECHECK_ORDER \in BOOLEAN
ASSUME MaxSleeps \in Nat /\ MaxSleeps >= 1

VARIABLES
    pc,              \* sleeper lifecycle ∈ PCs (see below)
    cond,            \* BOOLEAN — the wait condition cond(arg)
    r_waiter,        \* BOOLEAN — sleeper is r->waiter (enqueued on the Rendez)
    on_timerlist,    \* BOOLEAN — sleeper has an entry on a timer base
    rb,              \* BOOLEAN — t->rendez_blocked_on is set
    tick,            \* the tick's progress on this entry: "idle", "base" (holds
                     \*   the base lock, has read r), "rlock" (holds r->lock too)
    timedout_flag,   \* BOOLEAN — t->sleep_timedout (set by the timeout scan)
    deadline_passed, \* BOOLEAN — monotonic time has reached the deadline
    n_sleep,         \* Nat — count of running->sleeping transitions (episodes)
    n_wake           \* Nat — count of waker-effected sleeping->running

vars == <<pc, cond, r_waiter, on_timerlist, rb, tick, timedout_flag,
          deadline_passed, n_sleep, n_wake>>

\* "running"       — executing inside tsleep: first entry, or back from
\*                   sched() and off its timer base, about to (re-)evaluate.
\* "sleeping"      — committed to sleep: r->waiter set, on a timer base
\*                   (if deadlined), thread state SLEEPING.
\* "woken"         — a waker has made it RUNNABLE; it has not yet run past
\*                   timer_base_leave, so its entry may still be linked.
\* "done_woken"    — tsleep returned TSLEEP_AWOKEN  (condition satisfied).
\* "done_timedout" — tsleep returned TSLEEP_TIMEDOUT (deadline; cond false).
PCs      == {"running", "sleeping", "woken", "done_woken", "done_timedout"}
Terminal == {"done_woken", "done_timedout"}

TypeOk ==
    /\ pc              \in PCs
    /\ cond            \in BOOLEAN
    /\ r_waiter        \in BOOLEAN
    /\ on_timerlist    \in BOOLEAN
    /\ rb              \in BOOLEAN
    /\ tick            \in {"idle", "base", "rlock"}
    /\ timedout_flag   \in BOOLEAN
    /\ deadline_passed \in BOOLEAN
    /\ n_sleep         \in 0..MaxSleeps
    /\ n_wake          \in 0..(MaxSleeps + 1)

(***************************************************************************)
(* The waiter has just called tsleep; nothing has happened yet. cond is    *)
(* FALSE (an already-true cond is the uninteresting fast path); the waiter *)
(* is on neither the Rendez nor a timer base; time has not passed         *)
(* the deadline.                                                            *)
(***************************************************************************)
Init ==
    /\ pc              = "running"
    /\ cond            = FALSE
    /\ r_waiter        = FALSE
    /\ on_timerlist    = FALSE
    /\ rb              = FALSE
    /\ tick            = "idle"
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
    /\ UNCHANGED <<pc, r_waiter, on_timerlist, rb, tick, timedout_flag,
                   deadline_passed, n_sleep, n_wake>>

(***************************************************************************)
(* AdvanceTime — the monotonic counter reaches the deadline. Write-once.   *)
(* Only meaningful when the modeled tsleep carries a deadline.             *)
(***************************************************************************)
AdvanceTime ==
    /\ HAS_DEADLINE
    /\ ~deadline_passed
    /\ deadline_passed' = TRUE
    /\ UNCHANGED <<pc, cond, r_waiter, on_timerlist, rb, tick, timedout_flag,
                   n_sleep, n_wake>>

(***************************************************************************)
(* Expired — the deadline-reached predicate the resume path evaluates:     *)
(* either the timer scan set t->sleep_timedout, or (the entry case) the    *)
(* absolute deadline already lay in the past before the thread enqueued.   *)
(* FALSE whenever the modeled tsleep has no deadline.                       *)
(***************************************************************************)
Expired == HAS_DEADLINE /\ (timedout_flag \/ deadline_passed)

(***************************************************************************)
(* Commit — the waiter, in "running", evaluates whether to sleep, under     *)
(* the Rendez lock, so not while the tick holds it. This is                *)
(* both the first entry into tsleep and every resume from sched() after a  *)
(* wake; the impl loop is identical at both (cond re-checked under the     *)
(* Rendez lock).                                                            *)
(*                                                                         *)
(*   cond true     -> return AWOKEN  (the fast path on entry; success on   *)
(*                    a resume).                                            *)
(*   else Expired  -> return TIMEDOUT.                                      *)
(*   else          -> sleep: enqueue on the Rendez, and — if deadlined —   *)
(*                    onto a timer base; thread state -> SLEEPING.         *)
(*                                                                         *)
(* CORRECT order checks `cond` FIRST: a wait satisfied at the deadline     *)
(* succeeds. BUGGY_RECHECK_ORDER checks Expired first and loses that race. *)
(***************************************************************************)
Commit ==
    /\ pc = "running"
    /\ tick # "rlock"
    /\ LET sleepBranch ==
              /\ n_sleep < MaxSleeps
              /\ pc'           = "sleeping"
              /\ r_waiter'     = TRUE
              /\ rb'           = TRUE
              /\ on_timerlist' = HAS_DEADLINE
              /\ n_sleep'      = n_sleep + 1
              /\ UNCHANGED <<cond, tick, timedout_flag, deadline_passed, n_wake>>
           wokenBranch ==
              /\ pc' = "done_woken"
              /\ UNCHANGED <<cond, r_waiter, on_timerlist, rb, tick, timedout_flag,
                             deadline_passed, n_sleep, n_wake>>
           timedoutBranch ==
              /\ pc' = "done_timedout"
              /\ UNCHANGED <<cond, r_waiter, on_timerlist, rb, tick, timedout_flag,
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
(* enqueued on the Rendez (r->waiter # NULL); after the tick has already   *)
(* cleared r_waiter, Wakeup finds no waiter and is a no-op (disabled here).*)
(* That single-consumer check is what makes the wakeup / timeout race      *)
(* resolve to exactly one wake. It takes the Rendez lock alone, so it      *)
(* leaves the timer entry where it is.                                     *)
(***************************************************************************)
Wakeup ==
    /\ r_waiter
    /\ pc = "sleeping"
    /\ tick # "rlock"
    /\ pc'       = "woken"
    /\ r_waiter' = FALSE
    /\ n_wake'   = n_wake + 1
    /\ UNCHANGED <<cond, on_timerlist, rb, tick, timedout_flag,
                   deadline_passed, n_sleep>>

(***************************************************************************)
(* The tick, on an expired entry. TickPick takes the base lock and reads   *)
(* the entry's Rendez; TickLock takes that Rendez's lock and unlinks;      *)
(* TickFire re-checks, wakes if the thread is still the Rendez's sleeping  *)
(* waiter, and drops both. So TickFire has two outcomes: a timeout, and,   *)
(* on a stale entry, nothing.                                              *)
(*                                                                         *)
(* The base lock excludes the owner while its mark is set, and the owner   *)
(* cannot run Commit without the Rendez lock. BUGGY_UNLINK_EARLY unlinks   *)
(* in TickPick, so the owner reads its mark clear and leaves while the     *)
(* tick is still on its way to a Rendez it has only the address of.        *)
(***************************************************************************)
TickPick ==
    /\ tick = "idle"
    /\ on_timerlist
    /\ deadline_passed
    /\ tick'         = "base"
    /\ on_timerlist' = IF BUGGY_UNLINK_EARLY THEN FALSE ELSE on_timerlist
    /\ UNCHANGED <<pc, cond, r_waiter, rb, timedout_flag, deadline_passed,
                   n_sleep, n_wake>>

TickLock ==
    /\ tick = "base"
    /\ tick'         = "rlock"
    /\ on_timerlist' = FALSE
    /\ UNCHANGED <<pc, cond, r_waiter, rb, timedout_flag, deadline_passed,
                   n_sleep, n_wake>>

TickFire ==
    /\ tick = "rlock"
    /\ tick' = "idle"
    /\ IF BUGGY_TIMEOUT_STALE \/ (pc = "sleeping" /\ r_waiter)
       THEN /\ pc'            = "woken"
            /\ r_waiter'      = FALSE
            /\ timedout_flag' = TRUE
            /\ n_wake'        = n_wake + 1
            /\ UNCHANGED <<cond, on_timerlist, rb, deadline_passed, n_sleep>>
       ELSE UNCHANGED <<pc, cond, r_waiter, on_timerlist, rb, timedout_flag,
                        deadline_passed, n_sleep, n_wake>>

(***************************************************************************)
(* OwnerLeave — the woken thread has run and comes off the base, then      *)
(* clears rendez_blocked_on (timer_base_leave, then the clear, in tsleep's *)
(* resume path). A set mark means the base lock, so a tick that holds it   *)
(* blocks this; a clear mark needs no lock. That is why OwnerLeave may run *)
(* past a tick in "rlock", and Commit, next, may not.                      *)
(***************************************************************************)
OwnerLeave ==
    /\ pc = "woken"
    /\ on_timerlist => tick = "idle"
    /\ pc'           = "running"
    /\ on_timerlist' = IF BUGGY_OWNER_KEEPS THEN on_timerlist ELSE FALSE
    /\ rb'           = FALSE
    /\ UNCHANGED <<cond, r_waiter, tick, timedout_flag, deadline_passed,
                   n_sleep, n_wake>>

ClearFirst ==
    /\ BUGGY_CLEAR_FIRST
    /\ pc = "woken"
    /\ rb
    /\ rb' = FALSE
    /\ UNCHANGED <<pc, cond, r_waiter, on_timerlist, tick, timedout_flag,
                   deadline_passed, n_sleep, n_wake>>

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
    \/ TickPick
    \/ TickLock
    \/ TickFire
    \/ OwnerLeave
    \/ ClearFirst
    \/ Done

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

\* NoStaleTimerEntry — a thread has a timer entry only from its commit to its
\* leaving: asleep, or woken and not yet off the base. Pins the owner's unlink:
\* no thread runs on past tsleep's resume with an entry behind it. Violated by
\* BUGGY_OWNER_KEEPS.
NoStaleTimerEntry == on_timerlist => pc \in {"sleeping", "woken"}

\* TimerEntryHasRendez — a linked entry always names its Rendez; the tick reads
\* it under the base lock and extincts on NULL. Violated by BUGGY_CLEAR_FIRST.
TimerEntryHasRendez == on_timerlist => rb

\* RendezOutlivesTick — tsleep never returns, and so never hands the Rendez's
\* life back to its caller, while the tick is on its way to or inside that
\* Rendez's lock. Violated by BUGGY_UNLINK_EARLY.
RendezOutlivesTick == tick # "idle" => pc \notin Terminal

\* SleepingHasWaiter — a sleeping thread is always enqueued on its Rendez.
\* The Rendez waiter and the sleep state are set and cleared together.
SleepingHasWaiter == (pc = "sleeping") => r_waiter

\* NoDoubleWake — the waiter is transitioned out of "sleeping" by at most
\* one waker per sleep episode: the running count of waker-effected wakes
\* never exceeds the running count of sleep episodes. A second wake of an
\* already-woken thread (a double ready(), the runqueue-corruption class)
\* drives n_wake past n_sleep. Violated by BUGGY_TIMEOUT_STALE on the stale
\* entry the design permits.
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
    /\ TimerEntryHasRendez
    /\ RendezOutlivesTick
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
(*   WF(TickPick, TickLock, TickFire) — the periodic tick always eventually *)
(*                     fires once the deadline is reached.                  *)
(*   WF(OwnerLeave)  — a woken waiter is eventually scheduled.             *)
(*   WF(Commit)      — and re-evaluates.                                   *)
(*                                                                         *)
(* With a deadline (HAS_DEADLINE TRUE) these suffice: AdvanceTime -> the   *)
(* tick -> OwnerLeave -> Commit drives the waiter to done_timedout. With   *)
(* no deadline (tsleep_buggy_wedge.cfg) AdvanceTime and the tick are       *)
(* disabled, and an                                                        *)
(* unfair producer leaves the waiter in "sleeping" forever — the           *)
(* counterexample that motivates the deadline.                              *)
(***************************************************************************)
TsleepTerminates == <>(pc \in Terminal)

Liveness ==
    /\ WF_vars(Commit)
    /\ WF_vars(AdvanceTime)
    /\ WF_vars(TickPick)
    /\ WF_vars(TickLock)
    /\ WF_vars(TickFire)
    /\ WF_vars(OwnerLeave)

Spec_Live == Init /\ [][Next]_vars /\ Liveness

====
