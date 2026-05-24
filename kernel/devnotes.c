// P6-pouch-signals-impl (sub-chunk 13a): /dev/notes — the fd-shaped view
// of a Proc's note queue. The NOVEL.md §3.1 totalization in concrete form.
//
// Per ARCH §7.6.1 (binding design at 237f096). The Spoor is a stateless
// "this fd reads notes" marker — the actual queue is always the CALLING
// Proc's (`current_thread()->proc->notes`). Multiple opens in the same
// Proc share the queue; each open mints a fresh Spoor / handle but reads
// pop from the same per-Proc queue.
//
// If a devnotes Spoor ends up in another Proc's handle table (via
// SYS_DUP or SYS_SPAWN_WITH_FDS), reading from it accesses the NEW Proc's
// queue — not the opener's. The Spoor carries no per-instance binding;
// the queue identity is dynamic ("read MY notes" via the running Thread).
// This is by design — there's no information leak or UAF risk because the
// Spoor doesn't pin any specific queue; the queue lifetime is owned by
// the Proc that holds it.

#include <thylacine/dev.h>
#include <thylacine/notes.h>
#include <thylacine/poll.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../arch/arm64/uaccess.h"

// QTFILE — Plan 9 qid type for a regular file. The note Spoor is leaf-
// shaped; no walking, no subdirectories.
#define QTFILE  0x00

static void devnotes_reset(void)    { /* no-op */ }
static void devnotes_init(void)     { /* no-op */ }
static void devnotes_shutdown(void) { /* no-op */ }

static struct Spoor *devnotes_attach(const char *spec) {
    (void)spec;
    return dev_simple_attach(&devnotes, QTFILE);
}

static struct Walkqid *devnotes_walk(struct Spoor *c, struct Spoor *nc,
                                     const char **name, int nname) {
    (void)c; (void)nc; (void)name; (void)nname;
    return NULL;            // leaf Dev — no walking
}

static int devnotes_stat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devnotes_open(struct Spoor *c, int omode) {
    return dev_simple_open(c, omode);
}

static void devnotes_create(struct Spoor *c, const char *name,
                            int omode, u32 perm) {
    (void)c; (void)name; (void)omode; (void)perm;
}

static void devnotes_close(struct Spoor *c) {
    dev_simple_close(c);
}

// devnotes_read_cond — tsleep predicate. Returns 1 iff the queue has a
// note that the calling Thread can dequeue (NOTE_BIT_* not in its mask).
// Re-evaluated under p->notes->lock on every wake (tsleep takes the
// Rendez's own lock; the cond is then a peek under the queue lock).
//
// `arg` is the calling Thread (captured at devnotes_read entry — the
// Thread's mask is the gate). The Proc is `arg->proc`.
struct devnotes_read_cond_arg {
    struct Proc   *proc;
    struct Thread *thread;
};

static int devnotes_read_cond(void *arg) {
    struct devnotes_read_cond_arg *a = (struct devnotes_read_cond_arg *)arg;
    if (!a || !a->proc || !a->proc->notes) return 0;
    struct NoteQueue *q = a->proc->notes;

    // We're called under q->waiters.lock by tsleep; we need q->lock to
    // peek the queue safely. Take it; the lock-order rule is q->lock
    // first (the producer takes q->lock then drops it before wakeup which
    // takes waiters.lock — consumer takes them in the reverse order with
    // a try-style backoff if we needed nested holds; we DON'T nest here
    // because we only need a moment of q->lock).
    //
    // This works because rendez.sleep's cond callback is allowed to take
    // unrelated locks AS LONG AS the global lock-order discipline holds.
    // We do NOT take waiters.lock here (we'd deadlock on tsleep's own
    // hold); we only take q->lock, which is acquired/released cleanly.
    spin_lock(&q->lock);
    struct Note tmp;
    int has = notes_peek_locked(a->proc, a->thread, &tmp);
    spin_unlock(&q->lock);
    return has;
}

