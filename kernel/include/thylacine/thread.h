// Thread descriptor — Plan 9 Thread, Thylacine adaptation.
//
// Per ARCHITECTURE.md §7.3. A Thread owns its register context, a kernel
// stack, run state, and Proc backref. P2-A's minimal struct carries
// everything cpu_switch_context needs (struct Context, kstack); per-band
// EEVDF data + scheduler links land at P2-B.
//
// "current thread" is parked in TPIDR_EL1 — ARMv8's per-CPU register
// reserved for OS use. This makes the accessor SMP-correct without a
// per-CPU array indirection: each CPU has its own TPIDR_EL1.

#ifndef THYLACINE_THREAD_H
#define THYLACINE_THREAD_H

#include <thylacine/context.h>
#include <thylacine/types.h>
#include <thylacine/spinlock.h>
#include <thylacine/rendez.h>   // 8a-1b-beta: embedded per-Thread debug_rendez

struct Proc;
struct Rendez;
struct exception_context;   // 8a-1c: /proc/<pid>/regs trapframe pointer (debug_trapframe)

// Thread states. P2-A primarily uses RUNNING and RUNNABLE (only those
// transition during context switches). SLEEPING and EXITING land at
// P2-B (wait/wake) and Phase 2 close (exit/reap) respectively.
//
// State values are non-zero so a zero-initialized Thread is detectably
// invalid — kmem_cache_alloc with KP_ZERO returns all-zero memory; the
// Thread is only usable after explicit initialization sets state.
enum thread_state {
    THREAD_STATE_INVALID  = 0,    // zero-initialized; not usable
    THREAD_RUNNING        = 1,
    THREAD_RUNNABLE       = 2,
    THREAD_SLEEPING       = 3,    // P2-B: wait/wake protocol
    THREAD_EXITING        = 4,    // P2 close: exit/reap path
};

_Static_assert(THREAD_STATE_INVALID == 0,
               "THREAD_STATE_INVALID == 0 is the invariant that makes "
               "zero-initialized Thread structs detectably invalid "
               "(P2-A audit R4 F46 close)");

// THREAD_MAGIC — sentinel set at thread_create / thread_init; checked at
// thread_free. At offset 0 so SLUB's freelist write naturally clobbers
// it on kmem_cache_free; subsequent thread_free reads the clobbered
// value and extincts (P2-A audit R4 F42 close).
#define THREAD_MAGIC 0x54485244C0DEFADEULL    // 'THRD' || 0xC0DE'FADE

struct Thread {
    u64                magic;     // THREAD_MAGIC
    int                tid;
    enum thread_state  state;
    struct Proc       *proc;

    // Saved register context. Valid only when this thread is NOT
    // currently running on any CPU; for the running thread the live
    // CPU registers are canonical and ctx is stale until the next
    // cpu_switch_context call saves into it.
    struct Context     ctx;

    // Kernel stack. 16 KiB usable (THREAD_KSTACK_SIZE) + 16 KiB guard,
    // allocated as one 32 KiB order-3 chunk via alloc_pages; the bottom
    // THREAD_KSTACK_GUARD_PAGES are mapped no-access. kstack_base is the
    // LOW address (guard base); ctx.sp is set to kstack_base +
    // THREAD_KSTACK_TOTAL_SIZE (the top) on thread_create and grows down
    // through the 16 KiB usable region, faulting into the guard on overflow.
    //
    // The per-thread guard page IS present (P2-Dc). Normal max kernel thread
    // depth is only ~6 KiB; a "kernel stack overflow" during rfork_stress was
    // NOT honest depth -- it was the #788 use-after-free (thread_free freeing
    // a SLEEPING-but-still-on_cpu thread whose sched() was mid-
    // cpu_switch_context on a host-stalled secondary -> buddy-LIFO recycle of
    // the slot+kstack -> the stalled CPU's late register-save corrupts the
    // recycled thread's ctx.sp -> wild SP faults in its own guard). Fixed by
    // the on_cpu gate in thread_free (DEBUGGING-PLAYBOOK 6.12). Separately,
    // a dedicated per-CPU exception/IRQ stack is still NOT implemented (a
    // kernel IRQ builds its frame on the interrupted thread's own kstack);
    // that is unrelated to #788 and remains a documented future hardening.
    void              *kstack_base;
    size_t             kstack_size;

    // Linked-list links into Proc->threads (doubly-linked).
    struct Thread     *next_in_proc;
    struct Thread     *prev_in_proc;

