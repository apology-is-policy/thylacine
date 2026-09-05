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
updated: 2026-08-16
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

Three paths, **one queue**:

| path | how | who uses it |
|---|---|---|
| fd-shaped | open a note file, `read()` 32-byte records, `poll()` for free | native daemons; anything with an event loop |
| async handler | register a callback; the kernel rewrites the return-to-userspace context | ported code expecting signals |
| **Linux-phenotype frame** | the disposition lives in a per-Proc signal table, not in `handler_va`; a separate deliverer builds a Linux-shaped frame | a phenotyped guest, which never calls the native register syscall |

The third path is not a third *queue* — it is a second **frame shape** over the
same one queue, and it sits *above* the native handler branch in the deliverer
for a specific reason recorded below.

**Five** families of note name (this note said four against a five-row table
until 2026-08-16), distinguished by **who may post** and **what an uncaught one
does**:

| family | postable by | uncaught default |
|---|---|---|
| `interrupt` | anyone | terminate (unless the Proc reads its own notes) |
| `kill` | anyone | terminate, **uncatchable** |
| `pipe`, `child_exit` | kernel | nothing — queue for the fd reader |
| `tty:*` | **kernel only**, catchable | quit/hup terminate; susp stops; winch/cont informational |
| `snare:*` | **kernel only** | terminate — and not deliverable at all today |

The two prefix gates are not decoration. `notes_post` rejects any userspace post
of a `tty:`- or `snare:`-prefixed name, and for `tty:` **that gate is the only
barrier**: those names *are* in the known-note table, so without it `tty:cont`
would be postable through the ordinary parent-to-child path, letting an
unprivileged parent resume a debugger-stopped child. That is an
[[inv-i39]] hole closed by a string comparison, and the source labels it
load-bearing.

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
declines until it has a name of its own.

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
to a phenotyped Proc, and the helper now consults the signal table when the
native address is zero and the phenotype is Linux.

Left unfixed that was a **hang, not a fidelity gap**: the latch makes every
unmasked thread's sleep return interrupted at once, and while the delivery
success path drains the latch and self-corrects, the frame-push failure arms do
not. Worth keeping as a shape — *a fidelity gap in an exemption check becomes a
liveness bug when the thing being exempted is a latch.*

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

A bad name, an empty name, or a reserved prefix from userspace: rejected.
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
tying them to **each other**, not only to 32; and that any new note name lands
in both the name table and [[abi-note-names]].

## Seams

- No model. The two-consumers-of-one-queue shape usually earns one.
- Per-kind masking within the `tty:` and `snare:` families is one bit each today.
- `snare:*` is defined, prefix-gated, and **not deliverable**: fault termination
  calls the exit path directly rather than posting. The names, the mask bit and
  the gate are all in place for a substrate that does not consume them yet.

## Caveats

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

## Tests

`notes.*` covers the queue, both paths, the masks and the fd surface, including
the `S_IFCHR` report added when a missing metadata slot made `fstat` on a note
fd fail. The interactive Ctrl-C scenario exercises the uncaught-`interrupt`
terminate end to end.

## Referenced by

[[moc-kernel-ipc-wake]] · [[inv-i19]] · [[abi-note-names]] ·
[[sub-kernel-poll]] · [[sub-kernel-fault]] · [[sub-pouch-signal]]
