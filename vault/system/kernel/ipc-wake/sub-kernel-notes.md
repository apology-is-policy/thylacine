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
updated: 2026-08-03
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

Two paths, **one queue**:

| path | how | who uses it |
|---|---|---|
| fd-shaped | open a note file, `read()` 32-byte records, `poll()` for free | native daemons; anything with an event loop |
| async handler | register a callback; the kernel rewrites the return-to-userspace context | ported code expecting signals |

Four families of note name, distinguished by **who may post** and **what an
uncaught one does**:

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
   onto the user stack, save the entire user context into the thread, mark it
   in-handler, and rewrite the return context to land at the handler.

**Steps 2, 5's pop, and 5's push all happen under one lock hold**, and each was
a separate audit finding. Peek-then-pop split lets the other path steal the
note; pop-then-push split loses it on a failed push. Every failure inside the
hold re-enqueues at the head.

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

**`NOTE_MASK_SUPPORTED` has zero consumers.** It names the set of meaningful
mask bits, and nothing reads it — the mask syscall stores its argument verbatim.
The behaviour its comment describes (unknown bits succeed and do nothing) is
produced by the mask never being consulted for unknown bits at all, not by any
filter. It is also unpinned against the bit definitions it summarizes, and
already over-claims by one bit relative to the live name table. Task #61 — and
the second dormant declaration found in two batches, after
[[chg-2026-08-03-mapping-core-sweep]]'s W^X checker.

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
