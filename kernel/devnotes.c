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
#include <thylacine/syscall.h>              // t_stat + T_S_IF* (devnotes_stat_native)
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../arch/arm64/uaccess.h"

// QTFILE — Plan 9 qid type for a regular file. The note Spoor is leaf-
// shaped; no walking, no subdirectories.
#define QTFILE  0x00

// The device character. Named so the vtable and the stat_native guard
// cannot drift apart (the DEVPIPE_DC precedent).
#define DEVNOTES_DC  'n'

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

// #97 (the #96 sibling, found by the same sweep): POSIX fstat on a notes fd.
// devnotes had a 9P .stat -- which returns -1 -- but no .stat_native, so
// SYS_FSTAT fell through spoor_stat_native's NULL-slot arm and also returned
// -1. Fail-closed, but wrong: a caller that treats a non-EBADF fstat failure
// on a standard fd as FATAL (clang does exactly that on fds 0/1/2 -- the CL-4
// layer-3 shape that #96 hit through a pipe) would die on any Proc that had
// put its notes fd there.
//
// Shape: a character device. That is the tree's posture for a device-backed
// unseekable byte stream (cons and pts both present S_IFCHR). Adding this
// slot does NOT make the fd seekable -- .seekable stays false, so lseek and
// pread keep failing ESPIPE; stat_native and seekability were deliberately
// decoupled at RW-4 R2-F2 so exactly this kind of fix cannot smuggle in
// positioned I/O on a stream.
//
// qid_path stays 0 (what dev_simple_attach mints); it is NOT stamped per-open
// the way a pipe's is. Two independent reasons:
//   - The Spoor is a STATELESS marker whose queue is resolved from the running
//     Thread (see the file header) -- it can even be handed to another Proc,
//     where it reads THAT Proc's notes. There is no per-instance object for an
//     inode number to name, and stamping the opener's identity would be
//     actively wrong, since the fd's meaning follows the reader.
//   - Bits 40 and 41 must stay clear. pouch decodes is-a-pts as S_ISCHR + bit
//     40 and is-a-cons as S_ISCHR + bit 41; a stamped qid that happened to set
//     either would make a notes fd read as a terminal. This is the same reason
//     devdev withholds the cons flag from consctl.
// The /dev/tty precedent covers the result: one identity, per-process content.
//
// 0666 SYSTEM matches the world-usable /dev leaves (null/zero/full/random).
// Truthful rather than cosmetic: ANY Proc may open and read its own notes, so
// a tighter mode would become a lie the moment devnotes were perm_enforced.
static int devnotes_stat_native(struct Spoor *c, struct t_stat *out) {
    if (!c || c->dc != DEVNOTES_DC || !out) return -1;
    // Byte-zero first (I-13): every field this fill does not set reaches EL0
    // as a defined zero, with no stack garbage in the pad.
    for (size_t i = 0; i < sizeof(*out); i++) ((u8 *)out)[i] = 0;

    out->mode     = T_S_IFCHR | 0666u;
    out->nlink    = 1;
    out->qid_path = c->qid.path;
    out->qid_vers = c->qid.vers;
    out->qid_type = c->qid.type;
    // Redundant via spoor_stat_native (#100 stamps it after a clean fill), but
    // set here too so a direct vtable call is correct standalone.
    out->devno    = c->devno;
    out->size     = 0;   // a pending-note count would race the reader
    // Not advisory here: devnotes_read REJECTS n < sizeof(struct note_record),
    // so this is the minimum viable read buffer, not a preference.
    out->blksize  = (u32)sizeof(struct note_record);
    out->uid      = PRINCIPAL_SYSTEM;
    out->gid      = GID_SYSTEM;
    return 0;
}

static struct Spoor *devnotes_open(struct Spoor *c, int omode) {
    return dev_simple_open(c, omode);
}

static struct Spoor *devnotes_create(struct Spoor *c, const char *name,
                                       int omode, u32 perm, u32 gid) {
    (void)c; (void)name; (void)omode; (void)perm; (void)gid;
    return NULL;
}

