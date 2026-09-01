// Kernel pipe — connected Spoor pair over a shared ring buffer
// (P5-pipe-blocking; .poll added at P5-poll-a).
//
// Per ARCH §10.3 + docs/reference/51-pipe.md. The ring is a fixed-size
// PIPE_BUF_SIZE buffer (4 KiB; POSIX PIPE_BUF guarantee) with separate
// head / tail / count fields. Two Spoors point at the same ring via
// per-endpoint `struct pipe_endpoint` aux records; the Dev vtable
// dispatches based on each endpoint's `is_read_end` flag.
//
// Wait/wake: ONE mechanism for pollers and blockers alike. The ring's
// `poll_list` is the multi-waiter callback set (specs/poll.tla): devpipe_poll
// registers a poller's hook on it, and a blocking read/write registers a
// per-call hook of its own (pipe_block_locked below), each hook carrying the
// waiting thread's private stack Rendez. Every readiness mutation (data
// arrival, drain, either EOF) walks the list and wakes EVERY hook; a woken
// blocker re-samples the ring and sleeps again if another waiter got there
// first (specs/pipe.tla NoStuckReader / NoStuckWriter, multi-waiter form).
//
// Why not a Rendez per direction on the ring: `sleep()` is single-waiter and
// EXTINCTS on a second sleeper (sched.c). The ring's two Rendez were sound
// while a pipe had one reader and one writer -- the in-kernel uses of
// P5-pipe-blocking -- but pipe2 (#155), fork (LINEAGE) and CLONE_THREAD (N-3)
// made every endpoint a Spoor that many EL0 threads and Procs hold at once,
// so two children blocked in write() on one full pipe (`make -j 2>&1 | tee`)
// or two workers blocked in read() on one empty pipe (a jobserver, a prefork
// pool) reached the second-sleeper extinction from unprivileged code.

#include <thylacine/dev.h>
#include <thylacine/errno.h>
#include <thylacine/extinction.h>
#include <thylacine/notes.h>
#include <thylacine/pipe.h>
#include <thylacine/poll.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>   // main#96 / aux#148: struct t_stat + T_S_IFIFO
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../mm/slub.h"

// =============================================================================
// Internal types.
// =============================================================================

struct pipe_ring {
    u32                       magic;          // PIPE_RING_MAGIC
    int                       ref;            // 2 at creation; per-endpoint close drops by 1
    size_t                    count;          // bytes in buffer; 0..PIPE_BUF_SIZE
    size_t                    head;           // next write position; mod PIPE_BUF_SIZE
    size_t                    tail;           // next read position; mod PIPE_BUF_SIZE
    bool                      read_eof;       // read end closed → writes return -T_E_PIPE
    bool                      write_eof;      // write end closed → reads return 0 (EOF)
    spin_lock_t               lock;           // protects count/head/tail/{read,write}_eof
    struct poll_waiter_list   poll_list;      // every waiter: pollers AND blocked readers/writers
    u8                        buf[PIPE_BUF_SIZE];
};

struct pipe_endpoint {
    u32                magic;       // PIPE_ENDPOINT_MAGIC
    struct pipe_ring  *ring;
    bool               is_read_end;
};

// 56 bytes header — derived layout (was 88 while the ring carried a Rendez
// per direction; the two 16-byte Rendez left with the multi-waiter rewrite):
//   offset  0:  u32  magic           (4)
//   offset  4:  int  ref             (4)
//   offset  8:  size_t count         (8)
//   offset 16:  size_t head          (8)
//   offset 24:  size_t tail          (8)
//   offset 32:  bool read_eof        (1)
//   offset 33:  bool write_eof       (1)
//   offset 34-35: pad
//   offset 36:  spin_lock_t lock     (4; u32)
//   offset 40:  struct poll_waiter_list poll_list  (16: spin_lock+pad+ptr)
//   offset 56:  u8 buf[PIPE_BUF_SIZE]
_Static_assert(sizeof(struct pipe_ring) == 56 + PIPE_BUF_SIZE,
               "pipe_ring size pinned (56-byte header + 4 KiB buf)");

