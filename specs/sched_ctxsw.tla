---- MODULE sched_ctxsw ----
(***************************************************************************)
(* Kernel execution-mode consistency across a context switch.              *)
(*                                                                         *)
(* Gating spec for invariant I-21 (ARCHITECTURE.md §12.1, §28): the kernel *)
(* executes uniformly at EL1h. Models the relationship between a CPU's     *)
(* live SPSel and the execution mode the running thread requires, and      *)
(* shows that the uniform-EL1h model (P5-el1h-kernel) keeps them           *)
(* consistent by construction, while the pre-P5 EL1t/EL1h dual-mode model  *)
(* did not.                                                                *)
(*                                                                         *)
(* THE BUG THIS PINS (root-caused during P5-corvus-bringup-c):             *)
(*                                                                         *)
(*   The pre-P5 kernel ran normal kernel code at EL1t (SPSel=0, sp =       *)
(*   SP_EL0) and entered EL1h (SPSel=1, sp = SP_EL1) only transiently      *)
(*   inside exception handlers. cpu_switch_context saves/restores SP but   *)
(*   not SPSel, so a thread resumed in whatever mode the OUTGOING thread   *)
(*   left the CPU in. Under SMP work-stealing a thread could resume the    *)
(*   exception-exit path (vectors.S KERNEL_EXIT) in the wrong mode, where  *)
(*   `msr SP_EL0` against the currently-selected stack pointer is          *)
(*   CONSTRAINED UNPREDICTABLE — an EC=0 Undefined trap on the QEMU        *)
(*   target, silently killing a secondary CPU on every boot. A per-CPU     *)
(*   exception stack was also incompatible with migrating a mid-exception  *)
(*   thread.                                                               *)
(*                                                                         *)
(* THE FIX (Model 1 / P5-el1h-kernel): run the kernel UNIFORMLY at EL1h.   *)
(*   One stack bank (SP_EL1 = the per-thread kernel stack); SP_EL0 is       *)
(*   exclusively the userspace stack. There is no second mode for a        *)
(*   context switch to mis-restore — execution-mode consistency holds by   *)
(*   construction. Exception frames live on the per-thread kernel stack,   *)
(*   so they travel with the thread across a switch or a cross-CPU         *)
(*   migration.                                                            *)
(*                                                                         *)
(* MODEL: a single CPU; each thread carries an execution mode; a context   *)
(* switch retargets the CPU. The constant DUAL_MODE selects the kernel     *)
(* model:                                                                  *)
(*                                                                         *)
(*   DUAL_MODE = FALSE  (Model 1 — the fix, sched_ctxsw.cfg):              *)
(*     The kernel's normal mode is el1h; every thread runs el1h; an IRQ    *)
(*     preempt is el1h->el1h; the context switch cannot mismatch.          *)
(*     CtxSwitchModeConsistent holds.                                      *)
(*                                                                         *)
(*   DUAL_MODE = TRUE   (Model 2 — the pre-P5 bug, sched_ctxsw_buggy.cfg): *)
(*     The kernel's normal mode is el1t; an IRQ preempt diverges a thread  *)
(*     to el1h; BuggyModeSwitch (cpu_switch_context not carrying SPSel)    *)
(*     resumes a thread in the outgoing thread's mode. When the modes      *)
(*     differ, CtxSwitchModeConsistent is violated — the executable        *)
(*     counterexample for the secondary-CPU crash.                         *)
(*                                                                         *)
(* Kept as a focused sibling module of specs/scheduler.tla (the pipe.tla / *)
(* corvus.tla precedent): the property is a self-contained correctness     *)
(* statement about cpu_switch_context + the exception model, and folding   *)
(* it into the 929-line scheduler model would entangle its liveness        *)
(* fairness with the mode variables.                                       *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Threads,        \* set of thread identifiers (>= 2 to exercise a switch)
    DUAL_MODE       \* BOOLEAN — FALSE = Model 1 (uniform EL1h, the fix);
                    \*           TRUE  = Model 2 (EL1t/EL1h dual mode, the bug)

ASSUME Cardinality(Threads) >= 2
ASSUME DUAL_MODE \in BOOLEAN

VARIABLES
    thread_mode,    \* [Threads -> Modes] — each thread's execution mode. For
                    \*   the running thread this is its live mode; for a
                    \*   suspended thread it is the mode it must resume in.
    cpu_mode,       \* Modes — the CPU's live SPSel state.
    mode_running    \* Threads — the thread currently on the CPU.

vars == <<thread_mode, cpu_mode, mode_running>>

\* el1t = SPSel 0 — normal kernel mode under the pre-P5 dual-mode model.
\* el1h = SPSel 1 — exception-handler mode; the ONLY kernel mode under the
\*        uniform-EL1h model (P5-el1h-kernel).
Modes == {"el1t", "el1h"}

\* The mode the kernel runs normal (non-exception) code in. Under Model 1
\* this is el1h (there is no el1t kernel mode); under Model 2 it is el1t.
NormalMode == IF DUAL_MODE THEN "el1t" ELSE "el1h"

TypeOk ==
    /\ thread_mode  \in [Threads -> Modes]
    /\ cpu_mode     \in Modes
    /\ mode_running \in Threads

(***************************************************************************)
(* Initially every thread runs the kernel's normal mode, consistently      *)
(* (cpu_mode = thread_mode[mode_running]).                                 *)
(***************************************************************************)
Init ==
    /\ thread_mode  = [t \in Threads |-> NormalMode]
    /\ cpu_mode     = NormalMode
    /\ mode_running = CHOOSE t \in Threads : TRUE

(***************************************************************************)
(* ModePreempt — the running thread is IRQ-preempted. Hardware exception   *)
(* entry runs at EL1h; the thread's resume point is now inside KERNEL_EXIT *)
(* so its saved mode becomes el1h. Under Model 1 the thread was already    *)
(* el1h (NormalMode), so this is a no-op transition; under Model 2 it      *)
(* diverges the thread from el1t to el1h.                                  *)
(***************************************************************************)
ModePreempt ==
    /\ thread_mode[mode_running] = NormalMode
    /\ thread_mode' = [thread_mode EXCEPT ![mode_running] = "el1h"]
    /\ cpu_mode'    = "el1h"
    /\ UNCHANGED mode_running

(***************************************************************************)
(* ModeReturn — the running thread completes KERNEL_EXIT's `eret`, which   *)
(* restores SPSel from the saved SPSR: the thread is back in NormalMode.   *)
(***************************************************************************)
ModeReturn ==
    /\ thread_mode[mode_running] = "el1h"
    /\ cpu_mode = "el1h"
    /\ thread_mode' = [thread_mode EXCEPT ![mode_running] = NormalMode]
    /\ cpu_mode'    = NormalMode
    /\ UNCHANGED mode_running

(***************************************************************************)
(* ModeSwitch(next) — cpu_switch_context. Retargets the CPU to `next`. The *)
(* CPU's mode ends up equal to `next`'s mode: under Model 1 trivially (all *)
(* modes are el1h); under a hypothetical mode-carrying switch by explicit  *)
(* restore. Either way the post-state is consistent.                       *)
(***************************************************************************)
ModeSwitch(next) ==
    /\ next # mode_running
    /\ mode_running' = next
    /\ cpu_mode'     = thread_mode[next]
    /\ UNCHANGED thread_mode

(***************************************************************************)
(* BuggyModeSwitch(next) — cpu_switch_context under the pre-P5 dual-mode   *)
(* kernel: it carries SP but not SPSel, so the CPU keeps the OUTGOING      *)
(* thread's mode. Only reachable when DUAL_MODE — the uniform-EL1h model   *)
(* has no second mode for this to expose. When `next`'s mode differs from  *)
(* the stale cpu_mode, CtxSwitchModeConsistent catches it.                 *)
(***************************************************************************)
BuggyModeSwitch(next) ==
    /\ DUAL_MODE
    /\ next # mode_running
    /\ mode_running' = next
    /\ UNCHANGED <<thread_mode, cpu_mode>>

Next ==
    \/ ModePreempt
    \/ ModeReturn
    \/ \E next \in Threads : ModeSwitch(next)
    \/ \E next \in Threads : BuggyModeSwitch(next)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* CtxSwitchModeConsistent (invariant I-21) — the CPU's live SPSel always  *)
(* matches the running thread's mode. Under Model 1 (uniform EL1h) this    *)
(* holds by construction: every thread and the CPU are permanently el1h.   *)
(* Under Model 2 BuggyModeSwitch breaks it whenever it retargets the CPU   *)
(* across a thread-mode boundary — the resumed thread then runs the        *)
(* exception-exit path at the wrong EL and `msr SP_EL0` traps.             *)
(***************************************************************************)
CtxSwitchModeConsistent == cpu_mode = thread_mode[mode_running]

Invariants ==
    /\ TypeOk
    /\ CtxSwitchModeConsistent

====
