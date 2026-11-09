// SYS_FILE_LOCK (B-3h; ARCHITECTURE.md 9.6.11, operator votes 34-35).
//
// The file's server holds every lock and never waits: a conflict answers
// BLOCKED and 9P2000.L leaves the retry to the client. This file is that client
// half -- the op word, the rights, which owner a lock belongs to, and the retry
// loop. The owners: without SYS_FILE_LOCK_PROC the open file (the Spoor's own
// fid: Linux's OFD and flock semantics); with it the Proc (an entry kept with
// the handle table, whose contract is at struct lock_owner_ent in handle.h:
// Linux's POSIX F_SETLK semantics).

#include <thylacine/dev.h>
#include <thylacine/errno.h>
#include <thylacine/filelock.h>
#include <thylacine/handle.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../arch/arm64/timer.h"     // timer_now_ns -- the backoff deadline
#include "../arch/arm64/uaccess.h"   // the TEST answer: its range, its copy-out

// LOCK_WAIT's backoff between attempts: 1 ms, doubling to 64 ms. The server
// cannot tell us when a conflicting lock goes, so the wait is a poll; the cap
// bounds both the latency after a release and the RPC rate of a long wait.
#define FILE_LOCK_BACKOFF_MIN_NS   1000000ull
#define FILE_LOCK_BACKOFF_MAX_NS  64000000ull

static int file_lock_never(void *arg) { (void)arg; return 0; }

// Sleep `ns`. -T_E_INTR when a caught note the call may take, or a death, ends
// it early (a death's result is immaterial: the thread dies at its tail).
static int file_lock_backoff(u64 ns) {
    struct Rendez r;
    rendez_init(&r);
    u64 now = timer_now_ns();
    u64 dl  = now + ns;
    if (dl < now) dl = (u64)-1ll;
    if (dl == 0)  dl = 1;         // 0 is tsleep's "no deadline"
    int ts = tsleep_noteintr(&r, file_lock_never, NULL, dl);
    return (ts == TSLEEP_NOTEINTR || ts == TSLEEP_INTR) ? -T_E_INTR : 0;
}

// Before a take, every open of the file the Proc holds reads what the server
// holds from now on (dev9p: a cached open leaves its snapshot), so the lock
// covers what is read through any of them. c goes first: its descriptor may
// have left the table. The open-file owner learns its file first, as an
// entry's get does, so an open of a dead file sharing its number is left alone.
static int file_lock_acquire_sync(struct Proc *p, struct Spoor *c,
                                  struct Spoor *owner) {
    struct Dev *d = c->dev;
    if (!d->lock_sync) return 0;
    if (owner == c && d->lock_ident) {
        int ir = d->lock_ident(c);
        if (ir != 0) return ir;
    }
    int rc = d->lock_sync(c, DEV_LOCK_SYNC_ACQUIRE);
    if (rc != 0) return rc;
    return handle_lock_sync_file(p, owner, DEV_LOCK_SYNC_ACQUIRE, /*stop_cancelled=*/false);
}

