---- MODULE net_poll ----
(***************************************************************************)
(* Thylacine `dev9p.poll` -- poll-readiness over a 9P fd, the PROBE-then-   *)
(* observe bridge (net-6b; NET-DESIGN.md section 12.2).                     *)
(*                                                                         *)
(* A userspace `poll()`/`select()` over a `/net` fd reaches the kernel via *)
(* the new `dev9p.poll` vtable slot. Unlike the console (cons_poll.tla),    *)
(* whose readiness is a LOCAL edge an RX IRQ produces, a 9P socket's        *)
(* readiness lives in netd (a userspace stack server). The kernel learns it *)
(* only by issuing a readiness READ (a 9P Tread on a non-consuming netd     *)
(* `ready` file) that netd DEFERS until the socket is readable/writable per *)
(* the requested mask, then replies. So readiness must be ELICITED: a       *)
(* `dev9p.poll` that registers a hook but leaves no readiness read in flight *)
(* will never be told the socket became ready -- netd has no outstanding    *)
(* request to reply to.                                                     *)
(*                                                                         *)
(* The reply, when it comes, is demuxed by the kernel 9P client's #841      *)
(* elected reader. Because a `poll()` caller PARKS (it is precisely not     *)
(* doing a blocking read), no synchronous reader pumps that client, so the  *)
(* impl spawns a per-client poll-pump kthread (the Loom-4 SQPOLL analog) to *)
(* drive the reader. The reader's demux fires the async op's `on_complete`  *)
(* UNDER `c->lock`: it RECORDS the readiness bitmap into the dev9p fid and  *)
(* sets a relay flag -- it does NOT walk the poll-hook list there (that     *)
(* nests `poll_waiter_list_wake`'s plain lock + a `wakeup`, illegal under   *)
(* `c->lock`). The kthread drains the relay flag AFTER the pump, with       *)
(* `c->lock` released, and walks the hook list in PROCESS context -- the    *)
(* exact LS-8a `console_mgr` deferred-wake discipline (cons_poll.tla).      *)
(*                                                                         *)
(* WHAT THIS SPEC PINS                                                      *)
(*                                                                         *)
(*   The single-fd missed wakeup (cond check vs sleep, one Rendez) is       *)
(*   scheduler.tla's; the N-fd poll-hook register-then-observe and the hook *)
(*   lifetime are poll.tla's; the kthread-relayed deferred wake (the second *)
(*   register-then-observe, on the kthread's own sleep) is cons_poll.tla's. *)
(*   net_poll adds the one thing none of them covers: readiness here is not *)
(*   produced spontaneously -- it must be PROBED. The load-bearing          *)
(*   discipline is PROBE-then-observe: `dev9p.poll` must ensure a readiness *)
(*   read is OUTSTANDING (atomically with installing the hook and sampling  *)
(*   the cached bitmap) BEFORE the poller observes not-ready and parks. A   *)
(*   poller that parks with no probe in flight is doomed: the readiness     *)
(*   edge fires in netd, but with no request to answer netd stays silent,   *)
(*   the reader demuxes nothing, the relay never runs, and the poller       *)
(*   sleeps forever on a socket that is ready (NoMissedNetPoll -- I-9        *)
(*   generalized to the elicited-readiness relay). This spec proves a       *)
(*   poller is never left asleep on a ready, registered fd with the relay   *)
(*   quiescent AND no probe outstanding, and that a returned poll holds no  *)
(*   stale hook.                                                            *)
(*                                                                         *)
(* THE BUG THIS PINS                                                        *)
(*                                                                         *)
(*   BUGGY_LOST_READY -- `dev9p.poll` installs the hook and samples the     *)
(*     cached readiness but does NOT ensure a readiness read is in flight   *)
(*     before returning not-ready. The poller parks. The socket becomes     *)
(*     ready in netd, but with no outstanding readiness read netd never     *)
(*     replies; the reader demuxes nothing, `on_complete` never records,    *)
(*     the kthread never walks the hook list, and the poller sleeps forever *)
(*     (NoMissedNetPoll counterexample -- the edge dropped because the      *)
(*     probe was not outstanding before the observe). The fix is the        *)
(*     PROBE-then-observe order: install the hook, ensure a readiness read  *)
(*     is outstanding, and sample -- in one step under the fid's poll lock  *)
(*     -- so the readiness edge always has a request to answer and the      *)
(*     answer always reaches an installed hook.                            *)
(*                                                                         *)
(* CFG MATRIX (executable documentation per CLAUDE.md spec-first policy)    *)
(*                                                                         *)
(*   net_poll.cfg                   all buggy flags FALSE -- every safety   *)
(*                                   invariant holds.                       *)
(*   net_poll_liveness.cfg          Spec_Live -- PollerEventuallyServed: a  *)
(*                                   ready, registered socket always        *)
(*                                   eventually returns the poll. The probe *)
(*                                   elicits, the reader demuxes, the relay  *)
(*                                   delivers.                              *)
(*   net_poll_buggy_lost_ready.cfg  BUGGY_LOST_READY -- NoMissedNetPoll     *)
(*                                   counterexample: the poller parks with  *)
(*                                   no probe, the readiness edge is lost,   *)
(*                                   the poller sleeps forever on a ready    *)
(*                                   socket.                                *)
(*                                                                         *)
(* MODELING ASSUMPTIONS                                                     *)
(*                                                                         *)
(*   One poller, one fd. The headline is the PROBE-then-observe, not the    *)
(*   N-fd fan (poll.tla owns N fds; multiple pollers on one hook list       *)
(*   compose there). One poller fully exercises the elicited-readiness      *)
(*   relay.                                                                 *)
(*                                                                         *)
(*   `ready` -- netd reports the socket readable for the requested mask --  *)
(*   is monotonic FALSE -> TRUE: a readiness edge within one poll episode   *)
(*   (a consumer draining the bytes is a separate concern; poll.tla's       *)
(*   assumption). One probe elicits one reply; one episode needs no re-arm. *)
(*                                                                         *)
(*   The poll-pump kthread is modeled by the two relay actions it drives    *)
(*   (NetdReplyDemux = the reader's demux + `on_complete` record, under     *)
(*   `c->lock`; KthreadWalk = the post-pump hook-list walk, in process      *)
(*   context). Its own go-to-sleep register-then-observe (the relay's       *)
(*   second I-9 obligation) is cons_poll.tla's `MgrSleep` proof, reused     *)
(*   verbatim (the same `sleep(&rendez, cond)` contract) and not re-pinned; *)
(*   here the kthread is granted weak fairness to make relay progress.      *)
(*                                                                         *)
(*   Atomic actions model the critical sections: `Register` holds the fid's *)
(*   poll lock across hook-install + probe-ensure + cached-bitmap sample    *)
(*   (one `dev9p.poll` call); NetdReplyDemux is the reader's demux under    *)
(*   `c->lock`; KthreadWalk is `poll_waiter_list_wake` under the hook-list  *)
(*   lock with `c->lock` released. The poll(-1) infinite wait is modeled    *)
(*   (no timeout): the poller finishes ONLY when the relay flags it, so a   *)
(*   dropped probe strands it forever -- the sharpest statement of the      *)
(*   probe obligation. The timeout backstop is tsleep.tla's PollTerminates. *)
(*                                                                         *)
(* See NET-DESIGN.md section 12.2; ARCHITECTURE.md section 28 invariant     *)
(* I-9; poll.tla (the poller-side register-then-observe + hook lifetime);   *)
(* cons_poll.tla (the deferred relay through a process-context kthread);    *)
(* tsleep.tla / scheduler.tla (the Rendez sleep the relay and the poller    *)
(* both build on); kernel/dev9p.c (`dev9p_poll`), kernel/9p_client.c (the   *)
(* elected reader + `on_complete`), kernel/poll.c (`poll_waiter_list_wake`).*)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS
    BUGGY_LOST_READY   \* BOOLEAN -- TRUE: dev9p.poll installs the hook and
                       \*   samples the cached readiness but does NOT ensure
                       \*   a readiness read is outstanding before the poller
                       \*   observes not-ready and parks. The readiness edge
                       \*   that fires in netd then has no request to answer.

ASSUME BUGGY_LOST_READY \in BOOLEAN

VARIABLES
    ready,          \* BOOLEAN -- netd reports the socket readable for the
                    \*   requested mask (the POLLIN/POLLOUT readiness).
                    \*   Monotonic FALSE -> TRUE.
    probe,          \* BOOLEAN -- a readiness read (a deferred 9P Tread on the
                    \*   netd `ready` file) is OUTSTANDING. dev9p.poll's
                    \*   register step must ensure this before the observe.
    recorded,       \* BOOLEAN -- the poll-pump kthread's reader demuxed the
                    \*   readiness reply; `on_complete` recorded the cached
                    \*   readiness bitmap into the dev9p fid (under c->lock).
    pending,        \* BOOLEAN -- the relay flag: a recorded reply awaits the
                    \*   kthread's process-context hook-list walk (on_complete
                    \*   set it under c->lock; the kthread drains it after the
                    \*   pump, c->lock released).
    registered,     \* BOOLEAN -- the poller's poll_waiter hook is installed on
                    \*   the dev9p fid's poll_waiter_list.
    flagged,        \* BOOLEAN -- the poller's poll_waiter ready flag, set by
                    \*   the kthread's hook-list walk; the readiness the poller
                    \*   observes under its own Rendez lock.
    poller_pc       \* the poll() call's lifecycle (see PollerPCs).

vars == <<ready, probe, recorded, pending, registered, flagged, poller_pc>>

\* Poller: "start"      -- poll() entered; no hook installed, no probe.
\*         "registered" -- hook installed, probe ensured, readiness sampled;
\*                         the evaluate point (first entry and every resume).
\*         "sleeping"   -- parked on the poller's private Rendez.
\*         "done"       -- poll returned a ready revent.
PollerPCs      == {"start", "registered", "sleeping", "done"}
PollerTerminal == {"done"}

TypeOk ==
    /\ ready      \in BOOLEAN
    /\ probe      \in BOOLEAN
    /\ recorded   \in BOOLEAN
    /\ pending    \in BOOLEAN
    /\ registered \in BOOLEAN
    /\ flagged    \in BOOLEAN
    /\ poller_pc  \in PollerPCs

(***************************************************************************)
(* The poller has just called poll(); the socket is not (yet) ready, no    *)
(* probe is outstanding, nothing recorded, no hook installed, no flag, no   *)
(* relay pending.                                                           *)
(***************************************************************************)
Init ==
    /\ ready      = FALSE
    /\ probe      = FALSE
    /\ recorded   = FALSE
    /\ pending    = FALSE
    /\ registered = FALSE
    /\ flagged    = FALSE
    /\ poller_pc  = "start"

(***************************************************************************)
(* SocketReady -- netd's socket becomes readable for the requested mask    *)
(* (bytes arrive, the peer closes, the send buffer drains). The readiness   *)
(* edge, monotonic, fired once. Gated to the pre-terminal space -- after    *)
(* poll returns no one is waiting. This sets only netd's truth; it does NOT *)
(* by itself reach the kernel -- a probe must be outstanding to elicit the  *)
(* reply (NetdReplyDemux), which is the whole point of this spec.           *)
(***************************************************************************)
SocketReady ==
    /\ poller_pc \notin PollerTerminal
    /\ ~ready
    /\ ready' = TRUE
    /\ UNCHANGED <<probe, recorded, pending, registered, flagged, poller_pc>>

(***************************************************************************)
(* PollerRegister -- the CORRECT poll entry: PROBE-then-observe. The fid's  *)
(* `dev9p.poll`, in one step under the fid's poll lock, (1) installs the    *)
(* poll_waiter hook, (2) ENSURES a readiness read is outstanding iff the    *)
(* cached bitmap is not already ready, and (3) samples the cached readiness *)
(* as the initial flag. The probe-ensure is the load-bearing step: a        *)
(* readiness edge that fires after this always has a request to answer.     *)
(*                                                                         *)
(* BUGGY_LOST_READY drops step (2): the hook is installed and the bitmap    *)
(* sampled, but no probe is ensured -- so a later readiness edge in netd is *)
(* never elicited.                                                          *)
(***************************************************************************)
PollerRegister ==
    /\ poller_pc = "start"
    /\ poller_pc'  = "registered"
    /\ registered' = TRUE
    /\ flagged'    = recorded                      \* sample the cached bitmap
    /\ probe'      = IF BUGGY_LOST_READY THEN probe \* the bug: never ensure
                     ELSE IF recorded THEN probe    \* already ready: no probe
                     ELSE TRUE                      \* ensure outstanding
    /\ UNCHANGED <<ready, recorded, pending>>

(***************************************************************************)
(* PollerCommit -- the evaluate point (first entry and every resume). A     *)
(* `tsleep` on the poller's Rendez: the flag scan and the sleep transition  *)
(* are atomic under that Rendez lock. Flag set -> return ready (unhook).    *)
(* Else (poll(-1)) -> sleep. (The timeout branch is tsleep.tla's.)          *)
(***************************************************************************)
PollerCommit ==
    /\ poller_pc = "registered"
    /\ IF flagged
       THEN /\ poller_pc'  = "done"
            /\ registered' = FALSE      \* unhook on return (NoStaleHook)
       ELSE /\ poller_pc'  = "sleeping"
            /\ registered' = registered
    /\ UNCHANGED <<ready, probe, recorded, pending, flagged>>

(***************************************************************************)
(* NetdReplyDemux -- the elicited reply arrives. netd, holding the deferred *)
(* readiness read, sees the socket ready for the requested mask and replies *)
(* with the readiness bitmap (without consuming socket data). The kernel    *)
(* 9P client's poll-pump kthread, as the #841 elected reader, demuxes the   *)
(* Rread; the async op's `on_complete` fires UNDER `c->lock` and RECORDS the *)
(* bitmap into the dev9p fid AND sets the relay flag `pending`. The read is *)
(* consumed (`probe` clears). on_complete does NOT walk the hook list here  *)
(* (that is illegal under c->lock) -- KthreadWalk does, after the pump.     *)
(*                                                                         *)
(* Enabled only when a probe is outstanding (`probe`) AND the socket is     *)
(* ready (`ready`): with no probe, netd has nothing to answer -- the heart  *)
(* of BUGGY_LOST_READY.                                                     *)
(***************************************************************************)
NetdReplyDemux ==
    /\ probe
    /\ ready
    /\ ~recorded
    /\ recorded' = TRUE
    /\ pending'  = TRUE
    /\ probe'    = FALSE
    /\ UNCHANGED <<ready, registered, flagged, poller_pc>>

(***************************************************************************)
(* KthreadWalk -- the poll-pump kthread, after the pump returns with        *)
(* `c->lock` released, drains the relay flag and walks the poll-hook list   *)
(* (`poll_waiter_list_wake`, process context). The walk sets each           *)
(* registered poller's flag AND wakes its Rendez. Here: if a readiness was  *)
(* recorded and the poller is registered, set its flag and re-schedule a    *)
(* sleeping poller back to its evaluate point. This is the LS-8a            *)
(* console_mgr deferred-wake (cons_poll.tla's MgrDrainWalk).                *)
(***************************************************************************)
KthreadWalk ==
    /\ pending
    /\ pending' = FALSE
    /\ IF recorded /\ registered
       THEN /\ flagged'   = TRUE
            /\ poller_pc' = IF poller_pc = "sleeping" THEN "registered"
                                                      ELSE poller_pc
       ELSE /\ flagged'   = flagged
            /\ poller_pc' = poller_pc
    /\ UNCHANGED <<ready, probe, recorded, registered>>

(***************************************************************************)
(* Done -- terminal self-loop. Once poll has returned the model halts; the  *)
(* explicit stutter keeps the legitimate terminal state from tripping TLC's *)
(* -deadlock check, which then stays meaningful for the pre-terminal space  *)
(* (where the lost-ready stuck state lives, NOT terminal).                  *)
(***************************************************************************)
Done == poller_pc \in PollerTerminal /\ UNCHANGED vars

Next ==
    \/ SocketReady
    \/ PollerRegister
    \/ PollerCommit
    \/ NetdReplyDemux
    \/ KthreadWalk
    \/ Done

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

\* FlagImpliesReady -- the poller's flag is set only for a genuinely ready
\* socket: poll never reports readiness for a socket netd never reported
\* ready (no spurious return). recorded only fires after ready (NetdReplyDemux
\* guards on `ready`), and flagged only follows recorded, so flagged => ready.
FlagImpliesReady == flagged => ready

\* RecordedImpliesReady -- a recorded readiness reply implies netd reported the
\* socket ready: the reply is elicited by the probe only once netd is ready.
RecordedImpliesReady == recorded => ready

\* NoMissedNetPoll -- ARCH section 28 I-9 across the elicited-readiness relay:
\* the poller is never left asleep on a ready, registered socket with the relay
\* quiescent AND no probe outstanding -- i.e. nothing will ever deliver. In the
\* correct model this is unreachable: PROBE-then-observe leaves `probe` TRUE
\* when the poller parks not-ready, so the readiness edge is always elicited
\* (NetdReplyDemux -> pending), and the drain flags the registered poller.
\* BUGGY_LOST_READY reaches it: the poller parks with probe FALSE, the edge
\* fires, and with no request outstanding netd stays silent forever.
NoMissedNetPoll ==
    ~( ready /\ registered /\ poller_pc = "sleeping"
       /\ ~flagged /\ ~pending /\ ~probe )

\* NoStaleHook -- a returned poll holds no poll_waiter hook (poll.tla's
\* property, re-checked here: the relay must not leave a dangling hook).
NoStaleHook == (poller_pc \in PollerTerminal) => ~registered

\* DoneSound -- poll returns ready only with the flag actually set.
DoneSound == (poller_pc = "done") => flagged

Invariants ==
    /\ TypeOk
    /\ FlagImpliesReady
    /\ RecordedImpliesReady
    /\ NoMissedNetPoll
    /\ NoStaleHook
    /\ DoneSound

(***************************************************************************)
(* ============================== LIVENESS ================================ *)
(*                                                                         *)
(* PollerEventuallyServed -- once netd's socket is ready and the poller has  *)
(* registered, poll eventually returns. The whole point of the bridge: a    *)
(* ready socket must reach a parked poller through the probe -> reply ->     *)
(* demux -> relay-walk chain. Fairness grants netd NOTHING beyond the single *)
(* SocketReady edge; progress rides the elicited reply and the kthread       *)
(* relay:                                                                    *)
(*                                                                         *)
(*   WF(NetdReplyDemux) -- a ready socket with a probe outstanding eventually *)
(*                         elicits the reply (the kthread pumps the reader).  *)
(*   WF(KthreadWalk)    -- a pending relay flag eventually walks the hooks.   *)
(*   WF(PollerRegister), WF(PollerCommit) -- the poller eventually registers  *)
(*                         and re-evaluates after a wake.                     *)
(*                                                                         *)
(* Under BUGGY_LOST_READY this FAILS: with no probe ensured, NetdReplyDemux  *)
(* never enables, the relay never runs, and the poller never leaves          *)
(* "sleeping". (net_poll_liveness.cfg runs the clean spec.)                  *)
(***************************************************************************)
PollerEventuallyServed ==
    (ready /\ registered) ~> (poller_pc \in PollerTerminal)

Liveness ==
    /\ WF_vars(PollerRegister)
    /\ WF_vars(PollerCommit)
    /\ WF_vars(NetdReplyDemux)
    /\ WF_vars(KthreadWalk)

Spec_Live == Init /\ [][Next]_vars /\ Liveness

====
