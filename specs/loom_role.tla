---- MODULE loom_role ----
(***************************************************************************)
(* Thylacine Loom -- a waiter reads for every 9P client it waits on        *)
(* (LOOM.md 8.6 item 2; DEBUG-FS-DESIGN 5c.6; OPEN-BUGS 2026-09-30 11:01Z, *)
(* 2026-10-05 07:52Z and 18:56Z).                                          *)
(*                                                                         *)
(* A thread in SYS_LOOM_ENTER (min_complete = 1) waits for the CQE of any  *)
(* of its ring's async ops, which may be in flight on several 9P clients.  *)
(* Only the thread holding a client's READER ROLE reads that client's      *)
(* replies (ARCH 21.10, the #841 elected reader), and a dev9p client is    *)
(* SHARED, so the role can be held by another Proc's synchronous call.     *)
(* Nobody reads for an async op: its reply is read by whoever holds the    *)
(* role when it arrives, or by the waiter.                                 *)
(*                                                                         *)
(* THE MECHANISM (the operator's vote 2026-10-06: waiters fan in). The     *)
(* waiter scans every client with an op in flight and pumps one whose role *)
(* is free AND whose transport is ready -- bytes, or the EOF, at a frame   *)
(* boundary (p9_client_reader_pump_ready). It never takes a role over an   *)
(* empty stream, so it never blocks in a recv with nothing due. With       *)
(* nothing to pump it hooks each client under c->lock                      *)
(* (p9_client_reader_hook): a HELD role -> the client's role-waiter list,  *)
(* which the handoff wakes when it leaves the role free and undesignated;  *)
(* a FREE role with nothing to read -> the transport's readiness list,     *)
(* which every arrival wakes; a free role with a frame waiting -> unhook   *)
(* all and scan again. Then the CQ hook, and one sleep over all the hooks  *)
(* (one Rendez, one flag per hook: poll.c's poll_cond_any_flagged).        *)
(*                                                                         *)
(* The ENTER here stands for all three fan-in waiters: the non-SQPOLL      *)
(* ENTER, the SQPOLL kthread and the dev9p poll kthread run the same scan, *)
(* hook and sleep, and differ only in what ends the wait. A kthread takes  *)
(* no stop and no death, which removes behaviours, never adds them.        *)
(*                                                                         *)
(* HISTORY. Before 2026-09-30 an ENTER whose pump found the role held      *)
(* slept on the CQ list alone (BUGGY_NO_ROLE_HOOK): a synchronous reader   *)
(* departs once its own reply lands, its handoff designates only a         *)
(* synchronous waiter, and the async reply after it went unread. The fix   *)
(* hooked the role list -- for ONE client, the ring's first in-flight op's *)
(* (BUGGY_FIRST_CLIENT_ONLY), whose pump blocked in the recv whether or    *)
(* not anything was due (BUGGY_UNREADY_PUMP): another client's reply was   *)
(* never read while the first was held or slow, and a pump after another   *)
(* reader took its reply blocked blind (the old (E) residual).             *)
(*                                                                         *)
(* WHY A FOCUSED MODULE. loom.tla's ReplyArrives is enabled for any op in  *)
(* flight, under weak fairness: its liveness PRESUMES that some thread     *)
(* reads the reply. That thread is the 9P client's reader, which loom.tla  *)
(* does not model. This module discharges the premise and leaves loom.tla  *)
(* and its cfgs untouched (the loom_multishot / loom_order / loom_devgone  *)
(* precedent). It is also the model of the handoff, so it carries the two  *)
(* stop rules the waiter's wake depends on: the handoff skips a thread     *)
(* parked for a stop (stop_parked), and a stopped designee hands the role  *)
(* on before it parks.                                                     *)
(*                                                                         *)
(* THE ACTORS                                                               *)
(*                                                                         *)
(*   Ops == Clients \X 0..NSYNC. <<c, 0>> is the ring's async op on client *)
(*   c (on_complete set: no thread reads for it); <<c, i>>, i >= 1, is a   *)
(*   foreign synchronous call on c.                                         *)
(*   Per client c: holder[c] = NONE (c->reader_active false), ENTER or a   *)
(*   sync op of c; cq[c] = c's async CQE is posted.                         *)
(*   Each sync op s (client_wait; sph[s]):                                  *)
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
(*   The ENTER (loom_wait_for_completions; eph):                            *)
(*     "top"      -- the CQ sample: a CQE posted -> "returned";             *)
(*     "scan"     -- pump_ready on each client in `todo`, one per step,     *)
(*                   each under its c->lock: free and ready -> take the     *)
(*                   role ("reading" on ecl); else pass it over;            *)
(*     "hook"     -- p9_client_reader_hook on each client in `todo`;        *)
(*     "cqreg"    -- CqWaitRegister: hook the CQ list and sample the CQ     *)
(*                   under l->lock (posted -> skip the sleep);              *)
(*     "sleep"    -- the sleep's cond under the ENTER's Rendez lock: a      *)
(*                   flag set -> "unhook", else block;                      *)
(*     "sleeping" -- blocked; a waker sets a flag and makes it runnable;    *)
(*     "unhook"   -- every hook off, then "top";                            *)
(*     "reading"  -- holds ecl's role: one frame, then the handoff;         *)
(*     "returned".                                                          *)
(*   hk[c] = where the ENTER's hook for c is ("none", "role", "ready"),    *)
(*   hf[c] = its flag. The server replies to each sent request once         *)
(*   (replied, wire); a client in Deferred may hold its async op's reply    *)
(*   forever (a parked socket read, a QTPOLL arm).                          *)
(*                                                                         *)
(* EVERY RELEASE OF A ROLE RUNS THE HANDOFF. The sites that clear          *)
(* c->reader_active (client_wait's reader loop, client_pump_or_park_locked *)
(* -- the send path's one-frame self-pump --, pump_ready) each call        *)
(* client_handoff_reader_locked in the same c->lock hold. A sync reader    *)
(* departs on its own reply (ReadFrame) or at a frame boundary on a stop   *)
(* (StopReader, which also stands for every other early departure: a      *)
(* caught note, a death and the send path's self-pump all hand off without *)
(* the reader's own reply); the ENTER departs after its one frame          *)
(* (EnterRead).                                                             *)
(*                                                                         *)
(* EVERY ARRIVAL WAKES THE READINESS LIST. srvconn walks cn->poll_list     *)
(* after every s2c fill and a pipe walks its list after every write; the   *)
(* register and its sample share the backend's lock, nested under c->lock, *)
(* so the hook step is atomic against both a role change and an arrival.  *)
(*                                                                         *)
(* ABSTRACTIONS. A client's wire is a SET: the server orders its replies   *)
(* freely, so reading any sent reply over-approximates the FIFO. The       *)
(* handoff picks ANY designable op (the code: the lowest tag). The scan    *)
(* and the hook take the clients in ANY order (the code: a rotating        *)
(* cursor). A frame is atomic (a stop mid-frame blocks through to the      *)
(* boundary; reader_frame.tla), and a ready transport holds a whole frame: *)
(* a frame whose bytes have only started blocks the pump through the body, *)
(* bounded by the trusted server as every reader is (CF-3 B).              *)
(*                                                                         *)
(* OUT OF SCOPE. Session death: client_mark_dead_locked completes the      *)
(* client's async ops with error CQEs (loom_devgone.tla) and wakes both    *)
(* lists, and a dead client's hook returns -P9_E_IO, which ends nothing    *)
(* but that client's part of the scan. A stop of the ENTER's own thread    *)
(* (it parks in place and keeps its flags). The flood budget. A second     *)
(* waiter: each has its own hooks on the same lists, woken independently   *)
(* (poll.tla's argument). An EL0 holder of a pipe transport's read end     *)
(* that steals the ready bytes (it desyncs its own mount's stream).        *)
(*                                                                         *)
(* PROPERTIES                                                               *)
(*   NoMissedWake (the headline, I-9 over the role and readiness lists):   *)
(*     the ENTER never sleeps while some client has a frame waiting, a     *)
(*     free role and no sync op designated to take it -- nobody else would  *)
(*     read it. Stated without the hooks, so every buggy ENTER can          *)
(*     violate it.                                                          *)
(*   NoBlindRecv: the ENTER holds a role only over a waiting frame.         *)
(*   NoMissedCqWake: the ENTER never sleeps past a posted CQE.              *)
(*   EnterReturns (liveness): the ENTER returns when some client's reply   *)
(*     is not deferred, however the deferred ones and the sync traffic     *)
(*     behave.                                                              *)
(*                                                                         *)
(* BUGGY CONFIGS                                                            *)
(*   BUGGY_NO_ROLE_HOOK           a held role is not hooked (the pre-       *)
(*                                09-30 ENTER); EnterReturns violated (its  *)
(*                                cfg checks only the property, which      *)
(*                                proves the liveness check discriminates). *)
(*   BUGGY_FIRST_CLIENT_ONLY      the scan and the hooks see one client,    *)
(*                                the first in flight (the pre-10-06       *)
(*                                pick); EnterReturns violated.            *)
(*   BUGGY_UNREADY_PUMP           the pump takes a free role whatever the   *)
(*                                stream holds (the pre-10-06 pump_once);  *)
(*                                NoBlindRecv violated.                    *)
(*   BUGGY_ROLE_LATE_REGISTER     the hook files a client on the role list  *)
(*                                without re-sampling the role.            *)
(*   BUGGY_NO_ROLE_WAKE           the no-designee exit wakes nobody.        *)
(*   BUGGY_DESIGNATES_PARKED      the handoff designates a parked thread.   *)
(*   BUGGY_STOP_KEEPS_DESIGNATION a stopped designee parks without handing  *)
(*                                the role on.                              *)
(*   BUGGY_NO_READY_HOOK          a free role with nothing to read is not   *)
(*                                hooked at all.                           *)
(*   BUGGY_READY_LATE_REGISTER    the readiness hook trusts the scan's      *)
(*                                stale sample: a frame that arrived       *)
(*                                between them is missed.                  *)
(*   BUGGY_READY_HOOK_WHEN_HELD   a held role hooks the readiness list:     *)
(*                                the holder departs leaving a frame, and  *)
(*                                nothing arrives to wake the hook.        *)
(*   The last seven violate NoMissedWake.                                   *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Clients,                       \* the 9P clients the ring has async ops on
    NSYNC,                         \* foreign synchronous calls per client
    Deferred,                      \* clients whose async reply may never come
    MAX_STOPS,                     \* stops each sync op may take (job or debug)
    BUGGY_NO_ROLE_HOOK,
    BUGGY_FIRST_CLIENT_ONLY,
    BUGGY_UNREADY_PUMP,
    BUGGY_ROLE_LATE_REGISTER,
    BUGGY_NO_ROLE_WAKE,
    BUGGY_DESIGNATES_PARKED,
    BUGGY_STOP_KEEPS_DESIGNATION,
    BUGGY_NO_READY_HOOK,
    BUGGY_READY_LATE_REGISTER,
    BUGGY_READY_HOOK_WHEN_HELD

ASSUME Clients # {}
ASSUME NSYNC \in Nat
ASSUME Deferred \subseteq Clients
ASSUME MAX_STOPS \in Nat
ASSUME BUGGY_NO_ROLE_HOOK           \in BOOLEAN
ASSUME BUGGY_FIRST_CLIENT_ONLY      \in BOOLEAN
ASSUME BUGGY_UNREADY_PUMP           \in BOOLEAN
ASSUME BUGGY_ROLE_LATE_REGISTER     \in BOOLEAN
ASSUME BUGGY_NO_ROLE_WAKE           \in BOOLEAN
ASSUME BUGGY_DESIGNATES_PARKED      \in BOOLEAN
ASSUME BUGGY_STOP_KEEPS_DESIGNATION \in BOOLEAN
ASSUME BUGGY_NO_READY_HOOK          \in BOOLEAN
ASSUME BUGGY_READY_LATE_REGISTER    \in BOOLEAN
ASSUME BUGGY_READY_HOOK_WHEN_HELD   \in BOOLEAN

\* One-element tuples, so a holder compares with a sync op (a pair) by length.
NONE  == <<"none">>     \* the role is free
ENTER == <<"enter">>    \* the ENTER holds the role

Ops       == Clients \X (0..NSYNC)
Syncs     == Clients \X (1..NSYNC)
Home(o)   == o[1]
IsAsync(o) == o[2] = 0

\* The pre-10-06 pick: one client, the deferred one when there is one.
FirstClient == IF Deferred # {} THEN CHOOSE c \in Deferred : TRUE
                                ELSE CHOOSE c \in Clients : TRUE
Scanned == IF BUGGY_FIRST_CLIENT_ONLY THEN {FirstClient} ELSE Clients

SyncPhases  == {"idle", "wait", "reading", "sleeping", "parked", "done"}
EnterPhases == {"top", "scan", "hook", "cqreg", "sleep", "sleeping",
                "unhook", "reading", "returned"}
HookPlaces  == {"none", "role", "ready"}

VARIABLES
    holder,    \* [Clients -> Syncs \cup {NONE, ENTER}]: who holds each role
    sph,       \* [Syncs -> SyncPhases]
    sbe,       \* [Syncs -> BOOLEAN]: rpc->be_reader
    sdone,     \* [Syncs -> BOOLEAN]: rpc->done (the reply is demuxed)
    stops,     \* [Syncs -> 0..MAX_STOPS]: stops still to come
    replied,   \* the ops whose reply the server has sent
    wire,      \* sent replies not yet read (each on its op's client)
    cq,        \* [Clients -> BOOLEAN]: the client's async CQE is posted
    eph,       \* EnterPhases
    ecl,       \* the client whose role the ENTER holds ("reading"), else NONE
    todo,      \* the clients the scan or the hook has still to visit
    cqhook,    \* pw is on l->cq_waiters
    cqflag,    \* pw.ready
    hk,        \* [Clients -> HookPlaces]: where the ENTER's hook for c is
    hf         \* [Clients -> BOOLEAN]: that hook's ready flag

vars == <<holder, sph, sbe, sdone, stops, replied, wire, cq, eph, ecl, todo,
          cqhook, cqflag, hk, hf>>

\* A request is on its way to the server once its op is in client_wait; the
\* ring's ops were submitted before the wait began. (An IF, not a disjunction:
\* TLC splits an action-level \/ into branches and would apply sph to an async.)
Sent(o) == IF IsAsync(o) THEN TRUE ELSE sph[o] # "idle"

WireOf(c) == {o \in wire : Home(o) = c}

Wake(ep) == IF ep = "sleeping" THEN "sleep" ELSE ep

TypeOK ==
    /\ holder  \in [Clients -> Syncs \cup {NONE, ENTER}]
    /\ \A c \in Clients : holder[c] \in Syncs => Home(holder[c]) = c
    /\ sph     \in [Syncs -> SyncPhases]
    /\ sbe     \in [Syncs -> BOOLEAN]
    /\ sdone   \in [Syncs -> BOOLEAN]
    /\ stops   \in [Syncs -> 0..MAX_STOPS]
    /\ replied \subseteq Ops
    /\ wire    \subseteq replied
    /\ cq      \in [Clients -> BOOLEAN]
    /\ eph     \in EnterPhases
    /\ ecl     \in Clients \cup {NONE}
    /\ todo    \subseteq Clients
    /\ cqhook  \in BOOLEAN
    /\ cqflag  \in BOOLEAN
    /\ hk      \in [Clients -> HookPlaces]
    /\ hf      \in [Clients -> BOOLEAN]

Init ==
    /\ holder  = [c \in Clients |-> NONE]
    /\ sph     = [s \in Syncs |-> "idle"]
    /\ sbe     = [s \in Syncs |-> FALSE]
    /\ sdone   = [s \in Syncs |-> FALSE]
    /\ stops   = [s \in Syncs |-> MAX_STOPS]
    /\ replied = {}
    /\ wire    = {}
    /\ cq      = [c \in Clients |-> FALSE]
    /\ eph     = "top"
    /\ ecl     = NONE
    /\ todo    = {}
    /\ cqhook  = FALSE
    /\ cqflag  = FALSE
    /\ hk      = [c \in Clients |-> "none"]
    /\ hf      = [c \in Clients |-> FALSE]

(***************************************************************************)
(* The handoff on client c (client_handoff_reader_locked, c->lock held).   *)
(* Designate one of c's sync ops that is not the departing one, not done,  *)
(* not yet designated, in client_wait (not sending) and not parked for a   *)
(* stop; the active reader qualifies when another thread runs the handoff  *)
(* (the designation then lands on nothing). A sleeping designee wakes.     *)
(* With nobody to designate, a FREE role wakes the role list. Applied to   *)
(* the intermediate state (ph, be, dn, ep, f) a step has already produced; *)
(* `hold` is c's holder after the step. Sets sph', sbe', eph', hf'.        *)
(***************************************************************************)
DesignablePhases ==
    {"wait", "sleeping", "reading"}
        \cup (IF BUGGY_DESIGNATES_PARKED THEN {"parked"} ELSE {})

Designable(d, departing, ph, be, dn) ==
    /\ d # departing
    /\ ph[d] \in DesignablePhases
    /\ ~dn[d]
    /\ ~be[d]

Handoff(c, departing, hold, ph, be, dn, ep, f) ==
    LET cands == {d \in Syncs : Home(d) = c /\ Designable(d, departing, ph, be, dn)}
    IN  IF cands # {}
        THEN \E d \in cands :
               /\ sbe' = [be EXCEPT ![d] = TRUE]
               /\ sph' = IF ph[d] = "sleeping" THEN [ph EXCEPT ![d] = "wait"]
                                               ELSE ph
               /\ eph' = ep
               /\ hf'  = f
        ELSE /\ sbe' = be
             /\ sph' = ph
             /\ IF hold = NONE /\ hk[c] = "role" /\ ~BUGGY_NO_ROLE_WAKE
                THEN /\ hf'  = [f EXCEPT ![c] = TRUE]
                     /\ eph' = Wake(ep)
                ELSE /\ hf'  = f
                     /\ eph' = ep

(***************************************************************************)
(* The demux of o's reply (demux_frame_locked): a sync reply is stored and *)
(* wakes its sleeping owner; an async reply fires on_complete ->           *)
(* loom_post_cqe, which posts the CQE and wakes the CQ list (loom.tla's    *)
(* PostCqe).                                                                *)
(***************************************************************************)
DemuxPh(o, ph)  == IF ~IsAsync(o) /\ ph[o] = "sleeping" THEN [ph EXCEPT ![o] = "wait"]
                                                       ELSE ph
DemuxDn(o, dn)  == IF ~IsAsync(o) THEN [dn EXCEPT ![o] = TRUE] ELSE dn
DemuxCq(o)      == IF IsAsync(o) THEN [cq EXCEPT ![Home(o)] = TRUE] ELSE cq
DemuxCqflag(o)  == IF IsAsync(o) /\ cqhook THEN TRUE ELSE cqflag
DemuxEph(o, ep) == IF IsAsync(o) /\ cqhook THEN Wake(ep) ELSE ep

(***************************************************************************)
(* The server. An arrival walks the client's readiness list.               *)
(***************************************************************************)
ServerReply(o) ==
    /\ Sent(o)
    /\ o \notin replied
    /\ replied' = replied \cup {o}
    /\ wire'    = wire \cup {o}
    /\ IF hk[Home(o)] = "ready"
       THEN /\ hf'  = [hf EXCEPT ![Home(o)] = TRUE]
            /\ eph' = Wake(eph)
       ELSE UNCHANGED <<hf, eph>>
    /\ UNCHANGED <<holder, sph, sbe, sdone, stops, cq, ecl, todo,
                   cqhook, cqflag, hk>>

(***************************************************************************)
(* The sync ops.                                                            *)
(***************************************************************************)
\* The op is sent and reaches client_wait (sending is folded in: the handoff
\* skips a sender, and a sender self-elects on arrival).
Start(s) ==
    /\ sph[s] = "idle"
    /\ sph' = [sph EXCEPT ![s] = "wait"]
    /\ UNCHANGED <<holder, sbe, sdone, stops, replied, wire, cq, eph, ecl, todo,
                   cqhook, cqflag, hk, hf>>

\* client_wait's loop top with no stop pending: the reply stored -> return;
\* the role free -> become the reader; held -> clear a stale designation (F7)
\* and sleep on the rpc. The rpc-local cond (done or be_reader) is false here:
\* both were just read under c->lock, and a later demux or designation wakes it.
Elect(s) ==
    /\ sph[s] = "wait"
    /\ IF sdone[s]
       THEN /\ sph' = [sph EXCEPT ![s] = "done"]
            /\ UNCHANGED <<holder, sbe>>
       ELSE IF holder[Home(s)] = NONE
       THEN /\ holder' = [holder EXCEPT ![Home(s)] = s]
            /\ sph'    = [sph EXCEPT ![s] = "reading"]
            /\ sbe'    = [sbe EXCEPT ![s] = FALSE]
       ELSE /\ sph'    = [sph EXCEPT ![s] = "sleeping"]
            /\ sbe'    = [sbe EXCEPT ![s] = FALSE]
            /\ UNCHANGED holder
    /\ UNCHANGED <<sdone, stops, replied, wire, cq, eph, ecl, todo,
                   cqhook, cqflag, hk, hf>>

\* The sync reader reads a frame of its client and demuxes it. Its own reply
\* ends the loop in the same c->lock hold: release the role, hand it off,
\* return.
ReadFrame(s, o) ==
    /\ holder[Home(s)] = s
    /\ o \in wire
    /\ Home(o) = Home(s)
    /\ wire'   = wire \ {o}
    /\ sdone'  = DemuxDn(o, sdone)
    /\ cq'     = DemuxCq(o)
    /\ cqflag' = DemuxCqflag(o)
    /\ IF o = s
       THEN /\ holder' = [holder EXCEPT ![Home(s)] = NONE]
            /\ Handoff(Home(s), s, NONE, [DemuxPh(o, sph) EXCEPT ![s] = "done"],
                       sbe, DemuxDn(o, sdone), DemuxEph(o, eph), hf)
       ELSE /\ sph' = DemuxPh(o, sph)
            /\ eph' = DemuxEph(o, eph)
            /\ UNCHANGED <<holder, sbe, hf>>
    /\ UNCHANGED <<stops, replied, ecl, todo, cqhook, hk>>

\* A stop unwinds the reader's recv at a frame boundary: release, hand off
\* (skipping myself), park role-free -- one c->lock hold.
StopReader(s) ==
    /\ holder[Home(s)] = s
    /\ stops[s] > 0
    /\ stops'  = [stops EXCEPT ![s] = @ - 1]
    /\ holder' = [holder EXCEPT ![Home(s)] = NONE]
    /\ Handoff(Home(s), s, NONE, [sph EXCEPT ![s] = "parked"], sbe, sdone,
               eph, hf)
    /\ UNCHANGED <<sdone, replied, wire, cq, ecl, todo, cqhook, cqflag, hk>>

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
       THEN Handoff(Home(s), s, holder[Home(s)], [sph EXCEPT ![s] = "parked"],
                    [sbe EXCEPT ![s] = FALSE], sdone, eph, hf)
       ELSE /\ sph' = [sph EXCEPT ![s] = "parked"]
            /\ UNCHANGED <<sbe, eph, hf>>
    /\ UNCHANGED <<holder, sdone, replied, wire, cq, ecl, todo,
                   cqhook, cqflag, hk>>

\* The resume: stop_parked cleared under c->lock, then the loop top re-elects.
Resume(s) ==
    /\ sph[s] = "parked"
    /\ sph' = [sph EXCEPT ![s] = "wait"]
    /\ UNCHANGED <<holder, sbe, sdone, stops, replied, wire, cq, eph, ecl, todo,
                   cqhook, cqflag, hk, hf>>

(***************************************************************************)
(* The ENTER.                                                               *)
(***************************************************************************)
\* The give-up sample: min_complete is one, and every op is in flight until
\* its CQE posts, so "ready" and "nothing in flight" both read a posted CQE.
EnterTop ==
    /\ eph = "top"
    /\ IF \E c \in Clients : cq[c]
       THEN /\ eph' = "returned"
            /\ UNCHANGED todo
       ELSE /\ eph'  = "scan"
            /\ todo' = Scanned
    /\ UNCHANGED <<holder, sph, sbe, sdone, stops, replied, wire, cq, ecl,
                   cqhook, cqflag, hk, hf>>

\* p9_client_reader_pump_ready under c->lock: the role free and a frame
\* waiting -> take the role; anything else passes the client over.
EnterScan(c) ==
    /\ eph = "scan"
    /\ c \in todo
    /\ IF holder[c] = NONE /\ (WireOf(c) # {} \/ BUGGY_UNREADY_PUMP)
       THEN /\ holder' = [holder EXCEPT ![c] = ENTER]
            /\ eph'    = "reading"
            /\ ecl'    = c
            /\ todo'   = {}
       ELSE /\ todo'   = todo \ {c}
            /\ UNCHANGED <<holder, eph, ecl>>
    /\ UNCHANGED <<sph, sbe, sdone, stops, replied, wire, cq,
                   cqhook, cqflag, hk, hf>>

\* Nothing was pumpable: hook every client.
EnterScanned ==
    /\ eph  = "scan"
    /\ todo = {}
    /\ eph'  = "hook"
    /\ todo' = Scanned
    /\ UNCHANGED <<holder, sph, sbe, sdone, stops, replied, wire, cq, ecl,
                   cqhook, cqflag, hk, hf>>

\* p9_client_reader_hook under c->lock, the readiness register and its sample
\* under the backend's lock inside it: a held role -> the role list; a free
\* role with nothing to read -> the readiness list; a free role with a frame
\* waiting -> drop every hook and scan again.
EnterHook(c) ==
    /\ eph = "hook"
    /\ c \in todo
    /\ IF holder[c] # NONE
       THEN IF BUGGY_NO_ROLE_HOOK
            THEN /\ todo' = todo \ {c}
                 /\ UNCHANGED <<eph, hk, hf>>
            ELSE /\ hk'   = [hk EXCEPT ![c] = IF BUGGY_READY_HOOK_WHEN_HELD
                                               THEN "ready" ELSE "role"]
                 /\ hf'   = [hf EXCEPT ![c] = FALSE]
                 /\ todo' = todo \ {c}
                 /\ UNCHANGED eph
       ELSE IF BUGGY_ROLE_LATE_REGISTER
       THEN /\ hk'   = [hk EXCEPT ![c] = "role"]
            /\ hf'   = [hf EXCEPT ![c] = FALSE]
            /\ todo' = todo \ {c}
            /\ UNCHANGED eph
       ELSE IF WireOf(c) # {} /\ ~BUGGY_READY_LATE_REGISTER
       THEN /\ eph'  = "unhook"
            /\ todo' = {}
            /\ UNCHANGED <<hk, hf>>
       ELSE IF BUGGY_NO_READY_HOOK
       THEN /\ todo' = todo \ {c}
            /\ UNCHANGED <<eph, hk, hf>>
       ELSE /\ hk'   = [hk EXCEPT ![c] = "ready"]
            /\ hf'   = [hf EXCEPT ![c] = FALSE]
            /\ todo' = todo \ {c}
            /\ UNCHANGED eph
    /\ UNCHANGED <<holder, sph, sbe, sdone, stops, replied, wire, cq, ecl,
                   cqhook, cqflag>>

EnterHooked ==
    /\ eph  = "hook"
    /\ todo = {}
    /\ eph' = "cqreg"
    /\ UNCHANGED <<holder, sph, sbe, sdone, stops, replied, wire, cq, ecl, todo,
                   cqhook, cqflag, hk, hf>>

\* CqWaitRegister: hook l->cq_waiters and sample the CQ under l->lock.
EnterCqReg ==
    /\ eph = "cqreg"
    /\ cqhook' = TRUE
    /\ cqflag' = FALSE
    /\ eph'    = IF \E c \in Clients : cq[c] THEN "unhook" ELSE "sleep"
    /\ UNCHANGED <<holder, sph, sbe, sdone, stops, replied, wire, cq, ecl, todo,
                   hk, hf>>

\* The sleep's cond under the ENTER's Rendez lock: any hook's flag.
EnterSleep ==
    /\ eph = "sleep"
    /\ eph' = IF cqflag \/ \E c \in Clients : hk[c] # "none" /\ hf[c]
              THEN "unhook" ELSE "sleeping"
    /\ UNCHANGED <<holder, sph, sbe, sdone, stops, replied, wire, cq, ecl, todo,
                   cqhook, cqflag, hk, hf>>

\* Every hook off; a flag set after is stale, and the next register clears it.
EnterUnhook ==
    /\ eph = "unhook"
    /\ cqhook' = FALSE
    /\ hk'     = [c \in Clients |-> "none"]
    /\ eph'    = "top"
    /\ UNCHANGED <<holder, sph, sbe, sdone, stops, replied, wire, cq, ecl, todo,
                   cqflag, hf>>

\* The ENTER holds ecl's role: it reads one frame, demuxes it and departs
\* through the handoff (pump_ready is one-shot). Its hooks are off here.
EnterRead(o) ==
    /\ eph = "reading"
    /\ o \in wire
    /\ Home(o) = ecl
    /\ wire'   = wire \ {o}
    /\ sdone'  = DemuxDn(o, sdone)
    /\ cq'     = DemuxCq(o)
    /\ holder' = [holder EXCEPT ![ecl] = NONE]
    /\ ecl'    = NONE
    /\ Handoff(ecl, ENTER, NONE, DemuxPh(o, sph), sbe, DemuxDn(o, sdone),
               "top", hf)
    /\ UNCHANGED <<stops, replied, todo, cqhook, cqflag, hk>>

Next ==
    \/ \E o \in Ops : ServerReply(o)
    \/ \E s \in Syncs : Start(s)
    \/ \E s \in Syncs : Elect(s)
    \/ \E s \in Syncs, o \in Ops : ReadFrame(s, o)
    \/ \E s \in Syncs : StopReader(s)
    \/ \E s \in Syncs : StopWaiter(s)
    \/ \E s \in Syncs : Resume(s)
    \/ EnterTop
    \/ \E c \in Clients : EnterScan(c)
    \/ EnterScanned
    \/ \E c \in Clients : EnterHook(c)
    \/ EnterHooked
    \/ EnterCqReg
    \/ EnterSleep
    \/ EnterUnhook
    \/ \E o \in Ops : EnterRead(o)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

\* Each role has one holder, and the holder is the thread in the recv.
RoleConsistent ==
    /\ \A s \in Syncs : (sph[s] = "reading") <=> (holder[Home(s)] = s)
    /\ \A c \in Clients : (holder[c] = ENTER) <=> (eph = "reading" /\ ecl = c)

\* The ENTER's hooks are on their lists only while it waits.
HooksConsistent ==
    /\ cqhook => eph \in {"sleep", "sleeping", "unhook"}
    /\ \A c \in Clients :
           hk[c] # "none" => eph \in {"hook", "cqreg", "sleep", "sleeping", "unhook"}

\* Blocked, the ENTER holds no unconsumed wake: every waker made it runnable.
SleepingUnflagged ==
    eph = "sleeping" => (~cqflag /\ \A c \in Clients : ~(hk[c] # "none" /\ hf[c]))

\* The ENTER never sleeps past a posted CQE (loom.tla's NoMissedCqWake, here).
NoMissedCqWake == ~(eph = "sleeping" /\ \E c \in Clients : cq[c])

\* A designee of c that will run the election, or hand the role on before it
\* parks.
Designated(c) == \E s \in Syncs : Home(s) = c /\ sbe[s] /\ sph[s] = "wait" /\ ~sdone[s]

\* THE HEADLINE (I-9 over the role and readiness lists): the ENTER never sleeps
\* while some client has a frame waiting, a free role and no designee.
NoMissedWake ==
    ~(eph = "sleeping" /\ \E c \in Clients : /\ holder[c] = NONE
                                              /\ ~Designated(c)
                                              /\ WireOf(c) # {})

\* The ENTER holds a role only over a frame it can read: it never blocks in a
\* recv with nothing on the stream (the old (E) residual, closed by the
\* readiness gate).
NoBlindRecv == eph = "reading" => WireOf(ecl) # {}

Invariants ==
    /\ TypeOK
    /\ RoleConsistent
    /\ HooksConsistent
    /\ SleepingUnflagged
    /\ NoMissedCqWake
    /\ NoMissedWake
    /\ NoBlindRecv

(***************************************************************************)
(* ============================== LIVENESS ================================ *)
(*                                                                         *)
(* Weak fairness on the server's answer to every request that is not a     *)
(* deferred client's async op, the sync ops' election and reads, and every *)
(* ENTER step. WF on ServerReply is the trusted-server premise loom.tla    *)
(* also makes; a Deferred client is the server that holds a read (a parked *)
(* socket read, a QTPOLL arm), and the ENTER must not depend on it. NONE   *)
(* on Start, the stops or the resume: a sync op may never start, and a     *)
(* stopped one may stay stopped -- the ENTER must not depend on another    *)
(* Proc's resume. Stops are finite (MAX_STOPS), so a role is taken         *)
(* finitely often by sync ops.                                             *)
(***************************************************************************)
Answered == {o \in Ops : ~(IsAsync(o) /\ Home(o) \in Deferred)}

Liveness ==
    /\ \A o \in Answered : WF_vars(ServerReply(o))
    /\ \A s \in Syncs : WF_vars(Elect(s))
    /\ \A s \in Syncs, o \in Ops : WF_vars(ReadFrame(s, o))
    /\ WF_vars(EnterTop)
    /\ \A c \in Clients : WF_vars(EnterScan(c))
    /\ WF_vars(EnterScanned)
    /\ \A c \in Clients : WF_vars(EnterHook(c))
    /\ WF_vars(EnterHooked)
    /\ WF_vars(EnterCqReg)
    /\ WF_vars(EnterSleep)
    /\ WF_vars(EnterUnhook)
    /\ \A o \in Ops : WF_vars(EnterRead(o))

Spec_Live == Init /\ [][Next]_vars /\ Liveness

\* Claimed only when some client's async reply is answered (Clients # Deferred).
EnterReturns == <>(eph = "returned")

====
