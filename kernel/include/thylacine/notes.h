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
// F4 audit close (P6 hardening #3a): reserved bit for the snare:*
// fault-note family. Setting the bit defers delivery of EVERY snare:*
// note (per-fault-kind masking is a v1.x extension). At v1.0 snare:*
// names are NOT in g_known_notes (proc_fault_terminate calls exits
// directly without going through notes_post), so this bit has no
// consumer today; it's reserved so the docs/ERRORS.md "Bit-position
// assignment in note_mask" claim is honored by a real symbol. v1.x
// adds snare:* to g_known_notes for substrate-based delivery; this
// bit becomes load-bearing then.
#define NOTE_BIT_SNARE       4u

// Bitmask of every supported note. Userspace SYS_NOTE_MASK calls that set
// bits outside this mask succeed (we tolerate unknown bits — they just
// have no effect at v1.0; the supported set grows per chunk without ABI
// break). SYS_POSTNOTE with an unsupported note name returns -EINVAL.
//
// PTY-1b: the tty:* family bit. ONE bit for the whole family (winch /
// susp / cont / quit / hup) -- per-kind masking is a v1.x extension, the
// NOTE_BIT_SNARE precedent. Unlike SNARE this bit is load-bearing at
// v1.0: the tty:* names ARE in g_known_notes (deliverable, catchable),
// and a thread masking the bit defers every tty note.
#define NOTE_BIT_TTY         5u

// F4 audit close: includes NOTE_BIT_SNARE (bit 4) for the snare:*
// family even though no v1.0 consumer exists; reserves the bit
// position for v1.x. PTY-1b adds NOTE_BIT_TTY (bit 5), live.
#define NOTE_MASK_SUPPORTED  0x3fu

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

// LS-5 (P2 default disposition): the cooked-Ctrl-C note. A .rodata string
// literal -- safe to pass to exits() (whose by-reference exit_msg capture
// requires program-lifetime storage; F10 audit close). Used by the EL0-
// return-tail uncaught-interrupt default-terminate.
#define NOTE_NAME_INTERRUPT "interrupt"

// `snare:*` family — kernel-synthetic notes posted on EL0 unhandled
// fault. Per docs/ERRORS.md "Fault-note naming". Each name fits within
// NOTE_NAME_MAX = 16 bytes including NUL. The `snare:` prefix is
// reserved for kernel-synthetic posters; userspace SYS_POSTNOTE with
// a `snare:`-prefixed name is rejected at notes_post.
//
// Default action for any unhandled snare:* note: terminate the
// offending Proc via exits(name). The kernel does NOT extinct on EL0
// unhandled fault at v1.0 -- see kernel/proc.c::proc_fault_terminate
// + arch/arm64/exception.c::exception_sync_lower_el.
#define NOTE_NAME_SNARE_SEGV   "snare:segv"   // no VMA / W^X / perm
#define NOTE_NAME_SNARE_BUS    "snare:bus"    // VMA-covered but burrow can't satisfy
#define NOTE_NAME_SNARE_ALIGN  "snare:align"  // EL0 PC/SP alignment fault
#define NOTE_NAME_SNARE_BTI    "snare:bti"    // EL0 BTI fault
#define NOTE_NAME_SNARE_BRK    "snare:brk"    // EL0 brk #imm (assertion/debug)
#define NOTE_NAME_SNARE_ILL    "snare:ill"    // EL0 unknown sync EC
#define NOTE_NAME_SNARE_FPE    "snare:fpe"    // EL0 floating-point trap (reserved; no v1.0 path emits this)

// Length sanity -- the longest name + NUL must fit NOTE_NAME_MAX. The
// constants above are #define'd as string literals; sizeof on the
// literal returns the byte count INCLUDING the trailing NUL. Pin the
// longest one ("snare:align" = 11+1 = 12) at compile time.
//
// F7 audit close: the `<=` here (not `<`) is deliberate -- a literal
// of exactly NOTE_NAME_MAX bytes is 15 chars + NUL, which fits
// notes_name_copy's discipline of writing up to NOTE_NAME_MAX - 1
// source bytes + padding dst[NOTE_NAME_MAX - 1] = 0 (the padding NUL
// coincides bit-for-bit with the source NUL). Future entries up to
// the boundary are safe.
// `tty:*` family -- kernel-synthetic-POST, CATCHABLE notes (PTY-1b;
// PTY-DESIGN.md section 4, round-1 F4 + round-2 R2-F3). A NEW note class
// between `interrupt` (anyone-post + catchable) and `snare:*` (kernel-post
// + uncatchable-terminate): only the kernel may POST them (the tty signal
// seam + the controlling-terminal paths -- notes_post rejects a userspace
// SYS_POSTNOTE of any `tty:`-prefixed name, closing the parent-posts-cont-
// to-a-debug-stopped-child I-39 leak), but a target Proc may CATCH or mask
// them like any note (bash/vim/tmux install SIGTSTP handlers; SIGWINCH is
// routinely caught). Uncaught defaults: tty:quit / tty:hup TERMINATE (the
// LS-5 interrupt pattern -- fires only with no handler + not self-managing
// + unmasked); tty:susp STOPS (LIVE since PTY-1f: an UNCAUGHT susp is
// CONSUMED by the default stop at post time -- proc_job_stop_pgrp sets
// job_stop_req and never queues the note, so nothing stays pending across
// the stop; only a CAUGHT one is queued); tty:winch /
// tty:cont are informational (queue for the fd-read path, no default
// action -- the pipe/child_exit shape; cont's RESUME side effect is the
// kernel stop-clear -- proc_job_cont_pgrp -- not a note disposition).
#define NOTE_NAME_TTY_WINCH  "tty:winch"  // SIGWINCH -- winsize changed
#define NOTE_NAME_TTY_SUSP   "tty:susp"   // SIGTSTP  -- default STOP (PTY-1f)
#define NOTE_NAME_TTY_CONT   "tty:cont"   // SIGCONT  -- resume (kernel side)
#define NOTE_NAME_TTY_QUIT   "tty:quit"   // SIGQUIT  -- default terminate
#define NOTE_NAME_TTY_HUP    "tty:hup"    // SIGHUP   -- default terminate

