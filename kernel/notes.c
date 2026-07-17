// P6-pouch-signals-impl (sub-chunk 13a): kernel notes substrate.
//
// Per ARCH §7.6.1-§7.6.8 (binding design at 237f096) and the API in
// <thylacine/notes.h>. The queue is the truth; the fd is a view (devnotes);
// the async-handler path is a parallel consumer; mutual exclusion across
// the two paths is enforced by the queue lock.
//
// Mechanism — the queue is a fixed-size ring of NOTE_QUEUE_DEPTH entries.
// notes_post enqueues (under lock); the EL0-return-tail dispatch and
// devnotes_read each dequeue (under lock). When the queue becomes non-
// empty, the post path wakes the devnotes_read Rendez AND walks the
// SYS_POLL poll_waiter_list. The two consumers race; the queue lock
// serializes the consume.
//
// Coalesce policy — kernel-synthetic posters (notes_post_child_exit on
// exits(), notes_post_pipe on write-to-closed) call notes_post with
// synthetic=true. When the queue count is >= NOTE_COALESCE_THRESHOLD, a
// same-name same-source already-queued entry has its arg overwritten with
// the new value (the last child_exit arg wins). Userspace SYS_POSTNOTE
// passes synthetic=false; queue-full returns -EAGAIN, which the syscall
// surface translates to flat -1.

#include <thylacine/notes.h>

#include <thylacine/extinction.h>
#include <thylacine/poll.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/spinlock.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../arch/arm64/timer.h"
#include "../mm/slub.h"

// ---------------------------------------------------------------------------
// Name validation + NOTE_BIT_* lookup.
// ---------------------------------------------------------------------------
//
// The v1.0 supported set is a closed table. notes_name_to_bit returns the
// NOTE_BIT_* index for a supported name, or -1 for an unsupported / bogus
// name. notes_name_is_kill is a fast-path predicate the EL0-return-tail
// dispatch uses to recognize the non-catchable note (N-4).
//
// We accept input names ONLY from the closed v1.0 set. SYS_POSTNOTE rejects
// anything else with -EINVAL (per ARCH §7.6.5 — alarm/hangup/usr1/usr2/
// stop/cont are deferred); kernel-synthetic posters only call with the
// known names (child_exit, pipe).

struct note_name_entry {
    const char *name;
    u32         bit;
};

static const struct note_name_entry g_known_notes[] = {
    { "interrupt",  NOTE_BIT_INTERRUPT  },
    { "kill",       NOTE_BIT_KILL       },
    { "pipe",       NOTE_BIT_PIPE       },
    { "child_exit", NOTE_BIT_CHILD_EXIT },
    // PTY-1b: the tty:* family (kernel-synthetic-POST + catchable; ONE
    // family mask bit, the SNARE per-kind-masking-is-v1.x precedent).
    // notes_post's tty-prefix gate is the ONLY thing keeping userspace
    // posters out of these entries -- load-bearing, unlike the snare
    // future-proofing (snare:* is not in this table at v1.0).
    { NOTE_NAME_TTY_WINCH, NOTE_BIT_TTY },
    { NOTE_NAME_TTY_SUSP,  NOTE_BIT_TTY },
    { NOTE_NAME_TTY_CONT,  NOTE_BIT_TTY },
    { NOTE_NAME_TTY_QUIT,  NOTE_BIT_TTY },
    { NOTE_NAME_TTY_HUP,   NOTE_BIT_TTY },
};
#define NOTE_NUM_KNOWN  (sizeof(g_known_notes) / sizeof(g_known_notes[0]))

// strncmp-style name compare bounded by NOTE_NAME_MAX (so we don't read
// past the in-kernel buffer if the input lacks the NUL).
static int notes_name_eq(const char *a, const char *b) {
    for (u32 i = 0; i < NOTE_NAME_MAX; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == 0)    return 1;
    }
    return 0;     // ran off the end without NUL — caller's bug
}

// Returns -1 if `name` is not in the v1.0 supported set; otherwise the
// NOTE_BIT_* index.
static int notes_name_to_bit(const char *name) {
    for (u32 i = 0; i < NOTE_NUM_KNOWN; i++) {
        if (notes_name_eq(name, g_known_notes[i].name)) {
            return (int)g_known_notes[i].bit;
        }
    }
    return -1;
}

// P6 hardening #3a (scripture e45a571 -- docs/ERRORS.md): the `snare:`
// prefix is RESERVED for kernel-synthetic posts (the fault-note family
// kept in <thylacine/notes.h> NOTE_NAME_SNARE_*). Userspace SYS_POSTNOTE
// MUST NOT be able to fake a fault note in any Proc's queue. Returns 1
// if `name` starts with "snare:" (bounded read; NOTE_NAME_MAX-safe),
// 0 otherwise.
//
// notes_post enforces the gate explicitly so the rejection survives a
// future supported-set extension (today snare:* isn't in g_known_notes
// so notes_name_to_bit already rejects; the explicit prefix check
// future-proofs against that path being relaxed).
// PTY-1b (round-1 F4): the tty:* prefix is likewise kernel-synthetic-only
// on the POST axis -- but UNLIKE snare:*, the tty names ARE in
// g_known_notes (deliverable + catchable), so this prefix gate is the ONLY
// barrier keeping a userspace SYS_POSTNOTE out of them. Load-bearing:
// without it, `tty:cont` is postable via the ordinary parent-only note
// path (no CAP_DEBUG), letting an unprivileged parent resume a
// debugger-stopped child once the PTY-1f stop machinery reads cont -- the
// I-39 leak the design closes.
static int notes_name_has_tty_prefix(const char *name) {
    static const char prefix[] = "tty:";
    _Static_assert(sizeof(prefix) <= NOTE_NAME_MAX,
                   "tty: prefix must be shorter than NOTE_NAME_MAX so the "
                   "bounded compare below terminates within the name buffer");
    for (u32 i = 0; i + 1 < sizeof(prefix); i++) {
        if (name[i] == '\0') return 0;
        if (name[i] != prefix[i]) return 0;
    }
    return 1;
}