static void devnotes_close(struct Spoor *c) {
    dev_simple_close(c);
}

// F3 audit close: use the poll_waiter_list mechanism for wait/wake — not
// a single-waiter Rendez. The producer (notes_post) already calls
// poll_waiter_list_wake under q->lock; routing devnotes_read through the
// same list breaks the ABBA deadlock cycle the prior single-waiter design
// had with the cond-under-q->lock path. The cond callback now reads only
// pw->ready (no q->lock acquisition under the tsleep g_timerwait + rendez
// locks).
static int devnotes_read_cond(void *arg) {
    const struct poll_waiter *pw = (const struct poll_waiter *)arg;
    // pw->ready is set under list->lock by poll_waiter_list_wake AND the
    // setter calls wakeup(pw->rendez) which acquires/releases the rendez
    // lock that tsleep also holds when calling cond — the pairing of
    // those rendez-lock release-acquires synchronizes the producer's
    // set with this read. Plain load.
    return pw->ready ? 1 : 0;
}

// devnotes_read — pop one note from the calling Proc's queue, copy to
// the user buffer as a struct note_record (32 bytes). Blocks if empty
// (single-record-per-call at v1.0; vectored reads are a v1.x extension).
//
// F3 audit close: blocks via poll_waiter (multi-waiter-safe; ABBA-safe).
// F6 audit close: re-enqueues at head on partial uaccess failure, so
// N-2 (consumed exactly once) holds across a faulting user-VA copy.
// F8 audit close: loops back to tsleep on spurious wake instead of
// returning -1 (which would surface as EIO to userspace).
//
// Returns:
//   32     — one record copied
//    0     — EOF (currently unreachable at v1.0; queue never EOFs)
//   -1     — n < sizeof(struct note_record), or no calling Proc, or
//            persistent uaccess failure on the user buffer
static long devnotes_read(struct Spoor *c, void *buf, long n, s64 off) {
    (void)c; (void)off;
    if (!buf) return -1;
    if (n < (long)sizeof(struct note_record)) return -1;

    struct Thread *t = current_thread();
    if (!t || !t->proc || !t->proc->notes) return -1;
    struct Proc *p = t->proc;
    struct NoteQueue *q = p->notes;

    // Per-read private Rendez + poll_waiter, stack-allocated for the
    // duration of this read call. Lifetime invariant: unregistered
    // before return on every path.
    struct Rendez priv;
    rendez_init(&priv);
    struct poll_waiter pw;
    poll_waiter_init(&pw, &priv);

    for (;;) {
        // Atomic register-then-observe under q->lock: dequeue if a
        // mask-permitted entry exists, else register pw on the list +
        // tsleep on priv. The producer's notes_post calls
        // poll_waiter_list_wake under the SAME q->lock — if it ran
        // before we register, the queue is non-empty and we dequeue;
        // if it ran after, the wake walks our registered pw and sets
        // pw->ready.
        //
        // R2-F1 audit close: uses notes_dequeue_for_fd_locked (the kill-
        // skipping variant). kill is non-catchable; only the EL0-return-
        // tail dispatcher may pop it. A Proc reading /dev/notes must NOT
        // consume its own kill, otherwise SIGKILL is defeated.
        //
        // R2-F8 audit close: `buf` is the kernel scratch buffer from
        // sys_read_for_proc (NOT a user VA). Plain stores; no uaccess;
        // no fault path; no re-enqueue needed.
        spin_lock(&q->lock);
        struct Note popped;
        int got = notes_dequeue_for_fd_locked(p, t, &popped);
        if (got) {
            spin_unlock(&q->lock);
            // Plain kernel-to-kernel byte copy (buf is the sys_read_-
            // for_proc scratch on this Thread's kstack).
            u8 *u = (u8 *)buf;
            const u8 *src = (const u8 *)&popped;
            for (u32 i = 0; i < sizeof(struct note_record); i++) {
                u[i] = src[i];
            }
            return (long)sizeof(struct note_record);
        }

        // R3-F1 audit close: if the queue contains ONLY kill entries
        // (fd-read skipped them all), don't tsleep -- return -1 instead
        // so the syscall returns to user, which triggers the EL0-return-
        // tail dispatcher that DOES consume kill. Without this exit,
        // a Proc parked in devnotes_read on a kill-only queue would be
        // unkillable (N-4 violated -- the kill is enqueued but the only
        // path that delivers it requires the Thread to return to EL0,
        // which it can't do while tsleep'd).
        //
        // Dispatcher peek (`notes_peek_locked` -- kill-first scan)
        // returns kill if any kill is in queue. If so, we unblock the
        // syscall; the user's read() returns -1; the user retries (or
        // exits, or anything else); between syscalls the EL0-return-tail
        // fires and delivers kill.
        struct Note kill_check;
        if (notes_peek_locked(p, t, &kill_check) &&
            notes_name_is_kill(kill_check.name)) {
            spin_unlock(&q->lock);
            return -1;
        }

        // Empty for this Thread's mask. Register pw + tsleep.
        pw.ready = false;       // re-arm for this iteration
        poll_waiter_list_register(&q->poll_list, &pw);
        spin_unlock(&q->lock);

        // tsleep on the private Rendez until pw.ready is set by a
        // producer's poll_waiter_list_wake (which sets pw.ready under
        // list lock then wakes pw.rendez).
        int ts = tsleep(&priv, devnotes_read_cond, &pw, 0u);

        // Unregister; loop and re-attempt dequeue. Spurious wakes
        // (e.g., another consumer drained the queue between our wake
        // and our re-acquire) are absorbed by the loop — devnotes_read
        // doesn't return -1 on empty-after-wake.
        poll_waiter_list_unregister(&pw);

        // #811 (ARCH §8.8.1): death-interrupted -> the Proc is group-
        // terminating. pw is now unregistered (above), so it is safe to
        // return -- the Thread unwinds to its EL0-return die-check. Returning
        // AFTER the unregister is load-bearing: pw is stack-allocated and was
        // listed on the queue's poll_list; leaving it listed would dangle.
        // Do NOT loop (re-tsleep would re-INTR = livelock).
        if (ts == TSLEEP_INTR)
            return -1;
    }
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
// iff the queue has at least one mask-permitted non-kill readable note.
//
// Register-then-observe per the P5-poll audit-bearing discipline (the
// poll.h preamble + specs/poll.tla).
//
// R2-F6 audit close: uses notes_peek_for_fd_locked (the kill-skipping
// variant). A queued kill must NOT advertise POLLIN — fd-read consumers
// can't see kill (R2-F1), and advertising POLLIN on kill would draw them
// into a read() that returns nothing, looking like a phantom event.
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
        if (notes_peek_for_fd_locked(p, t, &tmp)) {
            revents |= POLLIN;
        }
    }
    // R4-F2 audit close: parallel to R3-F1 (read). If the queue contains
    // ONLY kill (fd-variant peek skips it), poll must NOT block silently
    // -- it would never wake. Report POLLERR so the caller's poll()
    // returns immediately; on the next EL0-return the dispatcher fires
    // and delivers kill. Without this, a Proc parked in SYS_POLL on
    // /dev/notes with a kill-only queue would sleep forever (R3-F1's
    // analog for poll).
    if (revents == 0) {
        struct Note kill_check;
        if (notes_peek_locked(p, t, &kill_check) &&
            notes_name_is_kill(kill_check.name)) {
            revents |= POLLERR;
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
    .dc       = DEVNOTES_DC,
    .name     = "notes",

    .reset    = devnotes_reset,
    .init     = devnotes_init,
    .shutdown = devnotes_shutdown,

    .attach   = devnotes_attach,
    .walk     = devnotes_walk,
    .stat     = devnotes_stat,
    .stat_native = devnotes_stat_native,   // #97

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
