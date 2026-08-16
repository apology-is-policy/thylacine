---
id: sub-kernel-loom
type: sub
parent: moc-kernel-async
title: "Loom — the io_uring inversion over 9P"
code:
  - kernel/loom.c
  - kernel/include/thylacine/loom.h
audit: hard
guarded-by: [inv-i29, inv-i30, inv-i32]
validated-by: [spec-loom, spec-loom-multishot, spec-loom-order, spec-loom-devgone, gate-smp]
locks: []
abis: []
design:
  - "docs/LOOM.md"
  - "docs/reference/107-loom.md"
created: 2026-08-02
updated: 2026-08-16
---
## Purpose

A syscall per file operation is a trap per operation. Loom is the shared-memory
alternative: userspace writes operation descriptors into a ring the kernel can
read, the kernel's 9P engine runs them, and replies come back as completion
entries. A batch of work costs one trap, or — with the poll thread — none.

The inversion in the name is that the opcodes are not a new namespace. They
*are* the 9P client's own surface, so one async layer covers files, the network
tree, process introspection, services and devices without any of them knowing.

## Contract

A ring is created with a power-of-two submission depth; the kernel allocates one
anonymous region holding a header, a submission index array, the entry array and
the completion array, maps it into the caller read-write, and reports the
geometry. Two more calls register the objects operations may name: a fixed table
of open file handles, and a fixed table of pinned buffer regions.

Then: userspace fills entries and advances its tail; the kernel consumes them,
and posts one completion per operation carrying the caller's opaque token and
either a byte count or a negative error. Enter submits, optionally waits for a
number of completions, and reaps.

**Every failure produces a completion.** A rejected opcode, an empty handle
slot, a rights denial, an allocation failure — all post a completion with a
negative result. A submission entry is never silently dropped, because userspace
has no other way to learn what happened to its token.

Fifteen of the twenty opcodes dispatch. The five that do not are the ones that
*mint or release a fid* — walk, open, create, clunk — plus a reserved
passthrough. Registered handles wrap already-open fids, so those five need a
registered-slot install and release surface that does not exist yet; they return
"not implemented" rather than pretending.

## Mechanism

### The private counter is the authority

The ring header is in the caller's own mapped region, so every word in it is
userspace-writable. The kernel therefore keeps its own submission head,
completion tail and entry-count masks, and treats the header's copies as a
mirror it publishes.

This is not defensive habit; it is the difference between a bug and a kernel
write through an attacker-chosen offset. A completion index computed as
`header.cq_tail & header.cq_mask` with both words hostile lands anywhere. Computed
as `private_tail & (private_entries - 1)` it is always inside the array, because
the private count was validated as a power of two at creation.

The user's own words *are* read — the submission tail bounds how much to drain,
the completion head computes fullness — and the argument for that is precisely
that neither can index anything. A hostile completion head only lets a Proc
overwrite its own unreaped completion, in its own region, or wait for the wrong
thing.

One user word does reach an index: the submission ring holds *indirection*
slots naming entries in the entry array. That one is range-checked against the
private entry count, and a bad value increments a dropped counter instead.

### Copy first, then decide

Each consumed entry is copied whole into kernel memory before any field is
examined. Everything downstream — the opcode switch, the bounds checks, the
builders that encode the wire message later — reads the copy. Nothing re-reads
the shared slot after the checks, which is what makes the checks mean anything.

Operations whose encoding needs more fields than the resolved state carries keep
the whole copied entry alongside the in-flight operation, so a builder running
minutes later still decodes from the snapshot.

### Pin at submit, never re-resolve at completion

When an operation names a registered handle, the submit path resolves the slot
and takes its own independent reference on the object, under the ring lock so a
concurrent re-registration cannot free it in the gap. It snapshots the rights
and checks them *there*. Completion acts on the pinned object and never
re-consults the table.

This is the shape of the io_uring credential-versus-work vulnerability class,
avoided by construction: an operation is bound to the object and the rights it
was admitted under, so replacing the table entry after submission cannot
redirect work already in flight.

The buffer-backed operations pin two objects this way; the two-fid operations —
rename, link — pin three, and additionally require both fids to belong to the
same session, because those messages name two fids in one namespace.

The registered buffer's kernel address is taken from the backing region's
direct-map base rather than the user virtual address, so the pin survives the
caller unmapping its own view.

### Back-pressure at submit, not at completion

The completion ring can fill. The obvious design drops or overwrites a
completion; both lose an operation's result. Instead the kernel refuses to
*consume* a submission unless the completion ring can still hold one more entry
beyond every posted-unreaped completion and every in-flight operation's eventual
one.

So the reservation is made when the operation starts, and a full ring
back-pressures at the front door: the entry waits for the next enter. The
completion-time full check remains as a guard, and its counter is meant to stay
at zero.

### What the completion callback may not do

