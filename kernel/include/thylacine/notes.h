// P6-pouch-signals-impl (sub-chunk 13a): Thylacine's note delivery primitive.
//
// Per ARCH §7.6.1-§7.6.8 (design landed at 237f096). Notes are the kernel's
// asynchronous-event mechanism — the Plan 9 model adapted to fit Thylacine's
// "the filesystem is the OS" conviction: notes are FD-SHAPED FIRST.
//
// Two delivery paths consume the same per-Proc NoteQueue:
//
//   1. THE FD-SHAPED PATH (the documented default; NOVEL.md §3.1):
//      Every Proc opens a kernel-owned note Spoor via SYS_NOTE_OPEN; reads
//      from the fd yield 32-byte struct note_record records; poll integrates
//      for free. Modern daemons (stratumd, libsodium) read notes in their
//      normal event loop — no async-cancel-safety hell, no siginfo_t ABI
//      nightmare.
//
//   2. THE ASYNC-HANDLER PATH (libc-compat opt-in):
//      A Proc registers a handler via SYS_NOTIFY(handler_va). At the EL0-
//      return tail (after the syscall handler writes ctx->regs[0] but before
//      the eret), the kernel pops the next deliverable note from the queue,
//      saves the current user context into per-Thread fields, mutates the
//      exception_context to land at handler_va with x0 = note name VA on
//      the user stack + x1 = note arg, and erets. The handler ends with
//      SYS_NOTED(NCONT) which restores the saved context.
//
// MUTUAL EXCLUSION across the two paths: every posted NON-kill note is
// consumed exactly once. The queue lock is the consume serializer — either
// the EL0-return-tail pops first (handler path wins) or devnotes_read pops
// first (fd-read path wins). `kill` is the exception: it bypasses the
// handler and mask, terminating the Proc at the next EL0-return.
//
// THE I-9 / I-19 invariants — ARCH §7.6.7 (sub-invariants N-1..N-5):
//   N-1 (queue ordering): notes consumed in post order per source.
//        EXCEPTION 1 (R3-F5 audit close): `kill` is special-cased -- it
//        is always delivered FIRST regardless of FIFO position, and on
//        re-enqueue (live_peers > 0 defer) it goes to the head, not the
//        original position.
//        EXCEPTION 2 (R4-F1 audit close): non-kill notes also relax to
//        BEST-EFFORT FIFO when a re-enqueue-at-head happens on user-stack
//        push failure (dispatcher's non-kill branch). The popped note
//        goes to head, which can reverse cross-name order vs mask-
//        deferred earlier same-source entries still in queue. This is a
//        v1.x perf-vs-correctness tradeoff (strict FIFO would require a
//        re-enqueue-at-original-index primitive with more bookkeeping).
//   N-2 (consumed exactly once): every non-`kill` note consumed once across
//        the handler + fd-read paths.
//   N-3 (handler re-entrancy): while in_handler == true, no further delivery
//        to that Thread. EXCEPTION (R2-F2 audit close): `kill` bypasses
//        in_handler -- kill is fully non-catchable.
//   N-4 (`kill` non-catchable): a `kill` note terminates the Proc at next
//        EL0-return regardless of mask / handler / in_handler. EXCEPTION
//        (R3-F3 documented v1.0 limitation): if the kill arrives in the
//        narrow TOCTOU window where the target Proc transitions from
//        single-thread to multi-thread (via SYS_THREAD_SPAWN), the kill
//        cannot be delivered until cross-thread shootdown lands (v1.x).
//   N-5 (fd lifecycle): a closed note Spoor fd does not affect future
//        SYS_NOTE_OPEN or queue state. The queue lives with the Proc.
//
// SYS_NOTED arg semantics (R4-F6 audit close):
//   - arg = 0 (NCONT): restore saved user context; resume pre-handler
//     execution. Always succeeds while in_handler.
//   - arg = 1 (NDFLT): take the note's default action (for the v1.0
//     supported set, every default is exits(name)). Requires
//     `live_peers == 0` -- exits extincts the kernel on live peer
//     Threads (cross-thread shootdown is v1.x). NDFLT in a multi-
//     thread Proc therefore returns -1; the handler must fall back to
//     NCONT or explicit per-Thread teardown (SYS_THREAD_EXIT).
//
// Spec-to-code suspended (CLAUDE.md, broadened 2026-05-23) — no
// specs/notes.tla module. The invariants above are pinned by the queue-lock
// discipline + the focused audit round + the runtime test suite.