// The ring is ~4 KiB — above the SLUB max-object threshold, so kmalloc
// routes it through alloc_pages (same path the p9_client uses). The
// endpoint is tiny (16 bytes); we use a SLUB cache for it to keep the
// per-pipe-create allocation footprint tight.
static struct kmem_cache *g_endpoint_cache;
static u64                g_pipe_allocated;
static u64                g_pipe_freed;
static bool               g_pipe_initialized;
// #96: monotonic pipe identity, stamped into BOTH ends' qid.path so fstat can
// report a distinct (and stable) inode per pipe. Starts at 1 -- 0 is the
// historical "unset" value every pipe carried, so leaving it unused keeps a
// stale-qid read distinguishable from a real one.
static u64                g_pipe_next_qid = 1;

// =============================================================================
// Ring buffer ops.
// =============================================================================

static long ring_write(struct pipe_ring *r, const u8 *buf, long len) {
    if (!r || r->magic != PIPE_RING_MAGIC) return -1;
    if (len <= 0) return 0;

    size_t avail = PIPE_BUF_SIZE - r->count;
    size_t to_write = ((size_t)len < avail) ? (size_t)len : avail;
    if (to_write == 0) return 0;

    // Two-segment copy: from head to end-of-buf, then wrap.
    size_t first = PIPE_BUF_SIZE - r->head;
    if (first > to_write) first = to_write;
    for (size_t i = 0; i < first; i++) {
        r->buf[r->head + i] = buf[i];
    }
    size_t second = to_write - first;
    for (size_t i = 0; i < second; i++) {
        r->buf[i] = buf[first + i];
    }

    r->head = (r->head + to_write) % PIPE_BUF_SIZE;
    r->count += to_write;
    return (long)to_write;
}

static long ring_read(struct pipe_ring *r, u8 *buf, long len) {
    if (!r || r->magic != PIPE_RING_MAGIC) return -1;
    if (len <= 0) return 0;

    size_t avail = r->count;
    size_t to_read = ((size_t)len < avail) ? (size_t)len : avail;
    if (to_read == 0) return 0;

    size_t first = PIPE_BUF_SIZE - r->tail;
    if (first > to_read) first = to_read;
    for (size_t i = 0; i < first; i++) {
        buf[i] = r->buf[r->tail + i];
    }
    size_t second = to_read - first;
    for (size_t i = 0; i < second; i++) {
        buf[first + i] = r->buf[i];
    }

    r->tail = (r->tail + to_read) % PIPE_BUF_SIZE;
    r->count -= to_read;
    return (long)to_read;
}

// =============================================================================
// Dev vtable.
// =============================================================================

static struct pipe_endpoint *priv_of(struct Spoor *c) {
    if (!c) return NULL;
    if (!c->aux) return NULL;
    struct pipe_endpoint *p = (struct pipe_endpoint *)c->aux;
    if (p->magic != PIPE_ENDPOINT_MAGIC) {
        extinction("pipe: corrupted endpoint magic (use-after-free?)");
    }
    return p;
}

static void devpipe_reset(void)    { /* no-op */ }
static void devpipe_shutdown(void) { /* no-op */ }
static void devpipe_init_noop(void) { /* registration via pipe_init */ }

static struct Spoor *devpipe_attach(const char *spec) {
    (void)spec;
    // attach via spec is not how pipes are constructed; tests + kernel
    // callers use pipe_create() directly. Plan 9's `/srv` posting story
    // (where a server creates a named pipe and clients walk to it) is
    // Phase 5+ once the syscall surface lands.
    return NULL;
}

static struct Walkqid *devpipe_walk(struct Spoor *c, struct Spoor *nc,
                                    const char **name, int nname) {
    (void)c; (void)nc; (void)name; (void)nname;
    return NULL;
}

static int devpipe_stat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

