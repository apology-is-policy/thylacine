// Process descriptor management (P2-A) + rfork/exits/wait lifecycle (P2-D).
//
// Per ARCHITECTURE.md §7.2 + §7.4 + §7.9. P2-A established the bare
// kproc + proc_alloc/proc_free + monotonic PID assignment. P2-D adds
// the multi-process lifecycle: rfork creates a new Proc with one
// initial Thread; exits transitions Proc to ZOMBIE; wait_pid reaps
// zombie children.
//
// At v1.0 P2-D, only RFPROC is supported in rfork — namespace, fd
// table, address space, credentials, etc. land in subsequent P2 sub-
// chunks. Multi-thread Procs and the Linux clone() flag translation
// land at Phase 5+ with the syscall surface.
//
// Bootstrap order (kernel/main.c calls in this order):
//   1. slub_init      — kmem_cache_create can allocate now
//   2. proc_init      — kproc (PID 0) appears
//   3. thread_init    — kthread (TID 0) appears, parented to kproc

#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/page.h>
#include <thylacine/pgrp.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../mm/slub.h"

static struct kmem_cache *g_proc_cache;
static struct Proc       *g_kproc;
static int                g_next_pid = 0;
static u64                g_proc_created;
static u64                g_proc_destroyed;

// Initialize a freshly-allocated Proc descriptor. Caller has already
// passed KP_ZERO to kmem_cache_alloc, so all fields are zero/NULL on
// entry; this only sets the non-zero-default values.
static void proc_init_fields(struct Proc *p, int pid) {
    p->magic = PROC_MAGIC;
    p->pid   = pid;
    p->state = PROC_STATE_ALIVE;
    rendez_init(&p->child_done);
    // parent / children / sibling / exit_status / exit_msg / threads /
    // pgrp all left NULL/0 via KP_ZERO; caller (proc_init for kproc;
    // rfork for child Procs) wires linkage explicitly.
}

void proc_init(void) {
    if (g_proc_cache) extinction("proc_init called twice");
    if (!kpgrp())     extinction("proc_init before pgrp_init "
                                 "(kproc needs a pgrp at allocation)");

    g_proc_cache = kmem_cache_create("proc",
                                     sizeof(struct Proc),
                                     8,
                                     KMEM_CACHE_PANIC_ON_FAIL);
    if (!g_proc_cache) {
        extinction("kmem_cache_create(proc) returned NULL");
    }

    g_kproc = kmem_cache_alloc(g_proc_cache, KP_ZERO);
    if (!g_kproc) extinction("kmem_cache_alloc(kproc) failed");
    proc_init_fields(g_kproc, 0);
    g_kproc->pgrp = kpgrp();

    // P2-Fc: kproc gets its own handle table. handle_init must run
    // before proc_init (main.c bootstrap order). Failures here panic —
    // boot can't continue without kproc.
    g_kproc->handles = handle_table_alloc();
    if (!g_kproc->handles) extinction("handle_table_alloc(kproc) failed");

    g_proc_created++;
    g_next_pid = 1;
}

struct Proc *kproc(void) {
    return g_kproc;
}

struct Proc *proc_alloc(void) {
    if (!g_proc_cache) extinction("proc_alloc before proc_init");
    struct Proc *p = kmem_cache_alloc(g_proc_cache, KP_ZERO);
    if (!p) return NULL;
    proc_init_fields(p, g_next_pid++);

    // R5-F F50/F53 close: count this Proc as created BEFORE any failure
    // path that might tear it down via proc_free. proc_free does its
    // own destroyed++ — this hoist keeps the counters balanced even
    // when rollback fires (created+1 / destroyed+1 net zero).
    g_proc_created++;