The callback fires from inside the 9P engine, under the client's lock. It may
not sleep and may not re-enter the engine. So it does the minimum — compute the
result, copy a read's payload while the receive buffer is still valid, post the
completion, mark state — and *flags* anything else.

Two things get flagged and deferred to a drive loop running outside that lock:
re-issuing a multishot operation, and dispatching a chain successor whose gate
just opened. Freeing the operation container and releasing its pins are deferred
too, because releasing a pin can sleep.

### Multishot, and ordering

A multishot operation posts a completion carrying a "more follows" flag and
re-arms, re-issuing the same builder against the same pinned object — the pin is
reused, never re-resolved. It terminates on an error reply, on its shot bound,
or if a shot's completion cannot post. The terminal completion clears the flag,
which is what a consumer waits on.

Ordering is a separate machine. An entry that sets link or drain — or any entry
consumed while the chain is non-empty — is held rather than dispatched, and an
admission pass walks the chain dispatching whatever is now legal: a linked
successor after its predecessor succeeded, a barrier after everything before it
finished. A failed link cancels its successors, each getting exactly one
cancellation completion. The chain is length-capped so a barrier-blocked burst
cannot grow it without bound.

### The poll thread

A ring can be created with a kernel thread that drains submissions and drives
the engine, making steady-state submission free of traps. It parks when there is
nothing admissible and announces a flag telling userspace an enter is needed to
wake it.

Its park condition is the interesting part. Waking on "submissions pending"
alone spins at full CPU when the completion ring is full and the user's tail
sits ahead: the drain refuses on the admission check, submits nothing, the
condition fires again, and the sleep returns without ever sleeping. So the
condition is *work pending **and** the completion ring can admit*, and the wake
comes from the user reaping and entering.

Because the thread belongs to the immortal kernel process, it cannot exit
normally — the normal exit path is fatal from there. It hand-rolls the tail of
the reap protocol instead: mask interrupts, mark itself exiting, release the
handshake flag, and switch away permanently. Teardown spins on that flag and
then reclaims it.

The thread is charged against the **creating** Proc's thread budget, not the
kernel process's — otherwise a ring is a way to buy a thread outside
[[inv-i32]] by parenting it somewhere exempt. The check and the increment
happen under one lock hold, because unlike ordinary thread creation a ring
setup has no other serialization point. Exempt Procs skip the *cap* but are
still counted, so the uncharge is unconditional either way.

The uncharge keys on a **flag**, not on the thread pointer: a setup that
charged and then failed to start the thread still owes the refund, and keying
on "is there a thread" would silently keep it.

### Who paid — the I-32 ledger

Loom's teardown is the one page-charge settlement point in the tree with **no
Proc argument**. A ring outlives the syscall that made it and is freed from a
handle close, so the Proc that was billed has to be recorded on the object.

Two questions look like one here, and conflating them was a live defect:

**Which Proc owns this ring?** Answered by a stored pointer plus its pid. The
pointer is safe because the ring handle is neither transferable nor dup-able,
so a ring is reachable only through its creator's handle table, and that table
is torn down while the creator's own structure is still allocated. That is an
*argument*, not an enforced invariant, so it is backstopped rather than
trusted: the magic-and-pid pair turns any future violation into a skipped
refund on a dying Proc — inert — instead of a write through a dangling pointer
or a charge stolen from a recycled one.

**Which Proc paid for this region?** A different question, and the ownership
proof does not answer it. Buffer registration accepts any writable anonymous
region of the owner — which a network ring that the driver allocated and shared
in satisfies exactly, and the shipped flow API registers the whole ring. So the
owner can hold a pin on pages it never bought.

The answer therefore comes from the **region**, not the ring. Each eager charge
stamps its payer on the Burrow; a settler *claims* that record — a read that
also **clears** it — and refunds only what comes back. A region the owner never
paid for returns zero, so nothing is refunded. The clear is what makes the
refund exactly-once: two paths racing to settle cannot both win.

The claim happens **before** the drop, because a freeing drop takes the record
with it. If the drop turns out not to free, the claim is put back. The window
between claim and restore is a real one, and its failure mode is deliberately
chosen: a concurrent settler sees the cleared record and skips, leaving a
charge that outlives its region until the payer's next release point. An
over-charge on the payer — never a refund to a Proc that did not pay.

**The direction of the error is the whole design.** An over-charge caps a Proc
early; an under-charge inflates its budget, which is the bound failing. Every
tie in this mechanism is broken toward over-charging.

### The two owner pointers must stay apart

The ring records the same Proc twice — once for the page ledger, once for the
thread ledger — and the two are bound at **opposite ends** of setup. That looks
like duplication and is not.

The page owner is bound **last**, after the final failure path, so it marks a
fully-constructed ring. Every rollback before that point uncharges explicitly
and then tears down a ring whose page owner is still unset, so teardown cannot
refund what the rollback already did.