// #96 -- SYS_FSTAT on a pipe fd. POSIX requires fstat(2) on a pipe to SUCCEED
// and report S_IFIFO; before this, devpipe had no .stat_native at all, so
// spoor_stat_native returned -1 for every pipe.
//
// That is not cosmetic. clang's FixupStandardFileDescriptors fstats fds 0/1/2
// at startup and treats a non-EBADF failure as FATAL -- the CL-4 masking layer
// 3, already fixed once for the console and once for /dev. The pipe was the
// THIRD door, and nothing had ever opened it because no pouch program had had
// a pipe on a standard fd at startup until the CL-5 build storm: GNU make gives
// the real stdin to only ONE job and hands every concurrent sibling the read
// end of a broken pipe (get_bad_stdin), so `make -j4` had job 1 succeed and its
// siblings die silently. Any program in a shell pipeline is on this path.
//
// Both ends of one pipe share the ring's id as qid.path (the POSIX convention:
// one pipe, one inode), so fstat can tell two distinct pipes apart -- a
// same-inode-for-every-pipe report is the kind of latent wrong answer that
// surfaces much later inside someone's file-identity comparison.
//
// The LINEAGE branch hit the same missing slot independently (aux#148) through
// a different door, which is worth recording because it says the gap was not
// clang-specific: `viv` decides whether to endow a container's stdio by
// fstat'ing its own 0/1/2, and joey hands viv a PIPE to capture the container's
// output -- so every fstat failed and an Alpine shell was spawned fd-less while
// the gate reported "the shell never ran". Two unrelated consumers, one absent
// vtable slot; assume a third exists.
static int devpipe_stat_native(struct Spoor *c, struct t_stat *out) {
    if (!c || c->dc != DEVPIPE_DC || !out) return -1;
    for (size_t i = 0; i < sizeof(*out); i++) ((u8 *)out)[i] = 0;
    // 0600: a pipe is readable+writable by its owner and reachable by no one
    // else -- it has no name in any namespace, so group/other bits are moot.
    out->mode     = T_S_IFIFO | 0600u;
    out->nlink    = 1;
    out->qid_path = c->qid.path;
    out->qid_vers = c->qid.vers;
    out->qid_type = c->qid.type;
    out->devno    = c->devno;
    // A pipe has no length: POSIX leaves st_size unspecified for FIFOs and
    // Linux reports 0. Reporting the buffered byte count would invite a caller
    // to size a read against it, which races the peer by construction.
    out->size     = 0;
    out->blksize  = PIPE_BUF_SIZE;
    out->uid      = PRINCIPAL_SYSTEM;
    out->gid      = GID_SYSTEM;
    return 0;
}

static struct Spoor *devpipe_open(struct Spoor *c, int omode) {
    if (!c) return NULL;
    c->flag |= 0x01;            // COPEN
    c->mode  = omode;
    return c;
}

static struct Spoor *devpipe_create(struct Spoor *c, const char *name, int omode, u32 perm, u32 gid) {
    (void)c; (void)name; (void)omode; (void)perm; (void)gid;
    return NULL;
}

// The blocker's sleep condition: its own hook was walked by a wake. `ready`
// is written under poll_list->lock by poll_waiter_list_wake, which then calls
// wakeup() on the hook's Rendez -- the release/acquire pair on that Rendez
// lock is what orders the store before this read, which sleep() evaluates
// under the same lock (the devnotes_read discipline). Plain load.
static int pipe_waiter_ready(void *arg) {
    const struct poll_waiter *pw = (const struct poll_waiter *)arg;
    return pw->ready ? 1 : 0;
}

// Block until the ring's poll_list is walked. ENTERED WITH r->lock HELD, and
// that is the whole protocol: the caller sampled the ring under the lock and
// found it not ready, so registering the hook in the SAME hold makes the
// sample + the registration one atomic step (register-then-observe,
// specs/poll.tla) -- a mutation cannot land between them, and every mutation
// walks the list after it lands, so the hook is either registered before the
// walk (woken) or registered after a sample that already saw the mutation.
// Drops r->lock, sleeps on a stack Rendez private to this call (one sleeper
// per Rendez by construction, however many threads share the pipe),
// unregisters on every exit, and returns sleep()'s verdict: SLEEP_OK means
// "re-sample" (another waiter may have consumed the edge), SLEEP_INTR means
// the Proc is group-terminating and the caller unwinds (#811). The list lock
// nests inside the ring lock (poll.h: object -> list), the Rendez lock inside
// neither; a hook never outlives the call (NoStaleHook).
static int pipe_block_locked(struct pipe_ring *r) {
    struct Rendez      priv;
    struct poll_waiter pw;
    rendez_init(&priv);
    poll_waiter_init(&pw, &priv);
    poll_waiter_list_register(&r->poll_list, &pw);
    spin_unlock(&r->lock);
    int rc = sleep(&priv, pipe_waiter_ready, &pw);
    poll_waiter_list_unregister(&pw);
    return rc;
}

