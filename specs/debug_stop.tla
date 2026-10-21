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
(* THE SEVEN BUG CLASSES (one knob each, each a named buggy cfg).           *)
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
(*                                                                         *)
(* THE BIRTH HOLD (DEBUG-FS-DESIGN 5f). With HELD the target is a child just *)
(* spawned SPAWN_DEBUG_HELD: one head Thread, born "unborn" (in exec_setup)  *)
(* with the per-Proc mark hold = "unborn". It reaches the birth tail, marks  *)
(* itself "parked" (waking its spawner's birth wait), and parks on its own   *)
(* rendez until the hold AND the stop are both clear -- never at EL0 while  *)
(* held. The debugger's `stop` CONVERTS the hold (deliver the stop, THEN     *)
(* clear the hold -- two stores in one g_proc_table_lock section, which the  *)
(* park's wait_lock observe can fall between); `start` and an explicit       *)
(* `detach` RELEASE it; the ctl-fd close without detach KEEPS it. The        *)
(* spawner waits (synchronously) until the child is no longer "unborn" or    *)
(* is dead, woken by every write of the mark and by the child's death; when  *)
(* the spawner dies with the hold still set, the child is killed (the        *)
(* operator's vote -- the orphan rule). Five knobs:                          *)
(*   BUGGY_HELD_RUNS_FREE       -- the birth park ignores the hold, so the   *)
(*       child runs before any debugger has it (NoEL0WhileHeld).             *)
(*   BUGGY_CONVERT_CLEARS_FIRST -- the conversion clears the hold BEFORE it   *)
(*       delivers the stop, so a park observing between the two stores sees  *)
(*       neither and runs (NoEL0WhileHeld).                                  *)
(*   BUGGY_ORPHAN_HOLD_STRANDS  -- no orphan rule: a hold whose spawner died *)
(*       parks the child forever (EventuallyHoldResolved).                   *)
(*   BUGGY_BIRTH_WAIT_UNWOKEN   -- clearing the hold does not wake the       *)
(*       spawner; only the "parked" mark does. A release that lands before  *)
(*       the child parks leaves the spawner asleep while the child runs     *)
(*       (BirthWaitReleases).                                                *)
(*   BUGGY_NO_DEATH_RECHECK     -- a park (the tail's or the birth park:     *)
(*       one loop in the impl) proceeds on its wake condition without        *)
(*       re-checking death. The EXITKILL release publishes gflag and only    *)
(*       then clears the stop, so a Thread that passed the loop's death      *)
(*       check just before the terminate reads the cleared stop and erets:   *)
(*       a dying child runs its first instructions (NoEL0WhileHeld), a       *)
(*       stopped Thread the ones after its stop (NoEretIntoDeath). Found by  *)
(*       the clean held cfg, 2026-09-29; the impl re-checks after the wake   *)
(*       condition, where the release/acquire pairing makes gflag visible.  *)
(* A LATCHED INTERRUPT (LS-5c; DEBUG-FS-DESIGN 5g). An interrupt-terminate  *)
(* may be latched on the target at any time (PostInterrupt; the ghost       *)
(* `latch`), and its wake (source "intr") reaches a Thread in either park.  *)
(* Every park sleeps death-only, so the wake is absorbed: the sleep         *)
(* re-checks the park's condition and sleeps again, and a parked Thread     *)
(* keeps its stop or hold until that clears or its group dies. A Thread     *)
(* that runs again meets the note at a checkpoint outside the model (the    *)
(* tail's note delivery is not modelled). The ghost is a latch the parks    *)
(* see: the impl acts on one only in a family the thread has not masked.    *)
(* The held spawn's birth wait is death-only too: a latch on the spawner    *)
(* (`slatch`) does not end it. Four knobs, each a latch exit a wait had:    *)
(*   BUGGY_TAIL_LATCH_ERETS      -- the tail park leaves on the latch and   *)
(*       the tail erets, as before 5g: a stopped Thread runs while the      *)
(*       debugger still holds its confirmation (NoLostStop,                 *)
(*       NoEL0AfterStopped).                                                *)
(*   BUGGY_BIRTH_LATCH_ERETS     -- the birth park leaves the same way: a   *)
(*       held child runs (NoEL0WhileHeld; audit round 1, F1).               *)
(*   BUGGY_LATCH_ENDS_STOP       -- a park ends its Thread on the latch, as *)
(*       the birth park did from 5f until 5g (ParkEndsOnlyInDeath).         *)
(*   BUGGY_SPAWNER_LATCH_RETURNS -- the birth wait returns on the           *)
(*       spawner's latch, as await_child_release did before 5g: the spawn   *)
(*       returns while its child is still loading (SpawnReturnsAfterBirth). *)
(* With HELD = FALSE every birth variable is constant and every birth       *)
(* action disabled; the latch is not a birth variable. Adding               *)
(* BUGGY_NO_DEATH_RECHECK gives the pre-5f model, the latch aside; without  *)
(* it the tail park also re-checks death.                                   *)
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
    BUGGY_EXITKILL_IGNORED,         \* TRUE = death-release always resumes, even a launched (exitkill) target
    HELD,                           \* TRUE = the target was spawned held (DEBUG-FS-DESIGN 5f)
    BUGGY_HELD_RUNS_FREE,           \* TRUE = the birth park ignores the hold
    BUGGY_CONVERT_CLEARS_FIRST,     \* TRUE = the conversion clears the hold before it delivers the stop
    BUGGY_ORPHAN_HOLD_STRANDS,      \* TRUE = no orphan rule for a hold whose spawner died
    BUGGY_BIRTH_WAIT_UNWOKEN,       \* TRUE = clearing the hold does not wake the spawner's birth wait
    BUGGY_NO_DEATH_RECHECK,         \* TRUE = the park erets on its wake condition without re-checking death
    BUGGY_BIRTH_LATCH_ERETS,        \* TRUE = the birth park leaves on the latch and erets
    BUGGY_TAIL_LATCH_ERETS,         \* TRUE = the tail park leaves on the latch and erets
    BUGGY_LATCH_ENDS_STOP,          \* TRUE = a park ends its Thread on the latch
    BUGGY_SPAWNER_LATCH_RETURNS     \* TRUE = the birth wait returns on the spawner's latch

ASSUME Cardinality(Threads) >= 1
\* A held child has executed nothing, so it has made no Threads: only its head.
ASSUME HELD => Cardinality(Threads) = 1

\* Wake sources that can target a parked ("stopped") Thread:
\*   "start"   -- the debugger's `start` verb (resume_req)
\*   "release" -- detach / ctl-fd close / debugger death (release_req)
\*   "death"   -- the group-terminate cascade (gflag)
\*   "intr"    -- a latched interrupt's wake (latch), which a park absorbs
Sources == {"start", "release", "death", "intr"}

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
\* The birth path of a held child (HELD only):
\*   "unborn"   -- the head Thread, still in exec_setup
\*   "btail"    -- at the birth tail, about to mark itself parked (under the lock)
\*   "bloop"    -- the birth park's loop top: death check, then the handshake
\*   "bacq"     -- about to acquire wait_lock for the birth handshake
\*   "breg"     -- holds wait_lock: register + observe the hold and the stop
\*   "bstopped" -- parked at the birth park (registered/findable, confirmable)
BirthPCs == {"unborn", "btail", "bloop", "bacq", "breg", "bstopped"}
PCs == {"el0", "tail", "acq", "reg", "obs_run", "obs_stop", "stopped", "dead", "sleep"}
       \cup BirthPCs

HoldVals  == {"none", "unborn", "parked"}
\* The spawner's birth wait: "scan" (about to check the child, atomically
\* registering on child_waiters if it must wait), "asleep" (registered),
\* "returned" (the spawn returned), "dead" (the spawner exited).
SpawnerPCs == {"scan", "asleep", "returned", "dead"}
\* The conversion's second store, pending inside its lock section.
CSteps == {"idle", "clear_hold", "set_stop"}

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
    fired,        \* [Threads -> [Sources -> BOOLEAN]]  wake delivered from a source
    hold,         \* the birth hold mark (Proc.debug_birth_hold)
    spc,          \* the spawner's birth-wait state
    swake,        \* a wake is pending on the spawner's child_waiters registration
    cstep,        \* the conversion's second store, still to come
    licensed,     \* ghost: the child may run -- its hold was released, or its
                  \* converted stop resumed (TRUE from the start when ~HELD)
    latch,        \* an interrupt-terminate is latched on the target (LS-5c)
    slatch        \* one is latched on the held spawn's spawner, inside its wait

\* The variables the tail's own actions never write: the birth hold's, and the
\* two latches.
bvars == <<hold, spc, swake, cstep, licensed, latch, slatch>>

vars == <<pc, gflag, sflag, attached, dbg_live, detach_req,
          resume_req, release_req, exitkill, wlock, confirmed, fired,
          hold, spc, swake, cstep, licensed, latch, slatch>>

WokenOf(t) == \E s \in Sources : fired[t][s]
Active(s)  == \/ (s = "start"   /\ resume_req)
              \/ (s = "release" /\ release_req)
              \/ (s = "death"   /\ gflag)
              \/ (s = "intr"    /\ latch)
NWake(t)   == Cardinality({s \in Sources : fired[t][s]})

\* Where the tail routes after the die-check passes: the correct handshake
\* (lock first) or the buggy out-of-lock observe.
HandshakeEntry(t) ==
    IF BUGGY_OBSERVE_BEFORE_REGISTER
      THEN IF sflag THEN "obs_stop" ELSE "obs_run"
      ELSE "acq"

\* A Thread parked on its own debug rendez: at the tail, or at the birth park.
Parked(t) == pc[t] \in {"stopped", "bstopped"}

\* Only the latch's wake fired: the one wake that clears neither a stop nor a
\* hold, so a park absorbs it (5g).
IntrOnly(t) == fired[t]["intr"] /\ \A s \in Sources \ {"intr"} : ~fired[t][s]

\* Where a park sends a Thread that meets a latched interrupt with its wake
\* condition false and no death published: nowhere -- it stays parked (5g).
\* The pre-5g exits: `erets` (the park's own knob) returns to EL0, and
\* BUGGY_LATCH_ENDS_STOP ends the Thread.
LatchExit(t, erets, parked) ==
    IF erets THEN [pc EXCEPT ![t] = "el0"]
    ELSE IF BUGGY_LATCH_ENDS_STOP THEN [pc EXCEPT ![t] = "dead"]
    ELSE [pc EXCEPT ![t] = parked]

ChildAlive == \E t \in Threads : pc[t] # "dead"

\* spawn_birth_released: the child reached its park or had its hold released
\* (anything but "unborn"), or is dead.
Released == ~ChildAlive \/ hold # "unborn"

\* No conversion is mid-section: an action that takes g_proc_table_lock cannot
\* run between the conversion's two stores.
Quiet == cstep = "idle"

\* Every write of the mark wakes the spawner's registration (a no-op when it
\* is not registered: it then re-reads the mark at its next scan).
SpawnerWake == IF spc = "asleep" THEN swake' = TRUE ELSE UNCHANGED swake

\* A write that CLEARS the hold. BUGGY_BIRTH_WAIT_UNWOKEN drops its wake.
SpawnerWakeOnClear == IF BUGGY_BIRTH_WAIT_UNWOKEN THEN UNCHANGED swake ELSE SpawnerWake

\* The child's death wakes its parent (proc_become_zombie_locked), so a step
\* that leaves every Thread dead wakes a registered spawner.
SpawnerDeathWake(np) ==
    IF spc = "asleep" /\ (\A u \in Threads : np[u] = "dead")
      THEN swake' = TRUE ELSE UNCHANGED swake

\* The birth park's wake condition: the hold AND the stop clear. Read in one
\* step here; the impl reads the hold first (ACQUIRE) against the conversion's
\* stop-then-clear (RELEASE), which is the same guarantee.
BirthWakeCond ==
    IF BUGGY_HELD_RUNS_FREE THEN ~sflag ELSE (hold = "none" /\ ~sflag)

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
    /\ hold \in HoldVals
    /\ spc \in SpawnerPCs
    /\ swake \in BOOLEAN
    /\ cstep \in CSteps
    /\ licensed \in BOOLEAN
    /\ latch \in BOOLEAN
    /\ slatch \in BOOLEAN

Init ==
    /\ pc = [t \in Threads |-> IF HELD THEN "unborn" ELSE "tail"]
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
    /\ hold = IF HELD THEN "unborn" ELSE "none"
    /\ spc = IF HELD THEN "scan" ELSE "returned"
    /\ swake = FALSE
    /\ cstep = "idle"
    /\ licensed = ~HELD
    /\ latch = FALSE
    /\ slatch = FALSE

(***************************************************************************)
(* ============================ THREAD ACTIONS =========================== *)
(***************************************************************************)

(* The EL0-return tail. CORRECT order checks the die-flag FIRST (death wins) *)
(* then routes to the stop handshake. BUGGY_STOP_BEFORE_DIE skips the        *)
(* die-check, so a flagged Thread enters the handshake and can re-park       *)
(* forever instead of dying.                                                *)
TailStep(t) ==
    /\ pc[t] = "tail"
    /\ LET np == IF BUGGY_STOP_BEFORE_DIE
                    THEN [pc EXCEPT ![t] = HandshakeEntry(t)]
                    ELSE IF gflag
                           THEN [pc EXCEPT ![t] = "dead"]
                           ELSE [pc EXCEPT ![t] = HandshakeEntry(t)]
       IN /\ pc' = np
          /\ SpawnerDeathWake(np)
    /\ UNCHANGED <<gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired,
                   hold, spc, cstep, licensed, latch, slatch>>

(* CORRECT: acquire the wait_lock (free -- the debugger's confirm-walk isn't  *)
(* mid-access on t) before touching registration/observation.               *)
Acquire(t) ==
    /\ pc[t] = "acq"
    /\ ~wlock[t]
    /\ wlock' = [wlock EXCEPT ![t] = TRUE]
    /\ pc' = [pc EXCEPT ![t] = "reg"]
    /\ UNCHANGED <<gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, confirmed, fired>>
    /\ UNCHANGED bvars

(* CORRECT register-then-observe, UNDER the lock: the Thread is now findable  *)
(* (would be confirmable) and re-checks sflag atomically. Park if still set,  *)
(* else proceed to EL0 -- unless death was published since the tail's die-  *)
(* check (the EXITKILL release terminates, THEN clears the stop this reads). *)
(* Release the lock. The latch changes nothing here but in a pre-5g exit     *)
(* (LatchExit).                                                              *)
RegisterObserve(t) ==
    /\ pc[t] = "reg"
    /\ wlock[t]
    /\ wlock' = [wlock EXCEPT ![t] = FALSE]
    /\ LET np == IF sflag
                    THEN IF latch /\ ~gflag
                           THEN LatchExit(t, BUGGY_TAIL_LATCH_ERETS, "stopped")
                           ELSE [pc EXCEPT ![t] = "stopped"]
                    ELSE IF gflag /\ ~BUGGY_NO_DEATH_RECHECK
                           THEN [pc EXCEPT ![t] = "dead"]
                           ELSE [pc EXCEPT ![t] = "el0"]
       IN /\ pc' = np
          /\ SpawnerDeathWake(np)
    /\ UNCHANGED <<gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, confirmed, fired>>
    /\ UNCHANGED <<hold, spc, cstep, licensed, latch, slatch>>

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
    /\ UNCHANGED bvars

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
    /\ UNCHANGED bvars

(* A woken parked Thread leaves "stopped" back to the tail, where it re-runs  *)
(* the die-check (death wins on resume). The wake(s) are consumed; the        *)
(* debugger's confirmation of t is dropped.                                  *)
(* The latch's wake alone, with the stop standing and no death published, is *)
(* absorbed (5g): the death-only sleep re-checks and sleeps again, and t     *)
(* stays parked and confirmed. A pre-5g exit leaves on it (LatchExit), the   *)
(* debugger still holding t's confirmation unless t died.                    *)
ResumeThread(t) ==
    /\ pc[t] = "stopped"
    /\ WokenOf(t)
    /\ LET absorbed == IntrOnly(t) /\ sflag /\ ~gflag
           np == IF absorbed
                   THEN LatchExit(t, BUGGY_TAIL_LATCH_ERETS, "stopped")
                   ELSE [pc EXCEPT ![t] = "tail"]
       IN /\ pc' = np
          /\ confirmed' = IF absorbed /\ np[t] # "dead"
                            THEN confirmed
                            ELSE confirmed \ {t}
          /\ SpawnerDeathWake(np)
    /\ fired' = [fired EXCEPT ![t] = [s \in Sources |-> FALSE]]
    /\ UNCHANGED <<gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock>>
    /\ UNCHANGED <<hold, spc, cstep, licensed, latch, slatch>>

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
    /\ UNCHANGED bvars

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
    /\ UNCHANGED bvars

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
    /\ UNCHANGED bvars

(* The `stop` verb: set the per-Proc stop flag (once per episode). On a held *)
(* target it CONVERTS the hold: the stop is delivered, then the hold is      *)
(* cleared (ConvertFinish) -- two stores in one g_proc_table_lock section    *)
(* that the park's wait_lock observe can fall between.                       *)
(* BUGGY_CONVERT_CLEARS_FIRST stores them in the other order.               *)
RequestStop ==
    /\ dbg_live
    /\ attached
    /\ Quiet
    /\ ~sflag
    /\ ~resume_req
    /\ ~release_req
    /\ IF hold = "none"
         THEN /\ sflag' = TRUE
              /\ UNCHANGED <<hold, cstep, swake>>
         ELSE IF BUGGY_CONVERT_CLEARS_FIRST
                THEN /\ hold' = "none"
                     /\ cstep' = "set_stop"
                     /\ SpawnerWakeOnClear
                     /\ UNCHANGED sflag
                ELSE /\ sflag' = TRUE
                     /\ cstep' = "clear_hold"
                     /\ UNCHANGED <<hold, swake>>
    /\ UNCHANGED <<pc, gflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired,
                   spc, licensed, latch, slatch>>

(* The conversion's second store, still inside the stop's lock section.    *)
ConvertFinish ==
    /\ cstep # "idle"
    /\ IF cstep = "clear_hold"
         THEN /\ hold' = "none"
              /\ SpawnerWakeOnClear
              /\ UNCHANGED sflag
         ELSE /\ sflag' = TRUE
              /\ UNCHANGED <<hold, swake>>
    /\ cstep' = "idle"
    /\ UNCHANGED <<pc, gflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired,
                   spc, licensed, latch, slatch>>

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
    /\ licensed
    /\ Quiet
    /\ (BUGGY_FAULT_STOP_UNGATED \/ attached)
    /\ ~sflag
    /\ ~resume_req
    /\ (BUGGY_FAULT_STOP_UNGATED \/ ~release_req)
    /\ sflag' = TRUE
    /\ UNCHANGED <<pc, gflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired>>
    /\ UNCHANGED bvars

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
         THEN pc[t] \in {"stopped", "bstopped", "obs_run", "obs_stop"}
         ELSE Parked(t)
    /\ confirmed' = confirmed \cup {t}
    /\ UNCHANGED <<pc, gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, fired>>
    /\ UNCHANGED bvars

(* The `start` verb: resume a completed stop -- clear sflag, arm the start    *)
(* wake source.                                                             *)
StartResume ==
    /\ dbg_live
    /\ attached
    /\ Quiet
    /\ sflag
    /\ confirmed = Threads
    /\ sflag' = FALSE
    /\ resume_req' = TRUE
    /\ licensed' = TRUE
    /\ UNCHANGED <<pc, gflag, attached, dbg_live, detach_req,
                   release_req, exitkill, wlock, confirmed, fired>>
    /\ UNCHANGED <<hold, spc, swake, cstep, latch, slatch>>

(* The `start` verb on a held target no stop has converted: RELEASE the hold *)
(* (cleared before the resume's wake, so the woken park observes it gone).  *)
StartRelease ==
    /\ dbg_live
    /\ attached
    /\ Quiet
    /\ hold # "none"
    /\ ~sflag
    /\ ~release_req
    /\ hold' = "none"
    /\ resume_req' = TRUE
    /\ licensed' = TRUE
    /\ SpawnerWakeOnClear
    /\ UNCHANGED <<pc, gflag, sflag, attached, dbg_live, detach_req,
                   release_req, exitkill, wlock, confirmed, fired, spc, cstep, latch, slatch>>

(* An explicit `detach` request.                                            *)
DetachReq ==
    /\ attached
    /\ dbg_live
    /\ ~detach_req
    /\ detach_req' = TRUE
    /\ UNCHANGED <<pc, gflag, sflag, attached, dbg_live,
                   resume_req, release_req, exitkill, wlock, confirmed, fired>>
    /\ UNCHANGED bvars

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
    /\ UNCHANGED bvars

(* The debugger process dies (crash / kill). Its handle table closes at exit  *)
(* (#68/#926), which releases the slot below.                                *)
DbgDie ==
    /\ dbg_live
    /\ dbg_live' = FALSE
    /\ UNCHANGED <<pc, gflag, sflag, attached, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired>>
    /\ UNCHANGED bvars

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
(*                                                                              *)
(* THE BIRTH HOLD: an explicit detach also RELEASES a hold (clear, then the     *)
(* resume's wake); the implicit release -- the fd closing without detach --     *)
(* KEEPS it, because the hold is the spawner's, not the slot's. Either way a    *)
(* child whose hold is gone after the release is free to run (licensed).        *)
ReleaseSlot ==
    /\ attached
    /\ Quiet
    /\ (detach_req \/ ~dbg_live)
    /\ attached' = FALSE
    /\ confirmed' = {}
    /\ IF BUGGY_STRAND_ON_CLOSE
         THEN UNCHANGED <<sflag, release_req, gflag, hold, swake, licensed>>
         ELSE IF (~BUGGY_EXITKILL_IGNORED /\ exitkill /\ ~dbg_live /\ ~detach_req)
                THEN /\ sflag' = FALSE
                     /\ gflag' = TRUE
                     /\ UNCHANGED <<release_req, hold, swake, licensed>>
                ELSE /\ sflag' = FALSE
                     /\ release_req' = TRUE
                     /\ hold' = IF detach_req THEN "none" ELSE hold
                     /\ licensed' = (licensed \/ hold' = "none")
                     /\ IF hold' # hold THEN SpawnerWakeOnClear ELSE UNCHANGED swake
                     /\ UNCHANGED gflag
    /\ UNCHANGED <<pc, dbg_live, detach_req, resume_req, exitkill, wlock, fired,
                   spc, cstep, latch, slatch>>

(***************************************************************************)
(* ============================= DEATH PATH ============================== *)
(***************************************************************************)

(* A group termination publishes gflag once (kill / SYS_EXIT_GROUP / an LS-5  *)
(* terminate-interrupt).                                                    *)
SetGflag ==
    /\ Quiet
    /\ ~gflag
    /\ gflag' = TRUE
    /\ UNCHANGED <<pc, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired>>
    /\ UNCHANGED bvars

(***************************************************************************)
(* ============================ WAKE DELIVERY =========================== *)
(***************************************************************************)

(* Deliver a wake to a parked Thread from an active source, under t's         *)
(* wait_lock. CORRECT: a single-wake latch (~WokenOf) -- only the first       *)
(* source wakes; a second is a no-op. BUGGY_DOUBLE_WAKE drops the latch, so a *)
(* start racing a release both deliver -> two wakes to one park.             *)
(* A wake of a birth-parked Thread whose hold still stands is dropped: the   *)
(* park re-checks the hold and re-parks at once, the same state (the impl's *)
(* implicit-release wake does exactly this).                                *)
(* A latched interrupt's wake is not dropped there, and it reaches the tail  *)
(* park too: the waker wakes whatever rendez a Thread sleeps on. Either park *)
(* absorbs it (ResumeThread, BirthResume).                                   *)
WakeFrom(t, s) ==
    /\ Parked(t)
    /\ (pc[t] = "bstopped" => (s \in {"death", "intr"} \/ hold = "none"))
    /\ ~wlock[t]
    /\ Active(s)
    /\ ~fired[t][s]
    /\ IF BUGGY_DOUBLE_WAKE THEN TRUE ELSE ~WokenOf(t)
    /\ fired' = [fired EXCEPT ![t][s] = TRUE]
    /\ UNCHANGED <<pc, gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed>>
    /\ UNCHANGED bvars

(***************************************************************************)
(* ============================= BIRTH PATH ============================== *)
(***************************************************************************)

(* exec_setup completes: the head Thread reaches its birth tail.            *)
BirthArrive(t) ==
    /\ pc[t] = "unborn"
    /\ pc' = [pc EXCEPT ![t] = "btail"]
    /\ UNCHANGED <<gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired>>
    /\ UNCHANGED bvars

(* el0_birth_park's arrival, under g_proc_table_lock: "unborn" -> "parked",  *)
(* waking the spawner (a no-op when the hold was already released or        *)
(* converted). The tail's die-check before it is folded into the loop's.    *)
BirthMark(t) ==
    /\ pc[t] = "btail"
    /\ Quiet
    /\ IF hold = "unborn"
         THEN /\ hold' = "parked"
              /\ SpawnerWake
         ELSE UNCHANGED <<hold, swake>>
    /\ pc' = [pc EXCEPT ![t] = "bloop"]
    /\ UNCHANGED <<gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired,
                   spc, cstep, licensed, latch, slatch>>

(* The park loop's top: death wins, on every pass.                           *)
BirthLoop(t) ==
    /\ pc[t] = "bloop"
    /\ LET np == IF gflag THEN [pc EXCEPT ![t] = "dead"]
                           ELSE [pc EXCEPT ![t] = "bacq"]
       IN /\ pc' = np
          /\ SpawnerDeathWake(np)
    /\ UNCHANGED <<gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired,
                   hold, spc, cstep, licensed, latch, slatch>>

BirthAcquire(t) ==
    /\ pc[t] = "bacq"
    /\ ~wlock[t]
    /\ wlock' = [wlock EXCEPT ![t] = TRUE]
    /\ pc' = [pc EXCEPT ![t] = "breg"]
    /\ UNCHANGED <<gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, confirmed, fired>>
    /\ UNCHANGED bvars

(* Register-then-observe under wait_lock, as the tail's handshake does, but   *)
(* with the birth condition: proceed only with the hold AND the stop clear,   *)
(* and then only if death has not been published since the loop's check (a  *)
(* release that follows a terminate publishes it to whoever reads the        *)
(* release -- one step here, like the hold-then-stop read).                  *)
(* The latch changes nothing here but in a pre-5g exit (LatchExit).         *)
BirthRegisterObserve(t) ==
    /\ pc[t] = "breg"
    /\ wlock[t]
    /\ wlock' = [wlock EXCEPT ![t] = FALSE]
    /\ LET np == IF ~BirthWakeCond
                    THEN IF latch /\ ~gflag
                           THEN LatchExit(t, BUGGY_BIRTH_LATCH_ERETS, "bstopped")
                           ELSE [pc EXCEPT ![t] = "bstopped"]
                    ELSE IF gflag /\ ~BUGGY_NO_DEATH_RECHECK
                           THEN [pc EXCEPT ![t] = "dead"]
                           ELSE [pc EXCEPT ![t] = "el0"]
       IN /\ pc' = np
          /\ SpawnerDeathWake(np)
    /\ UNCHANGED <<gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, confirmed, fired>>
    /\ UNCHANGED <<hold, spc, cstep, licensed, latch, slatch>>

(* A woken birth park goes back to its loop top, never to the ordinary tail: *)
(* the thread has no instruction to return to yet. The latch's wake alone,   *)
(* with the park's condition false and no death published, is absorbed, as   *)
(* at the tail.                                                              *)
BirthResume(t) ==
    /\ pc[t] = "bstopped"
    /\ WokenOf(t)
    /\ LET absorbed == IntrOnly(t) /\ ~BirthWakeCond /\ ~gflag
           np == IF absorbed
                   THEN LatchExit(t, BUGGY_BIRTH_LATCH_ERETS, "bstopped")
                   ELSE [pc EXCEPT ![t] = "bloop"]
       IN /\ pc' = np
          /\ confirmed' = IF absorbed /\ np[t] # "dead"
                            THEN confirmed
                            ELSE confirmed \ {t}
          /\ SpawnerDeathWake(np)
    /\ fired' = [fired EXCEPT ![t] = [s \in Sources |-> FALSE]]
    /\ UNCHANGED <<gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock>>
    /\ UNCHANGED <<hold, spc, cstep, licensed, latch, slatch>>

(* An interrupt-terminate is posted to the target and latched (LS-5c: armed *)
(* on the note's commit when nothing in the target catches it; the ghost is *)
(* that case). Its wake reaches a parked Thread (WakeFrom "intr"). It runs  *)
(* under the note queue's lock, not g_proc_table_lock, so it can fall       *)
(* inside a conversion. Discretionary: nothing forces one to arrive.        *)
PostInterrupt ==
    /\ ~latch
    /\ \E t \in Threads : pc[t] # "dead"
    /\ latch' = TRUE
    /\ UNCHANGED <<pc, gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired>>
    /\ UNCHANGED <<hold, spc, swake, cstep, licensed, slatch>>

(***************************************************************************)
(* =========================== THE SPAWNER ============================== *)
(***************************************************************************)

(* The birth wait's scan, atomic with its registration (both under           *)
(* g_proc_table_lock): return if the child is released, else sleep on        *)
(* child_waiters.                                                            *)
SpawnerScan ==
    /\ spc = "scan"
    /\ Quiet
    /\ spc' = IF Released THEN "returned" ELSE "asleep"
    /\ swake' = FALSE
    /\ UNCHANGED <<pc, gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired,
                   hold, cstep, licensed, latch, slatch>>

SpawnerWakeUp ==
    /\ spc = "asleep"
    /\ swake
    /\ spc' = "scan"
    /\ swake' = FALSE
    /\ UNCHANGED <<pc, gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired,
                   hold, cstep, licensed, latch, slatch>>

(* The spawner exits -- after its spawn returned, or killed inside it. At its *)
(* ZOMBIE transition the orphan rule terminates a child whose hold is still  *)
(* set ("launcher exited"). BUGGY_ORPHAN_HOLD_STRANDS has no rule.           *)
SpawnerDie ==
    /\ HELD
    /\ spc \in {"scan", "asleep", "returned"}
    /\ Quiet
    /\ spc' = "dead"
    /\ swake' = FALSE
    /\ IF ~BUGGY_ORPHAN_HOLD_STRANDS /\ hold # "none" /\ ChildAlive
         THEN gflag' = TRUE
         ELSE UNCHANGED gflag
    /\ UNCHANGED <<pc, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired,
                   hold, cstep, licensed, latch, slatch>>

(* An interrupt-terminate latched on the SPAWNER inside its birth wait (the   *)
(* ghost `slatch`). The wait sleeps death-only (5g), so the latch's wake is   *)
(* absorbed -- a re-scan with nothing changed, so it is not modelled -- and   *)
(* the wait ends only on a release of the child, or in the spawner's death    *)
(* (SpawnerDie). Discretionary.                                               *)
PostSpawnerInterrupt ==
    /\ HELD
    /\ ~slatch
    /\ spc \in {"scan", "asleep"}
    /\ slatch' = TRUE
    /\ UNCHANGED <<pc, gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired>>
    /\ UNCHANGED <<hold, spc, swake, cstep, latch, licensed>>

(* BUGGY_SPAWNER_LATCH_RETURNS: the latch's wake ends the wait, and the       *)
(* spawn returns with its child unreleased -- await_child_release's break     *)
(* on any SLEEP_INTR before 5g, which a plain sleep returned on the latch.    *)
SpawnerLatchReturn ==
    /\ BUGGY_SPAWNER_LATCH_RETURNS
    /\ spc = "asleep"
    /\ slatch
    /\ spc' = "returned"
    /\ swake' = FALSE
    /\ UNCHANGED <<pc, gflag, sflag, attached, dbg_live, detach_req,
                   resume_req, release_req, exitkill, wlock, confirmed, fired,
                   hold, cstep, licensed, latch, slatch>>

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
    \/ StartRelease
    \/ ConvertFinish
    \/ \E t \in Threads : BirthArrive(t)
    \/ \E t \in Threads : BirthMark(t)
    \/ \E t \in Threads : BirthLoop(t)
    \/ \E t \in Threads : BirthAcquire(t)
    \/ \E t \in Threads : BirthRegisterObserve(t)
    \/ \E t \in Threads : BirthResume(t)
    \/ PostInterrupt
    \/ SpawnerScan
    \/ SpawnerWakeUp
    \/ SpawnerDie
    \/ PostSpawnerInterrupt
    \/ SpawnerLatchReturn

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
    \* The birth path and the spawner's wait are mechanical too; the spawner's
    \* exit (SpawnerDie) and the debugger's verbs stay discretionary.
    /\ WF_vars(ConvertFinish)
    /\ \A t \in Threads : WF_vars(BirthArrive(t))
    /\ \A t \in Threads : WF_vars(BirthMark(t))
    /\ \A t \in Threads : WF_vars(BirthLoop(t))
    /\ \A t \in Threads : WF_vars(BirthAcquire(t))
    /\ \A t \in Threads : WF_vars(BirthRegisterObserve(t))
    /\ \A t \in Threads : WF_vars(BirthResume(t))
    /\ WF_vars(SpawnerScan)
    /\ WF_vars(SpawnerWakeUp)
    /\ WF_vars(SpawnerLatchReturn)

Spec == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************)
(* ============================== INVARIANTS ============================= *)
(***************************************************************************)

(* NoLostStop (I-9 register-then-observe soundness): every Thread the         *)
(* debugger has CONFIRMED is genuinely parked. The correct handshake makes    *)
(* confirm sound (a confirmed Thread observed sflag under the lock and        *)
(* parked); the observe-before-register bug lets the debugger confirm a       *)
(* Thread that then runs at EL0 -- so it inspects a running target.           *)
(* A latched interrupt changes nothing: a park absorbs its wake (5g), and     *)
(* BUGGY_TAIL_LATCH_ERETS, which leaves on it, is caught here.                *)
NoLostStop ==
    \A t \in Threads : (t \in confirmed) => Parked(t)

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

(* NoEL0WhileHeld (the birth hold): a held child executes no EL0 instruction  *)
(* until its hold is released (start / explicit detach) or its converted     *)
(* stop is resumed. `licensed` records exactly those events; the park that   *)
(* ignores the hold, and the conversion that clears before it stops, each    *)
(* put the Thread at EL0 without one. Vacuous when ~HELD (licensed from the  *)
(* start).                                                                   *)
NoEL0WhileHeld ==
    (\E t \in Threads : pc[t] = "el0") => licensed

(* NoEretIntoDeath (both parks' proceed step): a Thread leaves a park for EL0  *)
(* only while no group termination is published. The loop's death check does *)
(* not give it alone: a terminate between that check and the wake condition's *)
(* read -- the EXITKILL release terminates, THEN clears the stop -- would let *)
(* the Thread eret; the re-check after the wake condition closes it. A kill   *)
(* after the proceed is ordinary death at the next checkpoint (ReEnterTail).  *)
NoEretIntoDeath ==
    [][\A t \in Threads : (pc[t] \in {"reg", "breg"} /\ pc'[t] = "el0") => ~gflag]_vars

(* ParkEndsOnlyInDeath (5g): a Thread leaves a park for "dead" only once a     *)
(* group termination is published -- a latched interrupt ends no parked        *)
(* Thread. The model has no note delivery, so a death without gflag can only   *)
(* be a park's latch exit, and the property is stated for every step.          *)
(* BUGGY_LATCH_ENDS_STOP breaks it.                                            *)
ParkEndsOnlyInDeath ==
    [][\A t \in Threads : (pc[t] # "dead" /\ pc'[t] = "dead") => gflag]_vars

(* SpawnReturnsAfterBirth (5g): a held spawn returns only once its child is    *)
(* released -- parked, or its hold released or converted -- or dead. A latch   *)
(* on the spawner does not end the wait, which sleeps death-only.              *)
(* BUGGY_SPAWNER_LATCH_RETURNS returns on the latch's wake. Vacuous when       *)
(* ~HELD: there the spawn has returned and there is no hold.                   *)
SpawnReturnsAfterBirth == spc = "returned" => Released

Safety ==
    /\ TypeOk
    /\ NoLostStop
    /\ NoEL0AfterStopped
    /\ ExactlyOnceResume
    /\ StopImpliesOwned
    /\ NoEL0WhileHeld
    /\ SpawnReturnsAfterBirth

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
(* A birth-parked Thread counts as stopped only once its hold is gone: a     *)
(* still-held one is parked by its spawner's hold, which the implicit release *)
(* deliberately keeps (EventuallyHoldResolved covers its end).                *)
StoppedByDebug(t) == pc[t] = "stopped" \/ (pc[t] = "bstopped" /\ hold = "none")

EventuallyResumed ==
    (attached /\ ~dbg_live) ~> (\A t \in Threads : ~StoppedByDebug(t))

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
    (sflag /\ attached) ~> ( \/ (\A t \in Threads : pc[t] \in {"stopped", "bstopped", "dead"})
                             \/ ~sflag )

(* EventuallyHoldResolved (liveness -- the NoStrand analog for the hold): a   *)
(* child whose spawner died with the hold still set eventually dies. A live   *)
(* spawner may keep its child held as long as it likes (that is what it      *)
(* asked for); only the orphaned hold must end. BUGGY_ORPHAN_HOLD_STRANDS    *)
(* leaves it parked forever.                                                 *)
EventuallyHoldResolved ==
    (spc = "dead" /\ hold # "none" /\ ChildAlive) ~> ~ChildAlive

(* BirthWaitReleases (liveness): a held spawn eventually returns (or its      *)
(* spawner dies inside it) -- the child parks, dies, or is released, and      *)
(* each of those wakes the wait. BUGGY_BIRTH_WAIT_UNWOKEN strands it behind  *)
(* a release that landed before the park.                                   *)
BirthWaitReleases ==
    (spc \in {"scan", "asleep"}) ~> (spc \in {"returned", "dead"})

(* A released hold is never set again.                                       *)
HoldMonotone == [][hold = "none" => hold' = "none"]_hold

====