    // P2-Fc: each Proc gets its own handle table. On OOM (now reachable
    // post-R5-F F50; PANIC_ON_FAIL dropped from g_handle_table_cache),
    // delegate cleanup to proc_free — which is future-proof against
    // struct Proc growth (R5-F F53). The Proc has no pgrp / threads /
    // children yet (KP_ZERO), so proc_free's pgrp_unref(NULL) +
    // handle_table_free(NULL) + lifecycle gates all no-op cleanly.
    p->handles = handle_table_alloc();
    if (!p->handles) {
        p->state = PROC_STATE_ZOMBIE;
        proc_free(p);              // does its own g_proc_destroyed++
        return NULL;
    }

    return p;
}

void proc_free(struct Proc *p) {
    if (!p)                       extinction("proc_free(NULL)");
    // Magic check catches double-free and corrupt-Proc passes. SLUB's
    // freelist write at kmem_cache_free clobbers magic; subsequent free
    // reads the clobbered value and trips here.
    if (p->magic != PROC_MAGIC)   extinction("proc_free of corrupted or already-freed Proc");
    if (p == g_kproc)             extinction("proc_free attempted on kproc");
    if (p->thread_count)          extinction("proc_free with live threads (caller must drain)");
    if (p->threads)               extinction("proc_free with non-NULL threads list");
    if (p->children)              extinction("proc_free with live children (caller must reap or re-parent)");
    // P2-D: state must be ZOMBIE (came through exits) — no other path
    // legitimately reaches proc_free in the lifecycle. ALIVE means we
    // forgot exits; INVALID means we're freeing an uninitialized Proc.
    if (p->state != PROC_STATE_ZOMBIE)
        extinction("proc_free of non-ZOMBIE Proc (lifecycle violation)");
    // P2-Eb: release the namespace. Most Procs have a private pgrp
    // (refcount 1; freed here). Phase 5+ shared namespaces decrement
    // refcount and free only at last release.
    pgrp_unref(p->pgrp);
    p->pgrp = NULL;

    // P2-Fc: release the handle table. Closes any in-use slots first
    // (defensive — well-behaved Procs close all handles before exits;
    // but a Proc that crashed mid-session leaves stragglers).
    handle_table_free(p->handles);
    p->handles = NULL;

    kmem_cache_free(g_proc_cache, p);
    g_proc_destroyed++;
}

u64 proc_total_created(void)   { return g_proc_created; }
u64 proc_total_destroyed(void) { return g_proc_destroyed; }

// =============================================================================
// P2-D: rfork / exits / wait_pid
// =============================================================================

// Link a child Proc onto a parent's children list (head insertion).
// Single-threaded at v1.0 P2-D — no locking; rfork is called only by
// kernel test code on the boot CPU. Phase 5+ adds a per-Proc lock.
static void proc_link_child(struct Proc *parent, struct Proc *child) {
    child->parent  = parent;
    child->sibling = parent->children;
    parent->children = child;
}

// Unlink a child from its parent's children list. The child's state is
// expected to be ZOMBIE (post-exits); after unlink, caller proc_frees
// the child.
static void proc_unlink_child(struct Proc *parent, struct Proc *child) {
    if (parent->children == child) {
        parent->children = child->sibling;
    } else {
        struct Proc *prev = parent->children;
        while (prev && prev->sibling != child) prev = prev->sibling;
        if (!prev) extinction("proc_unlink_child: child not in parent's list");
        prev->sibling = child->sibling;
    }
    child->sibling = NULL;
    child->parent  = NULL;
}

// Re-parent a Proc's children to kproc on exit. At Phase 5+ this targets
// init (PID 1); at v1.0 there's no init yet so kproc adopts orphans.
// kproc never calls wait_pid, so adopted orphans become permanent
// zombies — acceptable at v1.0 because the only test scenarios that
// exercise exits don't have orphan grandchildren. Phase 2 close adds
// a kthread reaper or moves to PID 1.
static void proc_reparent_children(struct Proc *p) {
    while (p->children) {
        struct Proc *c = p->children;
        p->children = c->sibling;
        c->parent = g_kproc;
        c->sibling = g_kproc->children;
        g_kproc->children = c;
    }
}