static void devpipe_close(struct Spoor *c) {
    struct pipe_endpoint *p = priv_of(c);
    if (!p) return;
    struct pipe_ring *r = p->ring;
    if (!r || r->magic != PIPE_RING_MAGIC) {
        extinction("pipe: close on endpoint with corrupted ring");
    }

    // EOF propagation: closing the read end sets read_eof (every sleeping
    // writer returns -EPIPE); closing the write end sets write_eof (every
    // sleeping reader returns 0). Per specs/pipe.tla CloseRead / CloseWrite
    // + their buggy variants -- the wake is REQUIRED for missed-wakeup-
    // freedom, and it is the ONE wake: the flag is set under r->lock, the
    // lock is dropped, and poll_waiter_list_wake walks every hook on the
    // ring -- pollers (the surviving end is now POLLHUP- / POLLERR-ready)
    // and blocked readers/writers alike; each re-samples after the wake.
    spin_lock(&r->lock);
    if (p->is_read_end) r->read_eof  = true;
    else                r->write_eof = true;
    spin_unlock(&r->lock);
    poll_waiter_list_wake(&r->poll_list);

    // Drop this endpoint's ring ref. When both endpoints have been
    // closed, the ring is freed.
    //
    // R15 F234 close: atomic decrement under ACQ_REL ordering. Without
    // atomics, concurrent close of two endpoints on two CPUs would
    // race on r->ref → lost-update or both-see-zero hazards. fetch_sub
    // returns PRE; pre == 1 means we were the last endpoint (post == 0)
    // and own the free. pre <= 0 is the underflow diagnostic case.
    int pre = __atomic_fetch_sub(&r->ref, 1, __ATOMIC_ACQ_REL);
    if (pre <= 0) {
        extinction("pipe: ring refcount underflow");
    }
    if (pre == 1) {
        r->magic = 0;       // UAF defense — readers see magic clobber
        kfree(r);
        __atomic_fetch_add(&g_pipe_freed, 1u, __ATOMIC_RELAXED);
    }
    // Free the endpoint priv. The Spoor's aux is now dangling — caller
    // must not dereference c after spoor_clunk returns, which spoor.c
    // documents as the contract.
    p->magic = 0;
    kmem_cache_free(g_endpoint_cache, p);
    c->aux = NULL;
}