// One attempt, as the owner the op names. Never waits.
static int file_lock_once(struct Proc *p, struct Spoor *c, u32 type,
                          bool proc_owner, u64 start, u64 len,
                          struct t_file_lock *test) {
    bool take = !test && type != SYS_FILE_LOCK_UN;
    if (!proc_owner) {
        // A set-lock gives up what it replaces, and every open's staged writes
        // land first, as Linux's NFS client syncs the whole mapping. c's own land
        // in its lock call: its descriptor may have left the table. A flush a
        // caught note cancelled cancels a take, as in that lock call; an unlock,
        // the re-check's among them, goes ahead. Any other failure is latched for
        // its open to report.
        if (!test) {
            int rr = handle_lock_sync_file(p, c, DEV_LOCK_SYNC_RELEASE, take);
            if (rr != 0) return rr;
        }
        if (take) {
            int sr = file_lock_acquire_sync(p, c, c);
            if (sr != 0) return sr;
        }
        return c->dev->lock(c, type, start, len, test);
    }

    for (int tries = 0; ; tries++) {
        struct Spoor *owner = NULL;
        int rc = handle_lock_owner_get(p, c, /*mint=*/take, &owner);
        if (rc != 0) return rc;
        if (!owner && !test) return 0;   // an unlock: the Proc holds nothing on the file
        if (!owner) {
            // The Proc holds nothing on the file, so any fresh owner asks the
            // question F_GETLK asks: which lock of ANOTHER owner is in the way.
            // Not the open file's own fid: an open-file lock conflicts with a
            // Proc lock even within one Proc.
            int err = -T_E_NOLCK;
            struct Spoor *tmp = c->dev->lock_owner(c, &err);
            if (!tmp) return err;
            rc = tmp->dev->lock(tmp, type, start, len, test);
            spoor_clunk(tmp);
            return rc;
        }
        // A set-lock gives up what it replaces: an unlock its range, a
        // conversion its old type. Every open's staged writes land first, and
        // a flush a caught note cancelled cancels a take.
        if (!test) {
            int rr = handle_lock_sync_file(p, owner, DEV_LOCK_SYNC_RELEASE, take);
            if (rr != 0) {
                handle_lock_owner_put(p, owner, false);
                return rr;
            }
        }
        if (take) {
            int sr = file_lock_acquire_sync(p, c, owner);
            if (sr != 0) {
                handle_lock_owner_put(p, owner, false);
                return sr;
            }
        }
        rc = owner->dev->lock(owner, type, start, len, test);
        if (rc == -T_E_STALE && tries == 0) {
            // The entry's file has died, and its number may now name c's file
            // (a server reuses freed inode numbers), which is how the entry was
            // found for c. Its locks died with it: retire it, and ask again.
            handle_lock_owner_retire(p, owner);
            handle_lock_owner_put(p, owner, false);
            continue;
        }
        handle_lock_owner_put(p, owner, rc == 0 && take);
        return rc;
    }
}

// Does descriptor `fd` still name `c`? Linux re-checks this after a POSIX lock
// is granted (fcntl_setlk): a lock whose descriptor was closed during the call
// would otherwise outlive the close that should have dropped it.
static bool file_lock_fd_names(struct Proc *p, hidx_t fd, struct Spoor *c) {
    struct Handle h;
    if (handle_get(p, fd, &h) < 0) return false;
    bool same = h.kind == KOBJ_SPOOR && h.obj == c;
    handle_put(&h);
    return same;
}

