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
(* THE EPISODE AND THE RE-ARM (2026-09-21, B-0 audit round 4, F1)          *)
(*                                                                         *)
(*   IM-1's trusted episode gives the console a SECOND hook list: a caller *)
(*   frozen by an open episode registers on `episode_poll_list`, which the *)
(*   per-byte relay never walks, so a frozen poller is not woken once per  *)
(*   secret keystroke. The list is chosen by state AT REGISTER TIME. That  *)
(*   was sound while poll RETURNED on every wake (the caller re-polled and *)
(*   re-registered on the list its new state called for). Since the poll   *)
(*   re-arm (poll.tla: a wake is a hint; an empty re-sample sleeps AGAIN)  *)
(*   a hook that only re-SAMPLES stays on the list it was put on: a caller *)
(*   that registered frozen, woken at END with nothing buffered, re-sleeps *)
(*   on the episode list -- and the keystrokes after END are relayed to    *)
(*   the other one. The fix is at the poll core: every re-arm pass         *)
(*   UNREGISTERS and RE-REGISTERS, so a Dev re-chooses its list for the    *)
(*   state it samples. The same re-registration keeps the privacy half:    *)
(*   a caller that registered BEFORE the SAK is moved to the episode list  *)
(*   by BEGIN's walk, so no secret keystroke's relay reaches it.            *)
(*                                                                         *)
(*   BUGGY_NO_REREGISTER -- the as-built d5c58d76 re-arm (sample-only      *)
(*     re-sample; hooks stay put). NoMissedConsPoll counterexample: Begin, *)
(*     Register(frozen -> E), sleep, End, wake, empty re-sample, re-sleep  *)
(*     on E, a key: the relay walks P only.                                 *)
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
(*   cons_poll_buggy_no_reregister.cfg BUGGY_NO_REREGISTER --               *)
(*                                   NoMissedConsPoll counterexample: a     *)
(*                                   poller stranded on the episode list.    *)
(*   cons_poll_buggy_no_reregister_cadence.cfg BUGGY_NO_REREGISTER --       *)
(*                                   NoSecretCadence counterexample: a      *)
(*                                   pre-SAK poller left on poll_list gets  *)
(*                                   one pass per secret keystroke.          *)
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
(*   poll.tla's assumption). It arrives only while no episode is open: a   *)
(*   keystroke DURING one belongs to the trusted reader, and BEGIN        *)
(*   discards the ring.                                                     *)
(*                                                                         *)
(*   One episode (`episode_used`), which is enough for both halves: a      *)
(*   poller that registers before it (the privacy half) and one that       *)
(*   registers during it (the liveness half). A frozen caller samples no   *)
(*   readiness at all (IM-1: POLLIN would leak the count of key bytes).     *)
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
    BUGGY_MGR_LOST_WAKE,  \* BOOLEAN -- TRUE: the console_mgr's go-to-sleep
                          \*   is a hand-rolled check-then-sleep (observe
                          \*   poll_wake_pending, then commit), not the
                          \*   register-then-observe sleep(). A flag set in
                          \*   the gap is lost.

    BUGGY_NO_REREGISTER   \* BOOLEAN -- TRUE: the poll re-arm re-SAMPLES but
                          \*   leaves the hook on the list it was registered on
                          \*   (the as-built d5c58d76 loop).

ASSUME BUGGY_MGR_LOST_WAKE \in BOOLEAN
ASSUME BUGGY_NO_REREGISTER \in BOOLEAN

VARIABLES
    data,            \* BOOLEAN -- the console ring holds >= 1 byte (POLLIN
                     \*   readiness). Monotonic FALSE -> TRUE.
    pending,         \* BOOLEAN -- g_cons poll_wake_pending: an RX IRQ asked
                     \*   the mgr to walk the poll-hook list.
    onlist,          \* "none" | "P" | "E" -- which console hook list holds
                     \*   the poller's poll_waiter: poll_list or
                     \*   episode_poll_list.
    flagged,         \* BOOLEAN -- the poller's poll_waiter ready flag: a
                     \*   HINT set by any walk of its list (poll.tla).
    poller_pc,       \* the poll() call's lifecycle (see PollerPCs).
    mgr_pc,          \* the console_mgr's lifecycle (see MgrPCs).
    mgr_saw,         \* BOOLEAN -- BUGGY path only: the mgr's stale snapshot
                     \*   of pending, taken before it commits to sleep.
    spurious_used,   \* BOOLEAN -- the one modeled benign mgr wake has fired.
    frozen,          \* BOOLEAN -- an episode is open (IM-1): the poller's
                     \*   Proc is not the attached one, so it is FROZEN.
    episode_used,    \* BOOLEAN -- the one modeled episode has begun.
    secret_woke      \* BOOLEAN -- history: a keystroke relayed DURING the
                     \*   episode reached the frozen poller's hook.

vars == <<data, pending, onlist, flagged, poller_pc, mgr_pc,
          mgr_saw, spurious_used, frozen, episode_used, secret_woke>>

\* Poller: "start"      -- poll() entered; no hook installed.
\*         "registered" -- hook installed, readiness sampled; the evaluate
\*                         point (first entry and every re-arm pass).
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

\* The readiness a sample reports: none at all for a frozen caller.
Ready == data /\ ~frozen

\* The list cons_poll registers on, chosen by the state it samples.
ListFor == IF frozen THEN "E" ELSE "P"

registered == onlist # "none"

TypeOk ==
    /\ data          \in BOOLEAN
    /\ pending       \in BOOLEAN
    /\ onlist        \in {"none", "P", "E"}
    /\ flagged       \in BOOLEAN
    /\ poller_pc     \in PollerPCs
    /\ mgr_pc        \in MgrPCs
    /\ mgr_saw       \in BOOLEAN
    /\ spurious_used \in BOOLEAN
    /\ frozen        \in BOOLEAN
    /\ episode_used  \in BOOLEAN
    /\ secret_woke   \in BOOLEAN

Init ==
    /\ data          = FALSE
    /\ pending       = FALSE
    /\ onlist        = "none"
    /\ flagged       = FALSE
    /\ poller_pc     = "start"
    /\ mgr_pc        = "sleeping"
    /\ mgr_saw       = FALSE
    /\ spurious_used = FALSE
    /\ frozen        = FALSE
    /\ episode_used  = FALSE
    /\ secret_woke   = FALSE

(***************************************************************************)
(* DataArrives -- the RX IRQ producer, outside an episode. A byte enters   *)
(* the ring (the POLLIN edge), poll_wake_pending is set under g_cons.lock, *)
(* and the console_mgr's Rendez is woken. Monotonic: fires once.            *)
(***************************************************************************)
DataArrives ==
    /\ poller_pc \notin PollerTerminal
    /\ ~data
    /\ ~frozen
    /\ data'    = TRUE
    /\ pending' = TRUE
    /\ mgr_pc'  = IF mgr_pc = "sleeping" THEN "awake" ELSE mgr_pc
    /\ UNCHANGED <<onlist, flagged, poller_pc, mgr_saw, spurious_used,
                   frozen, episode_used, secret_woke>>

(***************************************************************************)
(* SecretKey -- a keystroke DURING the episode. It is the trusted reader's *)
(* (corvus consumes it), but the RX IRQ relays a poll wake like any byte:  *)
(* pending is set and the mgr woken. What the privacy half pins is whose   *)
(* hook that relay reaches. Not fair; may repeat.                           *)
(***************************************************************************)
SecretKey ==
    /\ poller_pc \notin PollerTerminal
    /\ frozen
    /\ pending' = TRUE
    /\ mgr_pc'  = IF mgr_pc = "sleeping" THEN "awake" ELSE mgr_pc
    /\ UNCHANGED <<data, onlist, flagged, poller_pc, mgr_saw, spurious_used,
                   frozen, episode_used, secret_woke>>

(***************************************************************************)
(* Begin / End -- the SAK opens the episode, SYS_CONSOLE_EPISODE_END (or   *)
(* the trusted Proc's death) closes it. BEGIN discards the ring. Each      *)
(* transition walks BOTH lists (cons_episode_wake_all), so a hook on       *)
(* either is flagged and a sleeping poller resumes to its evaluate point.  *)
(***************************************************************************)
WalkBoth ==
    /\ flagged'   = (flagged \/ registered)
    /\ poller_pc' = IF poller_pc = "sleeping" /\ registered
                    THEN "registered" ELSE poller_pc

Begin ==
    /\ ~episode_used
    /\ poller_pc \notin PollerTerminal
    /\ ~data
    /\ frozen'       = TRUE
    /\ episode_used' = TRUE
    /\ WalkBoth
    /\ UNCHANGED <<data, pending, onlist, mgr_pc, mgr_saw, spurious_used,
                   secret_woke>>

End ==
    /\ frozen
    /\ frozen' = FALSE
    /\ WalkBoth
    /\ UNCHANGED <<data, pending, onlist, mgr_pc, mgr_saw, spurious_used,
                   episode_used, secret_woke>>

(***************************************************************************)
(* PollerRegister -- the poll entry (poll.tla's Register, one fd):         *)
(* cons_poll registers on the list its sampled state calls for and samples *)
(* readiness in one step under g_cons.lock -- register-then-observe.       *)
(***************************************************************************)
PollerRegister ==
    /\ poller_pc = "start"
    /\ poller_pc' = "registered"
    /\ onlist'    = ListFor
    /\ flagged'   = Ready
    /\ UNCHANGED <<data, pending, mgr_pc, mgr_saw, spurious_used,
                   frozen, episode_used, secret_woke>>

(***************************************************************************)
(* PollerEvaluate -- the evaluate point, first entry and every re-arm pass *)
(* (poll.tla: a flag is a HINT). A flag set -> re-arm: the flag cleared,   *)
(* readiness re-sampled; ready -> return (unhook). Not ready -> sleep      *)
(* again (poll(-1): 0 only at a deadline, and there is none here). The     *)
(* CORRECT re-arm re-REGISTERS in the same step -- the hook moves to the   *)
(* list its new state calls for. BUGGY_NO_REREGISTER leaves it where it    *)
(* was put. No flag -> sleep (the tsleep commit, atomic under the Rendez). *)
(***************************************************************************)
PollerEvaluate ==
    /\ poller_pc = "registered"
    /\ IF Ready
       THEN /\ poller_pc' = "done"
            /\ onlist'    = "none"
            /\ flagged'   = flagged
       ELSE IF flagged
            THEN /\ poller_pc' = "sleeping"
                 /\ flagged'   = FALSE
                 /\ onlist'    = IF BUGGY_NO_REREGISTER THEN onlist ELSE ListFor
            ELSE /\ poller_pc' = "sleeping"
                 /\ flagged'   = FALSE
                 /\ onlist'    = onlist
    /\ UNCHANGED <<data, pending, mgr_pc, mgr_saw, spurious_used,
                   frozen, episode_used, secret_woke>>

(***************************************************************************)
(* MgrDrainWalk -- the console_mgr drains pending and walks poll_list (the *)
(* per-byte relay walks ONLY poll_list; that is the episode design). The   *)
(* walk flags every hook on it (a hint) and wakes a sleeping poller. A     *)
(* walk that reaches the poller while it is frozen during the episode AND  *)
(* finds its flag clear is recorded: that walk buys the frozen poller one  *)
(* more pass, i.e. a secret keystroke's cadence arriving at its hook. (A   *)
(* flag already set -- BEGIN's own walk, not yet consumed -- gains nothing.)*)
(***************************************************************************)
MgrDrainWalk ==
    /\ mgr_pc = "awake"
    /\ pending
    /\ pending' = FALSE
    /\ IF onlist = "P"
       THEN /\ flagged'     = TRUE
            /\ poller_pc'   = IF poller_pc = "sleeping" THEN "registered"
                                                        ELSE poller_pc
            /\ secret_woke' = (secret_woke \/ (frozen /\ ~flagged))
       ELSE /\ flagged'     = flagged
            /\ poller_pc'   = poller_pc
            /\ secret_woke' = secret_woke
    /\ UNCHANGED <<data, onlist, mgr_pc, mgr_saw, spurious_used,
                   frozen, episode_used>>

MgrSleep ==
    /\ ~BUGGY_MGR_LOST_WAKE
    /\ mgr_pc = "awake"
    /\ ~pending
    /\ mgr_pc' = "sleeping"
    /\ UNCHANGED <<data, pending, onlist, flagged, poller_pc,
                   mgr_saw, spurious_used, frozen, episode_used, secret_woke>>

MgrObserve ==
    /\ BUGGY_MGR_LOST_WAKE
    /\ mgr_pc = "awake"
    /\ ~pending
    /\ mgr_saw'   = FALSE
    /\ mgr_pc'    = "deciding"
    /\ UNCHANGED <<data, pending, onlist, flagged, poller_pc,
                   spurious_used, frozen, episode_used, secret_woke>>

MgrCommitSleep ==
    /\ BUGGY_MGR_LOST_WAKE
    /\ mgr_pc = "deciding"
    /\ mgr_pc' = IF mgr_saw THEN "awake" ELSE "sleeping"
    /\ UNCHANGED <<data, pending, onlist, flagged, poller_pc,
                   mgr_saw, spurious_used, frozen, episode_used, secret_woke>>

SpuriousWake ==
    /\ ~spurious_used
    /\ mgr_pc = "sleeping"
    /\ mgr_pc'        = "awake"
    /\ spurious_used' = TRUE
    /\ UNCHANGED <<data, pending, onlist, flagged, poller_pc, mgr_saw,
                   frozen, episode_used, secret_woke>>

Done == poller_pc \in PollerTerminal /\ UNCHANGED vars

Next ==
    \/ DataArrives
    \/ SecretKey
    \/ Begin
    \/ End
    \/ PollerRegister
    \/ PollerEvaluate
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

\* NoMissedConsPoll -- ARCH section 28 I-9 across the deferred relay AND the
\* episode: the poller is never left asleep, hooked, unflagged, on a console
\* that is ready for it, with the relay quiescent. The correct model cannot
\* reach it: while a poll wake is pending the mgr is awake, and the walk that
\* clears pending flags the poller -- provided the poller is on the list the
\* relay walks, which the re-registering re-arm guarantees whenever it is not
\* frozen. BUGGY_MGR_LOST_WAKE reaches it (the relay drops), and so does
\* BUGGY_NO_REREGISTER (the poller sits on the episode list after END).
NoMissedConsPoll ==
    ~( Ready /\ registered /\ poller_pc = "sleeping"
       /\ ~flagged /\ mgr_pc = "sleeping" )

\* NoSecretCadence -- the IM-1 privacy half: no relay of a keystroke made
\* during the episode ever reaches the frozen poller's hook. With the
\* re-registering re-arm, BEGIN's walk moves a poller that registered before
\* the SAK onto the episode list.
NoSecretCadence == ~secret_woke

\* NoStaleHook -- a returned poll holds no hook.
NoStaleHook == (poller_pc \in PollerTerminal) => ~registered

\* DoneSound -- poll returns only a console that is ready FOR THIS CALLER.
DoneSound == (poller_pc = "done") => (data /\ ~frozen)

Invariants ==
    /\ TypeOk
    /\ NoMissedConsPoll
    /\ NoSecretCadence
    /\ NoStaleHook
    /\ DoneSound

(***************************************************************************)
(* ============================== LIVENESS ================================ *)
(* PollerEventuallyServed -- a console ready for the poller (bytes, no     *)
(* episode) with a hook installed is eventually returned. Fairness grants  *)
(* the producers nothing; progress rides the relay and the re-arm.         *)
(***************************************************************************)
PollerEventuallyServed ==
    (Ready /\ registered) ~> (poller_pc \in PollerTerminal)

Liveness ==
    /\ WF_vars(PollerRegister)
    /\ WF_vars(PollerEvaluate)
    /\ WF_vars(MgrDrainWalk)

Spec_Live == Init /\ [][Next]_vars /\ Liveness

====