static long devpipe_read(struct Spoor *c, void *buf, long n, s64 off) {
    (void)off;
    struct pipe_endpoint *p = priv_of(c);
    // #100 (ER-3): see the devpipe_write twin for why `!p` stays a flat -1.
    if (!p)                      return -1;
    if (!p->is_read_end)         return -T_E_BADF;   // wrong end
    if (n < 0 || !buf)           return -T_E_INVAL;
    if (!p->ring)                return -T_E_BADF;   // torn-down endpoint
    struct pipe_ring *r = p->ring;

    // A 0-length read is a no-op -> 0 (POSIX), BEFORE any block decision: an
    // empty pipe must not sleep (blocking) or return EAGAIN (CNONBLOCK) for a
    // read that wanted no bytes.
    if (n == 0) return 0;

    // Blocking read. Loop:
    //   - take lock; if data -> drain + drop lock + wake every waiter + return.
    //   - if writeEof + empty -> drop lock + return 0 (EOF).
    //   - else -> register on poll_list in the same hold + sleep; re-sample.
    //
    // The register-then-observe step + the per-call Rendez make the protocol
    // miss-wakeup-free for ANY number of readers (specs/pipe.tla
    // NoStuckReader, multi-waiter form; scheduler.tla NoMissedWakeup at the
    // rendez layer). A reader woken after a peer drained the bytes simply
    // finds count == 0 again and sleeps again.
    for (;;) {
        spin_lock(&r->lock);
        if (r->count > 0) {
            long got = ring_read(r, (u8 *)buf, n);
            spin_unlock(&r->lock);
            if (got > 0) {
                // A drained ring may have just become writable: wake every
                // hook -- a writer blocked on full, a poller on the write
                // end; each re-samples and filters.
                poll_waiter_list_wake(&r->poll_list);
            }
            return got;
        }
        if (r->write_eof) {
            spin_unlock(&r->lock);
            return 0;       // EOF
        }
        // #811 (ARCH §8.8.1): a death-interrupted sleep means the Proc is
        // group-terminating -- return so the Thread unwinds to its EL0-return
        // die-check (re-looping would re-register + re-INTR = livelock).
        // item 11 note (ARCH §8.8.3): this read is NOT yet caught-note-
        // interruptible. 11b-core lands the MECHANISM only; opting a read into
        // sleep_noteintr (returning -T_E_INTR on a queued caught note) is
        // deferred to 11c, which lands it TOGETHER with the native/phenotype
        // EINTR handling -- a native reader (libthyla-rs) is not EINTR-aware, so
        // returning EINTR here before that handling exists breaks it (e.g. the
        // ut shell's `$(cmd)` capture read, interrupted by the captured child's
        // own child_exit note). See design_caught_notes_do_not_interrupt_waits.
        // O_NONBLOCK (CNONBLOCK): the pipe is empty and not at EOF -- a blocking
        // read would sleep here, so a non-blocking read returns EAGAIN instead.
        // Placed AFTER the count>0 and write_eof checks so a non-blocking read
        // still drains available data and still returns 0 at EOF; it converts
        // ONLY the would-block case. It never registers a hook, so the I-9
        // wait/wake protocol (pipe.tla NoStuckReader) is untouched. `flag` is
        // an atomic read -- it is RMW'd from other lock domains (see spoor.h).
        if (spoor_flag_get(c) & CNONBLOCK) {
            spin_unlock(&r->lock);
            return -T_E_AGAIN;
        }
        // Registered under the lock we still hold; returns with it dropped.
        if (pipe_block_locked(r) == SLEEP_INTR)
            return -1;
        // Loop: re-sample with the lock held.
    }
}