s64 sys_file_lock_handler(u64 fd_raw, u64 op, u64 start, u64 len, u64 out_va) {
    struct Thread *t = current_thread();
    if (!t || !t->proc) return -(s64)T_E_BADF;
    struct Proc *p = t->proc;

    if (op & ~(u64)(SYS_FILE_LOCK_TYPE_MASK | SYS_FILE_LOCK_FLAGS_ALL))
        return -(s64)T_E_INVAL;
    u32  type       = (u32)(op & SYS_FILE_LOCK_TYPE_MASK);
    bool test       = (op & SYS_FILE_LOCK_TEST) != 0;
    bool wait       = (op & SYS_FILE_LOCK_WAIT) != 0;
    bool proc_owner = (op & SYS_FILE_LOCK_PROC) != 0;
    if (type > SYS_FILE_LOCK_UN)                return -(s64)T_E_INVAL;
    // F_GETLK asks about a lock to take, and has nothing to wait for.
    if (test && (type == SYS_FILE_LOCK_UN || wait)) return -(s64)T_E_INVAL;
    // The range's end, start + len, must fit in 64 bits: a server's table holds
    // it as a u64 (Stratum refuses more). len 0 is "to EOF".
    if (len != 0 && len > (u64)-1ll - start) return -(s64)T_E_INVAL;
    // Before any question goes to the server: the answer's copy-out recovers
    // only from a fault in the user half.
    if (test && !uaccess_user_range_ok(out_va, sizeof(struct t_file_lock)))
        return -(s64)T_E_FAULT;

    // POSIX: a read lock needs a descriptor open for reading, a write lock one
    // open for writing. An unlock and a test need neither.
    rights_t need = RIGHT_NONE;
    if (!test && type == SYS_FILE_LOCK_RD) need = RIGHT_READ;
    if (!test && type == SYS_FILE_LOCK_WR) need = RIGHT_WRITE;

    if (fd_raw > 0x7fffffffull)            return -(s64)T_E_BADF;
    hidx_t fd = (hidx_t)fd_raw;
    struct Handle h;
    if (handle_get(p, fd, &h) < 0)         return -(s64)T_E_BADF;
    if (h.kind != KOBJ_SPOOR || (h.rights & need) != need) {
        handle_put(&h);
        return -(s64)T_E_BADF;
    }
    struct Spoor *c = (struct Spoor *)h.obj;
    // A navigation-only (T_OPATH) descriptor is no open file to lock, as
    // Linux's fcntl refuses the lock family on O_PATH with EBADF.
    if (c->flag & CWALKONLY) {
        handle_put(&h);
        return -(s64)T_E_BADF;
    }
    if (!c->dev || !c->dev->lock) {
        handle_put(&h);
        return -(s64)T_E_NOLCK;
    }

    // ARCH 8.8.3: F_SETLKW is on signal(7)'s list. native_note_class makes the
    // WAIT of a lock to take interruptible and restartable, for a Proc whose table
    // can catch a note; an unlock and a test never wait.

    struct t_file_lock ans = { 0 };
    u64 backoff = FILE_LOCK_BACKOFF_MIN_NS;
    int rc;
    for (;;) {
        rc = file_lock_once(p, c, type, proc_owner, start, len,
                            test ? &ans : NULL);
        if (rc != -T_E_AGAIN || !wait) break;
        rc = file_lock_backoff(backoff);
        if (rc != 0) break;
        if (backoff < FILE_LOCK_BACKOFF_MAX_NS) backoff *= 2;
    }

    // The take has its answer, and what follows is a close's work: the unlock a
    // descriptor closed mid-call left owed, and the twin's last close. A close is
    // never interruptible (SYS_CLOSE is unclassed). Here a claim this call still
    // holds would unwind each wait at once, and the Tflush after it can cancel
    // the unlock on the server, leaving the lock held for the Proc's life while
    // the caller reads EBADF. Killable only from here; the mark stays as the loop
    // left it, for the restart.
    t->note_interruptible = false;

    // The owed unlock is answered before the call returns, a shortage of memory
    // only delaying it (the Dev's unlock contract): a program told EBADF may take
    // the range again at once, and an unlock that landed after that take would
    // drop it.
    if (rc == 0 && proc_owner && !test && type != SYS_FILE_LOCK_UN &&
        !file_lock_fd_names(p, fd, c)) {
        (void)file_lock_once(p, c, SYS_FILE_LOCK_UN, true, start, len, NULL);
        rc = -T_E_BADF;
    }
    // The open-file twin: the last descriptor closed during the call, and its
    // close saw no grant. A reference other than this call's can keep c alive
    // indefinitely, and nothing else would release this lock. Any answer: a take
    // that failed with its outcome unknown recorded a grant too, and the close
    // runs again harmlessly on one that took nothing. The first close answered for
    // any run; this one adds no loss to report.
    if (!proc_owner && !test && type != SYS_FILE_LOCK_UN && !spoor_fd_open(c)) {
        if (c->dev->fd_close) (void)c->dev->fd_close(c, /*heard=*/true);
        if (rc == 0) rc = -T_E_BADF;
    }
    handle_put(&h);

    if (rc == 0 && test &&
        uaccess_copy_out(out_va, &ans, sizeof(ans)) != 0)
        return -(s64)T_E_FAULT;
    return (s64)rc;
}