The thread owner is bound **first**, at the moment the charge succeeds, for the
identical reason read the other way: the rollbacks do *not* refund it, so
teardown must, which means it has to be recorded before anything can fail.

Same goal — settle exactly once — reached by inverting the discipline, because
one ledger is settled by the rollback and the other by the teardown. Merging
the two pointers, or moving either binding toward the other, silently breaks
whichever it was not written for: one double-refunds, the other leaks. Nothing
in the code says these are a matched pair.

## Data structures

**The ring** — one anonymous region: a 64-byte header, the submission index
array, the 64-byte entry array, the 16-byte completion array, each region
cache-line aligned and the whole page-rounded. At maximum depth it is roughly
400 KiB.

**The ring object** — the geometry (immutable after creation), the private
submission head and completion tail, the in-flight operation list and its count,
the deferred-re-arm count, the ordering chain and its length, the completion
wait-list, the poll thread and its handshake flags, and the two registration
tables. It opens with a magic word at offset zero, so a write through a freed
object is caught rather than acted on.

**An in-flight operation** — carries the engine's request record at offset zero,
so the completion callback recovers the container with a cast. Plus the pinned
handle, the optional second handle, the pinned buffer and its kernel address,
the resolved fids, the copied entry, the multishot state and the chain
back-pointer.

**A chain entry** — the copied entry, its link and drain flags, its state, and
the submission-order successor. Deliberately *layered on* the operation
lifecycle rather than merged with it.

Compile-time assertions pin every ABI structure's size and the load-bearing
field offsets — size alone would let a same-size field reorder shift the layout
the userspace mirror reads.

## Concurrency

The ring lock is a leaf. It is taken under the engine's client lock (the
completion path) and never the reverse; it nests nothing except brief atomics.
Everything that can sleep — releasing a pin, freeing a container, allocating a
chain entry, submitting to the engine — happens outside it.

The completion wait-list carries its own lock and is woken *after* the ring lock
is released, so the ordering is: publish the completion under the lock, then
walk the list. A waiter that sampled before the publish is found by the walk; one
that samples after sees the completion. That is the register-then-observe
discipline from the poll layer, and it is why the wait is not lost.

**The borrow guard.** To drive the engine, a caller needs the client of some
in-flight operation, and it must dereference that client after dropping the ring
lock. Between the two, a concurrent reaper plus a re-registration could free the
operation's pinned object and with it the client. So the lookup takes an *extra*
reference on that object, which the caller releases after the pump. A single
reaper made this safe once; the poll thread was a second one, and it is not.

**The join.** Teardown stops the poll thread before anything else, because the
thread is the only other mutator of the in-flight list. It sets the stop flag,
wakes the park, spins for the exit handshake and reclaims the thread — and only
then quiesces the remaining operations. The thread deliberately holds no
reference to the ring; one would deadlock this join.

**Quiescing.** Each surviving operation is abandoned through the engine under
the client's lock, which makes it mutually exclusive with a demultiplex that
might be completing it concurrently. Whichever wins, the ring is still allocated
— it is freed only after the loop — so there is no double completion and no
use-after-free.

## Invariants enforced

**[[inv-i29]]** — completion integrity. Every submitted operation produces
exactly one terminal completion; none is lost, duplicated, stale, or written
over an unreaped one. The submit-time reservation is what makes the last clause
structural rather than hopeful.

**[[inv-i30]]** — the submit-time pin, and the ring TOCTOU. Resolve and snapshot
at submit; never re-read a shared word after checking it.

**[[inv-i32]]** — on two axes. The ring region is charged to the creating
Proc's page budget, so rings are bounded like any other anonymous commitment;
the poll thread is charged to that same Proc's thread budget, so a ring is not
a way to buy a thread parented somewhere exempt. Both settle exactly once,
through the ledger above, and both break ties toward over-charging.

## Error paths

Negative errno in a completion for every rejection: bad opcode, out-of-range or
empty handle slot, missing rights, a walk-only handle attempting content I/O, a
bad buffer index or out-of-bounds slice, a degenerate two-name split, a
too-short input structure, a cross-session fid pair, a non-9P-backed handle,
allocation failure. Cancellation for a chain successor whose predecessor failed.

Enter returns `-1` only for a corrupt ring object or invalid flags — everything
about an individual operation is reported through its completion.

Two counters in the header are diagnostics: dropped submissions (a bad
indirection index) and overflowed completions (which the admission rule is meant
to keep at zero).

## Performance

The point is trap amortization, and the measured shape is what you would expect:
roughly a 7.7× improvement on no-op operations batched versus one enter each,
and roughly parity on durability barriers — because those are dominated by the
commit, not the trap.

The completion ring defaults to twice the submission depth, which is what gives
the admission rule room to work without back-pressuring a normally-reaping
consumer.