    // P2-Ba: EEVDF / run-tree fields. The scheduler's per-CPU run tree
    // (sched.c) keys on `vd_t`; band selects which of three priority
    // tiers (INTERACTIVE / NORMAL / IDLE — sched.h SCHED_BAND_*) the
    // thread belongs to; weight is the EEVDF weight (default 1; higher
    // → more CPU share — full EEVDF math at P2-Bc).
    //
    // runnable_{next,prev} link the thread into its band's sorted-by-
    // vd_t doubly-linked list. Both NULL when the thread is NOT in any
    // run tree (RUNNING / SLEEPING / freshly-created).
    s64                vd_t;
    u32                weight;
    u32                band;            // SCHED_BAND_* (sched.h)
    struct Thread     *runnable_next;
    struct Thread     *runnable_prev;

    // P2-Bb + #811 (ARCH §8.8.1): rendez backref. NULL when not sleeping on a
    // Rendez. ONLY the owning Thread mutates it -- SET at register (in sleep /
    // tsleep, under the owner's `wait_lock` + r->lock, with state =
    // THREAD_SLEEPING) and CLEARED on resume (under the owner's `wait_lock`).
    // `wakeup(r)` does NOT clear it (it walks via `r->waiter`, not this field).
    // The group-terminate / interrupt-terminate cascade only READS it, under
    // the peer's `wait_lock` -- the read-only waker->sleeper edge that keeps the
    // #811 lock graph acyclic and lets the cascade `wakeup()` a sleeper's
    // stack-allocated Rendez (Option-A pin) without it being popped. Diagnostic
    // + invariant aid: a SLEEPING thread with rendez_blocked_on != NULL is
    // sleeping on that specific Rendez; a debugger / extinction dump can name
    // the wait condition. (poll/futex still register a single owning Rendez per
    // sleep; the per-fd hooks are a separate poll_waiter list.)
    struct Rendez     *rendez_blocked_on;

    // P2-Bc: scheduler-tick preemption. `slice_remaining` is the
    // number of remaining timer ticks before this thread's slice
    // expires. Replenished to THREAD_DEFAULT_SLICE_TICKS on every
    // RUNNABLE → RUNNING transition (sched() pick-next path).
    // Decremented by sched_tick() (called from the timer IRQ
    // handler); when ≤ 0, sets g_need_resched so preempt_check_irq
    // triggers a context switch on IRQ-return.
    //
    // Read/written from IRQ context (sched_tick) AND from sched()
    // (replenish on pick + decrement-check from preempt_check_irq).
    // IRQ-mask discipline in sched() prevents the IRQ handler from
    // racing with the replenish.
    s64                slice_remaining;

    // P2-Cf: SMP wait/wake race close. True while this thread is
    // ACTIVELY RUNNING on some CPU (registers live; saved context in
    // ctx is stale). Set to true by sched() (or thread_switch) when
    // the thread is picked as `next`; cleared to false by the resume
    // path on the destination CPU AFTER cpu_switch_context completed
    // — meaning the thread is fully switched out, ctx is canonical,
    // and another CPU may safely pick this thread without racing
    // mid-save.
    //
    // wakeup() spins on this flag before transitioning a SLEEPING
    // waiter to RUNNABLE: if the waiter is mid-switch, ready() would
    // insert it into a runqueue while it's still being saved on
    // another CPU — peer pick + execution while ctx half-written.
    // Linux's `task->on_cpu`; same role.
    //
    // Accessed via __atomic_load_n / __atomic_store_n with acquire/
    // release ordering so cross-CPU readers see the consistent
    // monotonic transitions.
    volatile bool      on_cpu;

    // SMP redesign (deep-smp-review): CPU-pinned bootstrap thread. True for
    // every per-CPU idle thread (the boot CPU's bootcpu_idle + each
    // secondary's per_cpu_main idle) and for kthread -- all run on a static
    // boot/idle stack (kstack_base==NULL) that belongs to one specific CPU, so
    // try_steal MUST never migrate them (ARCH 8.4.2; modeled as IsPinned in
    // specs/sched_alpha.tla). This is the single clean unstealability predicate
    // that REPLACES the prior (kstack_base != NULL) gate + the g_bootcpu_idle
    // special case (the #860 root cause: g_bootcpu_idle owned a real kstack, so
    // the kstack_base gate did not exclude it). Set once at thread init; never
    // mutated. Occupies on_cpu's tail padding -- no struct-size change.
    bool               cpu_pinned;