// devnotes_read — pop one note from the calling Proc's queue, copy to
// the user buffer as a struct note_record (32 bytes). Blocks if empty
// (single-record-per-call at v1.0; vectored reads are a v1.x extension).
//
// Returns:
//   32     — one record copied
//    0     — EOF (currently unreachable at v1.0; queue never EOFs)
//   -1     — n < sizeof(struct note_record), or no calling Proc, or
//            other validation failure
static long devnotes_read(struct Spoor *c, void *buf, long n, s64 off) {
    (void)c; (void)off;
    if (!buf) return -1;
    if (n < (long)sizeof(struct note_record)) return -1;

    struct Thread *t = current_thread();
    if (!t || !t->proc || !t->proc->notes) return -1;
    struct Proc *p = t->proc;
    struct NoteQueue *q = p->notes;

    // Block until a dequeueable note is present (mask-permitted).
    struct devnotes_read_cond_arg a = { .proc = p, .thread = t };
    int rc = tsleep(&q->waiters, devnotes_read_cond, &a, 0u);
    // tsleep with deadline_ns = 0 = "no deadline"; returns TSLEEP_AWOKEN.
    (void)rc;     // single-waiter is structurally guaranteed by v1.0
                  // model; tsleep only returns AWOKEN here.

    // Dequeue under q->lock.
    spin_lock(&q->lock);
    struct Note popped;
    int got = notes_dequeue_locked(p, t, &popped);
    spin_unlock(&q->lock);

    if (!got) {
        // Spurious wake (race with another consumer of the same queue,
        // e.g., the EL0-return-tail handler dispatch grabbed it first).
        // v1.0 returns -1 + leaves the user buffer untouched; userspace
        // re-issues the read (matches non-blocking semantics that callers
        // already expect from a Rendez-based wait/wake).
        return -1;
    }

    // memcpy popped → user buffer. note_record and Note are byte-
    // identical (see _Static_assert in notes.h). We bounce through a
    // local copy to avoid handing the user a raw pointer into the
    // kernel ring; uaccess_store_u8 walks the user-VA byte-by-byte
    // with fault fixup (the standard pattern for KOBJ_SPOOR read paths).
    u8 *u = (u8 *)buf;
    const u8 *src = (const u8 *)&popped;
    for (u32 i = 0; i < sizeof(struct note_record); i++) {
        if (uaccess_store_u8((u64)(uintptr_t)(u + i), src[i]) != 0) {
            // Partial write fault. We've already popped — the note is
            // GONE. v1.0 accepts the data loss (matches our docs at
            // ARCH §7.6.5 "queue overflow / consume policy"); a future
            // chunk could buffer + retry. Return -1.
            return -1;
        }
    }

    return (long)sizeof(struct note_record);
}

static struct Block *devnotes_bread(struct Spoor *c, long n, s64 off) {
    (void)c; (void)n; (void)off;
    return NULL;
}

static long devnotes_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)c; (void)buf; (void)n; (void)off;
    return -1;     // devnotes is read-only — posting is via SYS_POSTNOTE
}

static long devnotes_bwrite(struct Spoor *c, struct Block *bp, s64 off) {
    (void)c; (void)bp; (void)off;
    return -1;
}

// devnotes_poll — register the calling thread's poll_waiter on the queue's
// poll_list AND sample current readiness, atomically under q->lock. POLLIN
// iff the queue has at least one mask-permitted readable note.
//
// Register-then-observe per the P5-poll audit-bearing discipline (the
// poll.h preamble + specs/poll.tla).
static short devnotes_poll(struct Spoor *c, short events,
                           struct poll_waiter *pw) {
    (void)c;
    struct Thread *t = current_thread();
    if (!t || !t->proc || !t->proc->notes) return POLLERR;
    struct Proc *p = t->proc;
    struct NoteQueue *q = p->notes;

    short revents = 0;

    spin_lock(&q->lock);
    if (events & POLLIN) {
        struct Note tmp;
        if (notes_peek_locked(p, t, &tmp)) {
            revents |= POLLIN;
        }
    }
    // POLLOUT on devnotes is N/A (read-only); silently absent — the
    // poll caller observes "not ready for write," which is the
    // POSIX-correct answer for a read-only file.

    if (pw != NULL) {
        poll_waiter_list_register(&q->poll_list, pw);
    }
    spin_unlock(&q->lock);

    return revents;
}

static void devnotes_remove(struct Spoor *c) {
    (void)c;
}

static int devnotes_wstat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devnotes_power(struct Spoor *c, int on) {
    (void)c; (void)on;
    return NULL;
}

struct Dev devnotes = {
    .dc       = 'n',
    .name     = "notes",

    .reset    = devnotes_reset,
    .init     = devnotes_init,
    .shutdown = devnotes_shutdown,

    .attach   = devnotes_attach,
    .walk     = devnotes_walk,
    .stat     = devnotes_stat,

    .open     = devnotes_open,
    .create   = devnotes_create,
    .close    = devnotes_close,

    .read     = devnotes_read,
    .bread    = devnotes_bread,
    .write    = devnotes_write,
    .bwrite   = devnotes_bwrite,

    .poll     = devnotes_poll,

    .remove   = devnotes_remove,
    .wstat    = devnotes_wstat,
    .power    = devnotes_power,
};
