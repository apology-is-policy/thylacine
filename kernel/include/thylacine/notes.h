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
// SYS_NOTED arg semantics (R4-F6 audit close; rewritten by #15):
//   - arg = 0 (NCONT): restore saved user context; resume pre-handler
//     execution. Always succeeds while in_handler.
//   - arg = 1 (NDFLT): take the note's TRUE default action -- the one
//     `notes_default_action(name)` names, which is what would have happened
//     had no handler been installed. THREE outcomes, not one:
//       TERMINATE -- exits(name); noreturn. Single-thread goes ZOMBIE,
//         multi-thread cascades via proc_group_terminate (#811).
//       STOP      -- restore the pre-handler context exactly as NCONT does,
//         then arm the job stop; the EL0-return tail parks the thread. The
//         Proc resumes at the interrupted PC on tty:cont, which is what
//         "as if uncaught" means for a suspend.
//       IGNORE    -- restore and return. Identical to NCONT by construction:
//         doing nothing IS the default action.
//     Returns 0 for STOP/IGNORE (with ctx restored, so the EL0 x0 is the
//     saved pre-handler value, not this 0), -1 if not in a handler.
//
//     HISTORY, because the removed rule outlived its removal in this comment:
//     NDFLT once required `live_peers == 0` and returned -1 in a multi-thread
//     Proc. RW-8 R5-F1 deleted that refusal after #809/#811 made exits()
//     cascade instead of extinct -- the refusal had been silently swallowing
//     SIGINT/SIGTERM in multi-thread pouch daemons. There is no live-peers
//     gate today.
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

// #15: the DEFAULT ACTION of a note -- what happens when nobody catches it.
// One value per row of `g_known_notes`, so a note's disposition is a property
// OF the note rather than of whichever call site is asking. Before #15 the
// kernel had no such notion: SYS_NOTED(NDFLT) took the same action (terminate)
// for every name, which made ^Z under SIG_DFL a choice between dying and being
// ignored, and would have made child_exit's default fatal.
//
// The values are the three POSIX default dispositions, minus "core dump"
// (Thylacine has no core files at v1.0 -- tty:quit terminates without one).
enum note_default {
    NOTE_DFL_TERMINATE = 0,  // interrupt, kill, pipe, tty:quit, tty:hup
    NOTE_DFL_STOP      = 1,  // tty:susp -- the job-control suspend (I-20)
    NOTE_DFL_IGNORE    = 2,  // child_exit, tty:winch, tty:cont
};

// The default action for a supported note name; NOTE_DFL_TERMINATE for an
// unknown one. PURE -- a table lookup with no side effects, which is the point:
// the policy is unit-testable without driving the noreturn terminate leg.
//
// TERMINATE for an unknown name is deliberate. It is both what the kernel did
// for every name before #15 (so no name's behaviour regresses) and the POSIX
// majority disposition, and an unknown name cannot actually arrive: every
// caller passes a name that came out of `g_known_notes` in the first place.
enum note_default notes_default_action(const char *name);

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

// The two remaining v1.0 supported names, given macros so a second reader
// (VIVARIUM's signal decode) matches `g_known_notes` by symbol rather than by
// a re-typed literal. Same role as NOTE_NAME_KILL above.
#define NOTE_NAME_PIPE       "pipe"
#define NOTE_NAME_CHILD_EXIT "child_exit"

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
// + unmasked); tty:susp STOPS (LIVE since PTY-1f: when some thread has the
// family unmasked and nothing catches it, proc_job_stop_pgrp applies the
// stop at POST time and queues nothing; the susp is QUEUED when a handler
// or self-management catches it, when EVERY thread masks the family (POSIX
// pending -- the EL0 tail's stop consumer takes it once the mask lifts), or
// when the Proc has no thread yet, and the tail's stop arm applies the
// deferred default for the last two); tty:winch /
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

// The install-time discard (POSIX 2.4.3 / Linux flush_sigqueue_mask): remove
// every queued note named `name` from p's queue regardless of any thread's
// mask, draining the class latch per removal; returns the count removed. Never
// removes `kill` (N-4). Takes p->notes->lock itself -- call with NO note lock
// held. The phenotype rt_sigaction shell calls it AFTER storing a disposition
// that ignores (SIG_IGN, or SIG_DFL whose Linux default is ignore); the store-
// then-lock order against notes_post's under-lock disposition read is what
// makes "no stale ignored note survives" hold. See kernel/notes.c.
u32 notes_discard_name(struct Proc *p, const char *name);

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

// Would `name` take its DEFAULT action on `p`? False iff something intercepts
// it: a native handler (handler_va), a phenotype sigtab handler, or a phenotype
// SIG_IGN. Pure; no lock required (each read is a single atomic load).
//
// Exists because a decider that runs INSTEAD of notes_post cannot rely on the
// SIG_IGN drop inside it. proc_tty_susp_would_stop_locked is the caller that
// needs this: its uncaught arm stops the Proc directly, generating no note, so
// a phenotyped Proc's disposition has to be consulted before the decision
// rather than during the post.
bool notes_proc_default_applies(struct Proc *p, const char *name);