    // HMP foundation (deep-smp-review #864, ARCH §8.4.4): per-task utilization
    // estimate, a PELT-style signal in [0, SCHED_CAPACITY_SCALE]. Accrued while
    // the thread RUNS (sched_tick) and decayed when it blocks (sched() on the
    // SLEEPING path); read by select_target_cpu to bias a heavy task toward a
    // high-capacity CPU on a DECLARED-heterogeneous topology. INERT on uniform
    // targets (QEMU virt / RPi): select_target_cpu short-circuits to the prev
    // CPU when !sched_topology_hetero(), so util is computed but never changes
    // placement. The v1.0 estimator is a simple EWMA; the empirical PELT decay
    // constants + energy model are the deferred EAS tuning (ARCH §8.4.4 "the
    // verification boundary"), landing on real heterogeneous hardware. Occupies
    // the 4 bytes of on_cpu/cpu_pinned tail padding before timerwait_next -- no
    // struct-size change (the _Static_assert below still pins 1136).
    u32                util;

    // P5-tsleep: deadline-bounded Rendez sleep. A thread inside a
    // deadlined tsleep() is linked onto the global timer-wait list
    // (kernel/sched.c) via timerwait_{next,prev}; sched_tick() scans
    // that list on every timer fire and wakes any thread whose
    // sleep_deadline has passed. sleep_deadline is an architectural-
    // counter value (timer_get_counter units, 0 = unset); sleep_timedout
    // is set by the timeout wake so tsleep's resume can return
    // TSLEEP_TIMEDOUT. All four fields are touched only under the
    // timer-wait lock and r->lock; both links NULL when the thread is
    // not in a deadlined tsleep. Modeled by specs/tsleep.tla.
    struct Thread     *timerwait_next;
    struct Thread     *timerwait_prev;
    u64                sleep_deadline;
    bool               sleep_timedout;

    // P6-pouch-threads (sub-chunk 9): clear-child-tid address — the
    // user-VA of a 4-byte word the kernel atomically zeroes + torpor-
    // wakes on Thread exit (the SYS_THREAD_EXIT path; the SYS_EXITS path
    // also runs it for the main thread). 0 means "not set" (the default
    // from KP_ZERO + the v1.0 SYS_SET_TID_ADDRESS semantics for any
    // Thread that has not registered one). musl's __pthread_setup wires
    // it through SYS_SET_TID_ADDRESS — at sub-chunk 9 the syscall stores
    // the address here so pthread_join's torpor-wait on the same word
    // observes the exit.
    //
    // Concurrency contract (F10 audit close): only the OWNING Thread
    // writes this field (via SYS_SET_TID_ADDRESS, which validates as
    // current_thread()->clear_child_tid = tidptr) and only the owning
    // Thread reads it (at exit time via thread_clear_child_tid_handoff,
    // which runs synchronously inside SYS_THREAD_EXIT / SYS_EXITS on
    // the owning Thread's CPU). Same-thread sequential consistency —
    // no memory-ordering hazard. A FUTURE cross-thread setter (e.g.
    // SYS_SETTID_ADDRESS_FOR_THREAD writing to a peer's tidptr) would
    // require atomic store + appropriate memory ordering AND would
    // need to compose with the alignment-validation gate in
    // thread_clear_child_tid_handoff (F7 audit close).
    //
    // The kernel's exit-time clear+wake is best-effort: if the page is
    // unmapped at the moment of exit (joiner has munmap'd the worker
    // stack), uaccess_store_u32 returns -1 and torpor_wake is skipped.
    // Userspace bug, not ours to fix — but kernel never extincts on it.
    u64                clear_child_tid;

