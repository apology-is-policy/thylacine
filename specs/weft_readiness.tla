---- MODULE weft_readiness ----
(***************************************************************************)
(* Thylacine Weft -- the readiness ring (Weft-4); the Shenango single-      *)
(* cache-line readiness poke (NET-THROUGHPUT.md section 5.2-2 / 4.3; ARCH    *)
(* section 28 I-9, generalized to the shared-memory poke).                  *)
(*                                                                         *)
(* The Weft data plane (weft.tla) moves a flow's payload through a shared    *)
(* page with no per-op mediation. The remaining round-trip is the WAKE: a    *)
(* consumer parked waiting for the other side (a guest parked on its /net    *)
(* read, or netd waiting on a peer) must be woken when readiness arrives.    *)
(* Today that wake is a poll cadence (the ~50 ms RX-wake floor, NET-PERF     *)
(* N1). Weft-4 replaces it with a single shared cache line: the PRODUCER     *)
(* (netd) bumps a readiness edge counter (release); the CONSUMER (guest)     *)
(* observes it at memory speed (acquire) -- the busy-poll fast path -- and,   *)
(* when it would instead PARK, registers its park-intent and re-checks the   *)
(* counter so a poke in the window is not lost.                             *)
(*                                                                         *)
(* This is the PUSH counterpart of net_poll.tla's elicited PULL. net_poll    *)
(* keeps a readiness READ outstanding so netd has a request to answer (the   *)
(* dev9p.poll deferred Tread); Weft-4 ELIMINATES that probe -- netd writes    *)
(* the readiness edge straight into shared memory, so there is nothing to    *)
(* elicit. Distinct surface, distinct module (the cons_poll.tla / net_poll.  *)
(* tla / loom.tla precedent: a new I-9 wake mechanism gets its own focused   *)
(* model, leaving the audited weft.tla untouched).                          *)
(*                                                                         *)
(* WHAT THIS SPEC PINS                                                      *)
(*                                                                         *)
(*   The no-lost-wake is the STORE-BUFFER (SB) register-then-observe across  *)
(*   the shared cache line. Two parties write disjoint words and read the    *)
(*   other's; the bad outcome is BOTH reading the stale value -- the         *)
(*   consumer parks having observed "no edge" while the producer's edge,     *)
(*   posted in the window, found the consumer "not yet parked" and issued no *)
(*   wake. The discipline that forecloses it is REGISTER-then-OBSERVE: the   *)
(*   consumer publishes its park-intent (wait_active) BEFORE it re-reads the *)
(*   readiness counter, and the producer bumps the counter BEFORE it reads   *)
(*   the park-intent -- so at least one side sees the other's write (the     *)
(*   classic SB resolution: a full barrier / seq-cst on each side, the Linux *)
(*   set_current_state()+smp_mb() before the cond re-check). The impl        *)
(*   realizes the spec's atomic park/edge with __ATOMIC_SEQ_CST on each      *)
(*   handshake (kernel/weft.c), and the live Weft-6 wiring additionally      *)
(*   serializes the park-decision and the wake under the consumer's Rendez   *)
(*   lock (the #811 wait_lock register-then-observe) -- both are sound        *)
(*   realizations of the one atomic discipline modeled here. This spec       *)
(*   proves the consumer is never parked with an unprocessed readiness edge  *)
(*   (NoLostReadyWake -- I-9 across the shared-memory poke).                 *)
(*                                                                         *)
(* THE BUG THIS PINS                                                        *)
(*                                                                         *)
(*   BUGGY_OBSERVE_BEFORE_ARM -- the consumer re-reads the readiness counter *)
(*     BEFORE it publishes its park-intent (the inverted order). It observes *)
(*     "no edge", then -- in a separate step -- registers and parks. A       *)
(*     producer edge in the gap bumps the counter and reads wait_active      *)
(*     still clear (the consumer is not yet armed), so it issues NO wake;    *)
(*     the consumer then commits to park on the stale observation. The edge  *)
(*     is lost; the consumer sleeps forever on a ready channel               *)
(*     (NoLostReadyWake counterexample). The fix is the register-then-       *)
(*     observe order: publish the park-intent FIRST, then re-read the        *)
(*     counter, so an edge in the window is either seen (don't park) or      *)
(*     finds the consumer armed (the producer wakes it).                     *)
(*                                                                         *)
(* CFG MATRIX (executable documentation per CLAUDE.md spec-first policy)     *)
(*                                                                         *)
(*   weft_readiness.cfg                  BUGGY_OBSERVE_BEFORE_ARM FALSE --   *)
(*                                       every safety invariant holds.       *)
(*   weft_readiness_liveness.cfg         Spec_Live -- EventuallyDrained: a   *)
(*                                       posted readiness edge always        *)
(*                                       eventually reaches the consumer     *)
(*                                       (the poke wakes a parked consumer,  *)
(*                                       which then drains). The temporal    *)
(*                                       proof the wake never strands.       *)
(*   weft_readiness_buggy_lost_wake.cfg  BUGGY_OBSERVE_BEFORE_ARM --         *)
(*                                       NoLostReadyWake counterexample: the *)
(*                                       consumer parks with an unprocessed  *)
(*                                       edge and no pending wake.           *)
(*                                                                         *)
(* MODELING ASSUMPTIONS                                                      *)
(*                                                                         *)
(*   One readiness channel (one producer, one consumer). The channel is      *)
(*   direction-agnostic: Weft-6 instantiates one per direction (netd->guest  *)
(*   RX-ready, guest->netd TX-queued). One channel fully exercises the SB    *)
(*   register-then-observe; the N-channel fan is independent (each is one    *)
(*   writer per word, weft.tla leg (5)).                                     *)
(*                                                                         *)
(*   ready_seq is the producer's monotone edge counter (each readiness edge  *)
(*   bumps it); last_seen is the consumer's processed cursor. The consumer   *)
(*   drains to the current edge in one ConsumerProcess (last_seen := ready_  *)
(*   seq); modeling per-edge draining adds nothing to the wake property.     *)
(*   MaxSeq bounds the edges for TLC (2 in the clean/liveness cfgs: park ->   *)
(*   wake -> park -> wake; 1 in the buggy cfg suffices for the single lost   *)
(*   edge).                                                                  *)
(*                                                                         *)
(*   wait_active is CONSUMER-OWNED (single writer): the consumer sets it at  *)
(*   park and clears it on resume; the producer only READS it (to decide     *)
(*   whether to issue a Rendez wakeup) and NEVER writes it -- the single-    *)
(*   writer-per-word discipline. The producer's wake therefore re-schedules  *)
(*   the consumer (cons_pc parked -> running, modeling the Rendez wakeup);    *)
(*   the consumer clears wait_active when it next drains. A stale wait_active *)
(*   (set, consumer already running) at most costs a redundant -- harmless --  *)
(*   wakeup, never a lost one. The impl's wait_seq (the parked-at seq the    *)
(*   producer compares to avoid that redundant wakeup) is a wake-precision   *)
(*   optimization, not a safety obligation, so it is not modeled here.       *)
(*                                                                         *)
(*   Atomic actions model the seq-cst critical sections. The CORRECT park    *)
(*   (ConsumerPark) is ONE atomic step (publish park-intent + re-check the   *)
(*   counter -- the register-then-observe); BUGGY_OBSERVE_BEFORE_ARM splits   *)
(*   it into ConsumerObserve + ConsumerCommitPark to expose the window. A    *)
(*   producer edge (ProducerEdge) atomically bumps the counter and -- reading *)
(*   the park-intent in the same seq-cst-ordered step -- wakes an armed       *)
(*   consumer. The poll(-1)-style park is modeled (the consumer finishes a   *)
(*   park only when an edge wakes it), so a dropped wake strands it forever  *)
(*   -- the sharpest statement of the obligation; the timeout backstop is     *)
(*   tsleep.tla's, not re-pinned here.                                       *)
(*                                                                         *)
(* See NET-THROUGHPUT.md section 5.2-2 / 5.4 (the latency convergence);      *)
(* ARCHITECTURE.md section 28 invariant I-9; cons_poll.tla (the deferred     *)
(* relay wake -- the closest I-9 template); net_poll.tla (the elicited-pull  *)
(* counterpart this push replaces); scheduler.tla / tsleep.tla (the Rendez   *)
(* sleep the Weft-6 wiring builds on); kernel/weft.c (weft_ready_signal /    *)
(* weft_ready_observe / weft_ready_arm_park).                               *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS
    MaxSeq,                    \* the producer's edge-counter bound (TLC finiteness).
    BUGGY_OBSERVE_BEFORE_ARM   \* BOOLEAN -- TRUE: the consumer re-reads the readiness
                               \*   counter BEFORE publishing its park-intent (the
                               \*   inverted order). A producer edge in the gap is lost.

ASSUME MaxSeq \in Nat /\ MaxSeq >= 1
ASSUME BUGGY_OBSERVE_BEFORE_ARM \in BOOLEAN

VARIABLES
    ready_seq,      \* Nat -- the producer's monotone readiness edge counter (the
                    \*   shared cache line netd bumps on each RX/TX readiness edge).
    last_seen,      \* Nat -- the consumer's processed cursor (<= ready_seq). An edge
                    \*   is unprocessed iff last_seen < ready_seq.
    wait_active,    \* BOOLEAN -- consumer-owned: the consumer published "I am parking"
                    \*   (the register). The producer reads it to decide a wakeup.
    cons_pc         \* the consumer lifecycle (see ConsumerPCs).

vars == <<ready_seq, last_seen, wait_active, cons_pc>>

\* Consumer: "running"  -- active; the decision point (drain an edge, or park).
\*           "deciding" -- BUGGY path only: between observing the counter and
\*                         committing to park, NOT yet armed (the lost-wake window).
\*           "parked"   -- committed to sleep (wait_active published TRUE).
ConsumerPCs == {"running", "deciding", "parked"}

TypeOk ==
    /\ ready_seq   \in 0..MaxSeq
    /\ last_seen   \in 0..MaxSeq
    /\ last_seen <= ready_seq
    /\ wait_active \in BOOLEAN
    /\ cons_pc     \in ConsumerPCs

(***************************************************************************)
(* Init: no readiness edge posted, the consumer caught up and running, not  *)
(* parked.                                                                   *)
(***************************************************************************)
Init ==
    /\ ready_seq   = 0
    /\ last_seen   = 0
    /\ wait_active = FALSE
    /\ cons_pc     = "running"

(***************************************************************************)
(* ProducerEdge -- netd posts a readiness edge: it bumps the shared counter *)
(* (release) and, reading the consumer's park-intent in the same seq-cst-    *)
(* ordered step, wakes an armed consumer (re-schedules it -- the Rendez       *)
(* wakeup; it does NOT write the consumer-owned wait_active). A consumer     *)
(* that is running/deciding (not armed) is left as-is: there is nothing      *)
(* parked to wake, exactly as the impl reads wait_active = FALSE and issues  *)
(* no wakeup. Bounded by MaxSeq.                                             *)
(***************************************************************************)
ProducerEdge ==
    /\ ready_seq < MaxSeq
    /\ ready_seq' = ready_seq + 1
    /\ cons_pc'   = IF wait_active /\ cons_pc = "parked"
                    THEN "running"            \* the wakeup re-schedules the parked consumer
                    ELSE cons_pc              \* not parked: no wake (reads wait_active)
    /\ UNCHANGED <<last_seen, wait_active>>

(***************************************************************************)
(* ConsumerProcess -- the busy-poll fast path: the consumer, running, sees  *)
(* an unprocessed edge (ready_seq > last_seen) and drains to the current     *)
(* edge. Clears any stale park-intent it owns (it is demonstrably running).  *)
(* This is the lock-free acquire-load of the readiness counter + the in-     *)
(* place processing; no syscall, no wake needed.                             *)
(***************************************************************************)
ConsumerProcess ==
    /\ cons_pc = "running"
    /\ ready_seq > last_seen
    /\ last_seen'   = ready_seq
    /\ wait_active' = FALSE
    /\ UNCHANGED <<ready_seq, cons_pc>>

(***************************************************************************)
(* ConsumerPark -- the CORRECT go-to-park: register-then-observe in ONE     *)
(* atomic step. The guard last_seen = ready_seq IS the re-check (no edge to  *)
(* process); publishing wait_active = TRUE is the register. Because the      *)
(* publish and the re-check are atomic (seq-cst in the impl; the Rendez lock *)
(* at Weft-6), a ProducerEdge either PRECEDES this (the guard fails --        *)
(* ready_seq > last_seen -- so ConsumerProcess runs instead) or FOLLOWS it    *)
(* (wait_active is already TRUE, so ProducerEdge wakes the parked consumer). *)
(* No edge in the window is lost. The cons_poll.tla MgrSleep contract.       *)
(***************************************************************************)
ConsumerPark ==
    /\ ~BUGGY_OBSERVE_BEFORE_ARM
    /\ cons_pc = "running"
    /\ last_seen = ready_seq           \* the re-check: no unprocessed edge
    /\ wait_active' = TRUE             \* register: publish the park-intent
    /\ cons_pc'     = "parked"
    /\ UNCHANGED <<ready_seq, last_seen>>

(***************************************************************************)
(* ConsumerObserve / ConsumerCommitPark -- the BUGGY observe-then-register,  *)
(* split into two steps so a producer edge in the gap is lost.               *)
(*                                                                         *)
(*   ConsumerObserve     -- the consumer re-reads the counter (no edge seen) *)
(*                          and heads toward park. NOT yet armed             *)
(*                          (wait_active still FALSE).                       *)
(*   ConsumerCommitPark  -- the consumer commits to park on the stale        *)
(*                          observation, NOW publishing wait_active. A       *)
(*                          ProducerEdge between the two bumped the counter   *)
(*                          and read wait_active = FALSE (not armed), so it   *)
(*                          issued no wake; the consumer parks with the edge  *)
(*                          unprocessed and no wake coming.                   *)
(***************************************************************************)
ConsumerObserve ==
    /\ BUGGY_OBSERVE_BEFORE_ARM
    /\ cons_pc = "running"
    /\ last_seen = ready_seq           \* observe: no edge -- but not yet armed
    /\ cons_pc' = "deciding"
    /\ UNCHANGED <<ready_seq, last_seen, wait_active>>

ConsumerCommitPark ==
    /\ BUGGY_OBSERVE_BEFORE_ARM
    /\ cons_pc = "deciding"
    /\ wait_active' = TRUE             \* register AFTER the observe (too late)
    /\ cons_pc'     = "parked"
    /\ UNCHANGED <<ready_seq, last_seen>>

(***************************************************************************)
(* Done -- terminal self-loop. The legitimate quiescent terminal: all edges *)
(* posted (ready_seq = MaxSeq), the consumer caught up (last_seen = ready_   *)
(* seq) and parked, so no action is enabled. The explicit stutter keeps this *)
(* legitimate end-state from tripping TLC's -deadlock check, which then      *)
(* stays meaningful for the pre-terminal space (where the buggy lost-wake    *)
(* stuck state -- parked with last_seen < ready_seq -- lives, and which Done  *)
(* deliberately does NOT cover).                                             *)
(***************************************************************************)
Done ==
    /\ cons_pc = "parked"
    /\ last_seen = ready_seq
    /\ ready_seq = MaxSeq
    /\ UNCHANGED vars

Next ==
    \/ ProducerEdge
    \/ ConsumerProcess
    \/ ConsumerPark
    \/ ConsumerObserve
    \/ ConsumerCommitPark
    \/ Done

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

\* NoLostReadyWake -- ARCH section 28 I-9 across the shared-memory poke: the
\* consumer is never PARKED with an unprocessed readiness edge. In the correct
\* model a ProducerEdge wakes any armed consumer (it reads wait_active in the
\* same seq-cst-ordered step it bumps the counter), so a parked consumer is
\* always caught up. BUGGY_OBSERVE_BEFORE_ARM reaches the bad state: the
\* consumer commits to park after the edge fired without waking it.
NoLostReadyWake ==
    (cons_pc = "parked") => (last_seen = ready_seq)

\* ParkedIsArmed -- a parked consumer has published its park-intent. The
\* structural backing of the wake decision: the producer's "is the consumer
\* parked?" read (wait_active) is exactly the park state, so a real park is
\* always visible to a concurrent edge.
ParkedIsArmed ==
    (cons_pc = "parked") => wait_active

\* SeenBoundedByPosted -- the consumer never processes past the posted edge
\* (no spurious readiness): last_seen advances only to a ready_seq the producer
\* actually reached.
SeenBoundedByPosted ==
    last_seen <= ready_seq

Invariants ==
    /\ TypeOk
    /\ NoLostReadyWake
    /\ ParkedIsArmed
    /\ SeenBoundedByPosted

(***************************************************************************)
(* ============================== LIVENESS ================================ *)
(*                                                                         *)
(* EventuallyDrained -- once a readiness edge is posted (ready_seq > last_   *)
(* seen) the consumer always eventually processes it (last_seen catches up). *)
(* The whole point of the poke: a posted edge reaches the consumer, whether  *)
(* it is busy-polling (ConsumerProcess directly) or parked (the edge's wake  *)
(* re-schedules it, then ConsumerProcess drains). Fairness rides only the    *)
(* consumer's own forward actions -- the producer is granted nothing beyond   *)
(* the edges it posts:                                                        *)
(*                                                                         *)
(*   WF(ConsumerProcess) -- a running consumer with an unprocessed edge      *)
(*                          eventually drains it.                            *)
(*   WF(ConsumerPark)    -- a caught-up consumer eventually parks (so the    *)
(*                          next edge's wake path is exercised).             *)
(*                                                                         *)
(* The wake itself needs no separate fairness: ProducerEdge re-schedules a   *)
(* parked consumer atomically with the edge, so once the edge is posted the  *)
(* consumer is running and WF(ConsumerProcess) carries it. Under             *)
(* BUGGY_OBSERVE_BEFORE_ARM this FAILS: the consumer parks with the edge     *)
(* unprocessed and no wake re-schedules it, so ConsumerProcess never enables *)
(* and last_seen never catches up. (weft_readiness_liveness.cfg runs clean.) *)
(***************************************************************************)
EventuallyDrained ==
    (ready_seq > last_seen) ~> (last_seen = ready_seq)

Liveness ==
    /\ WF_vars(ConsumerProcess)
    /\ WF_vars(ConsumerPark)

Spec_Live == Init /\ [][Next]_vars /\ Liveness

====