static int notes_name_has_snare_prefix(const char *name) {
    static const char prefix[] = "snare:";
    // F5 audit close: use sizeof(prefix) - 1 directly as the
    // _Static_assert operand. ISO C11 §6.6 requires integer constant
    // expressions for _Static_assert; `static const u32 plen = ...`
    // is accepted by clang as a GNU extension but isn't a constant
    // expression per the standard. sizeof IS.
    _Static_assert(sizeof(prefix) - 1 < NOTE_NAME_MAX,
                   "snare: prefix must be shorter than NOTE_NAME_MAX so the "
                   "bounded compare cannot read past the in-kernel name buffer");
    for (u32 i = 0; i < sizeof(prefix) - 1; i++) {
        // Defense-in-depth: if `name` is shorter than the prefix and
        // NUL-terminates early, the mismatch on the NUL returns 0
        // (not a snare:* note).
        if (name[i] != prefix[i]) return 0;
    }
    return 1;
}

// R4-F4 audit close: notes_name_is_kill is now declared in the public
// header (`<thylacine/notes.h>`) -- the prior forward decl block here
// and the function-local extern decls in syscall.c are removed.

// F10 audit close: return the canonical string-literal pointer for a
// supported note name. The literal lives for the program's lifetime
// (read-only data section), so it's safe to capture by reference into
// p->exit_msg without a per-Proc copy buffer. Returns NULL if the input
// doesn't match a supported name (caller passes "unknown" then).
static const char *notes_canonical_name_ptr(const char *name) {
    for (u32 i = 0; i < NOTE_NUM_KNOWN; i++) {
        if (notes_name_eq(name, g_known_notes[i].name)) {
            return g_known_notes[i].name;
        }
    }
    return NULL;
}

// Bounded strncpy for the in-kernel note name buffer. Always NUL-
// terminates within NOTE_NAME_MAX. Caller has already validated the
// source is short enough.
static void notes_name_copy(char *dst, const char *src) {
    u32 i = 0;
    for (; i < NOTE_NAME_MAX - 1; i++) {
        dst[i] = src[i];
        if (src[i] == 0) break;
    }
    for (; i < NOTE_NAME_MAX; i++) {
        dst[i] = 0;
    }
}

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------

void notes_queue_init(struct NoteQueue *q) {
    spin_lock_init(&q->lock);
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->_pad = 0;
    for (u32 i = 0; i < NOTE_QUEUE_DEPTH; i++) {
        for (u32 j = 0; j < NOTE_NAME_MAX; j++) q->ring[i].name[j] = 0;
        q->ring[i].arg = 0;
        q->ring[i].sender_pid = 0;
        q->ring[i].timestamp_ns = 0;
    }
    poll_waiter_list_init(&q->poll_list);
}

struct NoteQueue *notes_queue_alloc(void) {
    struct NoteQueue *q = kmalloc(sizeof(*q), KP_ZERO);
    if (!q) return NULL;
    notes_queue_init(q);
    return q;
}

void notes_queue_free(struct NoteQueue *q) {
    if (!q) return;
    // Defense-in-depth: at free time the queue MUST be quiescent — no
    // registered poll_waiter. proc_free's calling sequence guarantees
    // this (ZOMBIE state set, all Threads EXITING, handles closed); a
    // violation here would be a serious lifecycle bug.
    if (q->poll_list.head != NULL) {
        extinction("notes_queue_free: poll_waiter still registered on "
                   "q->poll_list (poll caller leaked a hook?)");
    }
    kfree(q);
}

// ---------------------------------------------------------------------------
// Post + dequeue.
// ---------------------------------------------------------------------------

// Find an already-queued same-name same-source entry in [head, tail).
// Returns its ring index (>= 0) or -1 if not found. Caller MUST hold
// q->lock. Used only by the coalesce path (kernel-synthetic posters).
static int notes_find_locked(struct NoteQueue *q, const char *name,
                             u32 sender_pid) {
    u32 idx = q->head;
    for (u32 n = 0; n < q->count; n++) {
        if (q->ring[idx].sender_pid == sender_pid &&
            notes_name_eq(q->ring[idx].name, name)) {
            return (int)idx;
        }
        idx = (idx + 1) % NOTE_QUEUE_DEPTH;
    }
    return -1;
}

// LS-5c (P3-terminate, ARCH 8.8.2): arm the terminate latch when an
// `interrupt` lands in the queue of a Proc whose LS-5b disposition is
// terminate -- no async handler, not self-managing. Caller MUST hold q->lock.
// ALL latch writes are q->lock-serialized (this arm vs the clears in
// notes_set_handler / notes_mark_self_managing / notes_drain_intr_locked) --
// without that, a concurrent SYS_NOTIFY could store its handler and clear
// the latch BETWEEN this function's handler check and its set, stranding a
// stale armed latch behind a registered handler, which would *_INTR every
// future sleep of a surviving Proc. Masks are deliberately NOT consulted:
// they are per-Thread, and both the lock-free sleep predicate
// (thread_die_pending) and the EL0-return tail apply the observing thread's
// own mask. RELEASE pairs with the lock-free acquire reads.
//
// kproc guard: in-kernel tests post notes to kproc's queue via the boot
// thread (devnotes is stateless -- a notes fd reads the CALLER's queue), and
// kproc never registers a handler nor opens a notes fd, so without the guard
// a test's interrupt post would arm kproc and every subsequent kernel-thread
// sleep would return *_INTR; kproc threads also never EL0-return, so the
// latch could never be consumed.
// PTY-1b: the terminate class -- names whose UNCAUGHT default disposition
// is process termination at the EL0-return tail (the LS-5 interrupt pattern
// generalized: interrupt == SIGINT, tty:quit == SIGQUIT, tty:hup == SIGHUP).
// tty:susp's default is STOP, applied at POST time by proc_job_stop_pgrp
// (PTY-1f) -- an uncaught susp never reaches a queue, so it deliberately has
// NO latch here -- and tty:winch /
// tty:cont are informational (queue for the fd-read path, no default
// action -- the pipe/child_exit shape). Returns the class's LATCH flag
// (PROC_FLAG_INTR_TERMINATE_PENDING for interrupt,
// PROC_FLAG_TTY_TERMINATE_PENDING for the tty pair -- each latch pairs
// with ITS OWN family mask bit in the lock-free die predicate), or 0 for a
// non-terminate name.
static u32 notes_name_terminate_latch(const char *name) {
    int bit = notes_name_to_bit(name);
    if (bit == (int)NOTE_BIT_INTERRUPT)
        return PROC_FLAG_INTR_TERMINATE_PENDING;
    if (bit == (int)NOTE_BIT_TTY &&
        (notes_name_eq(name, NOTE_NAME_TTY_QUIT) ||
         notes_name_eq(name, NOTE_NAME_TTY_HUP)))
        return PROC_FLAG_TTY_TERMINATE_PENDING;
    return 0;
}