    // P6-pouch-signals-impl (sub-chunk 13a): note-delivery state per Thread.
    //
    // `note_mask`  — bit per NOTE_BIT_* (see <thylacine/notes.h>). Bit set
    //                means this Thread defers delivery of that note (queue
    //                entry is left in place; another Thread of the Proc, or
    //                a future unmask, picks it up). Set / cleared by
    //                SYS_NOTE_MASK.
    //
    // `in_handler` — true while an async note handler is RUNNING on this
    //                Thread (between the EL0-return-tail dispatch and the
    //                SYS_NOTED restore). The N-3 re-entrancy guard: the
    //                EL0-return-tail check skips delivery for any Thread
    //                with in_handler == true.
    //
    // `note_saved_*` — the user context snapshot captured at delivery,
    //                  restored at SYS_NOTED(NCONT). Inline rather than
    //                  heap-allocated so delivery is alloc-free (a kmalloc
    //                  failure mid-delivery would silently drop the
    //                  handler invocation, which is a worse failure mode
    //                  than a slightly larger Thread cache slot).
    //                  note_saved_regs[0..30] mirror exception_context's
    //                  x0..x30; sp_el0 / elr / spsr likewise. The same
    //                  layout is restored verbatim into the ctx of the
    //                  SYS_NOTED syscall's exception_context.
    u64                note_mask;
    bool               in_handler;
    u8                 _pad_in_handler[7];   // explicit 8-byte alignment
    u64                note_saved_regs[31];   // x0..x30
    u64                note_saved_sp_el0;
    u64                note_saved_elr;
    u64                note_saved_spsr;
    // SYS_NOTED(NDFLT) needs to know the note name to apply the default
    // action (`exits(name)` for the v1.0 supported set). Captured at
    // EL0-return-tail delivery; cleared when in_handler returns to false.
    // 16 bytes = NOTE_NAME_MAX. Avoids re-popping the queue at NOTED time
    // (which would race with another delivery / fd-read).
    char               note_handling_name[16];

    // P6 #811 (death-interruptible sleep, ARCH 8.8.1): per-Thread wait-lock,
    // the Plan 9 `p->rlock` analog. Protects this Thread's wait registration
    // (`rendez_blocked_on` + its THREAD_SLEEPING transition). Only the OWNING
    // Thread WRITES rendez_blocked_on (set at register, cleared on resume,
    // both under this lock); a group-terminate cascade (proc_group_terminate)
    // takes a peer's wait_lock to READ rendez_blocked_on and wake it, so the
    // cascade-vs-register read/write is serialized (acquire/release on
    // group_exit_msg alone loses the wakeup in the miss case). wait_lock is
    // the OUTERMOST wait-lock: order wait_lock -> g_timerwait.lock -> r->lock,
    // and it is NEVER held across sched() (a sleeper that held it would
    // deadlock the cascade). Zero (KP_ZERO from the SLUB cache) == unlocked,
    // so no explicit init is needed.
    spin_lock_t        wait_lock;

    // #360 preemption discipline: the number of plain spinlocks THIS thread
    // currently holds (see spinlock.h). Nonzero => preempt_check_irq will not
    // involuntarily deschedule the thread, and sched() asserts it is zero at
    // every voluntary entry (lock-across-sleep is forbidden). PER-THREAD, not
    // per-CPU, deliberately: the count travels with the thread across a
    // migration, so the mid-increment preempt+migrate window that corrupted a
    // per-CPU slot (the #360 first-cut bug: an IRQ reading the pre-increment
    // value passes the gate, the thread migrates, and the store lands in the
    // OLD CPU's slot -- poisoning it non-preemptible forever) is structurally
    // gone: the gate and the RMW target the same thread. Only this thread
    // (and its own nested IRQ handlers, which are inc/dec-balanced before
    // IRQ-return) mutates it. The ONE cross-thread lock handoff -- sched()'s
    // cs->lock pending-release, released by the NEXT thread -- bypasses the
    // count via spin_lock_raw/spin_unlock_raw (sound: sched runs fully
    // IRQ-masked, so that hold is non-preemptible by masking). KP_ZERO init.
    u32                preempt_count;

    // #68 F1: true while this (last-out) Thread runs the pre-ZOMBIE handle
    // close in thread_exit_self. thread_die_pending() returns false while
    // set, so the close's 9P sends / RPC waits / sleeps behave like a live
    // thread's -- group_exit_msg is set on EVERY SYS_EXIT_GROUP (a clean
    // exit_group(0) included), and treating the orderly final close as
    // "dying" short-circuited the dev9p write-behind close-flush (silent
    // data loss for a file left open at a multi-thread exit) and the
    // close-time Tclunk (a server-side fid leak per fd). Set/cleared ONLY
    // by the owning Thread around proc_close_handles_at_exit; read only
    // via thread_die_pending(self). Fits in the tail padding.
    bool               exit_close_active;

