---
id: sub-kernel-notes
type: sub
parent: moc-kernel-ipc-wake
title: "Notes — asynchronous events as a file, with a handler path bolted on for POSIX"
code: [kernel/notes.c, kernel/include/thylacine/notes.h, kernel/devnotes.c]
audit: hard
guarded-by: [inv-i19, inv-i9, inv-i39]
validated-by: [prose, gate-smp]
locks: []
hazards: []
abis: [abi-note-names]
design: ["docs/ARCHITECTURE.md", "docs/ERRORS.md"]
created: 2026-08-03
updated: 2026-11-09
---
## Purpose

Signals, re-decided. A note is an asynchronous event delivered to a Proc, and
the design conviction is that it should be **a file first**: a Proc opens a note
file, reads fixed-size records, and handles events in the same loop as
everything else. No async-signal-safety discipline, no re-entrancy hazards, no
`siginfo_t`.

The handler path — where the kernel rewrites a thread's user context to land in
a callback — exists because ported software expects it. It is the second
citizen here, and the file says so.

## Contract

**Imperium/session integration (2026-09-17).** Trusted SAK episode delivery
and session hangup use the existing note wake discipline. The hangup note is
registered by login's console shell to exit on session teardown; it must not
be interpreted as an ordinary foreground interrupt. The held process-table
lock to note-queue lock ordering is unchanged; episode ownership is specified
in [[sub-kernel-cons]].

**A new thread starts with its creator's mask (2026-10-29, operator vote;
thread-model C7; ARCH 7.6.2).** Both spawn arms -- `SYS_THREAD_SPAWN` and the
phenotype's clone -- copy the creating thread's `note_mask` into the new thread
before the admit links it, as POSIX `pthread_create` does. The queue is
Proc-directed, and since C7 every first entry takes the EL0-return tail, whose
notes leg runs before the entry's first instruction: a thread born unmasked would
take a note every other thread masks -- a `pipe` queued while all of them mask it
-- before it could mask itself. Before the vote a native thread started at zero
(the rfork rule, which still governs a new Proc), and the Go runtime, whose
`minit` makes a syscall before it masks `pipe`, died at its next new M. Only the
mask crosses: the handler snapshot a fork copies stays behind, since a new thread
runs on its own stack, not inside a handler.

Three paths, **one queue**:

| path | how | who uses it |
|---|---|---|
| fd-shaped | open a note file, `read()` 32-byte records, `poll()` for free | native daemons; anything with an event loop |
| async handler | register a callback; the kernel rewrites the return-to-userspace context | ported code expecting signals |
| **Linux-phenotype frame** | the disposition lives in a per-Proc signal table, not in `handler_va`; a separate deliverer builds a Linux-shaped frame | a phenotyped guest, which never calls the native register syscall |

The third path is not a third *queue* — it is a second **frame shape** over the
same one queue, and it sits *above* the native handler branch in the deliverer
for a specific reason recorded below. Since item 11c the table it reads is not the
phenotype's alone: `SYS_NOTE_DISPOSE` writes it for a native Proc, and the tail then
enters `handler_va` for a caught row and takes the default for every other.

**Eight** families of note name (this count has lagged its table three times --
four against five rows until 2026-08-16, five against six after `pipe`, and
`sak` was missing from the table until 2026-10-13 -- so count the rows), distinguished by **who may post** and **what an uncaught one does**:

| family | postable by | uncaught default |
|---|---|---|
| `interrupt` | anyone | terminate (unless the Proc reads its own notes) |
| `term` | anyone | terminate (unless the Proc reads its own notes); SIGTERM's note since 2026-10-13 |
| `usr1`, `usr2` | anyone | terminate (unless the Proc reads its own notes); SIGUSR1's and SIGUSR2's since 2026-11-02 |
| `kill` | anyone | terminate, **uncatchable** |
| `pipe` | kernel | terminate (SIGPIPE's default; a reader-less write posts it) |
| `child_exit` | kernel | nothing — queue for the fd reader; where a table governs, discarded at the tail |
| `tty:*` | **kernel only**, catchable | quit/hup terminate; susp stops; winch/cont informational |
| `sak` | **kernel only** (the trusted path, I-27) | nothing — catchable, ignored by default, so the TCB is never ended by it |
| `snare:*` | **kernel only** | terminate — and not deliverable at all today |

Each terminating death records its POSIX signal for a packed wait (below,
"Every death by note records its signal").

The two prefix gates are not decoration. `notes_post` rejects any userspace post
of a `tty:`- or `snare:`-prefixed name, and for `tty:` **that gate is the only
barrier**: those names *are* in the known-note table, so without it `tty:cont`
would be postable through the ordinary parent-to-child path, letting an
unprivileged parent resume a debugger-stopped child. That is an
[[inv-i39]] hole closed by a string comparison, and the source labels it
load-bearing.

`notes_post` answers `-T_E_INVAL` for every name it refuses (no note, a
`tty:`/`snare:` prefix or `sak` from a userspace poster) and `-T_E_AGAIN` for
a full queue -- the values its header always documented; until S-3
(2026-10-14) the code answered a bare -1 to both, and `SYS_POSTNOTE` passed
it on ([[sub-kernel-syscall-dispatch]], [[dec-2026-10-14-postnote-errnos]]).
The userspace name gates live in one PURE predicate,
`notes_name_user_postable(name)`, which `notes_post` asks of every
non-synthetic post and `SYS_POSTNOTE`'s cross arm asks before its walk, so a
refused name is refused even where the walk would answer without posting (a
dead child). The cross arm skips it for a NULL name: since B-3b (vote 6,
[[dec-2026-10-14-postnote-probe-getppid]]) an empty `SYS_POSTNOTE` name is
the probe, the same NULL-name walk the phenotype's `kill(pid, 0)` uses,
answered after the parent gate with nothing posted.
No `snare:` name is ever queued, but a Proc may END itself with one:
`notes_self_death_snare(name)` returns the kernel's own literal for
`snare:segv`, `snare:bus`, `snare:ill`, `snare:fpe` and `snare:abort` (NULL
for everything else, `align`, `bti` and `brk` included), and `SYS_POSTNOTE`'s
self arm calls `proc_fault_terminate` with it
([[dec-2026-10-14-self-death-snare]]). The set is the Linux phenotype's
self-death set; `notes.self_death_snare` compares the two row by row over
the death table.

The `snare:` gate is future-proofing by comparison: those names are not in the
table, so the name lookup already rejects them. The gate exists so the rejection
survives someone adding them later.

## Mechanism

**The queue** is a 16-entry ring per Proc, allocated with the Proc and freed
with it. Fixed depth is a deliberate bound: an unbounded event queue is a memory
DoS reachable by any Proc that can post.

Which raises the question a bounded queue always raises — what happens when it
is full? The answer is split by who is posting. **Kernel-synthetic** posters
(child exit, pipe-write-to-closed) *coalesce*: past a threshold, a new note of a
name already queued merges into the existing entry, keeping the most recent
argument. That makes kernel delivery contractually infallible. **Userspace**
posters get a plain "try again" — coalescing is a relief valve for the kernel,
not a promise to userspace.

**Delivery happens at the return-to-userspace tail**, after the syscall's return
value is written and before the return itself. That placement is the whole
async-safety story: the thread holds no kernel lock and is at a clean boundary,
so rewriting its context is safe.

The dispatcher, in order:

1. **Validate the user stack pointer** — before anything else, and deliberately
   so. The handler frame is pushed onto the user stack, and the stack pointer at
   this point is whatever userspace set. Without the bound, a Proc could point
   it at a kernel address and have the kernel write the note name there. The
   check runs *first* so a bogus stack pointer cannot cause a missed delivery
   decision; a queued `kill` simply waits for the pointer to become sane.
2. **Peek under the queue lock.** Kill first, ignoring the mask; then the first
   mask-permitted entry in order.
3. **If it is `kill`**: pop it under the same lock, then — having dropped the
   queue lock, because the process-table lock outranks it — check for live peer
   threads. No peers: terminate. Peers: put it back at the head. If putting it
   back **fails**, extinct: losing a kill after the poster was told it succeeded
   is an invariant violation, and crashing loudly beats surviving a kill.
4. **Otherwise honour the re-entrancy guard**, then either terminate on an
   uncaught default-terminate note, or leave it queued for the fd reader.
5. **Or build the handler frame**: pop, compute the frame address, push the name
   onto the user stack, save the user context into the thread, mark it
   in-handler, and rewrite the return context to land at the handler.

### The save was not the whole machine, and this note said it was — FIXED

**Disposition first, because the defect is the interesting half and that is
exactly how a disposition gets dropped:** this was #96 and it is **closed in
the tree**. Both delivery paths now save the FP/SIMD bank and share one
restore; the exec path zeros the area; a test asserts the restore actually
restores. What follows is the defect and why it hid, in the past tense
throughout.

An earlier revision of this section said delivery saves "the **entire** user
context". It did not, and that word was the defect in one syllable: the save
was the general registers, the user stack pointer, the link register and the
status register — and **not V0-V31, FPSR or FPCR**.

A note handler runs on the *same thread* as the code it interrupts, with **no
context switch**, so the context switcher's eager FP save never fires on this
path. Nothing else saved them. The first thing a handler did that touched a
vector register silently corrupted the interrupted computation — and that is
not exotic: any float arithmetic, any autovectorised `memcpy`, any `printf`
with a `%f` does it. Never an authority question, since the registers are the
Proc's own; **silent data corruption**, which had been live on the native path
from the signals sub-chunk until #96 closed it, and which ordinary compiled C
made reachable once the phenotype arrived.

**Why it hid for that long is the part worth generalising.** The four save
lines *were* exhaustive — with respect to `struct exception_context`. Every
reader who asked "does delivery save the context?" looked at the exception
frame, enumerated its fields, and found the enumeration complete. The FP
registers are not in that frame; they are preserved by a **different
mechanism** for a different reason, and that mechanism is entirely correct in
its own domain. **A save is complete with respect to a STRUCTURE, never with
respect to a MACHINE** — and state held by some other correct mechanism is
exactly the state an enumeration of the obvious structure cannot see.

The fix is a 520-byte 16-byte-aligned area carried **inline** on the thread
(`struct Thread` 1232 → 1760), and inline rather than allocated because
delivery must be alloc-free: an allocation failure mid-delivery would silently
drop the handler invocation. The existing switch-out FP slot **cannot** serve —
preempt the handler and the context switcher stores the *handler's* state
there, destroying the snapshot.

There are now **two save sites and one restore** — the phenotype deliverer and
the native tail, sharing the restore. The failure mode of missing one is worse
than having no fix at all: the still-live restore writes a *zeroed* area into
V0-V31. That was observed rather than argued (the guest leg reports V0 = 0, not
the handler's pattern), and both sites were revert-probed independently, each
failing at its own assertion. The pairing is argued as exhaustive by
construction: the in-handler flag is written in exactly three places — true at
each save site in straight-line code immediately after it, false at the restore
— and the restore returns early unless the flag is set and has one caller.

**Steps 2, 5's pop, and 5's push all happen under one lock hold**, and each was
a separate audit finding. Peek-then-pop split lets the other path steal the
note; pop-then-push split loses it on a failed push. Every failure inside the
hold re-enqueues at the head.

### The note NAME is the signal identity, and that bounds the POSIX mapping

The clearest thing the phenotype work established about this layer is a
property of the original design: **a note carries no signal number.** Its name
*is* its identity. So any N:1 mapping from signals onto one note name is
representable right up until dispositions exist, and then it is not.

The worked case: mapping both SIGINT and SIGTERM onto `interrupt` was recorded
as "a stated imprecision, not an oversight". Building `sigaction` showed it is
not imprecise but **unrepresentable**. A guest that ignores SIGINT while
leaving SIGTERM at default has no correct answer — honour the ignore and
SIGTERM goes silent too; refuse it and a Proc that asked to ignore Ctrl-C dies
on Ctrl-C. Both directions wrong, and it is exactly the call a shell makes. So
`interrupt` belongs to SIGINT alone (it *is* the Ctrl-C note) and SIGTERM
declined until it had a name of its own. It has one since 2026-10-13: `term`
([[dec-2026-10-13-term-note]]; Plan 9 APE's sigtab spells SIGTERM the same
way), catchable, terminating by default, with its own mask bit
(`NOTE_BIT_TERM`, 7), its own caught bit and
claim bit, and its own terminate latch -- so ignoring or masking Ctrl-C leaves
SIGTERM's disposition alone, as POSIX keeps them. `usr1` and `usr2` have the same
shape on bits 8 and 9 (2026-11-02, operator vote), so `NOTE_MASK_SUPPORTED` is 0x3ff.

The lesson generalises past this pair: in a design where the name is the
identity, **collapsing two identities onto one name is a decision that cannot
be un-made downstream** — no later layer can recover the distinction, because
the distinguishing bit was never carried.

### An ignored signal is discarded at generation, not at delivery

A phenotyped Proc's `SIG_IGN` disposition drops the note **inside the post**,
before it ever reaches the queue, and the post still reports success — matching
Linux, where `kill()` to a process ignoring the signal succeeds.

Post-time rather than delivery-time is load-bearing and the first attempt had
it the other way. An ignored note that reached the queue would occupy one of
the sixteen slots; it would **arm the terminate latch** (a Proc that ignores a
signal has no handler and is not self-managing, so it passes every arm gate);
and it would leave blocked threads unwinding `*_INTR` until the return tail got
round to dropping it. Never posting touches none of that machinery. **A drop at
the edge is not the same operation as a drop at the centre** when everything in
between has side effects on arrival.

### The phenotype branch sits above the native one, deliberately

"Does this Proc have a live handler?" — the question the uncaught-default logic
turns on — is answered by the native registration address, which is **0 for
every phenotyped Proc**, because a Linux guest never calls the native register
syscall. Its handler lives in the per-Proc signal table instead. So the
"someone will catch this, do not treat it as uncaught" exemption never applied
to a phenotyped Proc, and the helper now consults the table whenever one governs
(`notes_table_governs`: the Linux phenotype, or a native Proc's first
`SYS_NOTE_DISPOSE`).

Left unfixed that was a **hang, not a fidelity gap**: the latch makes every
unmasked thread's sleep return interrupted at once, and while the delivery
success path drains the latch and self-corrects, the frame-push failure arms do
not. Worth keeping as a shape — *a fidelity gap in an exemption check becomes a
liveness bug when the thing being exempted is a latch.*

### A table governs a native Proc too (item 11c, 2026-11-03)

Operator votes 18, 19 and 20 ([[dec-2026-11-03-note-dispose-table]], ARCH 8.8.3). What
a note does is read from one per-note table, `struct viv_sigtab`, for the Linux
phenotype and for a native Proc alike. `SYS_NOTE_DISPOSE` (139) writes a native row:
default, ignore or caught, with a restart flag on a caught one. The first call sets
`PROC_FLAG_NOTE_TABLE` (bit 25), and the table governs until the next exec. A native
fork copies the flag, the table and `handler_va`, as Plan 9's `sysrfork` copies
`p->notify`; exec clears all three. `notes_table_governs` is the one spelling of the
question, and every disposition reader asks it, never the phenotype. `SYS_NOTIFY` with
no table keeps Plan 9's shape: the handler catches every note and no wait unwinds.

A default row needs no table, so a governed Proc may have `sigtab` NULL, and every note
then takes its default. A Pouch image's constructor writes SIGPIPE's ignore row, which
allocates the table, and `child_exit`'s default row, which does not, so the Proc is governed
either way ([[sub-pouch-signal]]) and the tail discards a dead child's note. With no table and no handler that note waits for a notes fd nobody opened, and
twelve of the sixteen slots fill before such notes coalesce.

**A native row holds a token, and a token needs a taker.** The caught value is
`VIV_NATIVE_CAUGHT`, never an address, and exec resets a live row whichever phenotype
comes next, so a native row cannot reach a sigframe as a handler. `SYS_NOTE_DISPOSE`
refuses a caught row with neither a handler nor a notes fd, but that gate binds only at
the write: a fork carries the row and leaves the notes fd's mark behind, and a peer's
`SYS_NOTIFY(0)` can cross a dispose that passed it. So `notes_row_has_taker` asks again,
in `notes_proc_has_live_handler` and on the tail's row, and a stale token takes the
discard, terminate and stop arms as a default row does. A note under one otherwise has
no consumer, and SIGTERM and ^C cannot end the Proc. A guest's row is an address,
whatever its value. rfork reads the flag once, for the clone and the publication, and
the handler after it, so a child that takes a row also takes what it was written against.

**A disposition write re-reads what is pending** (`notes_disposition_reread`, from
`viv_sigaction_settled`, `notes_set_handler`, `notes_handler_resets` and the end of
`proc_exec_drop_image_state`). A note posted before the write was judged by the old row.
Under `q->lock` the re-read walks the queue and sets both latches to what the rows now
say; with the lock dropped it runs the terminate wake, the caught wake and
`proc_note_direct_rouse`. That closes [[seam-queue-note-fatal-after-post-wakes-nobody]]:
a note a wait rode out now ends the wait when its row turns fatal. Exec commits
`p->phenotype` before the drop, and the drop's test hook does not, so a test stores it.

**The terminate latches** are one per family (`interrupt`, `term`, `pipe`, `usr1`, `usr2`),
except the tty family, which latches per note: `PROC_FLAG_QUIT_TERMINATE_PENDING` for
`tty:quit` and `PROC_FLAG_HUP_TERMINATE_PENDING` for `tty:hup`, both paired with
`NOTE_BIT_TTY` in `thread_die_pending` (`notes_name_terminate_latch`). A latch names one
note, which ARCH 8.8.2's committed death relies on, since the two die with different
signals; a latch drains when the last queued note of its name goes.

**Which native calls a note may unwind** is `native_note_class` (syscall.c). It runs
only where a table governs and there is a handler or a notes fd, so a program that only
ignores SIGPIPE pays no stat per open file.

| call | unwinds | restarts |
|---|---|---|
| `READ`, `WRITE`, `PREAD`, `PWRITE` on a slow file (`file_is_slow`: fifo, char device, socket) | yes | yes |
| `WAIT_PID`, `SRV_ACCEPT` | yes | yes |
| `TORPOR_WAIT` | yes | only with no deadline (`regs[2] < 0`) |
| `POLL` | yes | no |
| `OPEN` with `SYS_WALK_OPEN_OINTR` | yes | yes |

Vote 20's bit is how an open that waits for a peer says so: Pouch's `accept` and
`connect` set it on the one open that does, and a regular open stays uninterruptible.
The bit covers the open's wait and not the walk to it, and so does the flag a Linux
dial sets: [[sub-kernel-stalk]] holds `note_interruptible` clear through the walk and
gives it back only around the final `dev->open`, because a failed walk has no errno to
carry `EINTR` and would read as `ENOENT`.
A self-managing Proc writes a caught row to let a note interrupt a listed wait; the note
stays for the fd, and with no row nothing unwinds.

**The kernel re-issues a restartable call (vote 19), for Pouch and for Linux guests.**
Musl re-issues nothing, and the claim that it did was false. The dispatcher records
`note_call_x0` and `note_restartable` at entry: Linux's from the positive list
`vivarium_call_restartable`, a native's from the table above. `thread_caught_note_unwinds`
sets the per-thread mark `note_call_unwound` when a wait really unwinds, and
`notes_call_restarts` asks the mark, the class and `regs[0] == -T_E_INTR`, never the
register alone: a server's `Rlerror(EINTR)` is an error to report, so `client_run`
clears the mark when a flush wait ends on the op's own reply. `notes_call_rewind` moves
`elr` back over the `svc` and restores `x0` before any frame or save exists, so a
handler's return re-issues the call. Linux delivery rewinds ahead of the frame and undoes
it if the copy-out or the consume fails; native delivery rewinds once the name is pushed.
A tail that delivers nothing (the row turned default or ignore while the wait unwound)
runs `notes_call_settle`, which rewinds unless a caught note is still pending for this
thread, since a handler then answers for the call. Every path clears the mark. This
closes the DEGRADED row of VIVARIUM 6.22.

**A fatal note does not wait out a handler.** The tail's `in_handler` guard sits above
the delivery body and now scans for a terminating note first
(`notes_terminate_note_name_locked`, which reads the mask), so a default-row fatal note
ends the Proc from inside a handler, as on Linux. It also has to: a native handler that
`longjmp`s out never reaches `SYS_NOTED`, `thread_note_handler_escaped` stays Linux-only
because a native runtime may run its handler on another stack, and with the latch armed
every wait would return at once for as long as the Proc spun.

### A caught note interrupts a blocked peer's wait (item 11)

A note with a live handler must run at the return tail — but a peer thread
blocked in a syscall reaches no return tail until something wakes it, and a
thread spinning at EL0 reaches one only at an interrupt. So a caught note ARMS a
per-Proc mask (`notes_arm_caught_note_locked`, the caught latch: the low half of
`Proc.note_caught`), and the posting walk (`proc_caught_note_wake`, under the
process-table lock) chooses the thread that takes it and wakes or kicks that
thread (next section); a stopped thread waits for the stop to clear.
`notes_post` cannot run that walk itself -- it needs the process-table lock,
which `notes_post` does not take -- so it is the POSTER's, and every poster that
holds the lock runs it: the interrupt and tty
fans beside the terminate wake, and the posts with no terminate twin -- a
child's exit (`proc_exit_notify_parent_locked`), a caught `tty:susp`, `tty:cont`
(the continue fan, and the orphan rule after both of its posts). Two posts run
outside the lock: a Proc noting itself takes the lock for its wakes after the
post, and `pipe` needs none -- it posts to the writer's own Proc from inside
the write, which delivers at that thread's return tail, as Linux sends SIGPIPE
to the writing thread alone. One caught note unwinds at most ONE sleeper. An unwinding sleeper claims the
note's family in the claim half, the high half of `Proc.note_caught` (`thread_caught_note_claim`,
the sleep arms' last test) by a CAS that re-validates the caught bit, and
records the family as its own (`Thread.note_claim`); its peers find the family
claimed and stay parked. In a Linux-phenotype Proc the walk usually takes that
claim on its pick's behalf (a designation, `Thread.note_target`), and the
designee's wait turns it into its own claim with no CAS. A claim or a
designation ends at its thread's EL0-return tail whatever that tail delivered:
`notes_deliver_at_el0_return` runs the delivery body (`notes_deliver_tail`),
once more after each stop it applied (below), and then `notes_release_claims`,
which clears the thread's claim and designation, ANDs their bits out of the
Proc's claim half and, if a family is still caught -- the tail ended short
of its note -- walks again, skipping this thread, whose tail has just passed the
note by. A thread's own claim stays open to it, so a wait that claimed and then
completed anyway cannot make the call's next wait refuse the note only it may
unwind for. The drain of a family's last queued note clears the caught bit
alone. Exec clears the claim half, the claim and the designation under the
thread's `wait_lock` (exec runs alone, but a walk can have designated it). A
thread that exits gives up both at its `THREAD_EXITING` commit, under its
Proc's thread lock (`notes_exit_drop_locked`), and walks again if a family's
note is still queued. The caught-note walks run under that lock
([[lock-proc-threads]]) and take no note queue's lock.
Each woken thread re-checks a lock-free predicate,
`thread_caught_note_deliverable`: a caught note is deliverable to *this* thread
iff it is armed AND unmasked by the thread's own `note_mask` AND in the supported
set (`caught & ~note_mask & NOTE_MASK_SUPPORTED`). Deliverable → the wait returns
`*_INTR` and the handler runs at the tail; not → the thread re-parks, its wait
intact.

The predicate is what closes the **arm-2 livelock** (the N-3 guard): a caught
note the re-entrancy guard will refuse — because a handler is already running —
is *not* deliverable, so a thread must not wake for it. Waking anyway would find
nothing to deliver, re-sleep, and be re-woken forever. The register-then-observe
discipline is [[inv-i9]]'s, shared with the death-wake and the report latches:
the arm stores under the lock the predicate re-reads under, so no wake is lost
between the check and the sleep.

**Only a wait its syscall marked interruptible unwinds (signal(7)'s list,
2026-09-29).** The predicate is also false unless the waiting thread's
`note_interruptible` is set. That flag is positive and false by default. The
vivarium dispatcher sets it for a Linux call that Linux lets a signal
interrupt; `syscall_dispatch` clears it on the way out; `userland_demand_page`
clears it around a page-in and restores it after. So `socket()`, `openat()`, a
regular file's `read()` and every page-in ride a caught note out, and the
handler runs at the call's return tail. Before this, both 9P waits (the RPC
wait and the elected reader's receive) unwound for any caught note, so a
`SIGCHLD` handler failed those calls with `EINTR` and a file page-in with
`SIGBUS`. Death is untouched: `thread_die_pending` never reads the flag
([[dec-2026-09-29-caught-signal-slow-calls]]). A native call sets it from
`native_note_class` (above).

**Every kernel wait a listed call reaches opts in (2026-10-05).** The flag is
necessary, not sufficient: the wait must opt in too, and until
[[chg-2026-10-05-signal7-list]] only the two 9P waits did, so a listed call
blocked anywhere else rode the note out. Now every wait a listed call can reach
opts in, and each unwinds with nothing consumed and returns `-T_E_INTR`: a
pipe's read and write ([[sub-kernel-pipe]], kept off an elected 9P reader's
un-opted receive), the console's read and write waits ([[sub-kernel-cons]]),
`ppoll`/`pselect6`'s park and its timeout-only sleep, which is musl's `pause()`
([[sub-kernel-poll]]), `wait4` (`WAIT_PID_NOTEINTR`, [[sub-kernel-proc]]) and
`futex` (`TORPOR_ERR_EINTR`, [[sub-kernel-torpor]]). One predicate decides,
`thread_caught_note_unwinds`: a table governs (`proc_caught_note_eintr_ready`)
and the claim won (`thread_caught_note_claim`, which re-runs the deliverable test
above). A 9P reader is not excepted: it unwinds at any byte of a frame, the client
keeping the partial frame (ARCH 8.8.1.1; until 2026-10-06 a reader mid-frame was,
`thread_reader_blocks_death`). The
four caught arms in `sleep_common` and `tsleep_common` call it, after the cond
re-test, the deadline, the stop detour and the die-check, and so does poll's
loop-level verdict, where readiness wins, then the note, then the deadline, as
in Linux's `do_poll`. A `noteintr` caller RETURNS on `NOTEINTR`: the claim lasts
until its EL0-return tail, so a second wait in the same call would unwind at
once and spin. Three waits a listed call reaches stay out, each for a reason
(ARCH 8.8.3): the 9P send side (an unwind drains nothing, so the retry spins),
`poll`'s settle (bounded by the server's answer or the fail-safe), and the notes
fd's read (its data is the queued note, so a post readies it). Under `SA_RESTART`
the kernel re-issues what signal(7) restarts (above).

