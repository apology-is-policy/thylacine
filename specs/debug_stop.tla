---- MODULE debug_stop ----
(***************************************************************************)
(* Thylacine Go-IDE Stage 8a: the debugger stop / continue / step state    *)
(* machine and its composition with the death path. Spec-first RE-ENABLED   *)
(* for this surface (user-voted 2026-07-14) -- an SMP wait/wake race on the  *)
(* most bug-prone lineage in the tree (#788/#806/#860/#809/#811/#68), the    *)
(* class the runtime tests are structurally blind to (the death_wake /       *)
(* loom / asid / allowance precedent). Design: docs/DEBUG-FS-DESIGN.md.      *)
(*                                                                         *)
(* WHAT THIS MODELS.                                                        *)
(*   A target Proc has N Threads. A debugger owns a revocable slot (the open *)
(*   debug ctl fd). The debugger `stop`s the target (per-Proc sflag), each   *)
(*   Thread parks at its EL0-return tail, and the debugger `start`s (or       *)
(*   detaches / dies) to resume. A group termination (gflag) races the whole *)
(*   thing. The tail order is die-check FIRST (death wins), stop-check       *)
(*   SECOND; a stopped Thread is inspected only after it parks.              *)
(*                                                                         *)
(* THE TAIL (per Thread). A Thread reaching the EL0-return tail:            *)
(*   - CORRECT order: die-check gflag; if set -> "dead"; else the stop       *)
(*     handshake (register-then-observe UNDER its wait_lock, the I-9 shape). *)
(*   - The handshake: acquire wlock -> register (findable) + observe sflag   *)
(*     atomically -> park ("stopped") if sflag still set, else proceed        *)
(*     ("el0"). Because it registers BEFORE observing, and the debugger's    *)
(*     confirm-walk takes the SAME wlock, the debugger can only confirm a     *)
(*     Thread that has genuinely parked -- no lost stop.                      *)
(*                                                                         *)
(* THE FIVE BUG CLASSES (one knob each, each a named buggy cfg).            *)
(*   BUGGY_STOP_BEFORE_DIE          -- the tail checks the stop BEFORE the    *)
(*       die-check, so a group-terminated Thread parks (and, on the death-   *)
(*       wake resume, re-parks) instead of dying -> death never completes    *)
(*       (DeathWinsOverStop / EventuallyAllDead).                            *)
(*   BUGGY_OBSERVE_BEFORE_REGISTER  -- the Thread observes sflag OUTSIDE the  *)
(*       lock and BEFORE registering, and the debugger confirms on that weak *)
(*       signal, so a Thread heading to EL0 is "confirmed stopped" while it   *)
(*       actually runs -> the debugger reads/writes a running target         *)
(*       (NoLostStop).                                                       *)
(*   BUGGY_DOUBLE_WAKE              -- resume has no single-wake latch, so a   *)
(*       `start` racing a `detach`/close both deliver a wakeup to one parked  *)
(*       Thread (ExactlyOnceResume).                                         *)
(*   BUGGY_STRAND_ON_CLOSE          -- releasing the slot (detach / ctl-fd     *)
(*       close / debugger death) neither clears the stop nor wakes the        *)
(*       parked Threads -> the target is stranded stopped forever (NoStrand). *)
(*   BUGGY_FAULT_STOP_UNGATED       -- the EC-path hardware fire (a bp / wp /  *)
(*       step completion) sets the per-Proc stop flag WITHOUT the `attached`  *)
(*       gate, so a fire racing a detach re-arms the stop after the slot was  *)
(*       released -> the target parks with no debugger left to resume it      *)
(*       (StopImpliesOwned -- 8a-2 SA-1). The correct EC path                 *)
(*       (proc_debug_fault_stop) delivers under g_proc_table_lock ONLY while  *)
(*       debug_owner != NULL, exactly RequestStop's gate for the hardware     *)
(*       trigger.                                                             *)
(*   BUGGY_STOP_SKIPS_SLEEPER       -- the stop does NOT wake an              *)
(*       interruptibly-SLEEPING Thread (the v1.0 Plan 9 non-preemptive stop:  *)
(*       proc_debug_stop_deliver flags + IPIs RUNNING peers but never wakes a *)
(*       sleeper). A Thread blocked in a syscall sleep when the stop arrives  *)
(*       is never driven to its EL0-return checkpoint, so it never parks, the *)
(*       target never fully-stops, and the debugger hangs on the halt forever *)
(*       (EventuallyStopSettles). The correct stop-of-a-sleeper (the          *)
(*       #811-analog for debug-stop) WAKES it -> it unwinds to the tail +     *)
(*       parks (the die-check still wins if a group-term also raced); on      *)
(*       resume its interrupted syscall RESTARTS. This is the multi-thread    *)
(*       Go-target blocker ground-truthed at 8c-1 (DELVE-PORT-DESIGN 17).     *)
(*       Note this is a LIVENESS bug (a hang), not a safety one: a sleeping   *)
(*       Thread is off-cpu (never confirmable, so NoEL0AfterStopped stays     *)
(*       vacuously safe) -- the target simply never becomes fully-stopped.    *)
(*   BUGGY_EXITKILL_IGNORED         -- releasing the slot on debugger DEATH    *)
(*       always RESUMES the target, even one the debugger LAUNCHED (marked     *)
(*       exitkill). The Plan 9 NoStrand-resume is correct for an ATTACHED      *)
(*       target (it pre-existed the debugger; leave it running) but WRONG for  *)
(*       a LAUNCHED one: a debugger-launched target must die WITH its launcher *)
(*       (PTRACE_O_EXITKILL), else it is orphaned to init and runs forever     *)
(*       (the HVF-idle debuggee leak). The correct release-cb (I-39 EXITKILL   *)
(*       refinement) TERMINATES an exitkill-marked target on debugger death    *)
(*       (proc_group_terminate, whose #811 cascade wakes the debug-parked      *)
(*       threads -> they die at the die-check) instead of proc_debug_resume.   *)
(*       An EXPLICIT detach still resumes (the debugger's deliberate choice --  *)
(*       it would use `kill` to terminate); only the IMPLICIT death-release     *)
(*       honors the mark. Violates EventuallyLaunchedDies.                      *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Threads,                        \* the target's Thread ids (>= 1; >= 2 to race)
    BUGGY_STOP_BEFORE_DIE,          \* TRUE = stop-check before die-check at the tail
    BUGGY_OBSERVE_BEFORE_REGISTER,  \* TRUE = observe sflag before registering
    BUGGY_DOUBLE_WAKE,              \* TRUE = resume has no single-wake latch
    BUGGY_STRAND_ON_CLOSE,          \* TRUE = slot release does not resume the target
    BUGGY_FAULT_STOP_UNGATED,       \* TRUE = the EC-path fire sets sflag without the attached gate
    BUGGY_STOP_SKIPS_SLEEPER,       \* TRUE = the stop does not wake an interruptibly-sleeping Thread
    BUGGY_EXITKILL_IGNORED          \* TRUE = death-release always resumes, even a launched (exitkill) target

ASSUME Cardinality(Threads) >= 1

\* Wake sources that can target a parked ("stopped") Thread:
\*   "start"   -- the debugger's `start` verb (resume_req)
\*   "release" -- detach / ctl-fd close / debugger death (release_req)
\*   "death"   -- the group-terminate cascade (gflag)
Sources == {"start", "release", "death"}

\* Thread program counters:
\*   "el0"      -- running at EL0 (the checkpoint target)
\*   "tail"     -- at the EL0-return tail (die-check + stop-check pending)
\*   "acq"      -- (correct) about to acquire wait_lock for the handshake
\*   "reg"      -- (correct) holds wait_lock, about to register + observe
\*   "obs_run"  -- (buggy) observed sflag=FALSE outside the lock -> will proceed
\*   "obs_stop" -- (buggy) observed sflag=TRUE  outside the lock -> will park
\*   "stopped"  -- parked on the debugger rendez (registered/findable)
\*   "dead"     -- terminated at the die-check (noreturn; never EL0)
\*   "sleep"    -- blocked in an interruptible syscall sleep (off-cpu; will NOT
\*                 reach the tail on its own -- it needs a wake). A pre-existing
\*                 sleeper is the multi-thread-stop hazard: the stop must drive it
\*                 to the checkpoint, else the target never fully-stops.
PCs == {"el0", "tail", "acq", "reg", "obs_run", "obs_stop", "stopped", "dead", "sleep"}

VARIABLES
    pc,           \* [Threads -> PCs]
    gflag,        \* group_exit_msg published (BOOLEAN, set once)
    sflag,        \* per-Proc stop requested (BOOLEAN, set once per episode)
    attached,     \* the debug slot is owned by a live ctl fd (BOOLEAN)
    dbg_live,     \* the debugger process is alive (BOOLEAN)
    detach_req,   \* an explicit `detach` was requested (BOOLEAN)
    resume_req,   \* a `start` was issued: wake all parked to EL0 (BOOLEAN)
    release_req,  \* a slot release was issued: wake all parked + detach (BOOLEAN)
    exitkill,     \* the target was LAUNCHED + marked kill-on-debugger-death (BOOLEAN)
    wlock,        \* [Threads -> BOOLEAN]  this Thread's wait_lock is held
    confirmed,    \* SUBSET Threads -- the debugger has confirmed these parked
    fired         \* [Threads -> [Sources -> BOOLEAN]]  wake delivered from a source

vars == <<pc, gflag, sflag, attached, dbg_live, detach_req,
          resume_req, release_req, exitkill, wlock, confirmed, fired>>

WokenOf(t) == \E s \in Sources : fired[t][s]
Active(s)  == \/ (s = "start"   /\ resume_req)
              \/ (s = "release" /\ release_req)
              \/ (s = "death"   /\ gflag)
NWake(t)   == Cardinality({s \in Sources : fired[t][s]})

\* Where the tail routes after the die-check passes: the correct handshake
\* (lock first) or the buggy out-of-lock observe.
HandshakeEntry(t) ==
    IF BUGGY_OBSERVE_BEFORE_REGISTER
      THEN IF sflag THEN "obs_stop" ELSE "obs_run"
      ELSE "acq"

TypeOk ==
    /\ pc \in [Threads -> PCs]
    /\ gflag \in BOOLEAN
    /\ sflag \in BOOLEAN
    /\ attached \in BOOLEAN
    /\ dbg_live \in BOOLEAN
    /\ detach_req \in BOOLEAN
    /\ resume_req \in BOOLEAN
    /\ release_req \in BOOLEAN
    /\ exitkill \in BOOLEAN
    /\ wlock \in [Threads -> BOOLEAN]
    /\ confirmed \subseteq Threads
    /\ fired \in [Threads -> [Sources -> BOOLEAN]]

Init ==
    /\ pc = [t \in Threads |-> "tail"]
    /\ gflag = FALSE
    /\ sflag = FALSE
    /\ attached = FALSE
    /\ dbg_live = TRUE
    /\ detach_req = FALSE
    /\ resume_req = FALSE
    /\ release_req = FALSE
    /\ exitkill = FALSE
    /\ wlock = [t \in Threads |-> FALSE]
    /\ confirmed = {}
    /\ fired = [t \in Threads |-> [s \in Sources |-> FALSE]]

(***************************************************************************)
(* ============================ THREAD ACTIONS =========================== *)
(***************************************************************************)

(* The EL0-return tail. CORRECT order checks the die-flag FIRST (death wins) *)
(* then routes to the stop handshake. BUGGY_STOP_BEFORE_DIE skips the        *)
(* die-check, so a flagged Thread enters the handshake and can re-park       *)
(* forever instead of dying.                                                *)
TailStep(t) ==
    /\ pc[t] = "tail"
    /\ IF BUGGY_STOP_BEFORE_DIE
         THEN pc' = [pc EXCEPT ![t] = HandshakeEntry(t)]
         ELSE IF gflag
                THEN pc' = [pc EXCEPT ![t] = "dead"]
                ELSE pc' = [pc EXCEPT ![t] = HandshakeEntry(t)]
    /\ UNCHANGED <<gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired>>

(* CORRECT: acquire the wait_lock (free -- the debugger's confirm-walk isn't  *)
(* mid-access on t) before touching registration/observation.               *)
Acquire(t) ==
    /\ pc[t] = "acq"
    /\ ~wlock[t]
    /\ wlock' = [wlock EXCEPT ![t] = TRUE]
    /\ pc' = [pc EXCEPT ![t] = "reg"]
    /\ UNCHANGED <<gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, confirmed, fired>>

(* CORRECT register-then-observe, UNDER the lock: the Thread is now findable  *)
(* (would be confirmable) and re-checks sflag atomically. Park if still set,  *)
(* else proceed to EL0. Release the lock.                                    *)
RegisterObserve(t) ==
    /\ pc[t] = "reg"
    /\ wlock[t]
    /\ wlock' = [wlock EXCEPT ![t] = FALSE]
    /\ IF sflag
         THEN pc' = [pc EXCEPT ![t] = "stopped"]
         ELSE pc' = [pc EXCEPT ![t] = "el0"]
    /\ UNCHANGED <<gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, confirmed, fired>>

(* BUGGY: the register happens AFTER the out-of-lock observe. A Thread that   *)
(* observed sflag=FALSE proceeds to EL0 even if the debugger has since set    *)
(* sflag and (buggily) confirmed it -- the lost stop.                        *)
RegisterBuggy(t) ==
    /\ pc[t] \in {"obs_run", "obs_stop"}
    /\ ~wlock[t]
    /\ IF pc[t] = "obs_stop"
         THEN pc' = [pc EXCEPT ![t] = "stopped"]
         ELSE pc' = [pc EXCEPT ![t] = "el0"]
    /\ UNCHANGED <<gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired>>

(* A Thread running at EL0 hits its next checkpoint (syscall / IRQ / tick)    *)
(* and re-enters the tail when a stop OR a death is pending. This is the      *)
(* Plan 9 non-preemptive stop: a running Thread stops/dies at its next        *)
(* checkpoint, not by interrupting it. (No pending work -> it keeps running.) *)
ReEnterTail(t) ==
    /\ pc[t] = "el0"
    /\ (sflag \/ gflag)
    /\ pc' = [pc EXCEPT ![t] = "tail"]
    /\ UNCHANGED <<gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired>>

(* A woken parked Thread leaves "stopped" back to the tail, where it re-runs  *)
(* the die-check (death wins on resume). The wake(s) are consumed; the        *)
(* debugger's confirmation of t is dropped.                                  *)
ResumeThread(t) ==
    /\ pc[t] = "stopped"
    /\ WokenOf(t)
    /\ pc' = [pc EXCEPT ![t] = "tail"]
    /\ confirmed' = confirmed \ {t}
    /\ fired' = [fired EXCEPT ![t] = [s \in Sources |-> FALSE]]
    /\ UNCHANGED <<gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock>>

(* A Thread running at EL0 makes a blocking syscall and SLEEPS (off-cpu on some *)
(* non-debug rendez -- a futex/torpor wait, a pipe/poll/read block). Enabled    *)
(* only with no stop/death pending: a Thread with a pending stop parks at its   *)
(* tail BEFORE it could start a new blocking wait. The hazard -- a stop/death   *)
(* arriving while a Thread ALREADY sleeps -- is modeled by EnterSleep (sflag     *)
(* clear) THEN RequestStop/SetGflag: the sleeper is then "sleep" with the flag   *)
(* set. (A resumed Thread returns to "el0" and may EnterSleep again -- the       *)
(* syscall-restart-on-resume of the correct stop-of-a-sleeper.)                 *)
EnterSleep(t) ==
    /\ pc[t] = "el0"
    /\ ~sflag
    /\ ~gflag
    /\ pc' = [pc EXCEPT ![t] = "sleep"]
    /\ UNCHANGED <<gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired>>

(* The stop-of-a-sleeper (the #811-analog for debug-stop) + the existing #811   *)
(* death-wake, modeled uniformly. A sleeping Thread is WOKEN when a              *)
(* group-terminate OR a stop is pending, and driven to its park via the         *)
(* die-check-first register-then-observe handshake -- modeled here as reaching   *)
(* "tail" (the die-check kills it on death; the handshake parks it on the        *)
(* debugger rendez on a stop). DEATH always wakes a sleeper (the shipped #811    *)
(* death-interruptible sleep). The STOP wakes it only in the CORRECT model;      *)
(* BUGGY_STOP_SKIPS_SLEEPER (the v1.0 Plan 9 non-preemptive stop) leaves a       *)
(* stop-only sleeper asleep forever -> the target never fully-stops              *)
(* (EventuallyStopSettles). The IMPL places that register-then-observe handshake *)
(* inside sleep() (DEBUG-FS-DESIGN 5c.2, the recommended nested park -- the      *)
(* syscall re-blocks in place on resume) or at the tail with syscall restart     *)
(* (5c.3); the load-bearing register-then-observe + death-first is identical, so *)
(* this action abstracts both. A resumed Thread returns toward EL0 (the syscall  *)
(* makes progress / re-blocks) -- modeled by the tail's normal ~sflag routing.   *)
StopWakesSleeper(t) ==
    /\ pc[t] = "sleep"
    /\ (gflag \/ (sflag /\ ~BUGGY_STOP_SKIPS_SLEEPER))
    /\ pc' = [pc EXCEPT ![t] = "tail"]
    /\ UNCHANGED <<gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired>>

(***************************************************************************)
(* =========================== DEBUGGER ACTIONS ========================== *)
(***************************************************************************)

(* Claim the one-debugger slot (the open ctl fd owns it; Einuse if taken).   *)
Attach ==
    /\ dbg_live
    /\ ~attached
    /\ attached' = TRUE
    /\ UNCHANGED <<pc, gflag, sflag, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired>>

(* The `stop` verb: set the per-Proc stop flag (once per episode).           *)
RequestStop ==
    /\ dbg_live
    /\ attached
    /\ ~sflag
    /\ ~resume_req
    /\ ~release_req
    /\ sflag' = TRUE
    /\ UNCHANGED <<pc, gflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired>>

(* The EC-path hardware fire (a bp / wp hit or a single-step completion) also  *)
(* requests the whole-Proc stop. Unlike the discretionary `stop` verb this is  *)
(* driven by the TARGET executing, and it arrives in the target's own          *)
(* exception context holding no lock (proc_debug_fault_stop then takes          *)
(* g_proc_table_lock to serialize with a concurrent detach). CORRECT: gated on  *)
(* `attached` -- deliver ONLY while a debugger owns the slot (debug_owner !=     *)
(* NULL), exactly RequestStop's gate. It does NOT require dbg_live: a fire in    *)
(* the debugger-dead-but-slot-not-yet-released window sets the flag, and         *)
(* ReleaseSlot then clears + wakes (still resumed). BUGGY_FAULT_STOP_UNGATED     *)
(* drops the gate (the pre-fix EC path set debug_stop_req with no lock + no      *)
(* owner check), so a fire racing a detach sets sflag with no debugger attached  *)
(* -> StopImpliesOwned fails and the target strands (SA-1).                      *)
FaultStop ==
    /\ (BUGGY_FAULT_STOP_UNGATED \/ attached)
    /\ ~sflag
    /\ ~resume_req
    /\ (BUGGY_FAULT_STOP_UNGATED \/ ~release_req)
    /\ sflag' = TRUE
    /\ UNCHANGED <<pc, gflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired>>

(* The delivery walk: mark t confirmed-parked, under t's wait_lock (so it     *)
(* cannot interleave with t's register-then-observe). CORRECT confirms only a *)
(* genuinely parked Thread; BUGGY trusts the out-of-lock observe (an obs_     *)
(* state), confirming a Thread that may still run.                           *)
Confirm(t) ==
    /\ dbg_live
    /\ attached
    /\ sflag
    /\ ~wlock[t]
    /\ t \notin confirmed
    /\ IF BUGGY_OBSERVE_BEFORE_REGISTER
         THEN pc[t] \in {"stopped", "obs_run", "obs_stop"}
         ELSE pc[t] = "stopped"
    /\ confirmed' = confirmed \cup {t}
    /\ UNCHANGED <<pc, gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, fired>>

(* The `start` verb: resume a completed stop -- clear sflag, arm the start    *)
(* wake source.                                                             *)
StartResume ==
    /\ dbg_live
    /\ attached
    /\ sflag
    /\ confirmed = Threads
    /\ sflag' = FALSE
    /\ resume_req' = TRUE
    /\ UNCHANGED <<pc, gflag, attached, dbg_live, detach_req,
                   release_req, exitkill, wlock, confirmed, fired>>

(* An explicit `detach` request.                                            *)
DetachReq ==
    /\ attached
    /\ dbg_live
    /\ ~detach_req
    /\ detach_req' = TRUE
    /\ UNCHANGED <<pc, gflag, sflag, attached, dbg_live,
                   resume_req, release_req, exitkill, wlock, confirmed, fired>>

(* The `exitkill` verb: mark the target as debugger-LAUNCHED, so a slot release *)
(* on debugger DEATH terminates it (die-with-launcher) rather than resuming it. *)
(* The debugger sets this once, right after attaching a target it spawned; an   *)
(* attached (pre-existing) target is never marked, so it resumes on release.     *)
(* Owner-gated (attached) in the impl -- the ctl `exitkill` verb.               *)
MarkExitkill ==
    /\ dbg_live
    /\ attached
    /\ ~exitkill
    /\ exitkill' = TRUE
    /\ UNCHANGED <<pc, gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, wlock, confirmed, fired>>

(* The debugger process dies (crash / kill). Its handle table closes at exit  *)
(* (#68/#926), which releases the slot below.                                *)
DbgDie ==
    /\ dbg_live
    /\ dbg_live' = FALSE
    /\ UNCHANGED <<pc, gflag, sflag, attached, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired>>

(* Release the slot on detach OR ctl-fd close (incl. debugger death). CORRECT  *)
(* clears the stop and arms the release wake source (resume the target) -- the *)
(* NoStrand-resume -- EXCEPT for a LAUNCHED (exitkill-marked) target released   *)
(* by debugger DEATH (not an explicit detach): that one is TERMINATED (gflag,   *)
(* the die-with-launcher I-39 EXITKILL refinement -- proc_group_terminate,      *)
(* whose #811 cascade wakes the debug-parked threads to die at the die-check).  *)
(* An explicit detach (detach_req) always resumes -- the debugger's deliberate  *)
(* choice; it would send `kill` to terminate. BUGGY_STRAND_ON_CLOSE frees the   *)
(* slot but neither clears sflag nor wakes (the stranded target). BUGGY_EXITKILL*)
(* _IGNORED drops the launched distinction -> always resumes -> the launched    *)
(* target is orphaned and runs forever (EventuallyLaunchedDies).                *)
(*                                                                              *)
(* MODELING BOUNDARY (audit F1): the IMPL's release-cb runs on the TARGET and   *)
(* cannot observe the debugger's liveness, so it terminates a marked ALIVE      *)
(* target on ANY ctl-fd close WITHOUT a prior detach -- i.e. debugger DEATH     *)
(* (~dbg_live, modeled here, the load-bearing #68 leak scenario) OR a LIVE      *)
(* debugger's bare SYS_CLOSE of the fd (dbg_live /\ ~detach_req, NOT a distinct *)
(* action here). Both have the IDENTICAL outcome (terminate a marked launched   *)
(* child), and the live-bare-close is unexercised (ambush always sends          *)
(* kill/detach first) + sound (within the debugger's slot authority), so this   *)
(* model abstracts it as the ~dbg_live case rather than adding a same-outcome   *)
(* bare-close action. The load-bearing property (EventuallyLaunchedDies on      *)
(* debugger death) is the one proven.                                           *)
ReleaseSlot ==
    /\ attached
    /\ (detach_req \/ ~dbg_live)
    /\ attached' = FALSE
    /\ confirmed' = {}
    /\ IF BUGGY_STRAND_ON_CLOSE
         THEN UNCHANGED <<sflag, release_req, gflag>>
         ELSE IF (~BUGGY_EXITKILL_IGNORED /\ exitkill /\ ~dbg_live /\ ~detach_req)
                THEN /\ sflag' = FALSE
                     /\ gflag' = TRUE
                     /\ UNCHANGED release_req
                ELSE /\ sflag' = FALSE
                     /\ release_req' = TRUE
                     /\ UNCHANGED gflag
    /\ UNCHANGED <<pc, dbg_live, detach_req, resume_req, exitkill, wlock, fired>>

(***************************************************************************)
(* ============================= DEATH PATH ============================== *)
(***************************************************************************)

(* A group termination publishes gflag once (kill / SYS_EXIT_GROUP / an LS-5  *)
(* terminate-interrupt).                                                    *)
SetGflag ==
    /\ ~gflag
    /\ gflag' = TRUE
    /\ UNCHANGED <<pc, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired>>

(***************************************************************************)
(* ============================ WAKE DELIVERY =========================== *)
(***************************************************************************)

(* Deliver a wake to a parked Thread from an active source, under t's         *)
(* wait_lock. CORRECT: a single-wake latch (~WokenOf) -- only the first       *)
(* source wakes; a second is a no-op. BUGGY_DOUBLE_WAKE drops the latch, so a *)
(* start racing a release both deliver -> two wakes to one park.             *)
WakeFrom(t, s) ==
    /\ pc[t] = "stopped"
    /\ ~wlock[t]
    /\ Active(s)
    /\ ~fired[t][s]
    /\ IF BUGGY_DOUBLE_WAKE THEN TRUE ELSE ~WokenOf(t)
    /\ fired' = [fired EXCEPT ![t][s] = TRUE]
    /\ UNCHANGED <<pc, gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed>>

Next ==
    \/ \E t \in Threads : TailStep(t)
    \/ \E t \in Threads : Acquire(t)
    \/ \E t \in Threads : RegisterObserve(t)
    \/ \E t \in Threads : RegisterBuggy(t)
    \/ \E t \in Threads : ReEnterTail(t)
    \/ \E t \in Threads : ResumeThread(t)
    \/ \E t \in Threads : EnterSleep(t)
    \/ \E t \in Threads : StopWakesSleeper(t)
    \/ \E t \in Threads : Confirm(t)
    \/ \E t \in Threads : \E s \in Sources : WakeFrom(t, s)
    \/ Attach
    \/ RequestStop
    \/ FaultStop
    \/ StartResume
    \/ DetachReq
    \/ MarkExitkill
    \/ DbgDie
    \/ ReleaseSlot
    \/ SetGflag

(* Weak fairness on the mechanical progress actions (the Threads' handshake,  *)
(* re-entry, resume, and every wake / slot release). The debugger's           *)
(* discretionary verbs (attach / stop / confirm / start / detach / die) and   *)
(* the kill (SetGflag) are NOT forced -- the liveness properties must hold     *)
(* against every schedule of those, once they have happened.                 *)
Fairness ==
    /\ \A t \in Threads : WF_vars(TailStep(t))
    /\ \A t \in Threads : WF_vars(Acquire(t))
    /\ \A t \in Threads : WF_vars(RegisterObserve(t))
    /\ \A t \in Threads : WF_vars(RegisterBuggy(t))
    /\ \A t \in Threads : WF_vars(ReEnterTail(t))
    /\ \A t \in Threads : WF_vars(ResumeThread(t))
    /\ \A t \in Threads : WF_vars(StopWakesSleeper(t))
    /\ \A t \in Threads : \A s \in Sources : WF_vars(WakeFrom(t, s))
    /\ WF_vars(ReleaseSlot)

Spec == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************)
(* ============================== INVARIANTS ============================= *)
(***************************************************************************)

(* NoLostStop (I-9 register-then-observe soundness): every Thread the         *)
(* debugger has CONFIRMED is genuinely parked. The correct handshake makes    *)
(* confirm sound (a confirmed Thread observed sflag under the lock and        *)
(* parked); the observe-before-register bug lets the debugger confirm a       *)
(* Thread that then runs at EL0 -- so it inspects a running target.           *)
NoLostStop ==
    \A t \in Threads : (t \in confirmed) => (pc[t] = "stopped")

(* NoEL0AfterStopped: once the debugger has confirmed the WHOLE target        *)
(* stopped, no Thread is executing at EL0 -- the frozen window a coherent     *)
(* mem/reg read relies on is real.                                          *)
NoEL0AfterStopped ==
    (confirmed = Threads) => (\A t \in Threads : pc[t] # "el0")

(* ExactlyOnceResume: a parked Thread receives at most one wakeup. The        *)
(* single-wake latch keeps a start racing a detach/close from double-waking   *)
(* one park (a lost / spurious wake on the reused rendez).                    *)
ExactlyOnceResume ==
    \A t \in Threads : NWake(t) <= 1

(* StopImpliesOwned (8a-2 SA-1): the per-Proc stop flag is set only while a    *)
(* debugger owns the slot. RequestStop and the CORRECT FaultStop both gate on  *)
(* `attached`, and ReleaseSlot clears sflag and attached together, so the stop *)
(* flag can never outlive the owner -- there is always a debugger (or a        *)
(* pending ReleaseSlot on debugger death) to resume the target. The ungated    *)
(* fault-stop (the pre-fix EC path) violates it: a fire sets sflag with no     *)
(* owner, so a parked target has no debugger left to resume it -> the strand.  *)
(* proc_debug_fault_stop's debug_owner check under g_proc_table_lock is the    *)
(* fix -- it serializes the fire against detach's slot release + resume.       *)
StopImpliesOwned == sflag => attached

Safety ==
    /\ TypeOk
    /\ NoLostStop
    /\ NoEL0AfterStopped
    /\ ExactlyOnceResume
    /\ StopImpliesOwned

(* DeathWinsOverStop (liveness): once a group termination is published, every *)
(* Thread eventually dies -- even against a live debugger holding a stop.     *)
(* The die-check-first tail order + the death cascade waking parked Threads   *)
(* guarantee it; stop-before-die breaks it (a parked Thread re-parks on the   *)
(* death-wake resume and never dies).                                       *)
EventuallyAllDead ==
    gflag ~> (\A t \in Threads : pc[t] = "dead")

(* NoStrand (liveness): if the debugger releases its slot or dies while       *)
(* holding a stop, the target is eventually resumed -- no debugger can strand *)
(* its quarry. The handle-lifetime-tied release guarantees it; the strand bug *)
(* breaks it (the slot frees but the stop is never cleared / woken).          *)
EventuallyResumed ==
    (attached /\ ~dbg_live) ~> (\A t \in Threads : pc[t] # "stopped")

(* EventuallyLaunchedDies (liveness -- the I-39 EXITKILL refinement): a          *)
(* debugger-LAUNCHED target (exitkill-marked) whose debugger DIES without an     *)
(* explicit detach eventually DIES -- it does not survive its launcher. The      *)
(* correct death-release (ReleaseSlot's exitkill branch -> gflag ->              *)
(* proc_group_terminate) guarantees it: the death cascade drives every Thread    *)
(* to the die-check. BUGGY_EXITKILL_IGNORED breaks it -- the launched target is  *)
(* resumed instead, orphaned to init, and runs forever (the leak). This REFINES  *)
(* NoStrand: for a launched target "not stranded" means dead, not resumed (and   *)
(* EventuallyResumed still holds -- "dead" is not "stopped").                    *)
EventuallyLaunchedDies ==
    (exitkill /\ ~dbg_live /\ ~detach_req) ~> (\A t \in Threads : pc[t] = "dead")

(* EventuallyStopSettles (liveness -- the multi-thread-stop COMPLETES): once a   *)
(* stop is requested and owned, the target eventually SETTLES -- either every    *)
(* Thread reaches a quiescent stop state (all "stopped", or "dead" if a group-   *)
(* terminate also raced) OR the debugger has cleared the stop (start / release). *)
(* The correct stop-of-a-sleeper guarantees it: StopWakesSleeper drives every    *)
(* sleeping Thread to its checkpoint, so the halt completes and the debugger's   *)
(* Confirm can reach all Threads. BUGGY_STOP_SKIPS_SLEEPER breaks it: a Thread    *)
(* sleeping when the stop arrives is never woken, so it never parks, the target   *)
(* never fully-stops, sflag can never clear (StartResume needs confirmed =        *)
(* Threads), and the debugger hangs on the halt forever -- the exact 8c-1         *)
(* multi-thread-Go-target blocker (DELVE-PORT-DESIGN 17). Safety is untouched     *)
(* (a sleeper is off-cpu, never confirmable, so NoEL0AfterStopped stays vacuous); *)
(* this is purely the hang.                                                       *)
EventuallyStopSettles ==
    (sflag /\ attached) ~> ( \/ (\A t \in Threads : pc[t] \in {"stopped", "dead"})
                             \/ ~sflag )

====