    // 8a-1b-beta (I-39; docs/DEBUG-FS-DESIGN.md section 4.2; specs/debug_stop.tla):
    // this Thread's OWN debugger park rendez. A thread observing a debugger stop
    // at its EL0-return tail (el0_return_stop_check) parks here via sleep(); the
    // resume cascade (proc_debug_resume) identifies a debug-parked peer by
    // `rendez_blocked_on == &debug_rendez` and wakes exactly it (never a syscall-
    // sleeper). PER-THREAD (single-waiter -- only this Thread ever sleeps here, so
    // a multi-thread target parks each thread on its own rendez, and the single-
    // waiter Rendez discipline is never violated by a shared park). Its lifetime
    // is the Thread's, so the proc_group_terminate death cascade -- which wakes
    // rendez_blocked_on (== &debug_rendez while parked) under wait_lock -- reaches
    // a stopped thread with no stack-rendez lifetime concern. KP_ZERO (SLUB) inits
    // it to { unlocked, no waiter } -- no explicit rendez_init needed.
    struct Rendez      debug_rendez;

    // 8a-1c (I-39; docs/DEBUG-FS-DESIGN.md section 4.5/5c.6): the EL0-entry
    // trapframe pointer for /proc/<pid>/regs. The trapframe is NOT at a fixed
    // kstack offset -- it lives at (SP_EL1 at the exception - EXCEPTION_CTX_SIZE),
    // which is only kstack_top-288 for a thread's very first entry; for a running
    // thread the outermost EL0 frame sits lower. Captured from the thread's vector
    // `ctx` at TWO points: el0_return_stop_check records it at the RETURN tail (a
    // TAIL-parked thread), and exception_sync_lower_el records it at the EL0-SYNC
    // ENTRY (the #88 fix -- a DETOUR-parked sleeper, proc_stop_sleeper_park,
    // never reaches the tail while stopped, so the entry frame is its ONLY source).
    // Read ONLY while the thread is fully-stopped (parked on debug_rendez, tail or
    // detour), so it is always fresh: the tail-park overwrites with the return ctx;
    // a detour-park is mid-syscall so the entry frame is the current outermost EL0
    // frame; the between-syscalls stale value is never read (a non-parked thread's
    // field is never read). Non-NULL during any syscall of any Proc (harmless -- a
    // non-debugged Proc is never fully-stopped, so it never reads its own).
    struct exception_context *debug_trapframe;

    // 8a-2b-2 (I-39; docs/DEBUG-FS-DESIGN.md section 5.5; specs/debug_step.tla):
    // the arm64 single-step machine, per-THREAD so it follows the thread across
    // an IRQ-preempt migration mid-step (the Linux per-task MDSCR.SS model -- SS
    // is per-PE, so a step must re-arm MDSCR.SS on the destination CPU's
    // ctx-switch-IN, or a migrated step runs free). `debug_ss_armed` = a step is
    // in flight: hwdebug_switch_in sets MDSCR.SS from it (SPSR.SS is armed in the
    // resume trapframe by el0_return_stop_check); the EC 0x32 handler clears it +
    // re-stops. `debug_stepover_va` (0 = none) is the breakpoint VA to SKIP while
    // stepping (the step-over-breakpoint dance: a thread stopped AT a bp would
    // re-trap on the resume instead of stepping, so switch_in loads that bp
    // disabled for this thread's step). Both set only while the target is
    // fully-stopped (the `step` verb, under g_proc_table_lock) + on the local
    // CPU under IRQ-mask; read (ACQUIRE) in the ctx-switch install. KP_ZERO inits
    // them (not stepping); NOT propagated by rfork.
    bool               debug_ss_armed;

    // 8c-3 (#89; docs/DEBUG-FS-DESIGN.md 5c.6): the elected 9P reader sets this
    // around its blocking recv so a debugger stop UNWINDS the recv (sleep/tsleep
    // return SLEEP_INTR/TSLEEP_INTR, reusing the death-interrupt propagation the
    // transport recv already tolerates) instead of parking IN PLACE (the 8c-2
    // detour default) -- a reader parked mid-recv holds reader_active and freezes
    // every survivor sharing the client. The caller (client_wait) classifies the
    // recv-return via the STABLE stop_unwound latch (below), NOT by re-reading
    // debug_stop_req (which races an async proc_debug_resume -- the F1 re-audit
    // fix); on a stop-unwind it releases the reader role, hands it to a survivor,
    // then parks role-free + re-elects on resume. Every
    // OTHER sleep (clear) parks in place (8c-2, preserves the syscall). Owner-only
    // access (only the reader thread reads/writes its own flag, in program order
    // -- no atomic needed); set/cleared within one reader_recv_frame, never
    // persists past it; KP_ZERO inits it false; NOT propagated by rfork. Fits the
    // debug_ss_armed padding -- no struct-size change.
    bool               stop_unwinds;