**The claim ends at the tail, not at the drain (VIV-EINTR round-2 F1, P1).** The
first version cleared a claim with its family's last drain, on the argument that
the claimant's tail delivers the note on its very next EL0 return. That holds for
the handler arm alone. The tail handles ONE note per return, and it can end short
of the claimed one: it discards a `SIG_DFL` default-ignore note queued in front (a
`child_exit` under a default `SIGCHLD`, a `tty:winch`), it stops, or the frame
will not build. The claim then outlived the unwind with nothing left to drain the
family, and every later wait -- the claimant's own retry included -- refused the
claimed family and parked: Ctrl-C was dead in a blocked `recv`. Two changes close
it. The claim is released by its owner's tail, as above. And the tail now loops
past a discarded note to the next one, as Linux's `get_signal` does, bounded by
`NOTE_QUEUE_DEPTH` so a flood of ignored notes cannot hold the thread there.
Since 2026-10-05 it also passes over the queue again after a stop it applied,
once the thread has parked for it, and those passes spend the same budget as
the discards ([[spec-tail-order]]). *An
argument from "every return delivers" must say what each return handles.*
Witnesses: `rendez.caught_note_tail_discards_and_releases` (the reviewer's chain on
the real tail: two ignored notes and a caught one, the tail loops to the caught
one, and the retried wait unwinds again), `rendez.caught_note_release_wakes_peer`
(the tail held until the peer has re-parked, so only the release's wake can
unwind it), and `notes.caught_note_claim_once`.