int rfork(unsigned flags, void (*entry)(void *), void *arg) {
    // P2-D: only RFPROC supported. Other flags reserved for subsequent
    // sub-chunks (RFNAMEG at P2-E, RFFDG at P2-F, RFMEM at P2-G, etc.).
    if (flags != RFPROC) {
        extinction("rfork: only RFPROC supported at P2-D");
    }
    if (!entry) extinction("rfork with NULL entry");

    struct Thread *t = current_thread();
    if (!t)                  extinction("rfork with no current thread");
    if (t->magic != THREAD_MAGIC)
                             extinction("rfork from corrupted current thread");
    struct Proc *parent = t->proc;
    if (!parent)             extinction("rfork from thread with no proc");
    if (parent->magic != PROC_MAGIC)
                             extinction("rfork from thread with corrupted proc");

    struct Proc *child = proc_alloc();
    if (!child) return -1;

    // P2-Eb: clone parent's namespace into the child. Maps to the spec's
    // ForkClone action — child gets a deep copy of parent's bindings;
    // subsequent bind/unmount on either is independent. RFNAMEG (shared
    // namespace) is unsupported at v1.0; the parent ALWAYS gets a clone.
    struct Pgrp *child_pgrp = pgrp_clone(parent->pgrp);
    if (!child_pgrp) {
        child->state = PROC_STATE_ZOMBIE;
        // child->pgrp is still NULL; proc_free's pgrp_unref(NULL) is a no-op.
        proc_free(child);
        return -1;
    }
    child->pgrp = child_pgrp;

    struct Thread *ct = thread_create_with_arg(child, entry, arg);
    if (!ct) {
        // Roll back proc_alloc + pgrp_clone. Transition to ZOMBIE so
        // proc_free's lifecycle gate passes; proc_free will pgrp_unref
        // the just-allocated pgrp.
        child->state = PROC_STATE_ZOMBIE;
        proc_free(child);
        return -1;
    }

    proc_link_child(parent, child);

    // Insert into the local CPU's run tree. ready() handles the state
    // transition (RUNNABLE → in-runtree). thread_create already set
    // state = THREAD_RUNNABLE.
    ready(ct);

    return child->pid;
}

void exits(const char *msg) {
    struct Thread *t = current_thread();
    if (!t)                  extinction("exits with no current thread");
    if (t->magic != THREAD_MAGIC)
                             extinction("exits from corrupted current thread");

    struct Proc *p = t->proc;
    if (!p)                  extinction("exits from thread with no proc");
    if (p->magic != PROC_MAGIC)
                             extinction("exits from thread with corrupted proc");
    if (p == g_kproc)        extinction("exits from kproc (boot thread)");
    if (p->state != PROC_STATE_ALIVE)
                             extinction("exits from non-ALIVE proc (double exits?)");
    // v1.0 P2-D: single-thread Procs (rfork creates 1 thread). Multi-
    // threaded exits requires terminating all sibling threads (Phase 5+
    // via cross-CPU IPI to halt + reap). Guard so we surface the limit
    // explicitly.
    if (p->thread_count != 1)
        extinction("exits with thread_count != 1 (multi-thread Procs not supported at P2-D)");

    // Re-parent any orphan children to kproc before transitioning.
    if (p->children) {
        proc_reparent_children(p);
    }

    // Capture exit status. At v1.0 the convention is "ok" = clean
    // exit (status 0); anything else = error (status 1). msg is
    // captured by reference; caller-owned (typically a string literal).
    p->exit_msg    = msg ? msg : "ok";
    p->exit_status = (msg && msg[0] == 'o' && msg[1] == 'k' && msg[2] == 0) ? 0 : 1;
    p->state       = PROC_STATE_ZOMBIE;

    // Mark the executing thread EXITING so sched() leaves it out of the
    // run tree (it will be reaped by the parent's wait_pid).
    t->state = THREAD_EXITING;

    // Wake parent's child_done Rendez. The parent re-checks the
    // children list under r->lock and finds this child in ZOMBIE
    // state. Producer-side state mutation (p->state = ZOMBIE) happened
    // BEFORE wakeup — wakeup() takes r->lock, which establishes the
    // happens-before edge; the parent's sleep cond predicate observes
    // the ZOMBIE state on resume.
    if (p->parent) {
        wakeup(&p->parent->child_done);
    }

    // Yield. Will not return — we're EXITING, sched() doesn't re-insert,
    // and there's no future wake target for us.
    sched();
    extinction("exits: returned from sched (impossible)");
}

