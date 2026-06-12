---- MODULE cons_poll ----
(***************************************************************************)
(* Thylacine pollable console -- the IRQ -> console_mgr -> poll-hook       *)
(* DEFERRED wake (LS-8a).                                                   *)
(*                                                                         *)
(* The console RX interrupt (`cons_rx_input`, arch/arm64/uart.c) runs in   *)
(* IRQ context. The blocking reader it wakes via `wakeup(&data_rendez)` is *)
(* fine -- `wakeup` on a `Rendez` is IRQ-safe (spin_lock_irqsave on the    *)
(* global timer-wait lock; scheduler.tla / tsleep.tla). But a POLLER does  *)
(* not block-read: it registers a `poll_waiter` hook on the console's hook *)
(* list and parks on its OWN `Rendez` (poll.tla). Waking it means walking  *)
(* that hook list -- `poll_waiter_list_wake` -- which takes a PLAIN        *)
(* (non-irqsave) spinlock and nests a `wakeup` inside it, so it CANNOT run *)
(* from IRQ context.                                                        *)
(*                                                                         *)
(* LS-8a therefore DEFERS the hook-list walk to the `console_mgr` kproc    *)
(* kthread -- the same process-context vehicle that already defers the     *)
(* Ctrl-C `interrupt` post and the SAK (kernel/cons.c). The RX IRQ sets a  *)
(* `poll_wake_pending` flag under `g_cons.lock` and `wakeup`s the mgr's    *)
(* `Rendez`; the mgr, in process context, drains the flag and calls        *)
(* `poll_waiter_list_wake`. This is precisely Linux's tty model -- the     *)
(* hard IRQ buffers the byte and schedules `flush_to_ldisc` work; the      *)
(* line-discipline cooking and the read/poll wakeups run in that work      *)
(* item, never the hard IRQ.                                                *)
(*                                                                         *)
(* WHAT THIS SPEC PINS                                                      *)
(*                                                                         *)
(*   The single-fd missed wakeup (cond check vs sleep, one Rendez) is      *)
(*   scheduler.tla's proof; the N-fd poll-hook register-then-observe and   *)
(*   the hook lifetime are poll.tla's; the deadline race is tsleep.tla's.  *)
(*   cons_poll adds what none of them covers: the wake is RELAYED through  *)
(*   an intermediary kthread. A producer in IRQ context can no longer wake *)
(*   the poller directly -- it sets `poll_wake_pending` and wakes the mgr; *)
(*   the mgr drains the flag and walks the hook list. The relay introduces *)
(*   a SECOND register-then-observe obligation, on top of poll.tla's: the  *)
(*   mgr's own sleep on its `Rendez` must be register-then-observe against *)
(*   `poll_wake_pending`, or a flag set as the mgr heads back to sleep is  *)
(*   lost and the relay never fires. This spec proves the COMPOSED relay   *)
(*   loses no wakeup: a poller with a registered hook and a ready console  *)
(*   is never left asleep with the relay quiescent (NoMissedConsPoll --    *)
(*   I-9 across the deferral).                                              *)
(*                                                                         *)
(* THE BUG THIS PINS                                                        *)
(*                                                                         *)
(*   BUGGY_MGR_LOST_WAKE -- the console_mgr's "no work pending, go back to *)
(*     sleep" is a hand-rolled check-then-sleep rather than the            *)
(*     register-then-observe `sleep(&mgr_rendez, cons_mgr_pending)`. The   *)
(*     mgr observes `poll_wake_pending == FALSE`, and in the gap before it *)
(*     commits to sleep an RX IRQ sets the flag and `wakeup`s the mgr's    *)
(*     Rendez -- but the mgr is not yet enqueued on it, so the wake is     *)
(*     lost. The mgr sleeps; the hook-list walk never happens; a poller    *)
(*     registered on a now-ready console sleeps forever (NoMissedConsPoll  *)
(*     counterexample). The fix is the register-then-observe sleep:        *)
(*     enqueue on `mgr_rendez` FIRST, then re-check `cons_mgr_pending`     *)
(*     under the lock, so a flag set during the window either keeps the    *)
(*     mgr off the sleep path or finds it enqueued to wake.                 *)
(*                                                                         *)
(* CFG MATRIX (executable documentation per CLAUDE.md spec-first policy)    *)
(*                                                                         *)
(*   cons_poll.cfg                  all buggy flags FALSE -- every safety  *)
(*                                   invariant holds.                       *)
(*   cons_poll_liveness.cfg         Spec_Live -- PollerEventuallyServed:   *)
(*                                   a ready console with a registered      *)
(*                                   poller always eventually returns the   *)
(*                                   poll. The relay delivers.              *)
(*   cons_poll_buggy_lost_wake.cfg  BUGGY_MGR_LOST_WAKE -- NoMissedConsPoll*)
(*                                   counterexample: the relay strands a    *)
(*                                   poller asleep on a ready console.       *)
(*                                                                         *)
(* MODELING ASSUMPTIONS                                                     *)
(*                                                                         *)
(*   One poller, one console. The headline is the RELAY, not the N-fd fan  *)
(*   (poll.tla owns N fds; multiple pollers on one hook list compose       *)
(*   there). One poller fully exercises the deferred wake.                  *)
(*                                                                         *)
(*   `data` -- the console has at least one byte buffered, the POLLIN      *)
(*   readiness -- is monotonic FALSE -> TRUE: a readiness edge within one  *)
(*   poll episode (a consumer draining the ring is a separate concern,     *)
(*   poll.tla's assumption).                                                *)
(*                                                                         *)
(*   The mgr's `Rendez` has OTHER wakers -- the Ctrl-C `interrupt` and SAK *)
(*   flags also wake it (kernel/cons.c `cons_mgr_pending`). `SpuriousWake` *)
(*   models one such benign wake: it is what puts the mgr in the           *)
(*   "awake, about to re-sleep" state in which the poll-pending relay race *)
(*   opens. Capped to fire once (`spurious_used`) -- a single occurrence   *)
(*   suffices to expose the lost wake and bounds the state space.           *)
(*                                                                         *)
(*   Atomic actions model the critical sections `g_cons.lock` (the flag +  *)
(*   ring count) and the poller's `Rendez` lock bracket. The correct mgr   *)
(*   sleep is ONE atomic step (the register-then-observe `sleep` holds the *)
(*   Rendez lock across enqueue + cond re-check); BUGGY_MGR_LOST_WAKE      *)
(*   splits it into observe + commit to expose the lost-wake window.        *)
(*                                                                         *)
(*   The poll(-1) infinite wait is modeled (no timeout): the poller        *)
(*   finishes ONLY when the relay flags it, so a dropped relay strands it  *)
(*   forever -- the sharpest statement of the relay obligation. The        *)
(*   timeout backstop is tsleep.tla's PollTerminates, not re-pinned here.   *)
(*                                                                         *)
(* See ARCHITECTURE.md (the pollable-cons / deferred-wake section), §28    *)
(* invariant I-9; poll.tla (the poller-side register-then-observe + hook   *)
(* lifetime); tsleep.tla / scheduler.tla (the Rendez sleep the mgr relay   *)
(* and the poller both build on); kernel/cons.c (`console_mgr_main`,       *)
(* `cons_rx_input`), kernel/poll.c (`poll_waiter_list_wake`).              *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS
    BUGGY_MGR_LOST_WAKE   \* BOOLEAN -- TRUE: the console_mgr's go-to-sleep
                          \*   is a hand-rolled check-then-sleep (observe
                          \*   poll_wake_pending, then commit), not the
                          \*   register-then-observe sleep(). A flag set in
                          \*   the gap is lost.

ASSUME BUGGY_MGR_LOST_WAKE \in BOOLEAN

VARIABLES
    data,            \* BOOLEAN -- the console ring holds >= 1 byte (POLLIN
                     \*   readiness). Monotonic FALSE -> TRUE.
    pending,         \* BOOLEAN -- g_cons poll_wake_pending: an RX IRQ asked
                     \*   the mgr to walk the poll-hook list.
    registered,      \* BOOLEAN -- the poller's poll_waiter hook is installed
                     \*   on the console's hook list.
    flagged,         \* BOOLEAN -- the poller's poll_waiter ready flag, set by
                     \*   the mgr's hook-list walk; the readiness the poller
                     \*   observes under its own Rendez lock.
    poller_pc,       \* the poll() call's lifecycle (see PollerPCs).
    mgr_pc,          \* the console_mgr's lifecycle (see MgrPCs).
    mgr_saw,         \* BOOLEAN -- BUGGY path only: the mgr's stale snapshot
                     \*   of pending, taken before it commits to sleep.
    spurious_used    \* BOOLEAN -- the one modeled benign mgr wake has fired.

vars == <<data, pending, registered, flagged, poller_pc, mgr_pc,
          mgr_saw, spurious_used>>

\* Poller: "start"      -- poll() entered; no hook installed.
\*         "registered" -- hook installed, readiness sampled; the evaluate
\*                         point (first entry and every resume from a wake).
\*         "sleeping"   -- parked on the poller's private Rendez.
\*         "done"       -- poll returned a ready revent.
PollerPCs      == {"start", "registered", "sleeping", "done"}
PollerTerminal == {"done"}

\* console_mgr: "sleeping"  -- parked on mgr_rendez.
\*              "awake"     -- running its loop: drain the flag, walk hooks,
\*                            or head back to sleep.
\*              "deciding"  -- BUGGY path only: between snapshotting pending
\*                            and committing to sleep (the lost-wake window).
MgrPCs == {"sleeping", "awake", "deciding"}

TypeOk ==
    /\ data          \in BOOLEAN
    /\ pending       \in BOOLEAN
    /\ registered    \in BOOLEAN
    /\ flagged       \in BOOLEAN
    /\ poller_pc     \in PollerPCs
    /\ mgr_pc        \in MgrPCs
    /\ mgr_saw       \in BOOLEAN
    /\ spurious_used \in BOOLEAN

(***************************************************************************)
(* The poller has just called poll; the console is empty, no hook is       *)
(* installed, no flag set, no IRQ pending; the console_mgr is parked.       *)
(***************************************************************************)
Init ==
    /\ data          = FALSE
    /\ pending       = FALSE
    /\ registered    = FALSE
    /\ flagged       = FALSE
    /\ poller_pc     = "start"
    /\ mgr_pc        = "sleeping"
    /\ mgr_saw       = FALSE
    /\ spurious_used = FALSE

(***************************************************************************)
(* DataArrives -- the RX IRQ producer. A byte enters the ring (the POLLIN  *)
(* edge), the poll_wake_pending flag is set under g_cons.lock, and the     *)
(* console_mgr's Rendez is woken: a sleeping mgr is re-scheduled to its    *)
(* loop. A mgr that is already "awake" (or "deciding") is untouched -- the *)
(* wakeup finds no enqueued waiter, exactly as the impl. Monotonic: fires  *)
(* once. Gated to the pre-terminal space (after poll returns, no one polls)*)
(***************************************************************************)
DataArrives ==
    /\ poller_pc \notin PollerTerminal
    /\ ~data
    /\ data'    = TRUE
    /\ pending' = TRUE
    /\ mgr_pc'  = IF mgr_pc = "sleeping" THEN "awake" ELSE mgr_pc
    /\ UNCHANGED <<registered, flagged, poller_pc, mgr_saw, spurious_used>>

(***************************************************************************)
(* PollerRegister -- the CORRECT poll entry (poll.tla's Register, one fd). *)
(* `dev->poll` installs the hook AND samples the console's readiness in    *)
(* one locked step: register-then-observe, so no readiness event slips     *)
(* between the sample and the hook going live. The sampled readiness is    *)
(* the initial flag.                                                        *)
(***************************************************************************)
PollerRegister ==
    /\ poller_pc = "start"
    /\ poller_pc'  = "registered"
    /\ registered' = TRUE
    /\ flagged'    = data
    /\ UNCHANGED <<data, pending, mgr_pc, mgr_saw, spurious_used>>

(***************************************************************************)
(* PollerCommit -- the evaluate point (first entry and every resume). A    *)
(* `tsleep` on the poller's Rendez: the flag scan and the sleep transition *)
(* are atomic under that Rendez lock. Flag set -> return ready (unhook).   *)
(* Else (poll(-1)) -> sleep. (The timeout branch is tsleep.tla's.)         *)
(***************************************************************************)
PollerCommit ==
    /\ poller_pc = "registered"
    /\ IF flagged
       THEN /\ poller_pc'  = "done"
            /\ registered' = FALSE      \* unhook on return (NoStaleHook)
       ELSE /\ poller_pc'  = "sleeping"
            /\ registered' = registered
    /\ UNCHANGED <<data, pending, flagged, mgr_pc, mgr_saw, spurious_used>>

(***************************************************************************)
(* MgrDrainWalk -- the console_mgr, awake with the flag set, drains it     *)
(* (under g_cons.lock) and walks the poll-hook list (`poll_waiter_list_-   *)
(* wake`, process context). The walk sets each registered poller's flag    *)
(* AND wakes its Rendez. Here: if the console is ready and the poller is   *)
(* registered, set its flag and re-schedule a sleeping poller to its       *)
(* evaluate point. The mgr stays awake to loop.                            *)
(***************************************************************************)
MgrDrainWalk ==
    /\ mgr_pc = "awake"
    /\ pending
    /\ pending' = FALSE
    /\ IF data /\ registered
       THEN /\ flagged'   = TRUE
            /\ poller_pc' = IF poller_pc = "sleeping" THEN "registered"
                                                      ELSE poller_pc
       ELSE /\ flagged'   = flagged
            /\ poller_pc' = poller_pc
    /\ UNCHANGED <<data, registered, mgr_pc, mgr_saw, spurious_used>>

(***************************************************************************)
(* MgrSleep -- the CORRECT go-to-sleep: register-then-observe. The mgr     *)
(* enqueues on its Rendez and re-checks `pending` under the lock in ONE    *)
(* atomic step (the `sleep(&mgr_rendez, cons_mgr_pending)` contract,       *)
(* scheduler.tla). The ~pending guard IS that re-check: a producer that    *)
(* set pending is a separate step that either precedes this (the guard     *)
(* fails, the mgr drains instead) or follows it (the mgr is enqueued, so   *)
(* the wakeup re-schedules it). No flag set in the window is lost.          *)
(***************************************************************************)
MgrSleep ==
    /\ ~BUGGY_MGR_LOST_WAKE
    /\ mgr_pc = "awake"
    /\ ~pending
    /\ mgr_pc' = "sleeping"
    /\ UNCHANGED <<data, pending, registered, flagged, poller_pc,
                   mgr_saw, spurious_used>>

(***************************************************************************)
(* MgrObserve / MgrCommitSleep -- the BUGGY hand-rolled check-then-sleep,  *)
(* split into two steps so a flag set in the gap is lost.                   *)
(*                                                                         *)
(*   MgrObserve     -- the mgr snapshots pending (FALSE: no work seen) and *)
(*                     heads toward sleep. NOT yet enqueued on its Rendez.  *)
(*   MgrCommitSleep -- the mgr commits to sleep on the stale snapshot. A   *)
(*                     `DataArrives` between the two set pending and woke   *)
(*                     the mgr -- but the mgr was "deciding", not enqueued, *)
(*                     so that wake was a no-op (see DataArrives). The mgr  *)
(*                     sleeps with pending TRUE: the relay is dropped.      *)
(***************************************************************************)
MgrObserve ==
    /\ BUGGY_MGR_LOST_WAKE
    /\ mgr_pc = "awake"
    /\ ~pending
    /\ mgr_saw'   = FALSE
    /\ mgr_pc'    = "deciding"
    /\ UNCHANGED <<data, pending, registered, flagged, poller_pc,
                   spurious_used>>

MgrCommitSleep ==
    /\ BUGGY_MGR_LOST_WAKE
    /\ mgr_pc = "deciding"
    /\ mgr_pc' = IF mgr_saw THEN "awake" ELSE "sleeping"
    /\ UNCHANGED <<data, pending, registered, flagged, poller_pc,
                   mgr_saw, spurious_used>>

(***************************************************************************)
(* SpuriousWake -- a benign non-poll wake of the console_mgr (the Ctrl-C / *)
(* SAK path also signals `mgr_rendez`). Fires once; it is what places the  *)
(* mgr in the "awake, about to re-sleep" state where the poll-pending      *)
(* relay race opens. Not fair (incidental).                                 *)
(***************************************************************************)
SpuriousWake ==
    /\ ~spurious_used
    /\ mgr_pc = "sleeping"
    /\ mgr_pc'        = "awake"
    /\ spurious_used' = TRUE
    /\ UNCHANGED <<data, pending, registered, flagged, poller_pc, mgr_saw>>

(***************************************************************************)
(* Done -- terminal self-loop. Once poll has returned, the model halts;    *)
(* the explicit stutter keeps the legitimate terminal state from tripping  *)
(* TLC's -deadlock check, which then stays meaningful for the pre-terminal *)
(* space (where the lost-wake stuck state lives, NOT terminal).             *)
(***************************************************************************)
Done == poller_pc \in PollerTerminal /\ UNCHANGED vars

Next ==
    \/ DataArrives
    \/ PollerRegister
    \/ PollerCommit
    \/ MgrDrainWalk
    \/ MgrSleep
    \/ MgrObserve
    \/ MgrCommitSleep
    \/ SpuriousWake
    \/ Done

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

\* FlagImpliesReady -- the poller's flag is set only for a genuinely ready
\* console: poll never reports POLLIN for an empty ring (no spurious return).
FlagImpliesReady == flagged => data

\* NoMissedConsPoll -- ARCH §28 I-9 across the deferred relay: the poller is
\* never left asleep on a ready, registered console with the relay quiescent
\* (the mgr also asleep, no flag). In the correct model this is unreachable
\* -- while a poll wake is pending the mgr is awake-or-draining (the ~pending
\* MgrSleep guard), and the drain that clears pending also flags the poller.
\* BUGGY_MGR_LOST_WAKE reaches it: the mgr sleeps with pending TRUE.
NoMissedConsPoll ==
    ~( data /\ registered /\ poller_pc = "sleeping"
       /\ ~flagged /\ mgr_pc = "sleeping" )

\* NoStaleHook -- a returned poll holds no poll_waiter hook (poll.tla's
\* property, re-checked here: the relay must not leave a dangling hook).
NoStaleHook == (poller_pc \in PollerTerminal) => ~registered

\* DoneSound -- poll returns ready only with the flag actually set.
DoneSound == (poller_pc = "done") => flagged

Invariants ==
    /\ TypeOk
    /\ FlagImpliesReady
    /\ NoMissedConsPoll
    /\ NoStaleHook
    /\ DoneSound

(***************************************************************************)
(* ============================== LIVENESS ================================ *)
(*                                                                         *)
(* PollerEventuallyServed -- once the console is ready and the poller has  *)
(* registered, poll eventually returns. The whole point of the relay: a    *)
(* ready console must reach a parked poller through the IRQ -> mgr -> hook  *)
(* chain. Fairness grants the producer NOTHING beyond the single           *)
(* DataArrives edge; progress rides the mgr relay and the poller's         *)
(* re-evaluation:                                                           *)
(*                                                                         *)
(*   WF(MgrDrainWalk) -- an awake mgr with a pending flag eventually       *)
(*                       drains it and walks the hook list.                 *)
(*   WF(PollerRegister), WF(PollerCommit) -- the poller eventually         *)
(*                       registers and re-evaluates after a wake.           *)
(*                                                                         *)
(* Under BUGGY_MGR_LOST_WAKE this FAILS: the relay drops, the poller never *)
(* leaves "sleeping". (cons_poll_liveness.cfg runs the clean spec.)        *)
(***************************************************************************)
PollerEventuallyServed ==
    (data /\ registered) ~> (poller_pc \in PollerTerminal)

Liveness ==
    /\ WF_vars(PollerRegister)
    /\ WF_vars(PollerCommit)
    /\ WF_vars(MgrDrainWalk)

Spec_Live == Init /\ [][Next]_vars /\ Liveness

====