#ifndef THYLACINE_NOTES_H
#define THYLACINE_NOTES_H

#include <thylacine/poll.h>
#include <thylacine/rendez.h>
#include <thylacine/spinlock.h>
#include <thylacine/types.h>

struct Proc;
struct Thread;

// Maximum length of a note name (including the NUL terminator). 16 bytes
// bounds the v1.0 supported set (the longest name "child_exit" is 11 + NUL
// = 12 bytes; the cap leaves slack for future entries). Plan 9 used 128; we
// pick 16 because v1.0 has a closed supported set and the smaller bound
// fits the ABI-pinned 32-byte struct note_record cleanly.
#define NOTE_NAME_MAX  16u

// Bounded per-Proc queue depth. 16 entries is enough that a Proc handling
// one note at a time can't easily fill the queue under normal load; the
// kernel-synthetic posters (child_exit on exits, pipe on write-to-closed)
// coalesce same-name notes when the queue is near capacity (see
// NOTE_COALESCE_THRESHOLD below) so synthetic delivery is contractually
// infallible.
#define NOTE_QUEUE_DEPTH  16u

// Coalesce threshold: when count >= this, kernel-synthetic posters of an
// already-queued same-name note merge (preserving the most recent arg,
// post-order ordering unchanged for that name). Userspace SYS_POSTNOTE
// callers see -EAGAIN at full queue instead — coalescing is a kernel-
// synthetic-only opportunistic relief valve, not a userspace contract.
#define NOTE_COALESCE_THRESHOLD  12u

// NOTE_BIT_* — the bit position of each supported note in the per-Thread
// note_mask. Bit set = the Thread defers delivery of that note. The mask
// is per-Thread so multi-thread Procs can have different threads accept
// different signals (POSIX pthread_sigmask semantics).
#define NOTE_BIT_INTERRUPT   0u
#define NOTE_BIT_KILL        1u
#define NOTE_BIT_PIPE        2u
#define NOTE_BIT_CHILD_EXIT  3u

// Bitmask of every supported note. Userspace SYS_NOTE_MASK calls that set
// bits outside this mask succeed (we tolerate unknown bits — they just
// have no effect at v1.0; the supported set grows per chunk without ABI
// break). SYS_POSTNOTE with an unsupported note name returns -EINVAL.
#define NOTE_MASK_SUPPORTED  0x0fu

// In-kernel note record. The ring lives in `struct NoteQueue.ring` (inline
// — the queue is heap-allocated once per Proc at proc_alloc).
struct Note {
    char  name[NOTE_NAME_MAX];   // NUL-terminated within
    u32   arg;                    // small int slot (child_exit packs pid+status; pipe = 0; interrupt = 0; kill = 0)
    u32   sender_pid;             // posting Proc's pid; 0 for kernel-synthetic
    u64   timestamp_ns;           // monotonic kernel time at post (timer_now_ns)
};
_Static_assert(sizeof(struct Note) == 32,
               "struct Note size pinned at 32 bytes (16 name + 4 arg + 4 "
               "sender_pid + 8 timestamp). Adjusting the layout grows the "
               "per-Proc NoteQueue allocation; update this assert "
               "deliberately so the change is intentional.");

// ABI-pinned SYS_NOTE_OPEN read-side wire record. devnotes_read copies one
// of these per read() call (vectored reads are a v1.x extension). The 32-
// byte layout matches struct Note byte-for-byte — devnotes_read does a
// straight memcpy under the queue lock; no field-level marshalling.
struct note_record {
    char  name[NOTE_NAME_MAX];
    u32   arg;
    u32   sender_pid;
    u64   timestamp_ns;
};
_Static_assert(sizeof(struct note_record) == 32,
               "struct note_record size pinned at 32 bytes — ABI for fd-"
               "shaped reads. devnotes_read copies one record per read() "
               "call at v1.0.");
