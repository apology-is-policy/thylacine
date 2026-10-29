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
#include "../arch/arm64/uaccess.h"   // uaccess_copy_out -- the TEST answer

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

// One attempt, as the owner the op names. Never waits.
static int file_lock_once(struct Proc *p, struct Spoor *c, u32 type,
                          bool proc_owner, u64 start, u64 len,
                          struct t_file_lock *test) {
    if (!proc_owner) return c->dev->lock(c, type, start, len, test);

    struct Spoor *owner = NULL;
    int rc;
    if (test || type == SYS_FILE_LOCK_UN) {
        rc = handle_lock_owner_get(p, c, /*mint=*/false, &owner);
        if (rc != 0) return rc;
        if (!owner && !test) return 0;   // the Proc holds nothing on the file
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
        if (!test) handle_lock_sync_file(p, owner, DEV_LOCK_SYNC_RELEASE);
    } else {
        rc = handle_lock_owner_get(p, c, /*mint=*/true, &owner);
        if (rc != 0) return rc;
    }
    rc = owner->dev->lock(owner, type, start, len, test);
    handle_lock_owner_put(p, owner,
                          rc == 0 && !test && type != SYS_FILE_LOCK_UN);
    return rc;
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
    // The last byte, start + len - 1, must fit in 64 bits; len 0 is "to EOF".
    if (len != 0 && len - 1 > (u64)-1ll - start) return -(s64)T_E_INVAL;

    // POSIX: a read lock needs a descriptor open for reading, a write lock one
    // open for writing. An unlock and a test need neither.
    rights_t need = RIGHT_NONE;
    if (!test && type == SYS_FILE_LOCK_RD) need = RIGHT_READ;
    if (!test && type == SYS_FILE_LOCK_WR) need = RIGHT_WRITE;

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

    // ARCH 8.8.3: F_SETLKW is on signal(7)'s list, so the WAIT of a lock to
    // take is interruptible (syscall_dispatch clears the flag on the way out).
    // An unlock and a test never wait, and stay uninterruptible.
    if (wait && type != SYS_FILE_LOCK_UN) t->note_interruptible = true;

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

    if (rc == 0 && proc_owner && !test && type != SYS_FILE_LOCK_UN &&
        !file_lock_fd_names(p, fd, c)) {
        (void)file_lock_once(p, c, SYS_FILE_LOCK_UN, true, start, len, NULL);
        rc = -T_E_BADF;
    }
    handle_put(&h);

    if (rc == 0 && test &&
        uaccess_copy_out(out_va, &ans, sizeof(ans)) != 0)
        return -(s64)T_E_FAULT;
    return (s64)rc;
}