_Static_assert(sizeof(NOTE_NAME_TTY_WINCH) <= NOTE_NAME_MAX,
               "NOTE_NAME_TTY_WINCH does not fit NOTE_NAME_MAX");
_Static_assert(sizeof(NOTE_NAME_TTY_SUSP) <= NOTE_NAME_MAX,
               "NOTE_NAME_TTY_SUSP does not fit NOTE_NAME_MAX");
_Static_assert(sizeof(NOTE_NAME_TTY_CONT) <= NOTE_NAME_MAX,
               "NOTE_NAME_TTY_CONT does not fit NOTE_NAME_MAX");
_Static_assert(sizeof(NOTE_NAME_TTY_QUIT) <= NOTE_NAME_MAX,
               "NOTE_NAME_TTY_QUIT does not fit NOTE_NAME_MAX");
_Static_assert(sizeof(NOTE_NAME_TTY_HUP) <= NOTE_NAME_MAX,
               "NOTE_NAME_TTY_HUP does not fit NOTE_NAME_MAX");

_Static_assert(sizeof(NOTE_NAME_SNARE_ALIGN) <= NOTE_NAME_MAX,
               "NOTE_NAME_SNARE_ALIGN does not fit NOTE_NAME_MAX");
_Static_assert(sizeof(NOTE_NAME_SNARE_SEGV)  <= NOTE_NAME_MAX,
               "NOTE_NAME_SNARE_SEGV does not fit NOTE_NAME_MAX");
_Static_assert(sizeof(NOTE_NAME_SNARE_BUS)   <= NOTE_NAME_MAX,
               "NOTE_NAME_SNARE_BUS does not fit NOTE_NAME_MAX");
_Static_assert(sizeof(NOTE_NAME_SNARE_BTI)   <= NOTE_NAME_MAX,
               "NOTE_NAME_SNARE_BTI does not fit NOTE_NAME_MAX");
_Static_assert(sizeof(NOTE_NAME_SNARE_BRK)   <= NOTE_NAME_MAX,
               "NOTE_NAME_SNARE_BRK does not fit NOTE_NAME_MAX");
_Static_assert(sizeof(NOTE_NAME_SNARE_ILL)   <= NOTE_NAME_MAX,
               "NOTE_NAME_SNARE_ILL does not fit NOTE_NAME_MAX");
_Static_assert(sizeof(NOTE_NAME_SNARE_FPE)   <= NOTE_NAME_MAX,
               "NOTE_NAME_SNARE_FPE does not fit NOTE_NAME_MAX");

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
// Known caveat (RW-0 F2, accepted): a console Ctrl-C is a synthetic
// `interrupt` post, so when the target queue already holds NOTE_QUEUE_DEPTH
// entries NONE of which is an interrupt, the coalesce pass finds no
// same-name slot to overwrite -> -EAGAIN -> the interrupt is dropped AND
// the LS-5c terminate latch is never armed (the arm rides a landed post).
// That Ctrl-C is lost. Unreachable for a typical foreground coreutil (its
// queue is near-empty; the precondition is 16 queued, unconsumed
// child_exit/pipe/user notes), so accepted at v1.0; a queue-pressure
// poster would revisit (e.g. reserve the head slot for interrupt/kill).
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

// LS-5 P2 (ARCH 8.8.2): would an uncaught `interrupt` default-terminate `p` at
// the EL0-return tail? True iff no async handler is registered, `p` is not
// self-managing (has not opened its notes fd), and a deliverable interrupt
// (queued AND unmasked for `t`) is present. Pure decision; no side effects.
// notes_deliver_at_el0_return calls it under q->lock and, on true, drops the
// lock + calls exits(NOTE_NAME_INTERRUPT). Exposed so the unit test can drive
// the full decision without the noreturn exits() path. Caller MUST hold
// p->notes->lock.
int notes_interrupt_should_terminate_locked(struct Proc *p, struct Thread *t);