// cond predicate for wait_pid's sleep: any child in ZOMBIE state, OR
// the parent has no children at all (-1 return path).
static int wait_pid_cond(void *arg) {
    struct Proc *parent = arg;
    if (!parent->children) return 1;          // no children → wake to return -1
    for (struct Proc *c = parent->children; c; c = c->sibling) {
        if (c->state == PROC_STATE_ZOMBIE) return 1;
    }
    return 0;
}

int wait_pid(int *status_out) {
    struct Thread *t = current_thread();
    if (!t)                  extinction("wait_pid with no current thread");
    struct Proc *p = t->proc;
    if (!p)                  extinction("wait_pid with no proc");
    if (p->magic != PROC_MAGIC)
                             extinction("wait_pid with corrupted proc");

    for (;;) {
        if (!p->children) {
            // No children at all (live or zombie). Nothing to wait for.
            return -1;
        }

        struct Proc *zombie = NULL;
        for (struct Proc *c = p->children; c; c = c->sibling) {
            if (c->state == PROC_STATE_ZOMBIE) {
                zombie = c;
                break;
            }
        }

        if (zombie) {
            int pid = zombie->pid;
            if (status_out) *status_out = zombie->exit_status;

            proc_unlink_child(p, zombie);

            // Reap: free the (single) thread + its kstack + the Proc.
            // The thread is in EXITING state — thread_free's state-
            // gate already accepts this (it only blocks RUNNING +
            // INVALID).
            struct Thread *ct = zombie->threads;
            if (!ct) extinction("wait_pid: zombie with no threads");
            if (ct->state != THREAD_EXITING)
                extinction("wait_pid: zombie thread not in EXITING state");

            // P2-Dd-pre: spin until ct is fully off-CPU. exits() on the
            // child set state=EXITING + sched()'d; the destination
            // CPU's resume code (sched_finish_task_switch in trampoline
            // OR the post-cpu_switch_context block in sched()) clears
            // ct->on_cpu via cs->prev_to_clear_on_cpu. Without this
            // spin, thread_free could race with the destination CPU
            // still mid-switch — TPIDR_EL1 on that CPU briefly points
            // at ct around set_current_thread(next), and freeing ct's
            // slot mid-window means the next sched() on that CPU sees
            // a clobbered magic ("sched() with corrupted current").
            // Mirrors the on_cpu spin in wakeup() for the wait/wake
            // race close (P2-Cf trip-hazard #16).
            while (__atomic_load_n(&ct->on_cpu, __ATOMIC_ACQUIRE)) {
                __asm__ __volatile__("yield" ::: "memory");
            }

            thread_free(ct);
            // thread_free unlinks ct from zombie->threads + decrements
            // thread_count, so by here zombie has thread_count==0 and
            // threads==NULL — proc_free's preconditions are met.

            proc_free(zombie);
            return pid;
        }

        // No zombie yet, but live children exist. Sleep on child_done.
        // exits() in any of our children will wakeup this Rendez. The
        // cond predicate re-evaluates "any zombie? or no children?"
        // under r->lock; sleep is atomic with the cond check (see
        // scheduler.tla NoMissedWakeup proof).
        sleep(&p->child_done, wait_pid_cond, p);
    }
}