## Prosecution

- **Never compute an index from a shared word.** The private counter and private
  mask are the authority; the header is a mirror. The one indirection slot that
  does index is range-checked, and must stay so.
- **Copy the entry before reading any field**, and never re-read the shared slot
  after validating.
- **Pins are taken at submit and released exactly once** — at reap, at abandon,
  or in teardown. Buffer, primary handle, and second handle each balance on every
  path, including every rung of the failure ladder.
- **Rights are snapshotted at submit and never re-checked at completion.** That
  is deliberate; re-checking is the bug the model has a counterexample for.
- **The completion callback must not sleep or re-enter the engine.** New work
  added there has to be flagged and deferred, like re-arm and chain admission.
- **The ring lock stays a leaf**, taken under the client lock and never the
  reverse.
- **The borrow guard must be held across any pump** that dereferences a
  borrowed client.
- **The poll thread must be joined before the in-flight list is touched**, and
  must never hold a ring reference.
- **A registered buffer must stay contiguous-by-type.** The single-base kernel
  address is only valid because the region is one physical chunk; admitting a
  scatter-gather type without making the address computation walk chunks yields
  a wrong kernel address with no tripwire.
- **Every failure path posts a completion.** A silently dropped submission is
  unobservable to the caller.
- **Never refund a page charge to the ring's owner without asking the region who
  paid.** "This Proc owns the ring" and "this Proc bought these pages" are
  different claims, and buffer registration accepts shared-in regions, so the
  first does not imply the second. Claim against the Burrow; refund only what
  the claim returns.
- **Claim before the drop, restore if it did not free.** The record dies with
  the region, so a claim after a freeing drop reads nothing and silently loses
  the charge.
- **Settle the thread budget on the flag, never on the thread pointer.** A
  charge whose thread never started still owes its refund.

## Seams

- **Concurrent admitters can over-reserve.** The room check and the in-flight
  bump are not atomic with each other, so two threads entering the same ring can
  admit slightly past the reservation. The chain's cancellation leg is hardened
  against it (revert and retry); the dispatch leg's residual is a dropped
  terminal completion under exact concurrency. It rests on the single-producer
  submission contract, and the exact coordination is owed work.
- **The fid-lifecycle opcodes are unimplemented**, pending a registered-slot
  install and release surface.
- **Suppressing a success completion is rejected**, because it would break the
  ordering model's "every finished operation posted" property; it needs a model
  carve-out first.
- **[[seam-loom-rearm-needs-blocking-enter]]** — re-arm runs only in the two
  drive loops, so a non-blocking consumer never re-arms a multishot stream.
- **[[seam-loom-sqpoll-owner-unbackstopped]]** — the thread ledger's owner
  pointer is dereferenced bare where the page ledger's identical pointer, resting
  on the identical argument, is validated against magic and pid.

## Caveats

- **The file and header both describe the first sub-chunk.** The header's
  status block says "the ring substrate… **no op flows yet** — the opcodes are
  reserved ABI", and lists work through the third sub-chunk as future. The file's
  opening line calls itself the ring substrate and says dispatch and completion
  posting live elsewhere. Fifteen opcodes dispatch *here*, and the file has since
  grown the poll thread, multishot, ordering, registered buffers and the
  zero-copy routing. Nothing is wrong with the code — the per-function comments
  are meticulous, carrying audit finding references at the exact lines they
  fixed — but a reader who starts at the top is told the file does almost none of
  what it does. The same drift, in the same place, as the console's and the
  entry area's header blocks.
- **An overflow-safety comment over-estimates the completion array** at twice its
  real maximum size, and the way it gets there is the interesting part: two
  errors in opposite directions. It uses the submission ring's entry *count*
  where the completion ring's is double, and the uniform sixty-four-byte entry
  *size* where a completion entry is a quarter of that. Four times too large
  against two times too small, landing at twice. So it is not a slip in the
  arithmetic — it is the conservative uniform bound applied one region too far,
  which is why it reads as deliberate. The direction is fail-safe and the
  headroom on the conclusion is enormous (the overflow it rules out needs about
  four thousand times the largest ring), but the number is decorative rather
  than load-bearing, and only the compensation makes it conservative: widen a
  completion entry and the same comment starts under-estimating.
- **A constant's rationale was replaced rather than its value**, which is the
  pattern worth copying. The registered-handle table bound used to be justified
  by matching the per-Proc handle limit; when that limit was lifted the match
  stopped being a reason, and the fix left the number alone and wrote the real
  argument — the table is charged per *ring*, and a Proc may hold many rings, so
  the bound was never about how many handles a Proc can hold. A stale-constant
  sweep that only re-checked values would have found nothing wrong here.

## Provenance

[[chg-2026-08-02-async-sweep]], [[chg-2026-08-16-loom-charge-ledger]].