// PTY-1b: the name-returning generalization of the above -- the canonical
// (.rodata, program-lifetime) name of the first DELIVERABLE terminate-class
// note (interrupt / tty:quit / tty:hup: queued AND its family bit unmasked
// for `t`), or NULL. Same handler / self-managing gates. The EL0-return
// tail passes the returned name to exits() so the exit_msg reports WHICH
// signal terminated the Proc. Caller MUST hold p->notes->lock.
const char *notes_terminate_note_name_locked(struct Proc *p, struct Thread *t);

// PTY-1b (PTY-DESIGN.md section 4): kernel-synthetic note fan-out to a
// process group -- the pgrp generalization of proc_console_post_interrupt.
// Delivers `name` (synthetic; the tty seam + controlling-terminal paths are
// the callers) to every ALIVE member of `pgid` EXACTLY ONCE, under one
// g_proc_table_lock hold: membership (p->pgid) is read under the same lock
// that serializes setpgid/rfork/exit, so a concurrent membership mutation
// orders entirely before or after the whole fan-out -- never a
// half-delivered group (the F14 argument); the hold also pins each member
// across its post, and the per-member LS-5c terminate-wake runs under it
// (the wake's contract). pgid 0 (the boot session's group -- kproc + joey)
// is REFUSED, fan-out count 0: the boot group is never a tty-signal target
// (defense-in-depth; the seam's fg_pgid can never be 0 since acquisition
// copies a setsid'd leader's pgid). Returns the count of members posted.
// Implemented in kernel/proc.c (needs the static g_proc_table_lock).
int notes_post_pgrp(u32 pgid, const char *name, u32 arg);

// The single-Proc sibling (PTY-1d): deliver `name` (synthetic) to the ALIVE
// Proc with `pid`, with the same one-lock-hold post + terminate-wake
// discipline. The tty seam's F13 second SIGHUP target (the controlling
// process -- the session leader -- when it is not in the foreground group)
// is the caller. Returns 1 if posted, 0 if no such ALIVE Proc.
// Implemented in kernel/proc.c.
int notes_post_pid(int pid, const char *name, u32 arg);

// =============================================================================
// LS-5c (P3-terminate, ARCH 8.8.2): the terminate-disposition interrupt latch.
// =============================================================================
//
// PROC_FLAG_INTR_TERMINATE_PENDING (proc.h) caches "an uncaught interrupt
// will terminate this Proc at its next EL0-return tail" so the #811
// sleep/tsleep register-then-observe can read it LOCK-FREE (the sleep path
// must never take q->lock -- the devnotes F3-close ABBA). ALL latch writes
// run under p->notes->lock: the set in notes_post's interrupt arm, and the
// clears in the three disposition-change choke points below + the
// drained-last-interrupt clear inside the dequeue helpers. The EL0-return
// tail re-validates against the live queue, so a stale-positive latch costs
// one spurious *_INTR unwind, never a wrong termination.
//
// Known caveat (RW-0 F4, accepted): the spurious-*_INTR window has a
// multi-thread variant. Thread B registering a handler (SYS_NOTIFY) or
// opening the notes fd (self-managing) AFTER an interrupt armed the latch
// but BEFORE a latch-woken thread A reaches its EL0-return tail clears the
// latch — but A has already unwound its blocked syscall *_INTR (e.g. a 9P
// RPC surfaces -P9_E_IO). No wrong termination (the tail re-validates);
// the cost is one EINTR-class return on A, the POSIX EINTR-with-handler
// shape. Unreachable at v1.0: multi-thread Procs are stratumd-class and
// never console-owner/foreground, and the single-thread variant requires
// the Proc to change its own disposition concurrently with a Ctrl-C.

// Register/clear the async note handler (the SYS_NOTIFY body). Stores
// handler_va with RELEASE (pairs with the dispatcher's acquire, F9) and, when
// registering (handler_va != 0), clears the terminate latch -- BOTH under
// q->lock, so the store+clear cannot interleave with notes_post's
// check-handler-then-arm and leave a stale armed latch behind a registered
// handler (which would *_INTR every future sleep of a surviving Proc).
void notes_set_handler(struct Proc *p, u64 handler_va);

// Mark `p` self-managing (the SYS_NOTE_OPEN tail; wraps
// proc_mark_self_managing_notes) and clear the terminate latch -- both under
// q->lock, same serialization rationale as notes_set_handler.
void notes_mark_self_managing(struct Proc *p);

// The WIDENED #811 death predicate (ARCH 8.8.1 + 8.8.2): true iff `t`, on
// returning to its EL0-return tail now, will die there -- its Proc is
// group-terminating (group_exit_msg set), OR a terminate-disposition
// `interrupt` is pending (the latch) and `interrupt` is not masked for `t`
// (a masked thread defers: it neither unwinds nor terminates until it
// unmasks). LOCK-FREE (atomic loads + the owner-read note_mask): callable
// from sleep/tsleep's register-then-observe under wait_lock/r->lock, from
// torpor's post-register check under torpor_lock, and from the 9P client's
// reader-unwind decision. Replaces every direct group_exit_msg load at
// those sites.
bool thread_die_pending(struct Thread *t);

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