static void notes_arm_intr_terminate_locked(struct Proc *p, const char *name) {
    u32 latch = notes_name_terminate_latch(name);
    if (latch == 0) return;
    if (p == kproc()) return;
    if (__atomic_load_n(&p->handler_va, __ATOMIC_ACQUIRE) != 0) return;
    if (proc_is_self_managing_notes(p)) return;
    __atomic_or_fetch(&p->proc_flags, latch, __ATOMIC_RELEASE);
}

// LS-5c: clear the terminate latch when the LAST queued `interrupt` is
// drained (the fd-read path of a Proc holding an inherited notes fd, or the
// dispatcher's handler-delivery pop). Caller MUST hold q->lock. A no-op
// unless the popped note is an interrupt and no interrupt remains queued.
static void notes_drain_intr_locked(struct Proc *p, struct NoteQueue *q,
                                    const struct Note *popped) {
    // PTY-1b: per-CLASS drain -- popping the last terminate-class note of a
    // class clears THAT class's latch (another class's pending note keeps
    // its own latch armed independently).
    u32 latch = notes_name_terminate_latch(popped->name);
    if (latch == 0) return;
    u32 idx = q->head;
    for (u32 n = 0; n < q->count; n++) {
        if (notes_name_terminate_latch(q->ring[idx].name) == latch)
            return;             // another same-class note remains queued
        idx = (idx + 1) % NOTE_QUEUE_DEPTH;
    }
    __atomic_and_fetch(&p->proc_flags, ~latch, __ATOMIC_RELEASE);
}

int notes_post(struct Proc *p, const char *name, u32 arg,
               struct Proc *sender, bool synthetic) {
    if (!p) return -1;
    if (!p->notes) return -1;
    if (!name) return -1;

    // P6 hardening #3a (scripture e45a571): the snare:* fault-note
    // prefix is reserved for kernel-synthetic posters. Userspace
    // SYS_POSTNOTE callers (synthetic=false) MUST NOT be able to
    // fake a fault note in any Proc's queue -- a faked snare:segv
    // would otherwise let a malicious Proc impersonate a memory
    // fault to fool a sibling Proc's fd-read consumer of /dev/notes.
    // (Today snare:* isn't in the supported set so notes_name_to_bit
    // below also rejects -- this explicit prefix check future-proofs
    // against snare:* being added to the supported set for kernel-
    // synthetic-only delivery.)
    if (!synthetic && notes_name_has_snare_prefix(name)) return -1;

    // PTY-1b (round-1 F4): the tty:* family is kernel-synthetic-only on the
    // POST axis. Unlike the snare gate above this one is load-bearing TODAY
    // (the tty names are in the supported set below).
    if (!synthetic && notes_name_has_tty_prefix(name)) return -1;

    // Validate name is in the v1.0 supported set.
    if (notes_name_to_bit(name) < 0) return -1;

    u32 sender_pid = sender ? (u32)sender->pid : 0u;
    u64 ts = timer_now_ns();

    struct NoteQueue *q = p->notes;
    spin_lock(&q->lock);

    // Coalesce path: kernel-synthetic + queue full or near-full + an
    // entry of the same (name, sender) is already present. Overwrite its
    // arg + timestamp (preserve queue position so N-1 ordering for the
    // SOURCE is maintained — the source has one slot's worth of "the
    // most recent thing I posted"). Userspace posts (synthetic == false)
    // never coalesce — -EAGAIN bubbles to userspace cleanly.
    if (q->count >= NOTE_COALESCE_THRESHOLD && synthetic) {
        int found = notes_find_locked(q, name, sender_pid);
        if (found >= 0) {
            q->ring[found].arg = arg;
            q->ring[found].timestamp_ns = ts;
            // LS-5c: the coalesced entry re-evaluates the disposition too --
            // the original post may have declined to arm (a handler existed
            // then) while THIS post's disposition is terminate.
            notes_arm_intr_terminate_locked(p, name);
            // Wake registered poll_waiters AND devnotes_read's pollers
            // (same list at F3 close).
            poll_waiter_list_wake(&q->poll_list);
            spin_unlock(&q->lock);
            return 0;
        }
    }

    // Full and no coalesce: -EAGAIN.
    if (q->count >= NOTE_QUEUE_DEPTH) {
        spin_unlock(&q->lock);
        return -1;
    }

    // Enqueue at tail.
    u32 idx = q->tail;
    notes_name_copy(q->ring[idx].name, name);
    q->ring[idx].arg = arg;
    q->ring[idx].sender_pid = sender_pid;
    q->ring[idx].timestamp_ns = ts;
    q->tail = (q->tail + 1) % NOTE_QUEUE_DEPTH;
    q->count++;

    // LS-5c (P3-terminate): a freshly-enqueued `interrupt` whose disposition
    // is terminate arms the lock-free latch the #811 sleep predicate reads.
    // The WAKE of already-blocked threads is the caller's duty (it requires
    // g_proc_table_lock for the p->threads walk): every interrupt-posting
    // site calls proc_interrupt_terminate_wake after this returns. A
    // not-yet-sleeping thread is covered without the wake -- its sleep's
    // register-then-observe reads the latch.
    notes_arm_intr_terminate_locked(p, name);

    // Wake every registered poll_waiter (including any devnotes_read
    // parked on this list per F3 close) BEFORE we drop the queue lock.
    // The register-then-observe discipline requires that any concurrent
    // dev->poll's sample either happens-before our enqueue (and sees
    // empty; the subsequent post wakes it) or after (and sees the new
    // entry). poll_waiter_list_wake takes its own lock per the documented
    // lock order (object → list).
    poll_waiter_list_wake(&q->poll_list);

    spin_unlock(&q->lock);
    return 0;
}