static long devpipe_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)off;
    struct pipe_endpoint *p = priv_of(c);
    // #100 (ER-3): the local rejects name their reason. `!p` stays a flat -1
    // for the reason ERRORS.md gives the !t/!p preamble guards -- it is an
    // internal invariant violation (a Spoor with a NULL aux), not a caller
    // error, and is unreachable from EL0.
    if (!p)                      return -1;
    if (p->is_read_end)          return -T_E_BADF;   // wrong end
    if (n < 0 || !buf)           return -T_E_INVAL;
    if (!p->ring)                return -T_E_BADF;   // torn-down endpoint
    struct pipe_ring *r = p->ring;

    // A 0-length write is a no-op -> 0 (POSIX), BEFORE any block/EPIPE decision.
    // The 9P transport (CNBFRAME below) only ever writes whole frames (>= 7 B),
    // so this never intercepts a transport write.
    if (n == 0) return 0;

    // CNBFRAME (the byte-pipe 9P transport's tx end): frame-atomic + non-
    // blocking. Commit the WHOLE buffer or return -T_E_AGAIN having written
    // NOTHING -- never partial (a stranded 9P-frame fragment desyncs the
    // shared stream; do_send treats a mid-frame EAGAIN as fatal, #349), never
    // sleep (a blocking write under the 9P client's held c->lock is the #360
    // lock-across-sleep extinction -- the round-B F1 defect). The caller
    // (client_send_flow) recovers from -T_E_AGAIN == P9_TRANSPORT_EAGAIN by
    // dropping c->lock in client_pump_or_park_locked; a frame <= msize <=
    // PIPE_BUF_SIZE always fits an empty pipe, so progress is guaranteed once
    // the reader drains. Same read_eof -> pipe-note + -T_E_PIPE as the loop.
    // Lockless read of `flag` (a fork-shared write end can be fcntl'd O_NONBLOCK
    // concurrently -- see spoor.h; CNBFRAME itself is transport-tx-only).
    if (spoor_flag_get(c) & CNBFRAME) {
        spin_lock(&r->lock);
        if (r->read_eof) {
            spin_unlock(&r->lock);
            struct Thread *t = current_thread();
            if (t && t->proc) notes_post_pipe(t->proc);
            return -T_E_PIPE;
        }
        if ((long)(PIPE_BUF_SIZE - r->count) >= n) {
            long put = ring_write(r, (const u8 *)buf, n);   // == n (it fits)
            spin_unlock(&r->lock);
            if (put > 0) poll_waiter_list_wake(&r->poll_list);
            return put;
        }
        spin_unlock(&r->lock);
        return -T_E_AGAIN;                                  // whole frame won't fit
    }

    // Blocking write. Loop:
    //   - take lock; if readEof -> drop lock + return -T_E_PIPE.
    //   - if space -> append + drop lock + wake every waiter + return.
    //   - else -> register on poll_list in the same hold + sleep; re-sample.
    //
    // Discipline matches devpipe_read's read side; specs/pipe.tla
    // NoStuckWriter (multi-waiter form) is the invariant.
    for (;;) {
        spin_lock(&r->lock);
        if (r->read_eof) {
            spin_unlock(&r->lock);
            // P6-pouch-signals-impl (sub-chunk 13a): synthesize the `pipe`
            // note to the writing Proc. Tolerant of NULL current thread
            // (defense-in-depth — write should only run on a userspace
            // path that always has a current Thread/Proc). notes_post_pipe
            // is synthetic=true so a queue-full Proc still observes "pipe
            // happened" via coalesce.
            //
            // The note is informational; the RETURN is the load-bearing
            // EPIPE signal. #100 (ER-3): it used to be a flat -1, under a
            // comment asserting "musl's write wrapper translates to errno".
            // No such wrapper exists -- pouch's src/unistd/write.c is a
            // plain tag-dispatch shim to syscall_cp, so -1 reached
            // __syscall_ret's flat-error sentinel and every write to a
            // closed pipe reported EIO. A stock-musl vivarium guest, which
            // has no -1 special case, read it as errno=1 = EPERM. So the
            // one errno POSIX makes load-bearing here was unobtainable
            // through BOTH boundaries, while T_E_PIPE sat defined and
            // ABI-pinned in errno.h with no emitter anywhere in the tree.
            {
                struct Thread *t = current_thread();
                if (t && t->proc) {
                    notes_post_pipe(t->proc);
                }
            }
            return -T_E_PIPE;
        }
        if (r->count < PIPE_BUF_SIZE) {
            long put = ring_write(r, (const u8 *)buf, n);
            spin_unlock(&r->lock);
            if (put > 0) {
                // A non-empty ring has just become readable: wake every hook
                // -- a reader blocked on empty, a poller on the read end.
                poll_waiter_list_wake(&r->poll_list);
            }
            return put;
        }
        // O_NONBLOCK (CNONBLOCK): the pipe is completely full (count ==
        // PIPE_BUF_SIZE) with a live reader -- a blocking write would sleep, so
        // a non-blocking write returns EAGAIN. Placed AFTER the read_eof (EPIPE)
        // and space checks, so a non-blocking write still delivers EPIPE and
        // still writes what fits (partial); it converts ONLY the full case. It
        // never registers a hook, so I-9 (pipe.tla NoStuckWriter) is untouched.
        // Byte-oriented, unlike CNBFRAME's frame-atomic tx above. `flag` is an
        // atomic read (RMW'd from other lock domains -- see spoor.h).
        if (spoor_flag_get(c) & CNONBLOCK) {
            spin_unlock(&r->lock);
            return -T_E_AGAIN;
        }
        // #811 (ARCH section 8.8.1): death-interrupted -> Proc group-
        // terminating; return so the Thread unwinds to its EL0-return
        // die-check. Registered under the lock we still hold.
        if (pipe_block_locked(r) == SLEEP_INTR)
            return -1;
    }
}

