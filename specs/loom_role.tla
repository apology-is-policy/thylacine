---- MODULE loom_role ----
(***************************************************************************)
(* Thylacine Loom -- an ENTER waits for the 9P READER ROLE (LOOM.md 8.6     *)
(* item 2; DEBUG-FS-DESIGN 5c.6; OPEN-BUGS 2026-09-30 11:01Z).              *)
(*                                                                         *)
(* A thread in SYS_LOOM_ENTER (min_complete >= 1) waits for the CQE of an  *)
(* async op in flight on a 9P client. Only the thread holding the client's *)
(* READER ROLE reads replies (ARCH 21.10, the #841 elected reader), and a  *)
(* dev9p client is SHARED, so the role can be held by another Proc's       *)
(* synchronous call. The ENTER's pump (p9_client_reader_pump_once) takes   *)
(* the role when it is free and demuxes ONE frame; when the role is held   *)
(* it returns 0 and the ENTER sleeps.                                      *)
(*                                                                         *)
(* THE DEFECT (pre-fix; BUGGY_NO_ROLE_HOOK). The ENTER slept on the ring's *)
(* CQ wait-list only. A synchronous reader departs once its own reply      *)
(* lands, and its handoff designates only a synchronous waiter (an async   *)
(* op has no thread to read for it). With none waiting it left the role    *)
(* free, nobody read the ENTER's reply, and the ENTER slept forever.       *)
(*                                                                         *)
(* THE MECHANISM. An ENTER whose pump finds the role held also hooks the   *)
(* client's role-waiter list, under c->lock with the role re-sampled       *)
(* (p9_client_role_wait_register: free -> re-pump; held -> hooked), then   *)
(* hooks the CQ list as before and sleeps on one Rendez with both hooks    *)
(* (loom_cqw_role_cond). A handoff that leaves the role free with nobody   *)
(* designated wakes the role list (client_handoff_reader_locked's exit),   *)
(* and the woken ENTER re-pumps.                                           *)
(*                                                                         *)
(* WHY A FOCUSED MODULE. loom.tla's ReplyArrives is enabled for any op in  *)
(* flight, under weak fairness: its liveness PRESUMES that some thread     *)
(* reads the reply. That thread is the 9P client's reader, which loom.tla  *)
(* does not model. This module discharges the premise for the ENTER's own  *)
(* pump and leaves loom.tla and its cfgs untouched (the loom_multishot /   *)
(* loom_order / loom_devgone precedent). It is also the first model of the *)
(* handoff itself, so it carries the two stop rules the ENTER's wake       *)
(* depends on: the handoff skips a thread parked for a stop (stop_parked), *)
(* and a stopped designee hands the role on before it parks.               *)
(*                                                                         *)
(* THE ACTORS                                                               *)
(*                                                                         *)
(*   The role: holder = NONE (c->reader_active false), ENTER, or a sync op. *)
(*   Each sync op s in Syncs (client_wait; sph[s]):                         *)
(*     "idle"     -- not yet in client_wait (also: still sending, which the *)
(*                   handoff skips and which self-elects on arrival);       *)
(*     "wait"     -- runnable at client_wait's loop top;                    *)
(*     "reading"  -- holds the role, blocked in the transport recv;         *)
(*     "sleeping" -- the non-reader sleep (cond: done or be_reader);        *)
(*     "parked"   -- parked for a stop (client_debug_stop_park,             *)
(*                   stop_parked set); leaves only on a resume, which may   *)
(*                   never come;                                            *)
(*     "done"     -- returned.                                              *)
(*   sbe[s] = rpc->be_reader (designated), sdone[s] = rpc->done.            *)
(*   The ENTER (loom_wait_for_completions, a non-SQPOLL ring; eph):         *)
(*     "top"      -- the CQ sample: the CQE posted -> "returned";           *)
(*     "pump"     -- pump_once under c->lock: free -> take it ("reading");  *)
(*                   held -> rc 0 ("hook");                                 *)
(*     "hook"     -- p9_client_role_wait_register under c->lock: free ->    *)
(*                   "top" (re-pump); held -> hooked, flag cleared;         *)
(*     "cqreg"    -- CqWaitRegister: hook the CQ list and sample the CQ     *)
(*                   under l->lock (posted -> skip the sleep);              *)
(*     "sleep"    -- the sleep's cond under the ENTER's Rendez lock: a      *)
(*                   flag set -> "unhook", else block;                      *)
(*     "sleeping" -- blocked; a waker sets a flag and makes it runnable;    *)
(*     "unhook"   -- both hooks off, then "top";                            *)
(*     "reading"  -- holds the role: one frame, then the handoff;           *)
(*     "returned".                                                          *)
(*   The server replies to each sent request once (replied, wire).          *)
(*                                                                         *)
(* EVERY RELEASE OF THE ROLE RUNS THE HANDOFF. The four sites that clear   *)
(* c->reader_active (client_wait's reader loop, client_pump_or_park_locked *)
(* -- the send path's one-frame self-pump --, pump_once, pump_once_deadline*)
(* ) each call client_handoff_reader_locked in the same c->lock hold.      *)
(* Here a sync reader departs on its own reply (ReadFrame) or at a frame   *)
(* boundary on a stop (StopReader, which also stands for every other early *)
(* departure: a caught note, a death and the send path's self-pump all     *)
(* hand off without the reader's own reply); the ENTER departs after its   *)
(* one frame (EnterRead).                                                   *)
(*                                                                         *)
(* ABSTRACTIONS. The wire is a SET: the server orders its replies freely,  *)
(* so reading any sent reply over-approximates the FIFO. The handoff picks *)
(* ANY designable op (the code: the lowest tag). A frame is atomic (a stop *)
(* mid-frame blocks through to the boundary; reader_frame.tla). The        *)
(* ENTER's sample, its client pick and its pump are two steps, top and     *)
(* pump, which keeps the window between them.                              *)
(*                                                                         *)
(* OUT OF SCOPE. Session death: client_mark_dead_locked completes the      *)
(* async op with an error CQE (loom_devgone.tla) and wakes both lists, and *)
(* a dead client's register returns -P9_E_IO. SQPOLL: its kthread is the   *)
(* ring's sole driver and never hooks the role. A stop of the ENTER's own  *)
(* thread (its pump unwinds with -P9_E_IO; its sleep parks in place and    *)
(* keeps its flags). The flood budget. A second ENTER: each has its own    *)
(* hooks on the same lists, woken independently (poll.tla's argument).     *)
(* ONE CLIENT PER RING: the ENTER pumps and hooks the client of the ring's *)
(* first in-flight op (loom_first_inflight_client), and the one ASYNC op   *)
(* here lives on that client. A ring whose ops span clients is a separate, *)
(* pre-existing strand (OPEN-BUGS 2026-10-05 07:52Z): another client's     *)
(* reply is never read while the first client is held or slow.             *)
(*                                                                         *)
(* THE KNOWN RESIDUAL (OPEN-BUGS (E), P3). An ENTER that pumps after       *)
(* another reader already read its op's reply blocks in the transport recv *)
(* with nothing due: a reader in the recv is blind to progress made        *)
(* elsewhere. EnterReturns excepts exactly that state (Blind), and         *)
(* loom_role_residual_blind.cfg shows it is reachable; when (E) is fixed,  *)
(* the exception goes and that cfg turns clean. BlindImpliesCq pins the    *)
(* exception to (E)'s sample->pump race: a blind ENTER's CQE is posted.    *)
(*                                                                         *)
(* PROPERTIES                                                               *)
(*   NoMissedRoleWake (the headline, I-9 on the role list): the ENTER never *)
(*     sleeps while the role is free and no sync op is designated to take   *)
(*     it -- nobody else would read its reply. Stated without "hooked", so *)
(*     the pre-fix ENTER violates it too.                                   *)
(*   NoMissedCqWake: the ENTER never sleeps past its own CQE.               *)
(*   BlindImpliesCq: the (E) carve-out covers only a posted CQE.            *)
(*   EnterReturns (liveness): the ENTER returns, or ends Blind for good.    *)
(*                                                                         *)
(* BUGGY CONFIGS                                                            *)
(*   BUGGY_NO_ROLE_HOOK           the pre-fix ENTER; EnterReturns violated  *)
(*                                (its cfg checks only the property, which  *)
(*                                proves the liveness check discriminates). *)
(*   BUGGY_ROLE_LATE_REGISTER     the hook trusts the pump's stale sample:  *)
(*                                a release between them is missed.         *)
(*   BUGGY_NO_ROLE_WAKE           the no-designee exit wakes nobody.        *)
(*   BUGGY_DESIGNATES_PARKED      the handoff designates a parked thread.   *)
(*   BUGGY_STOP_KEEPS_DESIGNATION a stopped designee parks without handing  *)
(*                                the role on.                              *)
(*   The last four violate NoMissedRoleWake.                                *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Syncs,                         \* foreign synchronous calls on the shared client
    MAX_STOPS,                     \* stops each may take (job or debug)
    BUGGY_NO_ROLE_HOOK,
    BUGGY_ROLE_LATE_REGISTER,
    BUGGY_NO_ROLE_WAKE,
    BUGGY_DESIGNATES_PARKED,
    BUGGY_STOP_KEEPS_DESIGNATION

ASSUME Syncs # {}
ASSUME MAX_STOPS \in Nat
ASSUME BUGGY_NO_ROLE_HOOK           \in BOOLEAN
ASSUME BUGGY_ROLE_LATE_REGISTER     \in BOOLEAN
ASSUME BUGGY_NO_ROLE_WAKE           \in BOOLEAN
ASSUME BUGGY_DESIGNATES_PARKED      \in BOOLEAN
ASSUME BUGGY_STOP_KEEPS_DESIGNATION \in BOOLEAN

NONE  == "none"      \* the role is free
ENTER == "enter"     \* the ENTER holds the role
ASYNC == "async"     \* the ENTER's op (on_complete set: no thread reads for it)
Ops   == Syncs \cup {ASYNC}

SyncPhases  == {"idle", "wait", "reading", "sleeping", "parked", "done"}
EnterPhases == {"top", "pump", "hook", "cqreg", "sleep", "sleeping",
                "unhook", "reading", "returned"}

VARIABLES
    holder,    \* NONE, ENTER or a sync op: who holds the reader role
    sph,       \* [Syncs -> SyncPhases]
    sbe,       \* [Syncs -> BOOLEAN]: rpc->be_reader
    sdone,     \* [Syncs -> BOOLEAN]: rpc->done (the reply is demuxed)
    stops,     \* [Syncs -> 0..MAX_STOPS]: stops still to come
    replied,   \* the ops whose reply the server has sent
    wire,      \* sent replies not yet read
    cq,        \* the ENTER's CQE is posted
    eph,       \* EnterPhases
    cqhook,    \* pw is on l->cq_waiters
    cqflag,    \* pw.ready
    rhook,     \* pw_role is on c->role_waiters_list
    rflag      \* pw_role.ready

vars == <<holder, sph, sbe, sdone, stops, replied, wire, cq, eph,
          cqhook, cqflag, rhook, rflag>>

\* A request is on its way to the server once its op is in client_wait; the
\* ENTER's op was submitted before the wait began. (An IF, not a disjunction:
\* TLC splits an action-level \/ into branches and would apply sph to ASYNC.)
Sent(o) == IF o = ASYNC THEN TRUE ELSE sph[o] # "idle"

TypeOK ==
    /\ holder  \in Syncs \cup {NONE, ENTER}
    /\ sph     \in [Syncs -> SyncPhases]
    /\ sbe     \in [Syncs -> BOOLEAN]
    /\ sdone   \in [Syncs -> BOOLEAN]
    /\ stops   \in [Syncs -> 0..MAX_STOPS]
    /\ replied \subseteq Ops
    /\ wire    \subseteq replied
    /\ cq      \in BOOLEAN
    /\ eph     \in EnterPhases
    /\ cqhook  \in BOOLEAN
    /\ cqflag  \in BOOLEAN
    /\ rhook   \in BOOLEAN
    /\ rflag   \in BOOLEAN

Init ==
    /\ holder  = NONE
    /\ sph     = [s \in Syncs |-> "idle"]
    /\ sbe     = [s \in Syncs |-> FALSE]
    /\ sdone   = [s \in Syncs |-> FALSE]
    /\ stops   = [s \in Syncs |-> MAX_STOPS]
    /\ replied = {}
    /\ wire    = {}
    /\ cq      = FALSE
    /\ eph     = "top"
    /\ cqhook  = FALSE
    /\ cqflag  = FALSE
    /\ rhook   = FALSE
    /\ rflag   = FALSE

(***************************************************************************)
(* The handoff (client_handoff_reader_locked, c->lock held). Designate one *)
(* sync op that is not the departing one, not done, not yet designated, in *)
(* client_wait (not sending) and not parked for a stop; the active reader  *)
(* qualifies when another thread runs the handoff (the designation then    *)
(* lands on nothing). A sleeping designee wakes. With nobody to designate, *)
(* a FREE role wakes the role list. Applied to the intermediate state      *)
(* (ph, be, dn, ep) a step has already produced; `hold` is the holder      *)
(* after the step. Sets sph', sbe', eph', rflag'.                          *)
(***************************************************************************)
DesignablePhases ==
    {"wait", "sleeping", "reading"}
        \cup (IF BUGGY_DESIGNATES_PARKED THEN {"parked"} ELSE {})

Designable(d, departing, ph, be, dn) ==
    /\ d # departing
    /\ ph[d] \in DesignablePhases
    /\ ~dn[d]
    /\ ~be[d]

Handoff(departing, hold, ph, be, dn, ep) ==
    LET cands == {d \in Syncs : Designable(d, departing, ph, be, dn)}
    IN  IF cands # {}
        THEN \E d \in cands :
               /\ sbe'   = [be EXCEPT ![d] = TRUE]
               /\ sph'   = IF ph[d] = "sleeping" THEN [ph EXCEPT ![d] = "wait"]
                                                 ELSE ph
               /\ eph'   = ep
               /\ rflag' = rflag
        ELSE /\ sbe' = be
             /\ sph' = ph
             /\ IF hold = NONE /\ rhook /\ ~BUGGY_NO_ROLE_WAKE
                THEN /\ rflag' = TRUE
                     /\ eph'   = IF ep = "sleeping" THEN "sleep" ELSE ep
                ELSE /\ rflag' = rflag
                     /\ eph'   = ep

(***************************************************************************)
(* The demux of o's reply (demux_frame_locked): a sync reply is stored and *)
(* wakes its sleeping owner; the async reply fires on_complete ->          *)
(* loom_post_cqe, which posts the CQE and wakes the CQ list (loom.tla's    *)
(* PostCqe).                                                                *)
(***************************************************************************)
DemuxPh(o, ph)  == IF o # ASYNC /\ ph[o] = "sleeping" THEN [ph EXCEPT ![o] = "wait"]
                                                    ELSE ph
DemuxDn(o, dn)  == IF o # ASYNC THEN [dn EXCEPT ![o] = TRUE] ELSE dn
DemuxCq(o)      == cq \/ o = ASYNC
DemuxCqflag(o)  == IF o = ASYNC /\ cqhook THEN TRUE ELSE cqflag
DemuxEph(o, ep) == IF o = ASYNC /\ cqhook /\ ep = "sleeping" THEN "sleep" ELSE ep

(***************************************************************************)
(* The server.                                                              *)
(***************************************************************************)
ServerReply(o) ==
    /\ Sent(o)
    /\ o \notin replied
    /\ replied' = replied \cup {o}
    /\ wire'    = wire \cup {o}
    /\ UNCHANGED <<holder, sph, sbe, sdone, stops, cq, eph,
                   cqhook, cqflag, rhook, rflag>>

(***************************************************************************)
(* The sync ops.                                                            *)
(***************************************************************************)
\* The op is sent and reaches client_wait (sending is folded in: the handoff
\* skips a sender, and a sender self-elects on arrival).
Start(s) ==
    /\ sph[s] = "idle"
    /\ sph' = [sph EXCEPT ![s] = "wait"]
    /\ UNCHANGED <<holder, sbe, sdone, stops, replied, wire, cq, eph,
                   cqhook, cqflag, rhook, rflag>>

\* client_wait's loop top with no stop pending: the reply stored -> return;
\* the role free -> become the reader; held -> clear a stale designation (F7)
\* and sleep on the rpc. The rpc-local cond (done or be_reader) is false here:
\* both were just read under c->lock, and a later demux or designation wakes it.
Elect(s) ==
    /\ sph[s] = "wait"
    /\ IF sdone[s]
       THEN /\ sph' = [sph EXCEPT ![s] = "done"]
            /\ UNCHANGED <<holder, sbe>>
       ELSE IF holder = NONE
       THEN /\ holder' = s
            /\ sph'    = [sph EXCEPT ![s] = "reading"]
            /\ sbe'    = [sbe EXCEPT ![s] = FALSE]
       ELSE /\ sph'    = [sph EXCEPT ![s] = "sleeping"]
            /\ sbe'    = [sbe EXCEPT ![s] = FALSE]
            /\ UNCHANGED holder
    /\ UNCHANGED <<sdone, stops, replied, wire, cq, eph,
                   cqhook, cqflag, rhook, rflag>>

\* The sync reader reads a frame and demuxes it. Its own reply ends the loop
\* in the same c->lock hold: release the role, hand it off, return.
ReadFrame(s, o) ==
    /\ holder = s
    /\ o \in wire
    /\ wire'   = wire \ {o}
    /\ sdone'  = DemuxDn(o, sdone)
    /\ cq'     = DemuxCq(o)
    /\ cqflag' = DemuxCqflag(o)
    /\ IF o = s
       THEN /\ holder' = NONE
            /\ Handoff(s, NONE, [DemuxPh(o, sph) EXCEPT ![s] = "done"], sbe,
                       DemuxDn(o, sdone), DemuxEph(o, eph))
       ELSE /\ sph' = DemuxPh(o, sph)
            /\ eph' = DemuxEph(o, eph)
            /\ UNCHANGED <<holder, sbe, rflag>>
    /\ UNCHANGED <<stops, replied, cqhook, rhook>>

\* A stop unwinds the reader's recv at a frame boundary: release, hand off
\* (skipping myself), park role-free -- one c->lock hold.
StopReader(s) ==
    /\ holder = s
    /\ stops[s] > 0
    /\ stops'  = [stops EXCEPT ![s] = @ - 1]
    /\ holder' = NONE
    /\ Handoff(s, NONE, [sph EXCEPT ![s] = "parked"], sbe, sdone, eph)
    /\ UNCHANGED <<sdone, replied, wire, cq, cqhook, cqflag, rhook>>

\* A stop reaches a non-reader: a sleeper unwinds (stop_unwinds) to the loop
\* top, where a stored reply would return first. A designee hands the role on
\* before it parks (the F6 shape); the handoff and the stop_parked set share
\* one c->lock hold.
StopWaiter(s) ==
    /\ sph[s] \in {"wait", "sleeping"}
    /\ ~sdone[s]
    /\ stops[s] > 0
    /\ stops' = [stops EXCEPT ![s] = @ - 1]
    /\ IF sbe[s] /\ ~BUGGY_STOP_KEEPS_DESIGNATION
       THEN Handoff(s, holder, [sph EXCEPT ![s] = "parked"],
                    [sbe EXCEPT ![s] = FALSE], sdone, eph)
       ELSE /\ sph' = [sph EXCEPT ![s] = "parked"]
            /\ UNCHANGED <<sbe, eph, rflag>>
    /\ UNCHANGED <<holder, sdone, replied, wire, cq, cqhook, cqflag, rhook>>

\* The resume: stop_parked cleared under c->lock, then the loop top re-elects.
Resume(s) ==
    /\ sph[s] = "parked"
    /\ sph' = [sph EXCEPT ![s] = "wait"]
    /\ UNCHANGED <<holder, sbe, sdone, stops, replied, wire, cq, eph,
                   cqhook, cqflag, rhook, rflag>>

(***************************************************************************)
(* The ENTER.                                                               *)
(***************************************************************************)
\* The give-up sample: with one op in flight, "ready" and "nothing in flight"
\* are both the CQE being posted.
EnterTop ==
    /\ eph = "top"
    /\ eph' = IF cq THEN "returned" ELSE "pump"
    /\ UNCHANGED <<holder, sph, sbe, sdone, stops, replied, wire, cq,
                   cqhook, cqflag, rhook, rflag>>

\* p9_client_reader_pump_once's entry, under c->lock.
EnterPump ==
    /\ eph = "pump"
    /\ IF holder = NONE
       THEN /\ holder' = ENTER
            /\ eph'    = "reading"
       ELSE /\ eph'    = "hook"
            /\ UNCHANGED holder
    /\ UNCHANGED <<sph, sbe, sdone, stops, replied, wire, cq,
                   cqhook, cqflag, rhook, rflag>>

\* p9_client_role_wait_register, under c->lock: the role re-sampled with the
\* hook, so a release before it is seen and one after it finds the hook.
EnterHook ==
    /\ eph = "hook"
    /\ IF BUGGY_NO_ROLE_HOOK
       THEN /\ eph' = "cqreg"
            /\ UNCHANGED <<rhook, rflag>>
       ELSE IF holder = NONE /\ ~BUGGY_ROLE_LATE_REGISTER
       THEN /\ eph' = "top"
            /\ UNCHANGED <<rhook, rflag>>
       ELSE /\ rhook' = TRUE
            /\ rflag' = FALSE
            /\ eph'   = "cqreg"
    /\ UNCHANGED <<holder, sph, sbe, sdone, stops, replied, wire, cq,
                   cqhook, cqflag>>

\* CqWaitRegister: hook l->cq_waiters and sample the CQ under l->lock.
EnterCqReg ==
    /\ eph = "cqreg"
    /\ cqhook' = TRUE
    /\ cqflag' = FALSE
    /\ eph'    = IF cq THEN "unhook" ELSE "sleep"
    /\ UNCHANGED <<holder, sph, sbe, sdone, stops, replied, wire, cq,
                   rhook, rflag>>

\* The sleep's cond under the ENTER's Rendez lock: loom_cqw_role_cond when the
\* role is hooked, loom_cqw_cond when it is not (rflag is set only on a hook).
EnterSleep ==
    /\ eph = "sleep"
    /\ eph' = IF cqflag \/ (rhook /\ rflag) THEN "unhook" ELSE "sleeping"
    /\ UNCHANGED <<holder, sph, sbe, sdone, stops, replied, wire, cq,
                   cqhook, cqflag, rhook, rflag>>

\* poll_waiter_list_unregister + p9_client_role_wait_unregister; a flag set
\* after is stale, and the next register clears it.
EnterUnhook ==
    /\ eph = "unhook"
    /\ cqhook' = FALSE
    /\ rhook'  = FALSE
    /\ eph'    = "top"
    /\ UNCHANGED <<holder, sph, sbe, sdone, stops, replied, wire, cq,
                   cqflag, rflag>>

\* The ENTER holds the role: it reads one frame, demuxes it and departs
\* through the handoff (pump_once is one-shot). Its hooks are off here.
EnterRead(o) ==
    /\ eph = "reading"
    /\ o \in wire
    /\ wire'   = wire \ {o}
    /\ sdone'  = DemuxDn(o, sdone)
    /\ cq'     = DemuxCq(o)
    /\ holder' = NONE
    /\ Handoff(ENTER, NONE, DemuxPh(o, sph), sbe, DemuxDn(o, sdone), "top")
    /\ UNCHANGED <<stops, replied, cqhook, cqflag, rhook>>

Next ==
    \/ \E o \in Ops : ServerReply(o)
    \/ \E s \in Syncs : Start(s)
    \/ \E s \in Syncs : Elect(s)
    \/ \E s \in Syncs, o \in Ops : ReadFrame(s, o)
    \/ \E s \in Syncs : StopReader(s)
    \/ \E s \in Syncs : StopWaiter(s)
    \/ \E s \in Syncs : Resume(s)
    \/ EnterTop
    \/ EnterPump
    \/ EnterHook
    \/ EnterCqReg
    \/ EnterSleep
    \/ EnterUnhook
    \/ \E o \in Ops : EnterRead(o)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

\* The role has one holder, and the holder is the thread in the recv.
RoleConsistent ==
    /\ \A s \in Syncs : (sph[s] = "reading") <=> (holder = s)
    /\ (eph = "reading") <=> (holder = ENTER)

\* The ENTER's hooks are on their lists only while it waits.
HooksConsistent ==
    /\ cqhook => eph \in {"sleep", "sleeping", "unhook"}
    /\ rhook  => eph \in {"cqreg", "sleep", "sleeping", "unhook"}

\* Blocked, the ENTER holds no unconsumed wake: every waker made it runnable.
SleepingUnflagged == eph = "sleeping" => (~cqflag /\ ~(rhook /\ rflag))

\* The ENTER never sleeps past its own CQE (loom.tla's NoMissedCqWake, here).
NoMissedCqWake == ~(eph = "sleeping" /\ cq)

\* A designee that will run the election, or hand the role on before it parks.
Designated == \E s \in Syncs : sbe[s] /\ sph[s] = "wait" /\ ~sdone[s]

\* THE HEADLINE (I-9 on the role list): the ENTER never sleeps while the role
\* is free and no sync op is designated to take it.
NoMissedRoleWake == ~(eph = "sleeping" /\ holder = NONE /\ ~Designated)

\* The (E) residual: the ENTER blocked in the recv with nothing on the wire and
\* nothing more due from any sent request.
Blind ==
    /\ eph = "reading"
    /\ wire = {}
    /\ \A o \in Ops : Sent(o) => o \in replied

\* The carve-out is exactly (E)'s sample->pump race: another reader posted the
\* CQE after the ENTER sampled, and the ENTER then took the free role.
BlindImpliesCq == Blind => cq

Invariants ==
    /\ TypeOK
    /\ RoleConsistent
    /\ HooksConsistent
    /\ SleepingUnflagged
    /\ NoMissedCqWake
    /\ NoMissedRoleWake
    /\ BlindImpliesCq

\* Expected VIOLATED (loom_role_residual_blind.cfg): Blind is reachable.
NoBlindRecv == ~Blind

(***************************************************************************)
(* ============================== LIVENESS ================================ *)
(*                                                                         *)
(* Weak fairness on the server, the sync ops' election and reads, and      *)
(* every ENTER step. WF on ServerReply is the trusted-server premise       *)
(* loom.tla also makes: every sent request is answered. A deferred-reply   *)
(* server (a parked socket read) breaks it by design, and EnterReturns is  *)
(* not claimed there. NONE on Start, the stops or the resume: a sync op may *)
(* never start, and a stopped one may stay stopped -- the ENTER must not   *)
(* depend on another Proc's resume. Stops are finite (MAX_STOPS), so the   *)
(* role is taken finitely often by sync ops.                               *)
(***************************************************************************)
Liveness ==
    /\ \A o \in Ops : WF_vars(ServerReply(o))
    /\ \A s \in Syncs : WF_vars(Elect(s))
    /\ \A s \in Syncs, o \in Ops : WF_vars(ReadFrame(s, o))
    /\ WF_vars(EnterTop)
    /\ WF_vars(EnterPump)
    /\ WF_vars(EnterHook)
    /\ WF_vars(EnterCqReg)
    /\ WF_vars(EnterSleep)
    /\ WF_vars(EnterUnhook)
    /\ \A o \in Ops : WF_vars(EnterRead(o))

Spec_Live == Init /\ [][Next]_vars /\ Liveness

\* The ENTER returns, or ends in the (E) blind recv for good: a Blind state
\* passed through on the way to some other strand does not satisfy it.
EnterReturns == <>(eph = "returned") \/ <>[]Blind

====