    // 8c-3 (#89; F1 frame-atomic fix): the elected 9P reader sets this for its
    // WHOLE recv tenure (reader_recv_frame entry..exit). A stop hitting the recv
    // MID-FRAME (bytes of the frame already consumed, stop_unwinds false) must
    // BLOCK THROUGH -- neither unwind (SLEEP_INTR would discard the consumed
    // partial bytes -> the survivor reads the frame TAIL as a header -> stream
    // desync = shared-session death / task-#50 corruption) nor park-in-place
    // (holds reader_active -> freezes survivors AND pins the partial frame). The
    // detour, when stop_no_park is set + stop_unwinds is false, FALLS THROUGH to
    // the normal register+sched so the reader finishes the frame (bounded by the
    // trusted server's delivery), then unwinds at the next frame boundary
    // (got==0, stop_unwinds true). DEATH still unwinds mid-frame (the die-check
    // below the detour is unchanged) -- the pre-existing death-mid-frame desync
    // is task #90 (needs a #811 narrowing; signoff). Same padding, no size change.
    bool               stop_no_park;

    // 8c-3 (#89; F1 re-audit fix): the STABLE "my recv was stop-unwound at a
    // boundary" latch. The sched detour's stop_unwinds branch returns SLEEP_INTR
    // -- byte-identical to a death-interrupt AND a transport error -> the client
    // classifier cannot tell them apart by return value. It USED to re-derive the
    // stop case by re-reading debug_stop_req (client_stop_pending), but that flag
    // is cleared ASYNCHRONOUSLY by proc_debug_resume (debugger detach/death via
    // devproc_debug_release_cb; NOT under c->lock), so a resume in the recv-return
    // -> classify window turned a benign stop-unwind into a spurious
    // client_mark_dead_locked of the SHARED session (whole-FS DoS). This latch is
    // SET by the detour and READ+cleared by the SAME reader thread at the
    // classifier, so a concurrent resume cannot flip it. Reset false at
    // reader_recv_frame entry; KP_ZERO inits it false; owner-only; not
    // rfork-propagated.
    bool               stop_unwound;

    u64                debug_stepover_va;
};

_Static_assert(sizeof(struct Thread) == 1184,
               "8c-3 appended stop_unwinds + stop_no_park + stop_unwound (bools) in "
               "the debug_ss_armed padding -- no size change (#89 reader-role-release "
               "+ the F1 frame-atomic block-through + the F1-re-audit stop-unwound "
               "classifier latch). "
               "8a-2b-2 appended debug_ss_armed (bool) + debug_stepover_va (u64) "
               "for the single-step machine: 1168 -> 1184. "
               "8a-1c appended debug_trapframe (8-byte struct exception_context* "
               "for /proc/<pid>/regs); the 16-aligned struct pads 1152 -> 1168. "
               "struct Thread size pinned at 1136 bytes (P6 #811 appended a "
               "tail spin_lock_t wait_lock -- death-interruptible sleep, "
               "ARCH 8.8.1; 1120 -> 1136 incl. tail padding). P6-pouch-signals-"
               "impl (sub-chunk 13a) appended note_mask + in_handler + the "
               "user-context save block (note_saved_regs[31] + sp_el0 + elr "
               "+ spsr) + note_handling_name[16] = 304 bytes after the 816-"
               "byte P6-pouch-threads baseline. SLUB cache slot grows "
               "accordingly. Adding ANY further field will grow the cache "
               "further; update this assert deliberately so the change is "
               "intentional.");
_Static_assert(__builtin_offsetof(struct Thread, magic) == 0,
               "magic must be at offset 0 (P2-A audit R4 F42)");

// Default kernel stack size per thread. 4 pages × 4 KiB = 16 KiB.
// Matches ARCHITECTURE.md §7.3 (THREAD_STACK_SIZE = 16 KiB).
//
// P2-Dc adds a 4-page (16 KiB) guard region BELOW the kstack. Total
// allocation per thread is 8 pages = 32 KiB at order=3 from buddy.
// The guard pages are marked no-access in TTBR0 — a stack-overflow
// access faults via the kernel's identity mapping, the exception
// handler reports it, and extinction("kstack overflow") fires.
#define THREAD_KSTACK_SIZE         (16 * 1024)
#define THREAD_KSTACK_GUARD_SIZE   (16 * 1024)
#define THREAD_KSTACK_TOTAL_SIZE   (THREAD_KSTACK_SIZE + THREAD_KSTACK_GUARD_SIZE)
#define THREAD_KSTACK_TOTAL_ORDER  3                  // 8 pages = 32 KiB
#define THREAD_KSTACK_GUARD_PAGES  4                  // bottom 4 pages