static struct Block *devpipe_bread(struct Spoor *c, long n, s64 off) {
    (void)c; (void)n; (void)off;
    return NULL;
}

static long devpipe_bwrite(struct Spoor *c, struct Block *bp, s64 off) {
    (void)c; (void)bp; (void)off;
    return -1;
}

// Compute the current revents bitmask for endpoint `p` on ring `r` under
// r->lock. Filters POLLIN/POLLOUT by `events`; output-only POLLHUP/POLLERR
// always returned. The read-end gets POLLIN when bytes are buffered and
// POLLHUP when the write side has closed (matching the read loop's two
// wake conditions). The write-end gets POLLOUT when space is available
// and POLLERR when the read side has closed (the EPIPE condition).
static short devpipe_revents_under_lock(struct pipe_ring *r,
                                        struct pipe_endpoint *p,
                                        short events) {
    short revents = 0;
    if (p->is_read_end) {
        if (r->count > 0)     revents |= POLLIN;
        if (r->write_eof)     revents |= POLLHUP;
    } else {
        if (!r->read_eof && r->count < PIPE_BUF_SIZE) revents |= POLLOUT;
        if (r->read_eof)      revents |= POLLERR;
    }
    // POSIX: POLLIN/POLLOUT only set when requested; POLLERR/POLLHUP/
    // POLLNVAL always returned regardless of `events`.
    return (short)((revents & (events | POLL_OUTPUT_ONLY)));
}

// .poll — register-then-observe per specs/poll.tla. Takes r->lock,
// computes revents from the current ring state, and (if pw != NULL)
// registers pw on r->poll_list — atomic with the sample under r->lock.
// pw == NULL is the post-wake sample-only call.
static short devpipe_poll(struct Spoor *c, short events,
                          struct poll_waiter *pw) {
    struct pipe_endpoint *p = priv_of(c);
    if (!p || !p->ring) return POLLERR;
    struct pipe_ring *r = p->ring;

    spin_lock(&r->lock);
    short revents = devpipe_revents_under_lock(r, p, events);
    if (pw) {
        poll_waiter_list_register(&r->poll_list, pw);
    }
    spin_unlock(&r->lock);
    return revents;
}

static void devpipe_remove(struct Spoor *c) { (void)c; }
static int  devpipe_wstat(struct Spoor *c, u8 *dp, int n)
                                            { (void)c; (void)dp; (void)n; return -1; }
static struct Spoor *devpipe_power(struct Spoor *c, int on)
                                            { (void)c; (void)on; return NULL; }

struct Dev devpipe = {
    .dc       = DEVPIPE_DC,
    .name     = "pipe",
    .reset    = devpipe_reset,
    .init     = devpipe_init_noop,
    .shutdown = devpipe_shutdown,
    .attach   = devpipe_attach,
    .walk     = devpipe_walk,
    .stat     = devpipe_stat,
    .stat_native = devpipe_stat_native,   // #96
    .open     = devpipe_open,
    .create   = devpipe_create,
    .close    = devpipe_close,
    .read     = devpipe_read,
    .bread    = devpipe_bread,
    .write    = devpipe_write,
    .bwrite   = devpipe_bwrite,
    .poll     = devpipe_poll,
    .remove   = devpipe_remove,
    .wstat    = devpipe_wstat,
    .power    = devpipe_power,
};

// =============================================================================
// Bring-up.
// =============================================================================

void pipe_init(void) {
    if (g_pipe_initialized) return;

    // Ring is allocated via kmalloc (routes through alloc_pages for the
    // ~4 KiB size). Only the endpoint gets a SLUB cache.
    g_endpoint_cache = kmem_cache_create("pipe_endpoint",
                                         sizeof(struct pipe_endpoint),
                                         8,
                                         0);
    if (!g_endpoint_cache) {
        extinction("kmem_cache_create(pipe_endpoint) returned NULL");
    }
    dev_register(&devpipe);
    g_pipe_initialized = true;
}

// =============================================================================
// pipe_create — the one constructor.
// =============================================================================