**Why the caught latch and the claim live in their own word.** Both were 7-bit
sub-fields of `proc_flags` until `term` needed an eighth bit in each and
`proc_flags` (a u32) had none free. They moved together into
`Proc.note_caught`, a u16 then and a u32 since `usr1` and `usr2` took bits 8 and 9
(2026-11-02; `term_sig` and `group_term_sig` left that slot for the pad after
`supp_gid_count`, so the struct has not grown): the latch is the low half
(`CAUGHT_LATCH_SHIFT`/`_MASK`), the claim the high half
(`CAUGHT_CLAIM_SHIFT`/`_MASK`). One word, because the
claim's CAS re-validates the caught bit. The move is sound because every
reader and writer touches those two fields alone (the arm, the drain, the
deliverable test, the claim CAS, the family drop, the exit drop, the
designation CAS, the walk, the pending test, exec's clear): none needs one
atomic snapshot of a sub-field together with another `proc_flags` bit.
The macros were renamed rather than kept, so code that still applies them to
`proc_flags` cannot compile: one test armed a tty caught bit through
`proc_flags` with the old shift, which would have set `PROC_FLAG_LEGATE_ROOT`
silently. `tools/check-flag-words.py` (build.sh) derives every `proc_flags`
define from the header and fails on a shared bit, so the freed bits cannot be
taken twice; `PROC_FLAGS_ALL`'s popcount `_Static_assert` repeats that at
compile time for the flags it lists.

### Which thread takes a caught note, and every EL0 return delivers it (2026-10-12)

Two defects shared one design ([[dec-2026-10-12-note-target]], ARCH 8.8.3). The
IRQ-from-EL0 tail had no notes leg (#964, [[seam-el0-irq-tail-no-notes]]), so a
thread that never entered the kernel never took a note: Ctrl-C did not end
`while(1);`, and a caught handler never ran. And the walk woke every blocked
thread, so a child's `SIGCHLD` returned `EINTR` from a worker's `ppoll` while
main was running (L285).

**Every EL0 return delivers.** The 0x480 vector branches into `.Lel0_return`,
the tail the synchronous vector takes ([[sub-kernel-exception]]), so the notes
leg runs on an IRQ return too, at any EL0 instruction; the whole-context save
already covered that. Delivery clears `SPSR.BTYPE` in the handler's context,
after the save and in both arms: an IRQ can arrive between an indirect branch
and its target, and the handler is not that target. The restore puts the saved
value back.

**The pick.** For each caught family queued and held by nobody,
`proc_caught_note_walk` picks one candidate: not exiting, not doomed by another
thread's exec (`proc_exec_dooms`, thread-model C5; the walk passes its own Proc,
since a test fixture may link a Thread whose `proc` is NULL), the family unmasked,
no handler running, and not in a wait that holds. It prefers the posting
thread, then main (the oldest live thread, `proc_main_thread_locked`:
`p->threads` is head-inserted, so the last one not exiting), any thread not
blocked, main blocked in an unwinding
wait, and any thread blocked in one. In a Linux-phenotype Proc it designates the
pick under the pick's `wait_lock`: it re-checks the pick there, CASes the
family's claim bit while the family is still caught and unclaimed, sets
`note_target`, and wakes the pick if it is in an unwinding wait or kicks it
(`sched_kick_thread`: `IPI_RESCHED` to the CPU it runs on) if not. A pick that
went stale is picked again up to `CAUGHT_REPICKS` (4) times, after which the walk
wakes every unwinding sleeper and lets one claim. A native Proc's walk only
kicks, since no native wait unwinds for a caught note. The pick decides who is
interrupted or kicked, not who may deliver: any thread's tail still delivers a
queued note whose family it has unmasked.

**The wait kind and the hand-off.** `sleep_common` and `tsleep_common` publish
the wait's kind (`Thread.caught_wait`: none, unwinds, holds) under the thread's
`wait_lock` before anything else (`sleep_caught_wait_enter`), and restore the
outer kind on the way out, since a stop park nests a wait inside a wait.
`thread_caught_wait_kind` answers "unwinds" exactly when every gate of
`thread_caught_note_unwinds` that cannot change during the wait passes. Whether a
table governs the Proc can change, since a peer's first row can come while the
wait stands, so the kind does not read it: the walk designates nobody until one
does, and from then on it finds that wait. A
designation the wait cannot honour -- every family when it holds, a masked one
when it unwinds -- is handed off before the thread blocks
(`notes_handoff_target`): the wait lock is dropped, the families leave the
thread and the Proc's claim sub-field, and the walk runs again. The walk
designates under the same lock the kind is published under, so no thread is
designated into a wait that will not unwind.

**Re-wake.** Every walk first wakes a holder of a caught, held family that is
blocked in an unwinding wait: a claim can outlive its note when another
thread's tail delivered it, and the family's next note must still unwind it.

**Stale reads.** The mask and the handler flag are the thread's own and written
without a lock, so the walk reads them exactly only for a blocked thread. An
awake thread designated on a stale value gives the designation up at its tail
or its next wait.

Modelled in `specs/caught_target.tla` ([[spec-caught-target]]): `OneClaimant`,
and `NoStrand` (a caught note stays queued forever only if no thread stays able
to take it), with a buggy cfg per defect. The witnesses are under Tests.

### A note aimed at one thread (thread-model C4, 2026-11-02)

Until this chunk a thread-directed signal reached a Proc: `tkill` and `tgkill` found
the thread, then posted a note any unmasked thread could take, and musl's
`pthread_kill` answered ENOSYS (operator vote 5; ARCH 7.6.4). Now the named thread
takes it and no other (`NoStolenDirect`, [[spec-caught-target]]). The queue and the
group's death stay what they were; this is a word beside them.

**The word.** `Thread.note_direct` is a u16 with one bit per family. A bit can stand
for a name because every name a program may post owns its family (`notes_direct_bit`;
`notes.direct_names_own_their_family` walks the table to keep that true). It coalesces
as a Linux standard signal does. It carries no sender and no time: a phenotype's
siginfo says `SI_KERNEL` for every note, claiming nothing about either. `kill` never
lands here, since it ends the group wherever it is aimed. A new thread starts with
the word empty, so neither a thread spawn nor a fork inherits it; exec keeps it, as
it keeps the queue, so a caught note waiting behind a mask can become fatal when exec
resets the handler.

**The post.** `proc_note_thread` holds `p->threads_lock` and no queue lock. In one
hold it finds the tid (absent: ESRCH), does nothing for a thread past
`THREAD_EXITING` or a Proc past `ALIVE`, drops a note the phenotype ignores, and sets
the bit with an atomic OR. It then rouses the target under the target's `wait_lock`,
unless the target is the caller. A fatal note wakes any rendez but `debug_rendez` and
kicks the thread; a caught one goes through `caught_rouse_locked`.

It rouses a masked target too. The owner writes `note_mask` with no lock and no fence,
then reads the word acquire-only, and the post sets the word and would then read the
mask: the store-buffer shape, where ARMv8 lets each see the other's old value. A post
that skipped the rouse on the mask could leave a thread that had just unmasked asleep
over a word it read empty. The target's re-read under `wait_lock` is the precision, as
the queue's wakers already have it, and a masked sleeper re-reads and parks again.
`caught_target_buggy_direct_stale_mask.cfg` holds the bug.

A self-managing Proc reads its notes from an fd with no per-thread reader and runs no
handler, so there the note joins the queue with its sender. `notes_mark_self_managing`
moves words already set, through `notes_move_to_queue_locked`. Their posts already
answered 0, so the move cannot be refused: the ring has `NOTE_RING_SPARE` slots past
`NOTE_QUEUE_DEPTH` that no post may fill, and past the depth a moved note coalesces
with a queued note of its name or takes one. Only the move fills a spare slot, each
time with a name not then queued at sender 0, so a full ring holds a moved note of
every family and the slots cannot run out, and running out would extinct, as losing a
kill's re-enqueue does. A static assert keeps one per family. Moved notes count against
admission until they are read, so after a move a post can be refused until the reader
has drained them too.
Every ring walk's modulus is `NOTE_RING_SLOTS`; the admission and the tail's budgets
stay at the depth.

**The disposition.** The post reads `SIG_IGN` and sets the bit in one hold of
`threads_lock`. `rt_sigaction` stores the disposition, then `notes_discard_name` ends
its `q->lock` hold and walks the threads under `threads_lock`. Whichever hold comes
first, the note is refused or removed.

A disposition that becomes terminate has the same shape. `viv_sigaction_settled`
(`SIG_DFL` for a family that terminates), `notes_handler_resets` (`SA_RESETHAND` at a
delivery) and `notes_set_handler` (`SYS_NOTIFY(0)`) store it and then call
`proc_note_direct_rouse`, which under `threads_lock` rouses every other
thread with a nonzero word: a post that held the lock first has set a bit the walk
sees, and one that holds it after reads the new disposition itself. Without it a peer
riding a caught note out in a hold wait would sleep on, since the unregistering thread's
tail reads only its own word.

**The tail.** `notes_tail_candidate_locked` takes a queued kill, then the lowest
unmasked direct bit, then the queue's FIFO: private before shared, as Linux's
`dequeue_signal`. `q->lock` does not guard the word, so a peer's `rt_sigaction` can
take a bit between the peek and the consume. `notes_tail_consume_locked` is therefore
a `fetch_and`, last, and on a raced discard it returns 0 and nothing is delivered. A
native push that faults puts the bit back (`notes_tail_unconsume_locked`); a native
Proc has no `rt_sigaction` to discard it in the meantime. A peer may have opened the
note fd while it was out, so a Proc now self-managing gets it on the queue instead: the
mark stores its flag under `q->lock`, so a flag not seen there means the mark's walk is
yet to come and moves the bit itself.

**Death.** Whether a direct note is fatal is read at use (`notes_direct_is_fatal`),
never latched at the post, so exec's reset and `SYS_NOTIFY(0)` are honest.
`thread_die_pending` asks the word after the queue's latches: a fatal note ends every
wait of this thread, and reading it true commits the group's death by the note's own
latch (below, "A latch's unwind commits the death"), so the peers' waits end too, as
the group death the tail's `exits_note` then runs would end them. There is no second
termination primitive. A masked fatal note stays
pending until that thread unmasks, as in Linux, where `complete_signal`'s group exit
is not reached while `wants_signal` refuses.
`notes_terminate_pending_name_locked` scans the word first, so the fault arm's
`FAULT_USER_DYING` finds what the predicate saw.

**No claim.** `thread_caught_note_deliverable` and `thread_caught_note_claim` answer
true for an unmasked direct bit with a live handler, and touch neither `note_claim`
nor `note_target`. The claim keeps a note many threads could take to one sleeper; this
note has one taker, and `OneClaimant` would break if it took one. The bit stays until
the tail, so a 9P call that carries on after a lost flush reads it again and unwinds
again, as a claim holder does. The walk is unchanged. Only a phenotype's waits unwind
(`proc_caught_note_eintr_ready`); a native thread rides the wait out and takes the
note at its tail, as it does a queued caught note.

**The entries.** `viv_kill`'s self arm and `postnote_walk_cb`'s cross arm call
`proc_note_thread` when a tid is named, with their gates unchanged; `kill(pid)` stays
process-directed. `SYS_POSTNOTE_TID` ([[sub-kernel-syscall-abi]]) is the native
entry, and patch 0067 points Pouch's `pthread_kill` and `raise` at it
([[sub-pouch-thread]]).

### A fault under a terminate latch is the latch's death, at the fault

A user fault left unresolved while a terminate latch is pending reaches the
EL0 fault arm as `FAULT_USER_DYING` ([[sub-kernel-fault]],
[[sub-kernel-exception]]), and the arm does not wait for the return tail:
`notes_fault_dying_latch` takes `q->lock` and ends the Proc with
`exits_note()` of the note `notes_terminate_note_name_locked` picks, the tail's
own choice -- so the death records that note's signal (SIGPIPE for a `pipe`),
not the fault's. The tail
defers every note but `kill` while a handler runs and while `SP_EL0` is
unusable, and the faulting instruction would otherwise re-execute into the
same fault forever. Ending the thread inside a running handler matches Linux,
whose `complete_signal` makes a fatal default signal a group exit at send time.
A committed death is died of first, whatever the queue holds now: a handler
installed since the commit leaves the pick empty. When no queued note of an armed
class would still default-terminate (a handler installed for it since the arm,
before the disposition's re-read clears the latch), those
latches are stale: cleared under the lock, and the arm returns so the
instruction re-executes. A class whose note is live but masked by the faulting
thread stays armed, because a peer may still die of it. Witnesses:
`notes.fault_dying_latch_live_note_exits`,
`notes.fault_dying_latch_clears_only_stale`, viv-pheno-probe L329/L330 (which
since gcpark-b require wait4 to read WIFSIGNALED SIGPIPE: a death by the fault
itself would read SIGSEGV).

### Every death by note records its signal (gcpark-b, 2026-10-12)

Operator votes "every note death records a signal" and "snare:abort"
(docs/ERRORS.md "Exit-status semantics" and "The wait status" are the binding
text). A Proc that does not end by its own exit ends because of a note, and the
kernel now records which, as a POSIX signal number beside the exit status
(`Proc.term_sig`, set at the ZOMBIE chokepoint -- [[sub-kernel-death]]). The
table is here: `g_known_notes` gained a `death_sig` column, nonzero exactly on
the `NOTE_DEFAULT_TERMINATE` rows (`interrupt` 2, `term` 15, `usr1` 10, `usr2` 12,
`kill` 9, `pipe` 13, `tty:quit` 3, `tty:hup` 1), and `g_snare_deaths` maps the fault family
(`snare:segv` 11, `snare:bus` and `snare:align` 7, `snare:ill` and `snare:bti`
4, `snare:brk` 5, `snare:fpe` 8, `snare:abort` 6). `notes_death_signal(name)`
reads both and answers 0 for a name that cannot kill. The numbers are Linux's
aarch64 ones, pinned to the vivarium's `VIV_SIG*` by `_Static_assert`, because
the packed wait word they land in is the Linux layout and both boundary-lines
pass it through unchanged.

Every death site names its note: the fault latch above, the uncaught default
terminate (`notes_noted_default`), the viv terminate arm and the tail's uncaught
terminate call `exits_note(name)`, which records `notes_death_signal(name)` with
exit status 1; the `kill` arm calls `exits_signalled(NOTE_SIG_KILL, "killed")`.
`exits_note` stops the kernel on a name that records no signal: every caller
passes a table row, and such a death would reap as an exit 1 and hide itself.
The viv arm's `"unknown"` and the uncaught-default arm's `"noted"` fallbacks
are gone with it (gcpark-b audit F1).
A raw wait still reads 1, so every native caller that predates the signal is
unchanged; only a caller that asks for the packed word sees the signal.

`snare:abort` is the one new name. Pouch's `abort()` is a single
`brk #SNARE_ABORT_BRK_IMM` (`0xab06`), and the EL0 brk arm
([[sub-kernel-exception]]) recognises the immediate and terminates with
`NOTE_NAME_SNARE_ABORT` instead of `snare:brk`. The immediate is ABI, as the
name is; `notes.h` lists the immediates it avoids (0x1 clang's trap, 0x3e8
gcc's, 0xf000 debuggers, 0x5500-0x55ff UBSan, 0xc471 WTF/JSC). Witness:
`notes.death_signal_table` (every name, the non-notes, both tables walked
row by row through `notes_known_name_for_test` and
`notes_snare_death_name_for_test` -- a known note kills iff it records a
signal, a snare death always records one -- and a cross-check against the
vivarium's numbers).

### A terminate latch wakes every sleep but a stop park's, and ends none of them (5g)

An uncaught note whose default is terminate (an `interrupt` nothing catches,
`term`, `usr1`, `usr2`, `tty:quit`, `tty:hup`, `pipe`) arms the LS-5c terminate latch -- one
per family, so `term`'s (`PROC_FLAG_TERM_TERMINATE_PENDING`) is not held by a
thread that masks only `interrupt` -- and its post
wakes every blocked thread of the Proc outside a stop park
(`proc_interrupt_terminate_wake`), and kicks every thread of the Proc running on
another CPU, so a thread spinning at EL0 takes an IRQ into its tail, whose notes
leg meets the latch (#964). An ordinary sleep then returns `SLEEP_INTR` and the thread dies of the note at
its return tail, the sleep's read having committed the death (next section):
`thread_die_pending` reports the latch in any family the thread has not masked, as well as group death, and, like group death and
ignoring the mask, an exec by another thread of the Proc (`proc_exec_dooms`,
thread-model C5, [[sub-kernel-proc]]); `thread_group_death_pending` reads the
same exec leg. Five waits read
`thread_group_death_pending` instead, `thread_die_pending`'s group-death leg
alone with the same hold. They are the tail's stop park, the
birth park, the nested stop park a sleep detours into, the vfork suspend and
the held spawn's birth wait, all through `sleep_death_only`
([[sub-kernel-rendez]]). `thread_death_reaches` asks the question the other
way round, for a caller choosing whether it may start a wait at all: a death
can end the thread's sleeps unless it is a kernel thread or held (dev9p's
clunk, [[sub-kernel-ninep-dev9p]]). All three read one hold,
`thread_death_held`: `loom_free`'s SQPOLL kthread join (`kthread_join_active`)
always, and the final close (`exit_close_active`) until a kill finds the Proc
already terminating and sets `PROC_FLAG_EXIT_CLOSE_FORCED` (ARCH 7.9.1 part B,
[[sub-kernel-death]]). Caught delivery stays shut under either flag, forced or
not: a forced close unwinds as a death, never for a caught note. The latch's walk passes
the stop parks by, since they
could only absorb its wake, and the parent suspends absorb it: a stopped thread
stays stopped, and a suspended parent stays suspended
(DEBUG-FS-DESIGN 5g, the operator's vote of 2026-09-30). No park consumes or
clears the latch. The note is met at the thread's next note checkpoint once
it runs, and a revocable latch (a peer can install a handler or open the notes
file) can no longer return a parent while its child still borrows its stack.

### A latch's unwind commits the death (vote 35, ARCH 8.8.2)

A wait that unwinds for a terminate latch answers on the footing that the thread dies:
the poll loop's 0, an untimed `sys_poll_sleep_for`'s 0, the 9P client's abandoned call,
dev9p's abandoned write-behind. A latch is revocable -- a handler installed or a notes
fd opened re-reads it away -- so a peer could revoke it between the unwind and the
tail, and the thread lived with an answer meant for a dying one (the poll false-0
class, OPEN-BUGS 2026-11-04). Reading true now commits. `thread_die_pending`'s latch
leg does one compare-and-swap on `proc_flags` (`notes_commit_latch`): it sets
`PROC_FLAG_TERMINATE_COMMITTED` and leaves armed only the latch it acted on -- the
first, in a fixed order, that the reader has unmasked, or a direct fatal note's own --
and that latch names the note (`notes_death_committed`). The swap is lock-free because
several readers hold spinlocks `q->lock` cannot nest under: sleep's
register-then-observe under `wait_lock` and `r->lock`, the 9P client under `c->lock`.
A commit made under `q->lock` was the rejected alternative. Every latch write -- the
arm, the drain, the disposition re-read, the self-managing mark, the fault arm's stale
clear, `notes_wait_untake`'s re-arm -- goes through `notes_latch_write_locked`, a swap
that refuses once the bit is set, so a handler's clear and a commit are decided by one
compare, and whichever lands first wins. Once committed:

- every thread's waits end, whatever its mask: the death is no longer deferrable;
- `el0_return_die_check` ends the Proc with `exits_note` of the named note, after the
  group-death arm and before the stop check ([[sub-kernel-death]]);
- the tail hands no note to a handler. `notes_tail_yield_or_die_locked` runs before
  both delivery paths and clears the latch of the note about to be delivered, since a
  latch armed on a note a handler now catches is stale; when that clear is refused, the
  thread dies of the committed note instead. No note is both handled and died of
  ([[inv-i19]]);
- the fault arm dies of it first, and the page-in reads the predicate once, after its
  read ([[sub-kernel-fault]]);
- no stop is asked of the Proc, no exec claims it, and rfork spawns no child from it
  ([[sub-kernel-proc]], [[sub-kernel-jobctl]]).

A masked read commits nothing. The tail's own deaths commit too: the native uncaught
arm, the phenotype's SIG_DFL terminate arm and the fault arm each commit the note they
die of under `q->lock` before releasing it (`notes_commit_note_locked`), because the
note stays queued while the Proc dies, and a peer's tail that met it after a handler
arrived would otherwise hand it on. If a peer committed another note's death first, the
tail dies of that one, the note every die check reads. `kill` has no latch and is never caught, so it
needs none. The commit wakes nobody: the thread that made it is
unwinding to its tail and dies there, and the group termination wakes the rest. Kernel
threads charged to a user Proc run in kproc and never reach the latch leg. Nothing
clears the bit. Witnesses: `notes.latch_read_commits_the_death` and its control
`notes.latch_clear_first_lets_it_live`, `notes.commit_takes_the_readers_latch`,
`notes.latch_names_its_note` (walked from `g_known_notes`), `notes.poll_zero_is_a_death`
(the class's own case, with a handler-first control that answers EINTR and lives),
`notes.tail_hands_no_committed_note`, `notes.tail_death_commits_its_note` and
`notes.fault_dying_latch_live_note_exits` (each reads the commit off the ZOMBIE),
`notes.tail_dies_of_the_committed_note`, `notes.fault_arm_meets_a_commit` (a handler
installed since the commit leaves the arm's pick empty; it dies anyway),
`notes.committed_death_refuses` and
`demand_page.file_read_revoked_latch_is_dying`. Decision:
[[dec-2026-11-05-commit-the-death]].

### A handler that escapes its frame must not deafen the Proc (bug-2)

Delivery is gated on `in_handler`: while a handler runs, `notes_deliver_at_el0_-
return` holds every further note (VIVARIUM 6.22's conservative imprecision), and
`in_handler` clears only at `rt_sigreturn` (`notes_noted_restore`) or exec. A
PHENO_LINUX handler that `siglongjmp`s to an ancestor `sigsetjmp` point escapes
its frame **without** `rt_sigreturn`, so `in_handler` stays stuck true — and the
N-3 guard then refuses every future caught-note delivery for the life of the
guest (permanent signal-deafness plus a queue pile-up).

The detector rides the stack geometry, and it is exact for a single-stack guest.
`note_saved_sp_el0` is the pre-handler SP; a live handler always runs BELOW it,
while a `siglongjmp` target is an ancestor frame — older, hence higher — so a
same-stack escape is always at or above the saved SP. Both operands are the
SP_EL0 bank (the kernel runs EL1h), so the compare never crosses banks; on that
condition the caller clears `in_handler`. The soundness is LOAD-BEARING on the
handler sharing the pre-handler stack, which holds only while `sigaltstack` is
unserved — a static assert ties the detector to the sigaltstack row so a renumber
trips the build. **Audit F1**: the `>=` false-clears across a genuine cross-stack
coroutine swap (a suspended context, not an abandonment), which can overwrite the
single `note_saved_*` slot; contained (guest-self-corruption, per-Thread,
kill-immune) and exotic (no v1.0 target does signal-driven cross-stack
switching), documented in VIVARIUM 6.23's DEGRADED row and tracked for v1.x.

### One mask restore at a handler's return, and the two waits (vote 24, 2026-11-03)

[[dec-2026-11-03-note-wait]], ARCH 8.8.3. Every delivery goes through
`notes_handler_mask_enter`, and `notes_noted_restore` puts `note_saved_mask` back with no
question about the phenotype. The native arm's running mask is the interrupted mask, the
row's `sa_mask`, and the note's own family unless `NOTE_DISP_NODEFER`. A native delivery
with no table adds nothing, and still saves. A native handler reads the row's flags in
`x2`. The old rule that a native handler's mask survives its return had no dependents:
Pouch, the debug child and joey's smoke change no mask in a handler, and Go's runtime has
no `noted`.

`Thread.note_wait_flags` (where `_pad_in_handler` was, so the size stays 1792):
`NOTE_WF_MASK_HELD` says this call runs under its own mask, and the thread's own sits in
`note_saved_mask` until the handler's return or the call's tail. `notes_interrupted_mask`
reads through it, so a frame's `uc_sigmask` reads the caller's mask and never the wait's.
`NOTE_WF_NOHAND` is Linux's `ERESTARTNOHAND`: re-issue the call if it unwound and no handler
ran. The call sets it itself and not its class, because `native_note_class` runs only for
a Proc with a table and either a handler or a fd, and a call with neither must still
re-issue after its tail. A tail that refuses the sp takes nothing, so the settle does not
re-issue there. It sets `note_interruptible` the same way: a peer can install the
first handler while the wait stands, and the wait has no other end. At the tail's end, in order: settle, restore if held, clear the
byte, release claims. The settle rewinds on `note_restartable || NOHAND`; the handler arm's
`SA_RESTART` rewind stays on `note_restartable` alone, so a `sigsuspend` under `SA_RESTART`
still answers `EINTR`.

`NOTE_DISP_RESETHAND`'s row store moved under `q->lock` on both paths, and the disposition
is re-read after the unlock, so two threads cannot both read caught. `SYS_NOTE_DISPOSE` and
`rt_sigaction` store their rows under the same lock (`viv_sigtab_store`), so the reset cannot
land on a row a peer wrote after the tail read it.

`notes_wait_suspend`: refuses a Proc no table governs. In a handler it leaves the masks
alone (the saved slot is in use) and waits on the queue hook for a note `~mask` admits and a
handler would take, then answers `EINTR`; the note runs at the outer handler's return. Otherwise it stores the mask,
peeks `notes_tail_candidate_locked` (a default stop or ignore queued under the old mask
needs the tail, and no latch wakes for it) and marks an unwind if one is there, not for a
self-managing Proc, whose fd holds its notes; then `tsleep_noteintr` forever. A pending
caught note reads the latch at register-then-observe.

`notes_wait_take`, on `notes_wait_on_queue`: hook on `q->poll_list` registered before it
looks, the thread's own word first (`notes_tail_consume_locked`), then FIFO with the pop and
both drains. `proc_note_thread` adds a queue wake for a direct note, which only the latch
used to reach. A kill-only queue answers `EINTR`, as `devnotes_read`'s R3-F1. A caught note
outside the word unwinds, after a last look for a take. `notes_wait_untake` puts a note back
after a copy-out fault: unconsume, re-arm terminate, and the wakes (N-2). A take word's bits
16..20 name single rows of the tty family; `notes_take_for_sigset` is the Linux row's
spelling.

`notes_wait_mask_begin` and `notes_wait_mask_end` hold a mask for a call that can also end
some other way (operator vote 28, [[dec-2026-11-04-poll-mask]]). Begin differs from a suspend's
front in one thing: a caught note goes on to the poll, whose pass reads the latch only after
finding nothing ready, so a ready fd answers first. A note only the tail can act on arms
nothing a poll reads, so begin marks an unwind and answers `EINTR`, with `NOHAND` even for a
timed call, since no time has gone. Begin leaves that out for a Proc no table governs, whose
tail would leave a default-ignored note queued and re-issue for ever
([[fnd-note-wait-r2-f3]]). End is Linux's `restore_saved_sigmask_unless`: an answer that is no
note's doing puts the mask back and clears both marks at once, so no handler runs for a note
only the call's mask let in. `EINTR` keeps it, and so does the 0 the poll core gives a death
wake: restored, the fatal note would wait behind the caller's mask and an untimed poll would
read a timeout ([[fnd-note-wait-r2-f1]]). In a handler neither does anything, so no note can
end the call: a coarser imprecision than a suspend's ([[fnd-note-wait-r2-f4]]).
`notes_wait_pending` reads the own word and the queue against
`notes_interrupted_mask`, or against everything in a handler, and takes nothing. The snare
family is left out of its answer, which is a take word. `notes_sigset_for_take` is the Linux
row's way back.

### The handler-time mask is Linux's, and it is passed on (phenotype)

For a PHENO_LINUX handler, delivery sets the running mask the Linux way: the
frame's `uc_sigmask` records the PRE-handler mask (what `rt_sigreturn` restores,
from `note_saved_mask`), while the live `note_mask` becomes `blocked | sa_mask |
the delivered signal` (unless `SA_NODEFER`), via `vivarium_handler_mask`. That is
the mask a running handler OBSERVES and PASSES ON: an `rt_sigprocmask` read inside
it, an `execve` from inside it (the image inherits `mask|sa_mask|sig`), a `fork`
from inside it (the child inherits the handler-time mask and its own `sigreturn`
restores the saved one). Delivery itself is unchanged — the `in_handler` guard
still holds every note for the handler's duration, so the widened mask cannot
admit a nested delivery here; it only stops a handler's own `rt_sigprocmask` from
outliving the handler.

### A masked STOP-class note becomes pending, and is consumed by its own scan

Most `tty:susp` suspends never reach the note queue: the job-control fan
([[sub-kernel-jobctl]]) takes the stop at POST time. The queue path exists for one
POSIX case — **every** thread masks `NOTE_BIT_TTY`, so the fan cannot stop and
posts `tty:susp` instead (a blocked stop signal becomes *pending*). Before PTY-1f's
round-2 F2 (#252) nothing could ever consume it: the EL0-return tail's case
analysis handled only the terminate class, so a STOP-class note fell through every
arm and was left queued with a comment claiming it awaited an "fd-read path" that
does not exist for a non-self-managing Proc and *cannot* exist for a phenotyped one
(a Linux guest has no notes fd). The `^Z` was silently lost, and one of the
sixteen queue slots went with it. `notes_stop_dequeue_locked` is the STOP-class
twin of the terminate scanner (`notes_stop_note_name_locked` is its yes/no form
for the tests): the tail consults it after the terminate check
misses, and on a hit applies the stop through `proc_job_stop_self` — the same
primitive `SYS_NOTED(NDFLT)` uses, so the #240 freshness (`susp_stop_armed`) and
orphan-rule guards both apply without restating.

**A stop the leg applies is parked in the leg.** Since 2026-10-05 the tail's
stop check runs before the notes leg (DEBUG-FS-DESIGN 4.2), so a stop the leg
applies has no later check to park it. The thread must not run another EL0
instruction under its own stop, so `notes_deliver_tail` reports the applied stop
(the stop arm returns true, as the orphan discard does), and
`notes_deliver_at_el0_return` runs the tail's die check and stop check again,
parks, and then looks at the queue afresh. Linux does the same: `get_signal`
parks in `do_signal_stop` and loops back for the next signal. The re-pass and
the discard loop share one budget of `NOTE_QUEUE_DEPTH`, so the masked tail is
bounded however the queue is flooded. `tail_order.tla` checks the order, the
re-pass and the budget ([[spec-tail-order]]). The kernel witness is
`rendez.tail_parks_for_the_stop_it_applies`. It runs the real leg, masked, on a
thread with a queued `tty:susp` and asserts that the thread parks on its own
`debug_rendez` and returns only once the stop is lifted. Its control, one
variable away, queues a `child_exit`, which stops nothing and returns at once. The peek only yields a note once
its family bit is *unmasked*, so the deferred stop lands exactly when the guest
unblocks the signal; a `tty:cont` that arrived meanwhile has already disarmed the
freshness flag, so the superseded `^Z` evaporates rather than resurrecting.

**The consume is its own function because a decision and its consumption written
as two scans are two predicates (P1).** `notes_stop_dequeue_locked` is
*class-filtered* — it returns the first deliverable STOP-class note at **any**
index, the same index-returning scan the peek runs. `notes_dequeue_locked` is
class-**blind**: it pops the first mask-permitted entry in FIFO order, i.e. the
queue head. On a one-note queue the two are indistinguishable, and every queue any
test had built held one note — so reusing the general pop looked correct. Put a
`child_exit` in front of the susp and they diverge: the stop applies, the
`child_exit` is popped into a stack local nobody reads (a wait notification
*silently destroyed*), and the `tty:susp` stays queued to re-fire. The lesson
generalizes past this file — only a test that puts something *between* the decision
and the consumption can tell the two scans apart (`notes.stop_dequeue_picks_its_own_note`,
whose leg A executes the class-blind pop and asserts it takes the wrong note). A
sibling correction (the c8ab2744 round) makes both class scans gate every hit on
`notes_proc_default_applies(p, name)` per note rather than on a fixed name — the
phenotype-sigtab reading that keeps a Linux guest's `SIG_DFL` susp from being
`exits()`ed on a caught `tty:hup` queued behind it (its regression lives with the
vivarium phenotype surface).

## Data structures

`struct Note` — 32 bytes, size-pinned: name, argument, sender pid, timestamp.
`struct note_record` is its userspace twin, pinned to the same 32 bytes **and
pinned equal to it**, so the fd read is a straight copy with no marshalling. The
third assert is the one that matters: it ties the two together, so they cannot
drift apart the way independently-pinned mirrors do.

`struct NoteQueue` — lock, ring, indices, and a waiter list.

The waiter list replaced a single-waiter rendezvous, which is the
[[sub-kernel-poll]] pattern arrived at the same way: each reader brings its own
rendezvous on its own stack, producers wake the whole list, and the ABBA between
a reader's condition check and a poster's wake disappears.

## Concurrency

**No poll-list walk runs under the queue lock** (the poll chunk, 2026-10-28):
`notes_post` drops `q->lock` and then calls `proc_wake_or_owe(&q->owed)`, which
owes the walk when the poster holds the process table lock (most callers) and
walks at once otherwise ([[spec-poll-owed]]); `notes_reenqueue_head_locked` no
longer wakes -- its callers walk after `spin_unlock`. The queue's `owed` record
is cancelled at the reap with the Proc's. Residue: the three re-enqueue walks of
`notes_deliver_at_el0_return` run after `q->lock` but still in the IRQ-masked
EL0-return tail; their hooks are only the Proc's own threads.

**The queue lock is the mutual-exclusion argument for the whole invariant.**
Both delivery paths take it; whichever gets it first owns the note.

Lock order is queue lock → the fault path's per-Proc lock (the user-stack push
can fault), and process-table lock → queue lock (which is why the kill branch
drops the queue lock before counting peers rather than nesting the other way).

**The user-stack push happens under the queue lock**, and that is worth
examining rather than accepting. Writing a user address can fault, and a fault
can demand-page. The source justifies it as "the allocator is non-blocking" —
true for anonymous and lazy-anonymous pages, which is what a stack is. It is
*not* the reason a file-backed page cannot appear here: that reason is that
file-backed regions are read-only, so a *write* never reaches the arm that
sleeps. The stated justification and the real one differ, and the real one
depends on a property (no writable file-backed mapping) that
[[sub-kernel-fault]] records as a v1.x seam. See Caveats.

The caught-note claim takes no lock: a CAS on `Proc.note_caught` takes it, an atomic
AND releases it, and only the release's walk -- run from the EL0-return tail,
which holds nothing, and only while the family is still caught -- takes the
Proc's `threads_lock` (since thread-model C3). A designation is made under the
designee's `wait_lock`, inside the walk's `threads_lock`, so the order is
`threads_lock` -> `wait_lock` -> `r->lock`. The sleep side's hand-off drops its
`wait_lock` before it takes `threads_lock`, and the exit drop runs under
`threads_lock` with no `wait_lock` held.

`Thread.note_direct` takes no lock of its own and is not guarded by `q->lock`: ARCH
7.3.1 has no edge between `q->lock` and `threads_lock`, and the post holds only the
second. Set by an atomic OR, taken by a `fetch_and` or an exchange, read with acquire.
The post's rouse runs under the target's `wait_lock`, the lock a thread entering a
wait reads the word under, so the entry and the post cannot both miss (I-9). The
self-managing fallback calls `notes_post` after it drops `threads_lock`.

## Invariants enforced

[[inv-i19]] — all five clauses, and the exceptions belong to the invariant
rather than to this note.

[[inv-i9]] — the fd read is register-then-observe: the reader installs its
waiter, then re-checks the queue, so a note arriving in between cannot be
missed. The condition callback reads only its own ready flag, deliberately
touching no queue state, which is what keeps it callable from inside the sleep
path.

[[inv-i39]] — via the `tty:` post gate, as above.

## Error paths

A bad name or a reserved prefix from userspace: rejected. An empty name at
`SYS_POSTNOTE` is the probe, never a post.
A full queue: coalesce for the kernel, "try again" for userspace. A bad stack
pointer, a failed push, an impossible dequeue: re-enqueue and return, leaving
the note for the next attempt. A failed kill re-enqueue: extinction, on purpose.

`NDFLT` from a handler in a multi-thread Proc fails, because the default action
is termination and that path predates the cross-thread shootdown. The handler
must fall back to resume or exit its own thread.

## Performance

Ring operations are O(depth) — the pop shifts entries to preserve ordering, and
the peek scans twice (once for kill, once for the mask). At depth 16 this is
noise, and the scans buy the ordering guarantee that a smarter structure would
have to reconstruct.

## Prosecution

On any change: that peek, pop and push stay inside one lock hold, and that every
failure path inside it re-enqueues; that kill keeps its two asymmetries (scanned
before the mask, skipped by the fd path); that the `tty:` prefix gate survives —
it is one string comparison holding an [[inv-i39]] boundary; that the stack
pointer validation stays *first*; that the two 32-byte structs keep the assert
tying them to **each other**, not only to 32; that any new note name lands
in both the name table and [[abi-note-names]]; and that a terminating name
carries a death signal and a non-terminating one carries none
(`notes.death_signal_table` walks both tables, so a new row is checked without
anyone extending a list, and `exits_note` stops the kernel on a death that
records no signal).

## Seams

- The two-consumers-of-one-queue shape has no model. The claim and the
  designation do (`caught_target.tla`).
- **A family word is a width every cast must keep.** `(u8)(1u << 8)` is 0 and it
  compiles, so a cast left narrow loses a claim or a designation with no error and
  every test on a family below bit 8 stays green. The fields' fit is asserted in
  `notes.c`; a cast is not, so `notes.family_past_the_first_byte` and
  `rendez.caught_note_family_past_the_first_byte` drive `usr2` through every path.
- **A self-managing Proc with a registered handler never gets its row's restart.**
  The tail's table branch is skipped for it, so the row's flags stay 0 and the settle
  refuses it: it sees `EINTR`, which POSIX allows. No in-tree program does both.
  OPEN-BUGS.
- **A native exec drops an ignored row**, where POSIX keeps it
  ([[seam-native-exec-drops-sig-ign]]).
- Per-kind masking within the `tty:` and `snare:` families is one bit each today.
- `snare:*` is defined, prefix-gated, and **not deliverable**: fault termination
  calls the exit path directly rather than posting (since gcpark-b,
  `exits_note()`, which records the fault's signal for a packed wait). No
  handler runs for a fault or for `abort()` (ERRORS.md: DEFERRED v1.x). The names, the mask bit and
  the gate are all in place for a substrate that does not consume them yet.

## Caveats

**Re-measured 2026-11-02: the first caveat below is history.** The kernel's
`NOTE_MASK_SUPPORTED` is `0x3ff` (IM-1 added `NOTE_BIT_SAK`, bit 6; `term` is bit 7;
`usr1` and `usr2` are bits 8 and 9), and it is consumed now:
`thread_caught_note_deliverable` and `thread_caught_note_claim` AND it into the
deliverable set, and `_Static_assert`s tie it to both halves of the 32-bit
`note_caught` and to the `u16` `Thread.note_claim`, `note_direct` and `note_target`.
libthyla-rs is `0x3ff`; pouch is `0x3af` (no `snare`,
no `sak`, a kernel-synthetic note for the native login authority). The
per-thread `note_mask` is `u64`, and so is every local the queue scans read it
into: they were `u32`, lossless only because of that bound, and the kernel now
builds with `-Werror=shorten-64-to-32`.

**`NOTE_MASK_SUPPORTED` has zero consumers** — and re-measuring it on
2026-08-16 found the caveat was scoped to one file when the constant lives in
**three, with three different values**:

| home | spelling | value | bits |
|---|---|---|---|
| `kernel/include/thylacine/notes.h` | `NOTE_MASK_SUPPORTED` | `0x3f` | 0-5, everything defined |
| `usr/lib/pouch/patches/0021-pouch-pty.patch` | `POUCH_NOTE_MASK_SUPPORTED` | `0x2f` | 0-3 + 5 — no snare |
| `usr/lib/libthyla-rs/src/lib.rs` | `T_NOTE_MASK_SUPPORTED` | `0x1f` | 0-4 — **no tty** |

Each is defensible read alone. The kernel's includes the reserved-but-
undeliverable `snare` bit and says so. Pouch's drops exactly that bit — the
deliverable set — and was explicitly widened to add `tty` at PTY-1b. The Rust
one has not moved since before PTY-1b and is simply **stale**.

**The three do not differ along one axis, which is what makes the staleness
invisible.** Kernel-vs-pouch differ over *is a reserved bit included*;
kernel-vs-Rust differ over *is this mirror current*; pouch-vs-Rust differ over
both at once. Lined up, `0x1f < 0x2f < 0x3f` reads like a deliberate spectrum
of narrowing scopes, and it is actually one policy disagreement and one
rotted copy wearing the same clothes. **Two orthogonal disagreements over one
scalar cannot be told apart by comparing the scalar.**

The consumer picture inverts the usual worry. The kernel's copy is inert (the
mask syscall stores its argument verbatim; the "unknown bits are tolerated"
behaviour its comment describes comes from the mask never being consulted for
unknown bits, not from any filter). The Rust copy is inert too — six mentions,
all of them the definition or a doc comment. **The only copy anything actually
reads is pouch's**, which initialises a shadow mask at two sites, and it is the
one holding the third distinct value.

The live consequence for native code is smaller than the table looks but real:
**`NOTE_BIT_TTY` does not exist in libthyla-rs at all** — zero occurrences, in
a crate that exposes the other five. Since PTY-1b the `tty:` notes are
deliverable, catchable, and maskable by that bit, and a native shell is exactly
the program that would want to defer them; it must write the bit position by
hand against a runtime that names every other one. Task #61, widened.

This remains the second dormant declaration found in two batches, after
[[chg-2026-08-03-mapping-core-sweep]]'s W^X checker — but the sharper reading
now is that a dormant *declaration* is cheap and a dormant declaration
**mirrored into places that are not dormant** is not.

**The uaccess-under-lock justification is right for the wrong reason** (above).
Sound today; the stated reason would survive the change that makes it false.

**Two mask bits are inert**, for different reasons: `kill`'s because kill
bypasses the mask by design (matching POSIX), `snare`'s because nothing delivers
those notes yet. Only the second is documented as inert.

## Provenance

Designed at the fd-first scripture commit and built as the signals sub-chunk;
hardened across four numbered audit rounds whose findings are still visible at
the lines they fixed. LS-5 added the uncaught-`interrupt` default terminate;
PTY-1b added the `tty:` family and its gate; hardening #3a added `snare:*`.
gcpark-b (2026-10-12) added `snare:abort` and the death-signal column, and
moved every death site onto `exits_note` / `exits_signalled`.

[[chg-2026-09-06-notes-caught-wait-mechanisms]] adds the three signal-delivery
mechanisms that landed after the 2026-08-16 update: the caught-note interruptible
wait (item 11 -- `notes_arm_caught_note_locked` + `thread_caught_note_deliverable`
+ the N-3 arm-2-livelock guard), the `siglongjmp`-escape `in_handler` clear
(bug-2, the stack-geometry detector + its F1), and the phenotype handler-time mask
(`vivarium_handler_mask`, `blocked|sa_mask|sig`). Already covered and borrowed:
`SIG_IGN`-at-generation and `pipe`-as-a-TERMINATE-note.

[[chg-2026-10-05-signal7-list]] opted in every kernel wait a call on signal(7)'s
list reaches (the paragraph above), behind the one predicate
`thread_caught_note_unwinds`.

## Tests

`notes.forced_close_lifts_the_hold` (2026-10-07) pins the hold: an unforced
exit close in a dying group holds every death (the control), the forced bit
lifts it for all three predicates but not for caught delivery, and the kthread
join holds every death, forced or not, inside the exit close or outside it.

`notes.*` covers the queue, both paths, the masks and the fd surface, including
the `S_IFCHR` report added when a missing metadata slot made `fstat` on a note
fd fail. The interactive Ctrl-C scenario exercises the uncaught-`interrupt`
terminate end to end.

The caught-note waits share one fixture in `test.c` (`test_caught_*`): a
Linux-phenotype Proc whose thread parks in the wait under test, a caught note
posted to it, and a leg that records whether the wait parked, unwound on the
post, or rode the note out. Fourteen witnesses use it
(`pipe_blocking.caught_note_*`, `poll.caught_note_*`,
`torpor.caught_note_ends_wait`, `rendez.caught_note_ends_wait4`,
`cons.caught_note_*`); six carry a native control one variable away, and
opening the phenotype gate turns exactly those six red. `viv-pheno-probe` legs L311-L318 drive each listed call
from a real Linux binary and require `EINTR` with the handler run.

The pick and the hand-off have their own witnesses in `test_rendez.c`, driven
on two kernel threads of one Proc by a step interpreter (`nt_*`):
`rendez.caught_note_prefers_awake` (main, awake, is designated and the sleeping
peer is not woken; released, the note reaches the peer), `_poster_first`,
`_handoff` (a wait that holds, an unwinding wait with the family masked, and a
`tsleep` that holds handing off to a `tsleep_noteintr` that unwinds),
`_exit_drop` (through the real `thread_exit_self`), `_holder_rewake`,
`_native_kicks_only` (both threads asleep, and main awake: the native walk
kicks main and designates no one; each shape has a Linux-Proc control one
variable away), and
`_repick_fallback` (`CAUGHT_REPICKS` stale picks, forced through
`caught_force_stale_for_test`, reach the fallback that wakes every unwinding
sleeper: exactly one unwinds and the other re-parks; one stale pick fewer
designates main alone).
`_one_unwind` and `_release_wakes_peer` were rewritten for the pick. The
fixture's `test_caught_run` now wakes a waiter that holds itself, since the walk
no longer wakes a wait that cannot unwind. On the device: viv-pheno-probe
L331-L333 (a spinning main takes a child's `SIGCHLD` while a polling peer is
left alone) and jc-probe's spinint and spinnote rungs (a native spinner ended by
`^C`, and one whose handler runs).

Thread-directed notes. `test_notes.c`: `notes.direct_fallback_records_its_sender`,
`notes.direct_move_survives_a_full_ring`,
`notes.direct_unconsume_after_the_mark_goes_to_the_queue`,
`notes.direct_names_own_their_family`,
`_reaches_only_its_thread`, `_post_gates`, `_ignored_and_discarded`,
`_candidate_discarded_delivers_nothing`, `_die_predicate`,
`_deliverable_takes_no_claim`, `_self_managing_uses_the_queue`. `test_rendez.c`, on
the `nt_*` interpreter: `rendez.direct_note_turns_fatal_when_the_handler_goes`,
`rendez.direct_note_unwinds_only_its_target`,
`_held_waits_ride_it_out`, `_read_at_wait_entry`, `_fatal_ends_any_wait`,
`_native_rides_it_out`. `syscall.native_holes_unassigned` pins the numbers beside 138 and
`vivarium.rejects_are_deliberate` the Linux rows they meet. On the device:
viv-pheno-probe L339-L343 (`tgkill` runs the handler on the blocked peer it names and
reads `EINTR` there while the caller's own `ppoll`s run their full time; a blocked
caller's self-directed note waits through an open peer; with no handler, `tgkill` at a
blocked peer ends the group with `SIGTERM`), and pouch-hello-threads' `kill-worker`,
`kill-masked`, `raise-masked`, `kill-blocked` and `kill-answers`.

Item 11c: `notes.table_refusals`, `.table_rows_govern`, `.table_token_needs_a_taker`,
`.table_reread`, `.table_exec`, `.restart_settle`, `.unwind_sets_the_mark`, `.native_class`;
`rendez.restart_undone_on_refused_frame`, `.tail_discards_dead_note`,
`.native_tail_past_sak`, `.reread_wakes_a_sleeper`; `vivarium.call_restartable`;
`9p_client.note_flush_server_eintr_stands`. On the device: pouch-hello-signals' blocked
legs, pouch-hello-sockets' `accept` and `recv`, pouch-hello-net's `accept-eintr`,
`accept-restart`, `connect` and the TCP `recv` legs, viv-pheno-probe L346-L349.

## Referenced by

[[moc-kernel-ipc-wake]] · [[inv-i19]] · [[abi-note-names]] ·
[[sub-kernel-poll]] · [[sub-kernel-fault]] · [[sub-pouch-signal]]