// F5/F6 audit close: re-enqueue at head. Caller MUST hold q->lock.
// Cannot fail at v1.0 since the caller just popped (so there is a free
// slot). Pre-decrement head with wrap; the new entry is the next dequeue.
int notes_reenqueue_head_locked(struct NoteQueue *q, const struct Note *n) {
    if (!q || !n) return -1;
    if (q->count >= NOTE_QUEUE_DEPTH) {
        // Should be unreachable for the caller pattern (just popped).
        // Defense-in-depth: refuse.
        return -1;
    }
    // Decrement head with modulo wrap.
    q->head = (q->head + NOTE_QUEUE_DEPTH - 1) % NOTE_QUEUE_DEPTH;
    q->ring[q->head] = *n;
    q->count++;
    // Caller has not changed the readiness state of the queue (a note
    // popped and re-pushed is back where it started); we wake regardless
    // so any consumer that was about to register-and-sleep finds the
    // queue non-empty.
    poll_waiter_list_wake(&q->poll_list);
    return 0;
}

int notes_peek_locked(struct Proc *p, struct Thread *t, struct Note *out) {
    if (!p || !p->notes || !out) return 0;
    struct NoteQueue *q = p->notes;
    if (q->count == 0) return 0;

    u32 mask = (t != NULL) ? t->note_mask : 0u;

    // F2 audit close: kill is non-catchable (N-4) — mask MUST NOT defer
    // its delivery. First pass scans for kill regardless of mask.
    {
        u32 idx = q->head;
        for (u32 n = 0; n < q->count; n++) {
            if (notes_name_is_kill(q->ring[idx].name)) {
                *out = q->ring[idx];
                return 1;
            }
            idx = (idx + 1) % NOTE_QUEUE_DEPTH;
        }
    }

    // No kill present; second pass scans for the first mask-permitted
    // entry in FIFO order.
    {
        u32 idx = q->head;
        for (u32 n = 0; n < q->count; n++) {
            int bit = notes_name_to_bit(q->ring[idx].name);
            if (bit >= 0 && (mask & (1u << (u32)bit)) == 0) {
                *out = q->ring[idx];
                return 1;
            }
            idx = (idx + 1) % NOTE_QUEUE_DEPTH;
        }
    }
    return 0;
}

// Pop the entry at `idx` from a ring of count `n` starting at head `head`.
// `idx` is an absolute ring index in [0, NOTE_QUEUE_DEPTH). Updates head/
// tail/count + shifts intermediate entries to preserve N-1 ordering.
static void notes_pop_at_locked(struct NoteQueue *q, u32 idx) {
    // Compute the entry's offset from head (0..count-1).
    u32 off = (idx + NOTE_QUEUE_DEPTH - q->head) % NOTE_QUEUE_DEPTH;
    // Shift down: each subsequent entry moves one slot toward head.
    // Iterate from `idx` forward (count - off - 1) times.
    u32 cur = idx;
    for (u32 i = off; i + 1 < q->count; i++) {
        u32 next = (cur + 1) % NOTE_QUEUE_DEPTH;
        q->ring[cur] = q->ring[next];
        cur = next;
    }
    // Zero the now-empty tail slot (defense-in-depth: dirty entries
    // can't leak into a peek/dequeue).
    u32 last = (q->head + q->count - 1) % NOTE_QUEUE_DEPTH;
    for (u32 j = 0; j < NOTE_NAME_MAX; j++) q->ring[last].name[j] = 0;
    q->ring[last].arg = 0;
    q->ring[last].sender_pid = 0;
    q->ring[last].timestamp_ns = 0;

    q->count--;
    q->tail = (q->head + q->count) % NOTE_QUEUE_DEPTH;
}

// R2-F1 / R2-F6 audit close: fd-read peek that SKIPS kill. Returns the
// first mask-permitted NON-KILL entry, or 0 if no such entry exists.
// kill stays queued for the dispatcher (which uses notes_peek_locked).
int notes_peek_for_fd_locked(struct Proc *p, struct Thread *t,
                             struct Note *out) {
    if (!p || !p->notes || !out) return 0;
    struct NoteQueue *q = p->notes;
    if (q->count == 0) return 0;

    u32 mask = (t != NULL) ? t->note_mask : 0u;
    u32 idx = q->head;
    for (u32 n = 0; n < q->count; n++) {
        if (notes_name_is_kill(q->ring[idx].name)) {
            idx = (idx + 1) % NOTE_QUEUE_DEPTH;
            continue;
        }
        int bit = notes_name_to_bit(q->ring[idx].name);
        if (bit >= 0 && (mask & (1u << (u32)bit)) == 0) {
            *out = q->ring[idx];
            return 1;
        }
        idx = (idx + 1) % NOTE_QUEUE_DEPTH;
    }
    return 0;
}

// R2-F1 audit close: fd-read dequeue. Skips kill. Used by devnotes_read.
int notes_dequeue_for_fd_locked(struct Proc *p, struct Thread *t,
                                struct Note *out) {
    if (!p || !p->notes || !out) return 0;
    struct NoteQueue *q = p->notes;
    if (q->count == 0) return 0;

    u32 mask = (t != NULL) ? t->note_mask : 0u;
    u32 idx = q->head;
    for (u32 n = 0; n < q->count; n++) {
        if (notes_name_is_kill(q->ring[idx].name)) {
            idx = (idx + 1) % NOTE_QUEUE_DEPTH;
            continue;
        }
        int bit = notes_name_to_bit(q->ring[idx].name);
        if (bit >= 0 && (mask & (1u << (u32)bit)) == 0) {
            *out = q->ring[idx];
            notes_pop_at_locked(q, idx);
            // LS-5c: draining the last queued interrupt un-arms the
            // terminate latch (an inherited-notes-fd reader consumed it).
            notes_drain_intr_locked(p, q, out);
            return 1;
        }
        idx = (idx + 1) % NOTE_QUEUE_DEPTH;
    }
    return 0;
}