int pipe_create(struct Spoor **out_read_end, struct Spoor **out_write_end) {
    if (!g_pipe_initialized) extinction("pipe_create before pipe_init");
    if (!out_read_end || !out_write_end) return -1;
    *out_read_end  = NULL;
    *out_write_end = NULL;

    struct pipe_ring *r = kmalloc(sizeof(*r), KP_ZERO);
    if (!r) return -1;
    r->magic     = PIPE_RING_MAGIC;
    // R15 F234 close: relaxed init — ring isn't published to other
    // CPUs until pipe_create stores the endpoints into caller pointers.
    __atomic_store_n(&r->ref, 2, __ATOMIC_RELAXED);
    r->count     = 0;
    r->head      = 0;
    r->tail      = 0;
    r->read_eof  = false;
    r->write_eof = false;
    spin_lock_init(&r->lock);
    poll_waiter_list_init(&r->poll_list);
    // buf[] already zero from KP_ZERO.

    struct pipe_endpoint *rd_priv = kmem_cache_alloc(g_endpoint_cache, KP_ZERO);
    if (!rd_priv) {
        r->magic = 0;
        kfree(r);
        return -1;
    }
    rd_priv->magic       = PIPE_ENDPOINT_MAGIC;
    rd_priv->ring        = r;
    rd_priv->is_read_end = true;

    struct pipe_endpoint *wr_priv = kmem_cache_alloc(g_endpoint_cache, KP_ZERO);
    if (!wr_priv) {
        rd_priv->magic = 0;
        kmem_cache_free(g_endpoint_cache, rd_priv);
        r->magic = 0;
        kfree(r);
        return -1;
    }
    wr_priv->magic       = PIPE_ENDPOINT_MAGIC;
    wr_priv->ring        = r;
    wr_priv->is_read_end = false;

    struct Spoor *rd = spoor_alloc(&devpipe);
    if (!rd) {
        wr_priv->magic = 0;
        kmem_cache_free(g_endpoint_cache, wr_priv);
        rd_priv->magic = 0;
        kmem_cache_free(g_endpoint_cache, rd_priv);
        r->magic = 0;
        kfree(r);
        return -1;
    }
    struct Spoor *wr = spoor_alloc(&devpipe);
    if (!wr) {
        // Rolling back the read-end Spoor: spoor_clunk would call
        // devpipe_close → drop ring ref → potentially free the ring.
        // But we still own the write-end priv that points at the ring.
        // Take the path that frees both pieces of state manually then
        // unrefs the Spoor without calling its close hook.
        rd->aux = NULL;                      // detach priv before close fires
        spoor_clunk(rd);                     // close sees NULL aux → no-op via priv_of
        wr_priv->magic = 0;
        kmem_cache_free(g_endpoint_cache, wr_priv);
        rd_priv->magic = 0;
        kmem_cache_free(g_endpoint_cache, rd_priv);
        r->magic = 0;
        kfree(r);
        return -1;
    }

    rd->aux = rd_priv;
    wr->aux = wr_priv;
    rd->qid.type = 0;                        // QTFILE
    wr->qid.type = 0;
    // #96: give the pipe an identity. BOTH ends share it -- one pipe, one
    // inode, the POSIX convention -- so fstat can distinguish two pipes while
    // still reporting the two ends of one pipe as the same object. Monotonic
    // and never reused; qid.path was previously left 0 for every pipe, and
    // nothing keys on it for dc == DEVPIPE_DC (the Larder is dev9p-only; the
    // cons/pts qid flags are dc-gated), so stamping it is inert elsewhere.
    {
        u64 id = __atomic_fetch_add(&g_pipe_next_qid, 1u, __ATOMIC_RELAXED);
        rd->qid.path = id;
        wr->qid.path = id;
    }

    __atomic_fetch_add(&g_pipe_allocated, 1u, __ATOMIC_RELAXED);
    *out_read_end  = rd;
    *out_write_end = wr;
    return 0;
}

u64 pipe_total_allocated(void) {
    return __atomic_load_n(&g_pipe_allocated, __ATOMIC_RELAXED);
}

u64 pipe_total_freed(void) {
    return __atomic_load_n(&g_pipe_freed, __ATOMIC_RELAXED);
}