// "current thread" is held in TPIDR_EL1, the per-CPU OS-use register.
// Accessed by inline mrs / msr — no function call overhead in the
// hot path. Returns NULL before thread_init runs (TPIDR_EL1 is zero
// at boot from BSS-default).
static inline struct Thread *current_thread(void) {
    u64 v;
    __asm__ __volatile__("mrs %0, tpidr_el1" : "=r"(v));
    return (struct Thread *)(uintptr_t)v;
}

static inline void set_current_thread(struct Thread *t) {
    __asm__ __volatile__("msr tpidr_el1, %0" :: "r"((u64)(uintptr_t)t) : "memory");
}

// #90 (ARCH 8.8.1.1): the elected 9P reader recv (reader_recv_frame,
// kernel/9p_client.c) is frame-atomic w.r.t. an async unwind. A dying reader
// observed MID-FRAME at a sleep-site die-check -- in a frame-atomic recv
// (stop_no_park set) with bytes of the current frame already consumed
// (stop_unwinds clear, i.e. got != 0) -- must NOT unwind: an immediate #811
// unwind would discard the consumed partial frame, and the survivor that takes
// over the reader role would read the frame TAIL as a header -> the shared byte
// stream desyncs (task-#50 corruption). It BLOCKS THROUGH instead, finishing
// the frame (bounded by the trusted server's whole-frame delivery, CF-3 B),
// then unwinds at the next boundary. Reuses the 8c-3 stop latches: stop_no_park
// = "in a frame-atomic recv", stop_unwinds = "at a boundary (got==0)" -- both
// already exactly the predicates death needs, so no new field. True iff the
// die-check must DEFER (block through) rather than unwind. Every non-reader
// sleeper has stop_no_park clear -> false -> the die-check fires immediately,
// exactly as before #90.
static inline bool thread_reader_blocks_death(const struct Thread *t) {
    return t->stop_no_park && !t->stop_unwinds;
}

// Bring up the thread subsystem. Allocates the kernel thread (TID 0),
// wires it to kproc (set up by proc_init), parks it in TPIDR_EL1 as
// the current thread. Must be called after proc_init (so kproc exists)
// and after slub_init (for the SLUB caches).
//
// The boot CPU was already running on the boot stack — kthread's ctx is
// initialized to zero; the FIRST cpu_switch_context call out of kthread
// will save the live registers into kthread.ctx. No "initial save" is
// needed.
void thread_init(void);

// Accessor for the kernel thread (TID 0). Returns NULL before thread_init.
struct Thread *kthread(void);

// P2-Cd: allocate the idle Thread for a secondary CPU.
//
// Same role kthread plays on the boot CPU: a Thread descriptor that
// sched_init records as "this CPU's idle." Doesn't own a kstack — the
// thread runs on the per-CPU boot stack already in use by per_cpu_main
// (the trampoline assigned that stack via SP_EL0 in start.S).
//
// State = THREAD_RUNNING (the caller is "running" as this thread by
// the time sched_init is called); ctx is zero-initialized and gets
// filled in by the first cpu_switch_context save (same pattern as
// kthread).
//
// Caller is expected to:
//   1. Call thread_init_per_cpu_idle(cpu_idx) on this CPU.
//   2. set_current_thread(returned).
//   3. sched_init(cpu_idx) — records the returned thread as this
//      CPU's idle.
//   4. Enter the per-CPU idle loop.
//
// Returns NULL on OOM (Thread alloc fail). cpu_idx >= 1 only — CPU 0's
// idle is the dedicated bootcpu_idle thread (thread_create_bootcpu_idle).
struct Thread *thread_init_per_cpu_idle(unsigned cpu_idx);

// SMP redesign (ARCH §8.4.2): allocate cpu0's idle Thread on a dedicated BSS
// stack with first-switch-in ctx (thread_trampoline → blr `entry`). CPU-pinned,
// band=IDLE, kstack_base==NULL. `stack_top` is the 16-aligned high edge of the
// dedicated BSS stack. Readied into cpu0's run_tree[IDLE] by
// sched_install_bootcpu_idle. Returns NULL on OOM. Retires the old off-tree
// real-kstack g_bootcpu_idle.
struct Thread *thread_create_bootcpu_idle(void (*entry)(void), void *stack_top);