_Static_assert(sizeof(struct note_record) == sizeof(struct Note),
               "struct note_record and struct Note must match byte-for-byte "
               "so devnotes_read can memcpy under the queue lock without "
               "field-level marshalling.");

// Per-Proc note queue. Allocated by notes_queue_alloc at proc_alloc; freed
// by notes_queue_free at proc_free. The `lock` serializes all queue
// mutations (post + dequeue); `poll_list` is the multi-waiter hook list
// shared by devnotes_read (a private Rendez + stack-allocated poll_waiter
// per read call -- R2-F3 audit close restructured from single-waiter
// Rendez to this pattern to break the ABBA with notes_post's wake) AND
// SYS_POLL on /dev/notes -- producers wake the entire list under q->lock.
struct NoteQueue {
    spin_lock_t              lock;
    u32                      head;        // index of next dequeue
    u32                      tail;        // index of next enqueue
    u32                      count;
    u32                      _pad;        // explicit 8-byte alignment
    struct Note              ring[NOTE_QUEUE_DEPTH];
    // F3 audit close (sub-chunk 13a): devnotes_read uses the poll_-
    // waiter_list mechanism — each reader has its own private Rendez +
    // stack-allocated poll_waiter; producers wake via poll_waiter_list_-
    // wake which is multi-waiter-safe. No per-queue single-waiter Rendez.
    struct poll_waiter_list  poll_list;   // register-then-observe hooks (devnotes_read AND SYS_POLL)
};

// `kill` recognition — the special-case non-catchable note. The constant is
// the byte sequence in struct Note.name (NUL-padded), surfaced here so the
// EL0-return-tail check and the syscall handlers can match it without
// re-typing the literal.
#define NOTE_NAME_KILL "kill"

// ============================================================================
// Public API
// ============================================================================

// Allocate a fresh NoteQueue (kmalloc'd; KP_ZERO + rendez_init + lock_init).
// Returns NULL on OOM. proc_alloc calls this; proc_free calls
// notes_queue_free.
struct NoteQueue *notes_queue_alloc(void);

// Release a NoteQueue. Caller must ensure no thread is currently
// registered on `q->poll_list` (handled by proc_free's ordering: ZOMBIE
// state is set, all threads EXITING, no devnotes_read can be in flight).
void notes_queue_free(struct NoteQueue *q);

// Post a note to `p->notes`. `name` is bounded to NOTE_NAME_MAX-1 chars +
// NUL; longer or empty names return -EINVAL. `sender` is the posting Proc
// (for sender_pid; pass NULL for kernel-synthetic). Returns 0 on success,
// -EINVAL on bad name, -EAGAIN on queue full (after coalesce attempt for
// kernel-synthetic; userspace SYS_POSTNOTE callers see -EAGAIN immediately).
//
// `synthetic == true` enables coalesce-on-full (last-arg-wins for same-name
// posts when queue count >= NOTE_COALESCE_THRESHOLD); false (userspace
// SYS_POSTNOTE path) skips coalesce — -EAGAIN bubbles to userspace.
//
// Wakes every registered hook on q->poll_list (devnotes_read parkers AND
// SYS_POLL pollers; multi-waiter via the poll_waiter_list mechanism).
int notes_post(struct Proc *p, const char *name, u32 arg,
               struct Proc *sender, bool synthetic);

// Dequeue the next deliverable note for `t` — the DISPATCHER variant.
// Two passes: (1) kill-first scan regardless of mask (N-4 non-catchable),
// (2) first mask-permitted non-kill entry. Returns 1 if a note was popped
// (written to *out), 0 if no deliverable note is present (queue empty or
// every entry masked AND no kill present). Caller MUST hold
// p->notes->lock.
//
// USED BY THE EL0-RETURN-TAIL DISPATCHER ONLY. The fd-read path uses
// `notes_dequeue_for_fd_locked` so that `kill` is invisible to fd consumers
// (R2-F1: a Proc reading /dev/notes would otherwise consume its own kill).
int notes_dequeue_locked(struct Proc *p, struct Thread *t,
                         struct Note *out);

