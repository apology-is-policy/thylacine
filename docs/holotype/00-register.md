# HOLOTYPE register

One row per finding, appended per RW (`docs/HOLOTYPE.md` §5). IDs are
`HT<NN>.F<n>` where NN = the RW number. Severity: soundness P0–P3;
non-soundness (T/P/X/G lenses) H1–H4. Every actionable row is also enqueued
as a tracked task at confirmation time — this register is the RW-13 triage
input, not the queue. Dispositions: FIXED (in-arc, with regression),
ACCEPTED (closed-with-justification + doc caveat), TRACKED (#N), REGISTERED
(non-soundness; triaged at RW-13).

| ID | Area | Lens | Sev | Where | Finding | Disposition |
|---|---|---|---|---|---|---|
| HT00.F1 | ut session shell (LS-5) | S | P1 | `usr/utopia` (ut main) | Fresh-prompt Ctrl-C logged the session out: the session shell is console owner (LS-5a) but opened its note queue LAZILY (first keystroke), so an uncaught `interrupt` before any input took the LS-5b default-terminate; login treats the shell's exit as logout | FIXED `@f145ce8` (eager open gated on `io::stdout_is_live()`; the bare-spawn boot check deliberately stays lazy — an eager open there would mint the note fd as fd 0). Regression: `tools/interactive/ls-5.exp` case A |
| HT00.F2 | kernel notes | S | P3 | `kernel/notes.c` (`notes_post`) | A console Ctrl-C is a synthetic `interrupt` post; a queue already holding NOTE_QUEUE_DEPTH (16) non-interrupt notes gives the coalesce pass nothing to overwrite → -EAGAIN → the Ctrl-C is dropped AND the LS-5c terminate latch never arms (the arm rides a landed post) | ACCEPTED at RW-0 close + `notes.h` known-caveat. Unreachable for a typical foreground coreutil (precondition: 16 queued unconsumed notes); revisit with any queue-pressure poster (e.g. reserve the head slot for interrupt/kill) |
| HT00.F3 | kernel notes | S | P3 | `kernel/notes.c` (`thread_die_pending`) | The lock-free latch leg had no kproc guard; correctness rested entirely on the single arm site (`notes_arm_intr_terminate_locked`) refusing kproc — a future arm path that forgot would put every kernel kthread sleep into perpetual `*_INTR` unwind (kproc threads never EL0-return, so the latch could never be consumed) | FIXED at RW-0 close (kproc skip on the latch leg; the wake `proc_interrupt_terminate_wake` already carried its own belt) + a forced-flag leg in `notes.die_pending_predicate` |
| HT00.F4 | kernel notes | S | P3 | `kernel/notes.c` + the latch consumers | Multi-thread disposition race: thread B registering a handler (SYS_NOTIFY) or opening the notes fd (self-managing) between an interrupt post and blocked thread A's resume → A takes one spurious `*_INTR` (e.g. a 9P RPC surfaces -P9_E_IO). The EL0 tail re-validates, so never a wrong termination | ACCEPTED at RW-0 close + `notes.h` known-caveat. v1.0-unreachable: multi-thread Procs are stratumd-class, never console-owner/foreground; POSIX parallel = EINTR-with-handler |