// Create a kernel thread of `proc` running `entry`. SLUB-allocates the
// Thread descriptor + 16 KiB kernel stack via alloc_pages(order=2);
// initializes ctx so the first switch-into the new thread lands at the
// trampoline which then blr's `entry`. Adds to proc->threads list.
//
// Returns NULL on OOM (Thread alloc fail or kstack alloc fail; cleanup
// is internal).
//
// `entry` takes no argument. For arg-passing (P2-D rfork), use
// thread_create_with_arg below.
//
// At P2-A, entry must not return — the trampoline halts on WFE if it
// does. P2-D adds exits() as the structured termination path.
struct Thread *thread_create(struct Proc *proc, void (*entry)(void));

// P2-D: same as thread_create but passes `arg` in x0 to `entry`.
// Internally sets ctx.x20 = arg; thread_trampoline does `mov x0, x20`
// before `blr x21` (entry pointer). The trampoline behavior is identical
// for both forms — thread_create is exactly thread_create_with_arg with
// arg=0; the call sites differ only in the entry signature they
// document.
struct Thread *thread_create_with_arg(struct Proc *proc,
                                      void (*entry)(void *),
                                      void *arg);

// P6-pouch-threads (sub-chunk 9): create a USER-mode Thread in `proc`.
// Sibling of thread_create_with_arg, but instead of running `entry` in
// EL1 the new thread will eret to EL0 at user_entry_va running on
// user_sp_va, with x0 = user_arg and TPIDR_EL0 = user_tls_va.
//
// SLUB-allocates the Thread descriptor + 16 KiB kstack (same kstack
// layout as the EL1-side helpers); ctx is laid out so the first
// cpu_switch_context into the new thread lands at thread_user_trampoline,
// which release-pairs sched_finish_task_switch + unmasks IRQs + erets
// to EL0 at user_entry_va.
//
// Caller-provided values are NOT validated here — the syscall handler
// is responsible for the user-VA bound + alignment checks. The kernel
// trampoline programs them verbatim into ELR_EL1 / SP_EL0 / TPIDR_EL0;
// a malformed address will fault at EL0 (which translates into a
// user-mode extinction for the misbehaving Proc, not a kernel issue).
//
// Returns the new Thread on success, NULL on OOM (Thread cache or
// kstack alloc fail).
struct Thread *thread_create_user(struct Proc *proc,
                                  u64 user_entry_va,
                                  u64 user_sp_va,
                                  u64 user_arg,
                                  u64 user_tls_va);

// Release a Thread descriptor + its kstack. Caller must ensure the
// thread is not current (current_thread() != t) and not still on any
// runqueue. Extincts on violation.
void thread_free(struct Thread *t);

// Direct context switch from current_thread to `next`. Updates state
// fields (prev → RUNNABLE, next → RUNNING), parks `next` in TPIDR_EL1,
// then calls cpu_switch_context.
//
// At P2-A this is the only way to multitask — there's no scheduler yet.
// Test code uses it to demonstrate the switching primitive works
// end-to-end; subsequent sub-chunks add the EEVDF dispatch on top.
//
// IRQ masking (#101): thread_switch masks IRQs across its entire
// mutate+switch+resume window (spin_lock_irqsave(NULL)), because the
// set_current_thread(next)-before-cpu_switch_context sequence leaves a torn
// state and the timer IRQ now drives preemption (vectors.S IRQ-from-EL1 ->
// preempt_check_irq -> sched()). Without the mask a tick landing in the window
// corrupts the switch. This mirrors sched()'s own discipline; the mask is
// balanced per-thread (the saved state rides prev's kstack, restored when prev
// is switched back to). The stale pre-#101 comment ("P2-A does NOT mask IRQs")
// was written when preemption-via-IRQ did not yet exist.
void thread_switch(struct Thread *next);

// #101 regression hook (test-only; 0 in production). thread_switch busy-spins
// this many ns inside its torn window when nonzero, forcing a preempt to land
// there -> a deterministic guard for the IRQ-mask discipline. Set/reset by
// context.switch_irq_safe only.
extern volatile u64 g_thread_switch_test_window_ns;

// Diagnostic.
u64 thread_total_created(void);
u64 thread_total_destroyed(void);

#endif // THYLACINE_THREAD_H