// PTY-1b: the name-returning generalization of the above -- the canonical
// (.rodata, program-lifetime) name of the first DELIVERABLE terminate-class
// note (interrupt / tty:quit / tty:hup: queued AND its family bit unmasked
// for `t`), or NULL. Same handler / self-managing gates. The EL0-return
// tail passes the returned name to exits() so the exit_msg reports WHICH
// signal terminated the Proc. Caller MUST hold p->notes->lock.
const char *notes_terminate_note_name_locked(struct Proc *p, struct Thread *t);

// The STOP-class twin (round-2 F2): the canonical name of the first note whose
// DEFAULT action is STOP and that is deliverable to `t` (queued AND its family
// bit unmasked), or NULL. Same reader gates as the terminate twin, except the
// disposition test goes through notes_proc_default_applies so a phenotyped
// Proc's sigtab is consulted.
//
// A stop-class note is QUEUED whenever the post-time fan (job_stop_cb)
// declined to apply the stop, and it posts on EVERY such reason: a handler or
// self-management catches it, every thread masks the family (the POSIX "a
// blocked stop signal becomes pending" case), or the Proc has no thread yet
// (the spawn window). The reader gates plus the per-note disposition gate keep
// the first out of this arm; this is where a pending stop lands once the mask
// lifts, and where a spawn-window ^Z -- posted with susp_stop_armed set -- is
// taken by the first thread's first EL0 return.
//
// PREDICATE ONLY, and NOT on the dispatcher's path -- the dispatcher calls
// notes_stop_dequeue_locked below, which answers the same question and
// consumes in one call. (It used to call this and then dequeue separately;
// that is the split that let the two disagree.) This is kept because the
// dispatcher takes no Thread argument, so a unit test can only drive the
// DECISION here -- but a test that drives only this is testing a function
// production no longer calls, so pair it with the consumer.
// Caller MUST hold p->notes->lock.
const char *notes_stop_note_name_locked(struct Proc *p, struct Thread *t);

// Consume the note notes_stop_note_name_locked selected. Returns 1 and writes
// `out` on a hit, 0 otherwise. Same gates, same class-filtered scan.
//
// It exists because the general notes_dequeue_locked is CLASS-BLIND: it pops
// the first mask-permitted entry in FIFO order, which is the queue head, not
// necessarily the note the stop decision was about. With [child_exit,
// tty:susp] queued, the tail stopped correctly and popped the child_exit into
// a stack local nobody reads -- destroying it -- while the tty:susp stayed
// queued to re-fire. Never reach for the general dequeue from a path that
// selected its note by class. Caller MUST hold p->notes->lock.
int notes_stop_dequeue_locked(struct Proc *p, struct Thread *t,
                              struct Note *out);

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

// item 11 (ARCH §8.8.3): the NON-death sibling of thread_die_pending. True iff a
// CAUGHT, deliverable note (a handler is installed OR the Proc self-manages its
// notes fd) of a family UNMASKED for `t` is queued -- so `t`'s caught-note-
// interruptible sleep should unwind SLEEP_NOTEINTR and return -T_E_INTR while
// LIVING (the note delivers at the EL0-return tail). LOCK-FREE, same shape as
// thread_die_pending; read only at the sleep sites' caught_ok arm, ALWAYS after
// the die-check (death wins). Disjoint from thread_die_pending: the caught
// latch and the terminate latch are never both set for one note.
bool thread_caught_note_deliverable(struct Thread *t);

// bug-2 (VIVARIUM 6.23): does `t` hold a PHENO_LINUX note handler that ESCAPED
// its frame -- siglongjmp'd to an ancestor sigsetjmp point without rt_sigreturn,
// so in_handler is stuck true and the N-3 re-entrancy guard would otherwise
// refuse every future caught-note delivery for the life of the guest? True iff
// in_handler is set, the Proc is PHENO_LINUX, and the current SP_EL0 (sp_el0,
// the caller's ctx->sp) has unwound AT OR ABOVE the pre-handler sp captured at
// delivery (note_saved_sp_el0). A live handler always runs BELOW that sp (the
// sigframe is pushed below it; nested/deep handlers only go lower), and a
// siglongjmp target must be an ANCESTOR frame ON THAT STACK (returned env is UB)
// -- older, hence higher -- so the discrimination is total for a single-stack
// guest, not a heuristic (the cross-stack swapcontext-to-a-higher-stack case is
// the documented F1 exception -- VIVARIUM 6.23 + the section-9 DEGRADED row).
// Both operands are the SP_EL0 bank. PURE: the caller clears in_handler on true.
// LOAD-BEARING: sigaltstack(132) stays ENOSYS (else an alt-stack handler runs at
// an unrelated sp and the compare is meaningless).
bool thread_note_handler_escaped(const struct Thread *t, u64 sp_el0);

// 11b-9p (item 11): does `p`'s reader handle -T_E_INTR? A caught-note wait unwind
// returns EINTR to userspace, and a reader that does not expect it breaks (the
// native ut $(cmd) capture, 11b-core's build bug). PHENO_LINUX programs go through
// musl, which is EINTR-aware by construction (POSIX); native (libthyla-rs) readers
// are NOT until they retry -T_E_INTR (11c). So the caught-note sleep unwind is gated
// on this in the sched caught branch: only an EINTR-ready Proc unwinds; a native
// reader's opted-in sleep degrades to death-only (the note delivers late, the
// pre-item-11 behavior -- no regression). Widens to native per-reader at 11c.
bool proc_caught_note_eintr_ready(struct Proc *p);

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