int notes_dequeue_locked(struct Proc *p, struct Thread *t, struct Note *out) {
    if (!p || !p->notes || !out) return 0;
    struct NoteQueue *q = p->notes;
    if (q->count == 0) return 0;

    u32 mask = (t != NULL) ? t->note_mask : 0u;

    // F2 audit close: kill bypasses the mask (N-4 non-catchable). First
    // pass scans for kill in FIFO order regardless of mask state.
    {
        u32 idx = q->head;
        for (u32 n = 0; n < q->count; n++) {
            if (notes_name_is_kill(q->ring[idx].name)) {
                *out = q->ring[idx];
                notes_pop_at_locked(q, idx);
                return 1;
            }
            idx = (idx + 1) % NOTE_QUEUE_DEPTH;
        }
    }

    // No kill present; second pass scans for the first mask-permitted
    // entry in FIFO order.
    {
        u32 idx = q->head;
        for (u32 n = 0; n < q->count; n++) {
            int bit = notes_name_to_bit(q->ring[idx].name);
            if (bit >= 0 && (mask & (1u << (u32)bit)) == 0) {
                *out = q->ring[idx];
                notes_pop_at_locked(q, idx);
                // LS-5c: the handler-delivery pop of the last queued
                // interrupt un-arms the terminate latch (normally already
                // clear -- notes_set_handler cleared it at registration --
                // so this is defense-in-depth for the dispatcher pop).
                notes_drain_intr_locked(p, q, out);
                return 1;
            }
            idx = (idx + 1) % NOTE_QUEUE_DEPTH;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Synthetic posters.
// ---------------------------------------------------------------------------

void notes_post_child_exit(struct Proc *parent, int child_pid, int status) {
    if (!parent) return;
    // Pack pid (low 16 bits) + status (low 16 bits) into the 32-bit arg.
    // Truncation is acceptable at v1.0 — pid fits in 16 bits for any
    // realistic concurrent-process count; status is the WIFEXITED low byte
    // plus a flag bit or two. Future wider arg / extended record format
    // is a v1.x extension.
    u32 arg = ((u32)(child_pid & 0xffff) << 16) | (u32)(status & 0xffff);
    (void)notes_post(parent, "child_exit", arg, NULL, true);
}

void notes_post_pipe(struct Proc *writer) {
    if (!writer) return;
    (void)notes_post(writer, "pipe", 0u, NULL, true);
}

// ---------------------------------------------------------------------------
// kill-name predicate (used by the EL0-return-tail dispatch).
// ---------------------------------------------------------------------------

int notes_name_is_kill(const char *name);
int notes_name_is_kill(const char *name) {
    return notes_name_eq(name, NOTE_NAME_KILL);
}

// ---------------------------------------------------------------------------
// EL0-return-tail dispatch — the async-handler delivery path.
// ---------------------------------------------------------------------------
//
// Called from arch/arm64/exception.c::exception_sync_lower_el at the tail
// of every EL0→EL1→EL0 return (after the syscall arm writes regs[0] but
// before vectors.S erets). The kernel holds no locks at this point.
//
// Pops the next deliverable note for the calling Thread, optionally
// terminates the Proc (`kill` non-catchable; LS-5: an uncaught `interrupt`),
// or rewrites ctx to land the async handler. Notes are LEFT QUEUED when:
//   - no handler is registered (the fd-read path is the only consumer) --
//     EXCEPT an uncaught, unmasked `interrupt` on a NON-self-managing Proc,
//     which default-terminates the Proc (LS-5 P2, ARCH 8.8.2: SIGINT's
//     "default = die, catchable"). A self-managing Proc (opened its notes fd)
//     or one with a registered handler is exempt -- it consumes / catches
//     the interrupt itself.
//   - the thread is already in a handler (re-entrancy guard, N-3)
//   - every queued note is in the thread's mask (deferred)

#include "../arch/arm64/exception.h"
#include "../arch/arm64/uaccess.h"

// Forward-declare exits() — defined in kernel/proc.c.
__attribute__((noreturn)) extern void exits(const char *msg);

// LS-5 (P2 default disposition): is there an `interrupt` note in `q`
// deliverable to `t` -- present in the ring AND not masked for `t`? Caller
// MUST hold q->lock. A masked interrupt is NOT deliverable (the program chose
// to defer it), matching the disposition's "NOT masked" clause. The scan (vs.
// just inspecting the dispatcher candidate) is deliberate: a queued interrupt
// behind another unconsumed note must still trigger the terminate -- a non-
// self-managing handler-less Proc never drains its queue, so relying on FIFO
// position would let a leading child_exit/pipe mask the interrupt forever.
// PTY-1b: generalized from the interrupt-only scan -- the first queued
// terminate-class note (interrupt / tty:quit / tty:hup) whose FAMILY mask
// bit is unmasked for `t`, as its canonical .rodata name; NULL if none. A
// masked family's note is NOT deliverable (the program chose to defer it),
// per-family: a thread that masked interrupts still terminates on an
// unmasked tty:hup, and vice versa.
static const char *notes_terminate_pending_name_locked(struct NoteQueue *q,
                                                       struct Thread *t) {
    u32 mask = (t != NULL) ? t->note_mask : 0u;
    u32 idx = q->head;
    for (u32 n = 0; n < q->count; n++) {
        const char *name = q->ring[idx].name;
        if (notes_name_terminate_latch(name) != 0 &&
            (mask & (1u << (u32)notes_name_to_bit(name))) == 0)
            return notes_canonical_name_ptr(name);
        idx = (idx + 1) % NOTE_QUEUE_DEPTH;
    }
    return NULL;
}

// LS-5 P2 (ARCH 8.8.2): would an uncaught `interrupt` default-terminate this
// Proc at the EL0-return tail? True iff (a) no async handler is registered (a
// handler catches interrupt via the async-delivery path), (b) the Proc is not
// self-managing (has not opened its notes fd -- a self-managing Proc consumes
// its own notes via devnotes_read), and (c) a deliverable interrupt (queued
// AND unmasked for `t`) is present. Pure decision; no side effects. The
// dispatcher calls this under q->lock and, on true, drops the lock + calls
// exits(NOTE_NAME_INTERRUPT). Non-static + header-declared so the unit test
// exercises the full decision without driving the noreturn exits(). Caller
// MUST hold p->notes->lock.
int notes_interrupt_should_terminate_locked(struct Proc *p, struct Thread *t) {
    return notes_terminate_note_name_locked(p, t) != NULL;
}

// PTY-1b: the name-returning generalization (interrupt + tty:quit +
// tty:hup). Same gates as the historical interrupt-only decision: a
// registered handler catches EVERY note (the async-delivery path runs it;
// acquire pairs with SYS_NOTIFY's release, F9), and a self-managing Proc
// consumes its own notes -- neither ever default-terminates. The tail
// passes the returned canonical name to exits() so the exit_msg reports
// WHICH signal terminated the Proc.
const char *notes_terminate_note_name_locked(struct Proc *p, struct Thread *t) {
    if (!p || !p->notes) return NULL;
    if (__atomic_load_n(&p->handler_va, __ATOMIC_ACQUIRE) != 0) return NULL;
    if (proc_is_self_managing_notes(p)) return NULL;
    return notes_terminate_pending_name_locked(p->notes, t);
}

// LS-5c: the SYS_NOTIFY body. The handler store and the latch clear run
// under q->lock so they serialize against notes_post's check-handler-then-arm
// (see notes_arm_intr_terminate_locked). The store keeps RELEASE (pairs with
// the dispatcher's acquire, F9) -- q->lock alone would not order it for the
// LOCKLESS handler_va readers.
void notes_set_handler(struct Proc *p, u64 handler_va) {
    if (!p) return;
    struct NoteQueue *q = p->notes;
    if (!q) {       // pre-notes-alloc rollback window; no latch can exist
        __atomic_store_n(&p->handler_va, handler_va, __ATOMIC_RELEASE);
        return;
    }
    spin_lock(&q->lock);
    __atomic_store_n(&p->handler_va, handler_va, __ATOMIC_RELEASE);
    if (handler_va != 0) {
        // Registering a handler changes the disposition to CATCH: a queued
        // terminate-class note now delivers to the handler at the tail.
        // Un-arm BOTH latches (a handler catches every note class).
        // (Unregistering does NOT re-arm -- the unregistering thread reaches
        // its own tail right after this syscall, where LS-5b re-evaluates,
        // and a blocked peer is then covered by the death cascade exits()
        // triggers; see ARCH 8.8.2.)
        __atomic_and_fetch(&p->proc_flags,
                           ~(PROC_FLAG_INTR_TERMINATE_PENDING |
                             PROC_FLAG_TTY_TERMINATE_PENDING),
                           __ATOMIC_RELEASE);
    }
    spin_unlock(&q->lock);
}

// LS-5c: the SYS_NOTE_OPEN tail. The self-managing mark and the latch clear
// run under q->lock, same serialization rationale as notes_set_handler.
void notes_mark_self_managing(struct Proc *p) {
    if (!p) return;
    struct NoteQueue *q = p->notes;
    if (!q) {
        proc_mark_self_managing_notes(p);
        return;
    }
    spin_lock(&q->lock);
    proc_mark_self_managing_notes(p);
    __atomic_and_fetch(&p->proc_flags,
                       ~(PROC_FLAG_INTR_TERMINATE_PENDING |
                         PROC_FLAG_TTY_TERMINATE_PENDING),
                       __ATOMIC_RELEASE);
    spin_unlock(&q->lock);
}

// The widened #811 death predicate (ARCH 8.8.1 + 8.8.2). LOCK-FREE: atomic
// loads of group_exit_msg + proc_flags, and the OWNER-read note_mask (`t` is
// always the calling thread at every site -- sleep/tsleep's register-then-
// observe, torpor's post-register check, the 9P reader's unwind decision --
// and a thread's mask only changes by its own SYS_NOTE_MASK, never while it
// is inside one of those calls). A masked thread reads false from the latch
// leg: it neither unwinds nor terminates until it unmasks (masking defers --
// matching the EL0-return tail, which also applies the observing thread's
// mask, so a latch-woken thread never unwinds into a tail that refuses to
// act). group_exit_msg is checked FIRST and ignores the mask (death is not
// deferrable; N-4).
bool thread_die_pending(struct Thread *t) {
    if (!t) return false;
    // #68 F1: the exit-close window is ORDERLY FINALIZATION, not duress.
    // group_exit_msg is set on EVERY SYS_EXIT_GROUP (a clean exit_group(0)
    // included), and the LS-5 interrupt default-terminate path calls
    // exits() with the terminate LATCH deliberately still armed -- so
    // without this gate the closing thread's dev9p write-behind flush and
    // Tclunk sends short-circuited (client_self_dying), silently dropping
    // staged writes and leaking server-side fids on every normal
    // multi-thread exit AND every Ctrl-C'd default-terminate. The gate
    // suppresses BOTH death legs while set. While the flag is set the
    // closer's sends/waits behave like a live thread's; the wedged-server
    // strand this re-admits is the pre-#68 reap-time exposure RELOCATED
    // from the parent's wait_pid onto the already-dying Proc -- and,
    // unlike the old strand, NOT breakable by a further kill (a wedged
    // flagged close parks the dying Proc unreapably; precondition = a
    // wedged trusted server, an already system-degraded state -- the
    // bounded/abortable close-flush is the recorded v1.x seam). The window
    // is one bounded close pass; only the owning thread sets/clears it
    // (inside proc_close_handles_at_exit), and every caller passes self,
    // so the read needs no synchronization.
    if (t->exit_close_active) return false;
    struct Proc *p = t->proc;
    if (!p) return false;
    if (__atomic_load_n(&p->group_exit_msg, __ATOMIC_ACQUIRE) != NULL)
        return true;
    // kproc never takes the latch leg (RW-0 F3 defense-in-depth): the sole
    // arm site (notes_arm_intr_terminate_locked) already refuses kproc, but
    // a future arm path that forgot the guard would otherwise put every
    // kernel kthread sleep into perpetual *_INTR unwind -- and kproc threads
    // never EL0-return, so the latch could never be consumed.
    if (p != kproc()) {
        u32 flags = __atomic_load_n(&p->proc_flags, __ATOMIC_ACQUIRE);
        // PTY-1b: each terminate latch pairs with ITS OWN family mask bit
        // (interrupt <-> NOTE_BIT_INTERRUPT, the tty:quit/hup latch <->
        // NOTE_BIT_TTY) so a thread that masked one family never spuriously
        // unwinds for the other's pending note -- preserving "a latch-woken
        // thread never unwinds into a tail that refuses to act".
        if ((flags & PROC_FLAG_INTR_TERMINATE_PENDING) != 0 &&
            (t->note_mask & (1u << NOTE_BIT_INTERRUPT)) == 0)
            return true;
        if ((flags & PROC_FLAG_TTY_TERMINATE_PENDING) != 0 &&
            (t->note_mask & (1u << NOTE_BIT_TTY)) == 0)
            return true;
    }
    return false;
}

void notes_deliver_at_el0_return(struct exception_context *ctx);
void notes_deliver_at_el0_return(struct exception_context *ctx) {
    if (!ctx) return;
    struct Thread *t = current_thread();
    if (!t || !t->proc || !t->proc->notes) return;
    struct Proc *p = t->proc;
    struct NoteQueue *q = p->notes;

    // F1 audit close: ctx->sp at this point is the attacker-controllable
    // SP_EL0 the user had at syscall entry. uaccess_store_u8 at EL1
    // does NOT validate user-VA bounds (PAN unconfigured at v1.0), so a
    // userspace process that pre-sets SP to a kernel VA would otherwise
    // get the kernel to write the note name into kernel memory. Reject
    // any ctx->sp that isn't a sane user VA. The bound is intentionally
    // strict -- sp must point well within the user half AND must leave
    // NOTE_NAME_MAX bytes below it for the frame.
    //
    // NB: this validation happens BEFORE the in_handler / queue checks so
    // we cannot miss a delivery due to bound failure on the sp. If sp is
    // bogus AND a kill is queued, the kill stays queued until sp becomes
    // sane -- a misbehaving user can't escape kill, but kill cannot do
    // its work either. That's acceptable: a user with corrupted sp is
    // about to fault anyway.
    if (ctx->sp == 0)                              return;
    if (ctx->sp >= UACCESS_USER_VA_TOP)             return;
    if (ctx->sp < (u64)NOTE_NAME_MAX)               return;

    // F9 audit close: acquire-load handler_va so a multi-thread Proc
    // observes a coherent value vs a concurrent SYS_NOTIFY's store.
    u64 handler_va = __atomic_load_n(&p->handler_va, __ATOMIC_ACQUIRE);

    // Peek under q->lock to identify the dispatcher candidate.
    spin_lock(&q->lock);
    struct Note candidate;
    if (!notes_peek_locked(p, t, &candidate)) {
        spin_unlock(&q->lock);
        return;
    }

    // R2-F2 audit close: kill is non-catchable and MUST bypass the
    // in_handler re-entrancy guard. The prior code checked in_handler
    // BEFORE the kill peek, so a Proc whose handler was running (or
    // stuck) became kill-immune. Fix: check kill FIRST; if kill is
    // present, fall through to the kill branch regardless of in_handler.
    //
    // R2-F7 audit close: pop the kill atomically with the peek (under
    // the same q->lock) so a concurrent consumer cannot race-pop it
    // between peek and pop. We then drop q->lock to acquire
    // proc_table_lock for the live_peers check (q->lock -> proc_table_
    // lock would reverse the established proc_table_lock -> q->lock
    // order). If live_peers > 0, re-enqueue the kill at head; else
    // exits.
    if (notes_name_is_kill(candidate.name)) {
        struct Note kill_popped;
        int kill_got = notes_dequeue_locked(p, t, &kill_popped);
        spin_unlock(&q->lock);
        if (!kill_got) {
            // Defense-in-depth: peek returned kill, dequeue under same
            // lock must return kill too. Reaching here would be a bug.
            return;
        }
        irq_state_t s = proc_table_lock_acquire();
        int live_peers = proc_count_live_peers_locked(p, t);
        proc_table_lock_release(s);
        if (live_peers != 0) {
            // Re-enqueue the kill at head. SYS_POSTNOTE refuses kill to
            // multi-thread Procs at v1.0, so this path is defense-in-
            // depth for a kernel-internal poster (or the R3-F3 TOCTOU
            // race against SYS_THREAD_SPAWN). The kill stays queued;
            // when peer Threads eventually exit on their own, the
            // survivor picks it up.
            //
            // R3-F4 audit close: extinct on re-enqueue failure. Kill
            // loss is a kernel-fatal invariant violation (N-2 + N-4);
            // a silent drop would let the Proc survive a kill that the
            // poster already received "success" for. Better to crash
            // the kernel deliberately so the bug surfaces.
            spin_lock(&q->lock);
            int re_rc = notes_reenqueue_head_locked(q, &kill_popped);
            spin_unlock(&q->lock);
            if (re_rc != 0) {
                extinction("notes_deliver_at_el0_return: re-enqueue of "
                           "kill failed (queue full) -- N-2/N-4 violation; "
                           "kernel-fatal rather than silently lose kill");
            }
            return;
        }
        exits("killed");
        // unreachable
    }

    // N-3 re-entrancy guard: skip non-kill delivery while a handler is
    // running. (R2-F2: kill bypasses this check above.)
    if (t->in_handler) {
        spin_unlock(&q->lock);
        return;
    }

    // No async handler registered.
    if (handler_va == 0) {
        // LS-5 P2 (ARCH 8.8.2): an uncaught `interrupt` default-terminates a
        // NON-self-managing Proc -- SIGINT's "default = die, catchable". A
        // self-managing Proc (opened its notes fd) consumes its own notes via
        // devnotes_read, so it is exempt; a program catches interrupt by
        // registering a handler (handler_va != 0, handled below) or masking it
        // (then it is not "deliverable" here). Only `interrupt` newly default-
        // terminates: other uncaught notes (child_exit / pipe) stay queued for
        // the fd-read path as before, and `kill` (N-4) was handled above.
        const char *tname = notes_terminate_note_name_locked(p, t);
        if (tname) {
            spin_unlock(&q->lock);
            // exits() handles single-thread (-> ZOMBIE, status 1) AND multi-
            // thread (-> the #811 proc_group_terminate cascade); it is
            // noreturn. The terminating note is left in the about-to-be-
            // freed queue -- no consume needed; the Proc is dying. Same
            // terminate primitive the `kill` branch uses (exits("killed")).
            // PTY-1b: tname is the canonical .rodata name (interrupt /
            // tty:quit / tty:hup) -- program-lifetime storage, exits-safe.
            exits(tname);
            // unreachable
        }
        // Otherwise leave the note queued for the fd-read path (a self-managing
        // Proc, or a non-interrupt note). The Proc that did SYS_NOTE_OPEN +
        // read on the fd will see it.
        spin_unlock(&q->lock);
        return;
    }

    // Async-handler delivery. Pop under the SAME q->lock as the peek so
    // the popped note IS the candidate (round-2 audit close M1). Then
    // push under the SAME q->lock so a failing push can re-enqueue at
    // head without losing the note (round-2 close M2 — the F5/F6 race).
    struct Note popped;
    int got = notes_dequeue_locked(p, t, &popped);
    if (!got) {
        // Defense-in-depth: peek returned a candidate, dequeue under the
        // same lock must return the same note. Reaching here would be a
        // queue-mutation bug. Drop the lock and bail.
        spin_unlock(&q->lock);
        return;
    }

    // Compute new_sp under lock. Defense-in-depth on alignment-down.
    u64 new_sp = (ctx->sp - NOTE_NAME_MAX) & ~(u64)0xf;
    if (new_sp == 0 || new_sp >= UACCESS_USER_VA_TOP) {
        (void)notes_reenqueue_head_locked(q, &popped);
        spin_unlock(&q->lock);
        return;
    }

    // Push the note name into the user-stack frame. uaccess_store_u8
    // faults route through p->vma_lock (acyclic with q->lock); buddy is
    // non-blocking at v1.0; so holding q->lock through uaccess is safe.
    // On failure: re-enqueue at head under the same lock (no race — the
    // queue could not have been filled by others during our hold).
    for (u32 i = 0; i < NOTE_NAME_MAX; i++) {
        if (uaccess_store_u8(new_sp + i, (u8)popped.name[i]) != 0) {
            (void)notes_reenqueue_head_locked(q, &popped);
            spin_unlock(&q->lock);
            return;
        }
    }

    spin_unlock(&q->lock);

    // Save the current user context into the Thread (inline cache; the
    // SYS_NOTED(NCONT) restore copies these back).
    for (u32 i = 0; i < 31; i++) {
        t->note_saved_regs[i] = ctx->regs[i];
    }
    t->note_saved_sp_el0 = ctx->sp;
    t->note_saved_elr    = ctx->elr;
    t->note_saved_spsr   = ctx->spsr;

    // Remember the note name for NDFLT's default action (F10 audit close
    // copies this into the Proc's exit_msg_buf on NDFLT path).
    for (u32 i = 0; i < NOTE_NAME_MAX; i++) {
        t->note_handling_name[i] = popped.name[i];
    }

    // Mark the thread in-handler.
    t->in_handler = true;

    // Rewrite ctx to land at handler_va. Plan-9-style handler signature:
    //   void handler(const char *note_name, u32 arg);
    // x0 = note_name VA (user-stack pushed); x1 = note arg.
    ctx->regs[0] = new_sp;
    ctx->regs[1] = (u64)popped.arg;
    ctx->sp      = new_sp;     // sp_el0 saved back to exception frame's sp
    ctx->elr     = handler_va;
    // spsr unchanged — the handler runs at EL0 with the same PSTATE the
    // syscall entered with.
}

// ---------------------------------------------------------------------------
// SYS_NOTED restore + default-action helpers (called from kernel/syscall.c).
// ---------------------------------------------------------------------------

// Restore the saved user context from a Thread's in-handler save block.
// Returns 0 on success, -1 if the Thread is not in a handler. The caller
// (sys_noted_handler) supplies ctx; on success ctx is rewritten with the
// pre-handler user state.
int notes_noted_restore(struct exception_context *ctx, struct Thread *t);
int notes_noted_restore(struct exception_context *ctx, struct Thread *t) {
    if (!ctx || !t) return -1;
    if (!t->in_handler) return -1;

    for (u32 i = 0; i < 31; i++) {
        ctx->regs[i] = t->note_saved_regs[i];
    }
    ctx->sp   = t->note_saved_sp_el0;
    ctx->elr  = t->note_saved_elr;
    ctx->spsr = t->note_saved_spsr;

    // Clear in-handler state. note_handling_name is left intact — it
    // becomes dead state after in_handler clears (no consumer reads it
    // unless in_handler is true).
    t->in_handler = false;
    return 0;
}

// Take the default action for the in-flight note (NDFLT). Calls exits
// with the note name as the status message. Never returns.
__attribute__((noreturn))
void notes_noted_default(struct Thread *t);
__attribute__((noreturn))
void notes_noted_default(struct Thread *t) {
    if (!t || !t->in_handler) {
        // Caller validated in_handler; reaching here is a programmer bug.
        extinction("notes_noted_default: not in handler (caller missed check)");
    }
    // F10 audit close: pass the canonical string-literal pointer to
    // exits — the literal lives in .rodata for the program's lifetime,
    // so p->exit_msg's by-reference capture is safe. The prior code
    // passed t->note_handling_name (a buffer inside struct Thread, freed
    // at thread_free during reap — UAF for any future exit_msg consumer
    // that reads after thread_free + before proc_free).
    const char *msg = notes_canonical_name_ptr(t->note_handling_name);
    if (!msg) {
        // Should be unreachable — note_handling_name was captured from
        // notes_peek_locked's output, which only returns supported-set
        // entries. Defense-in-depth.
        msg = "noted";
    }
    exits(msg);
    // unreachable
}
