// Scheduler dispatch (P2-Ba). Plan 9 idiom layer over a simplified EEVDF
// run tree. Per ARCH §8.
//
// State model:
//   - One run tree per priority band (INTERACTIVE / NORMAL / IDLE).
//     Implemented as a doubly-linked list sorted ascending by `vd_t`.
//     The head holds the minimum vd_t — the next-to-run for the band.
//   - Highest-priority band with runnable threads wins; within a band,
//     pick-min-vd_t.
//   - On yield: prev's vd_t is advanced past every other runnable
//     thread's vd_t (g_vd_counter++) so prev lands at the back of the
//     rotation.
//
// At P2-Ba this is FIFO-like dispatch keyed on monotonic vd_t. Full
// EEVDF math (vd_t = ve_t + slice × W_total / w_self with weighted
// virtual time advance + bounded latency proof I-17) lands at P2-Bc.
//
// UP + no preemption at P2-Ba: no locks. The run tree is mutated
// only from `sched()` and `ready()` which are called from kernel
// threads cooperatively. P2-Bc adds IRQ-mask discipline around the
// run-tree mutations once scheduler-tick preemption (timer IRQ →
// sched()) becomes possible.

#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

// Per-band sorted-by-vd_t doubly-linked list head pointer. NULL if the
// band has no runnable threads. The head's vd_t is the minimum.
static struct Thread *g_run_tree[SCHED_BAND_COUNT];

// Monotonic vd_t counter. Starts at 1 (kthread reserves vd_t=0); each
// `sched()` advances current's vd_t to g_vd_counter++.
static s64 g_vd_counter = 1;

// One-shot init flag.
static bool g_sched_initialized = false;

void sched_init(void) {
    if (g_sched_initialized) extinction("sched_init called twice");
    if (!current_thread()) extinction("sched_init before thread_init");

    for (unsigned b = 0; b < SCHED_BAND_COUNT; b++) {
        g_run_tree[b] = NULL;
    }
    g_vd_counter = 1;
    g_sched_initialized = true;
}

// True iff `t` is currently in some run tree (linked or as head).
static bool in_run_tree(struct Thread *t) {
    return t->runnable_next != NULL || t->runnable_prev != NULL ||
           g_run_tree[t->band] == t;
}

// Insert `t` into its band's tree, sorted ascending by vd_t. Ties (equal
// vd_t) place `t` AFTER existing equal-keyed nodes — FIFO within ties.
static void insert_sorted(struct Thread *t) {
    struct Thread **head = &g_run_tree[t->band];

    // Empty band or t < head: prepend.
    if (!*head || (*head)->vd_t > t->vd_t) {
        t->runnable_prev = NULL;
        t->runnable_next = *head;
        if (*head) (*head)->runnable_prev = t;
        *head = t;
        return;
    }

    // Walk to find first node with vd_t > t->vd_t (or end of list).
    // FIFO tie-breaking: walk past nodes with vd_t == t->vd_t.
    struct Thread *cur = *head;
    while (cur->runnable_next && cur->runnable_next->vd_t <= t->vd_t) {
        cur = cur->runnable_next;
    }

    // Insert after cur.
    t->runnable_prev = cur;
    t->runnable_next = cur->runnable_next;
    if (cur->runnable_next) cur->runnable_next->runnable_prev = t;
    cur->runnable_next = t;
}

// Remove `t` from its band's tree. Caller has confirmed t is in the tree.
static void unlink(struct Thread *t) {
    if (t->runnable_prev) {
        t->runnable_prev->runnable_next = t->runnable_next;
    } else {
        // t was the head.
        g_run_tree[t->band] = t->runnable_next;
    }
    if (t->runnable_next) {
        t->runnable_next->runnable_prev = t->runnable_prev;
    }
    t->runnable_next = NULL;
    t->runnable_prev = NULL;
}

// Pick the highest-priority runnable thread; remove from its tree.
// Returns NULL if no thread is runnable across any band.
static struct Thread *pick_next(void) {
    for (unsigned b = 0; b < SCHED_BAND_COUNT; b++) {
        struct Thread *t = g_run_tree[b];
        if (t) {
            unlink(t);
            return t;
        }
    }
    return NULL;
}

void ready(struct Thread *t) {
    if (!t)                       extinction("ready(NULL)");
    if (t->magic != THREAD_MAGIC) extinction("ready of corrupted Thread");
    if (t->state != THREAD_RUNNABLE)
                                  extinction("ready of non-RUNNABLE Thread");
    if (t->band >= SCHED_BAND_COUNT)
                                  extinction("ready: invalid band");
    if (in_run_tree(t))           extinction("ready of already-runnable Thread");

    insert_sorted(t);
}

void sched(void) {
    if (!g_sched_initialized) extinction("sched() before sched_init");

    struct Thread *prev = current_thread();
    if (!prev) extinction("sched() with no current thread");
    if (prev->magic != THREAD_MAGIC) extinction("sched() with corrupted current");

    struct Thread *next = pick_next();
    if (!next) {
        // No other runnable thread; keep prev running.
        return;
    }

    // Advance prev's vd_t past all currently-runnable threads. Insert
    // prev at the back of its band's rotation. State updates symmetric
    // with thread_switch's pattern (prev → RUNNABLE before the asm
    // switch; next → RUNNING + set_current_thread before the call).
    prev->vd_t = g_vd_counter++;
    prev->state = THREAD_RUNNABLE;
    insert_sorted(prev);

    next->state = THREAD_RUNNING;
    set_current_thread(next);

    cpu_switch_context(&prev->ctx, &next->ctx);

    // Resumption: prev was switched back to. State and current_thread
    // were set by whichever peer called sched() with prev as the
    // pick-next target. prev is no longer in the run tree (the peer's
    // pick_next removed it before switching).
}

void sched_remove_if_runnable(struct Thread *t) {
    if (!t) return;
    if (t->state != THREAD_RUNNABLE) return;
    if (!in_run_tree(t)) return;
    unlink(t);
}

unsigned sched_runnable_count(void) {
    unsigned n = 0;
    for (unsigned b = 0; b < SCHED_BAND_COUNT; b++) {
        for (struct Thread *t = g_run_tree[b]; t; t = t->runnable_next) {
            n++;
        }
    }
    return n;
}

unsigned sched_runnable_count_band(unsigned band) {
    if (band >= SCHED_BAND_COUNT) return 0;
    unsigned n = 0;
    for (struct Thread *t = g_run_tree[band]; t; t = t->runnable_next) {
        n++;
    }
    return n;
}