// Peek the dispatcher's next deliverable note (kill-first; mask-permitted
// otherwise). Returns 1 if an entry exists (copied to *out), 0 if empty.
// Caller MUST hold p->notes->lock. Used by the EL0-return-tail dispatcher.
int notes_peek_locked(struct Proc *p, struct Thread *t,
                      struct Note *out);

// R2-F1 audit close: fd-read variant of dequeue. Skips kill entirely
// (kill is non-catchable and only the EL0-return-tail dispatcher may pop
// it). Returns the first mask-permitted NON-KILL entry. Used by
// devnotes_read.
int notes_dequeue_for_fd_locked(struct Proc *p, struct Thread *t,
                                struct Note *out);

// R2-F1 / R2-F6 audit close: fd-read peek. Same kill-skip semantics as
// notes_dequeue_for_fd_locked. Used by devnotes_poll for POLLIN sampling
// (so the fd doesn't advertise readability based on a kill the fd-read
// would refuse to consume).
int notes_peek_for_fd_locked(struct Proc *p, struct Thread *t,
                             struct Note *out);

// Predicate: 1 iff `name` is the literal "kill". Used by the EL0-return-
// tail dispatcher (the non-catchable detection), devnotes_read's R3-F1
// fix (detecting a kill-only queue to bail out of tsleep loop), and
// devnotes_poll's R4-F2 fix (POLLERR for kill-only queue).
//
// Contract (R4-F5 audit close): `name` MUST be either NUL-terminated
// within NOTE_NAME_MAX characters, OR be at least NOTE_NAME_MAX bytes
// long. The comparison is bounded at NOTE_NAME_MAX so a 16-byte non-
// terminated buffer is safe; a shorter non-terminated buffer would read
// past its end.
int notes_name_is_kill(const char *name);

// F5 + F6 audit close (sub-chunk 13a): re-enqueue a previously-dequeued
// note at the HEAD of the queue. Used by devnotes_read on uaccess failure
// and by notes_deliver_at_el0_return on user-stack-push failure to
// preserve the N-2 (consumed exactly once) invariant — the note is put
// back so the next consume can pick it up. Caller MUST hold
// p->notes->lock. Returns 0 on success. Cannot fail at v1.0 since the
// caller just popped (so there is space).
int notes_reenqueue_head_locked(struct NoteQueue *q, const struct Note *n);

// =============================================================================
// Synthetic posters — kernel-internal callers (proc.c::exits, pipe.c write
// path). These wrap notes_post with the appropriate canonical name + arg
// packing + synthetic=true.
// =============================================================================

// Post the `child_exit` note to `parent` (a child of `parent` exited; arg
// packs `(child_pid << 16) | (status & 0xffff)`). Tolerant of NULL parent
// (no-op if exits has no parent — the init-Proc edge case). Tolerant of
// queue-full via coalesce: a queue that already holds a `child_exit` near
// the threshold has its head-of-bucket arg overwritten with the latest
// (child_pid, status); the parent still observes "a child exited" but may
// have to wait_pid() through the others in a loop.
void notes_post_child_exit(struct Proc *parent, int child_pid, int status);

// Post the `pipe` note to `writer` (a Proc that just write()d to a closed
// pipe). The kernel's write path also returns -EPIPE; the note is the
// signal-equivalent. Tolerant of queue-full via coalesce.
void notes_post_pipe(struct Proc *writer);

// =============================================================================
// Bring-up
// =============================================================================

// Initialize a NoteQueue in place. Used by notes_queue_alloc; exposed for
// kernel-test harnesses that drive the queue directly without going through
// the proc_alloc path.
void notes_queue_init(struct NoteQueue *q);

#endif  // THYLACINE_NOTES_H
