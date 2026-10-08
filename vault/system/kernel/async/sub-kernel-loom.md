---
id: sub-kernel-loom
type: sub
parent: moc-kernel-async
title: "Loom — the io_uring inversion over 9P"
code:
  - kernel/loom.c
  - kernel/test/test_loom.c
  - kernel/test/loom_receipt_fixture.h
  - kernel/test/loom_private_fixture.h
  - tools/host-tests/loom-receipts.c
  - tools/test-loom-receipts.py
  - kernel/include/thylacine/loom.h
  - kernel/include/thylacine/loom_service_abi.h
  - tools/check-loom-service-abi.py
audit: hard
guarded-by: [inv-i29, inv-i30, inv-i32]
validated-by: [spec-loom, spec-loom-multishot, spec-loom-order, spec-loom-devgone, spec-loom-service, spec-loom-service-buffers, gate-smp]
locks: []
abis: [abi-loom-ring, abi-loom-service]
design:
  - "docs/LOOM.md"
  - "docs/reference/107-loom.md"
created: 2026-08-02
updated: 2026-10-07
---

## Approved private service lifecycle

Current Loom starts from attached service handles; synchronous native service
setup and the mid-frame join trust assumption remain outside its asynchronous
contract. The approved extension is reviewed in docs/ASYNC-SERVICE-LIFECYCLE.md
and docs/ASYNC-MEMORY-DESIGN-REVIEW.md. Legacy rings and the current authority
contract are unchanged. AS-0 has compiled record mirrors and a bounded lifecycle
model. AS-1 supplies the nonblocking transport/handshake helpers in
[[sub-kernel-ninep-transport]]. The exact boundary is [[abi-loom-service]] and
implementation progress is recorded in docs/ASYNC-SERVICE-STATUS.md.

The EMPTY PRIVATE OWNER is now implemented: a private Loom can be created,
latched closed on its last handle, queued to a retirer kernel thread and
destroyed, with the ring's backing charge settled inside the same `v->lock`
interval that decides finality. **No private service handler is enabled and no
syscall reaches the creation path**, so the only difference an unmodified system
can observe is none; the admission surfaces that would otherwise accept work on
such a ring refuse it explicitly rather than by absence.

WHAT IS AND IS NOT QUALIFIED, because "implemented" and "qualified" are not the
same claim. The port compiles, boots, and passes the full suite, and each of its
two new witnesses has been shown RED against a mutation of the thing it guards --
including a mutation of the shipped destructor itself, which is what
distinguishes a test of this lifecycle from a test of a transcription of it.
Activation gates have NOT passed: the private path, the replacement memory
accounting and the clipboard all stay non-default, and the 128 MiB protection is
retained until the replacement accounting passes its own gates. The A72/KVM axis
is an explicit, owned, recorded residual rather than a silent gap.

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

**`SETATTR` is where the submit-time snapshot meets its limit, and the async
path fail-closes rather than guess.** A `SETATTR` operation's authority is not
uniform. The SIZE axis — a truncate — is authorized by `RIGHT_WRITE` on the fd,
which the submit path already snapshots; so async truncate is admitted, on a
non-`O_PATH` (`CWALKONLY`) handle, with the same `INT64_MAX` size bound the sync
handler applies (the #81 hollow-rights close — an `O_PATH` handle is born `R|W`
but `perm_check`-exempt, so its `RIGHT_WRITE` is hollow — extended to the async
path here). But MODE/UID/GID are authorized by *identity*, the sync side's
owner-only `perm_wstat_check`, which the submit path cannot evaluate without a
blocking owner-stat. (The poll thread may never block; a submitter's own
`ENTER` may, and since 2026-09-23 the directory-mutation gate below does. So
async identity-setattr now has a mechanism to reuse, on non-SQPOLL rings only.
It has not been done.) So the async
`SETATTR` splits by authority kind: SIZE dispatches, and MODE/UID/GID are
**rejected fail-closed** — v1.0 Loom `SETATTR` is truncate-only; an async
identity-setattr (a submit-stat, or a completion-recheck design) is a v1.x seam.
The audit that found this found the same `O_PATH` truncate bypass on the async
path as on the sync one, *plus* the broader gap that the async path ran no
identity check at all — both closed by the split. (`9p_client.loom_setattr_e2e`:
chmod rejected and never on the wire; truncate reaches the wire; `O_PATH`
truncate rejected.)

**The directory-mutation ops re-check the DAC at submit (LOOM.md 8.5.1,
2026-09-23).** `MKDIR`, `MKNOD`, `SYMLINK`, `UNLINKAT`, `RENAMEAT` and `LINK`
used to gate on the handle's `RIGHT_WRITE` alone. The header said "the
identity axis stays the dev9p server's"; A-3b had made that false, since the
kernel is the only rwx enforcer and Stratum checks dataset scope only. Their
directory handle is normally `O_PATH`, whose `RIGHT_WRITE` is hollow, so any
principal that could X-search to a directory could create, unlink, rename, link
or symlink entries in it. `SYMLINK` is the only way to make a symlink in the
guest, so the widest door was the one every symlink goes through.

The gate now does what the sync twins do:
- stat each mutated directory and `perm_check` it for W|X (`RENAMEAT` both,
  `LINK` only where the link lands; `-EIO` with no stat, `-EACCES` denied);
- check against the ring creator's LIVE identity. `ident` is stamped at setup
  before the handle publishes; the ring is non-transferable, so the creator is
  every submitter;
- refuse the six ops on an SQPOLL ring (`-EOPNOTSUPP`) before any stat, because
  the poll thread must never block on a wire RPC;
- resolve a create's gid: 0 means the primary group, anything else goes through
  the chgrp rule of `perm_wstat_check`, and the wire carries the resolved value.
  On a caped session (IDENTITY-DESIGN 3.2) 0 goes out as `P9_NOGID`, so the
  server keeps its own group, and a named gid, even the primary one, is a chgrp
  the cape refuses (`-EACCES`);
- send a create's mode as its rwx bits only (`MKNOD` keeps its type), as the
  sync create does, so a create cannot plant the setuid, setgid or sticky bits
  that `SYS_WSTAT` refuses;
- check each child name with `sys_copy_component`'s rule (`-EINVAL` before any
  stat) on a copy taken into the op at submit. The build sends that copy, so
  userspace cannot rewrite a checked name in the shared buffer.

`LCREATE` also creates a child. It is not dispatched yet (`-ENOSYS`, the #916
seam), and it joins this gate when it is.

`9p_client.loom_dirmut_dac` holds the truth table per op, with every refusal
checked against the server's own count of what reached it;
`9p_client.loom_dirmut_names` holds the name rule.
`9p_client.loom_dirmut_sqpoll` and `9p_client.loom_create_gid` hold the other
rules; the last also reads the create mode off the wire.

The parent's DAC needs nothing extra under the cape: the stat it checks comes
through dev9p's one conversion, which already reports the cape's owner.
`GETATTR` is the one place Loom hands userspace the server's attributes
directly, so its completion copy applies the cape itself: the cape's uid and gid
replace the server's and are marked valid, as the kernel's own stat reports
them. Userspace sees the owner the kernel's DAC enforces.
`9p_client.loom_cape` holds both rules.

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

Until 2026-09-23 that held for one driver only. Two drivers on one ring could
over-admit: two `ENTER`s of a multi-thread Proc, or an `ENTER` beside the poll
thread. An op between its consume and its dispatch was counted nowhere, so a
sibling admitted into its slot and the guard then dropped a completion (I-29).
This was the Loom-5 audit's owed F2 residual. The directory-mutation gate made
it worse, because its stat can hold a driver in that gap for a whole RPC. The
gap is now counted: `admitting` holds the slot from the consume, or the chain
claim, until the op's CQE posts, the op is in flight, or it parks HELD.
`loom.admission_counts_admitting` holds the arithmetic and the return to zero
on every path.

The wait side counts `admitting` too. A blocking `ENTER` no longer gives up while a sibling is mid-submit; with nothing in flight it sleeps rather than pumps, and only while that is still the state. Every release of a reservation wakes the CQ waiters, so an op that went in flight meanwhile finds a thread to pump it. `loom.wait_counts_admitting` holds it. A `DRAIN` counts `admitting` too: an op a sibling consumed and has not disposed of is a prior op the drain must wait for, and `loom.drain_waits_for_admitting` holds that.

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
handshake flag, **wake the joiner**, and switch away permanently. Teardown
sleeps on that wake and then reclaims it.

Until 2026-09-22 teardown *spun* on the flag instead, and that was a hang. The
spin runs inside a syscall body, and a syscall body is non-preemptible: the
timer interrupt arrives and the preempt check declines to switch, because a
thread inside a syscall is not a thread the scheduler may take the CPU from.
Servicing an interrupt is not scheduling a thread. With one CPU there is no
peer to run the poll thread either, so the spin could not end. Measured: at
`-smp 1` every boot hung at `loom-smoke`'s exit — its last line printed, then
silence, and the boot banner never arrived. The whole gate matrix runs at four
or eight CPUs, which is the only reason this was not a standing red.

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

The claim happens **inside the drop**, under the Burrow's own lock, through
`burrow_unref_settled` ([[sub-kernel-burrow]]). A drop that does not end the
occupancy leaves the record alone; the drop that does takes it.

This dossier previously recorded the older arrangement — claim, drop, and
restore the claim if the drop turned out not to free — and said "the window
between claim and restore is a real one, and its failure mode is deliberately
chosen: a concurrent settler sees the cleared record and skips, leaving a charge
that outlives its region until the payer's next release point. An over-charge on
the payer — never a refund to a Proc that did not pay." **That was wrong, and it
was not a chosen trade-off but an unnoticed defect (AS-R9).** The lock
serialised each of the three operations and none of the gaps. A sibling holder's
final drop inside the window freed the descriptor, so the restore wrote through
a dead pointer — usually an `extinction`, since the free clobbers `magic` and
SLUB does not zero the slot — and the holder that actually freed the region found
an empty record and refunded nothing. Both `loom_drop_pin_settling` and the
displaced registered-buffer pins now settle through the drop; the full analysis
and the repair are in [[sub-kernel-burrow]].

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
whichever it was not written for: one double-refunds, the other leaks. **The
source now says this**, where previously nothing did.

### The thread ledger is backstopped, and the shape of the backstop is the point

The thread-ledger pointer was dereferenced bare while its sibling, forty lines
away in the same teardown, was validated against the owning process's magic word
and stored identifier. Both rested on the **same** lifetime argument — the ring
handle is non-transferable and non-dup-able, so a ring is reachable only through
its creator's table, torn down while that process is still allocated.

That argument is still believed and is **no longer trusted at the use**, matching
what an earlier round did for the page ledger. The sharper reason: the thread
ledger's use is a **write** — a decrement — where the page ledger's is a read. If
the argument ever stopped holding, the page ledger degrades to a skipped refund
(inert, as designed) while this one decrements a *recycled* process's counter: an
under-count on a process that never charged, inflating its thread budget. The
[[inv-i32]]-breaking direction, and the one every other tie here is deliberately
broken away from.

**The obvious one-line fix is wrong, and wrong on paths that are exercised.**
Routing the settle unconditionally through the page ledger's liveness helper
reads the **page** owner — the one bound *last*. Both rollbacks (poll-thread
start failure, handle allocation failure) reach teardown with the thread charge
outstanding and the page owner still unset, so the helper returns nothing, the
uncharge is **skipped**, and a thread charge leaks for the process's whole life.
That converts a backstop into a defect.

The correct form validates only when there is something to validate:

> take the liveness-checked owner **if the page owner is bound**; otherwise use
> the thread owner directly.

And the reason the fallback is safe is the good part: **an unset page owner is
itself proof the process is alive.** It means setup never reached its last
stanza, so no handle exists, so the only reference to the ring is the local one
in the setup syscall — execution is inside the creator's own call. *Validate when
there is something to validate; rely on the synchronous path when there is not.*

**A defence and its subject can require opposite treatments at one call site.**
The instinct that produced the wrong fix is exactly the instinct the section
above warns against — treating the two pointers as duplication — and it survives
knowing better, because a substitution that removes a redundant-looking read
looks like tidying rather than like a semantic change.

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
wakes the park, **sleeps** on a second rendezvous until the exit handshake, then
reclaims the thread — and only then quiesces the remaining operations. The
thread deliberately holds no reference to the ring; one would deadlock this
join.

The wait must be a sleep rather than a spin, for the reason above: only a
voluntary switch can hand the CPU to the very thread being joined. One wrinkle
makes the sleep less obvious than it looks. Sleeping is refused outright for a
thread whose process is terminating — and a peer thread closing this descriptor
during a group exit is exactly such a thread. The refusal cannot be honoured:
abandoning the join means reclaiming a thread that is still live. If it is
running, that is fatal and loud; if it is *sleeping*, it passes every gate the
reclaim checks and frees a thread that later resumes on recycled memory — a
silent use-after-free, and the worse half of why no abandon path exists.

So the join is made uninterruptible with the mechanism the kernel already has
rather than a new one. Closing a dying process's handle table suppresses the
death check for its whole duration, precisely so that close hooks which must
WAIT — the 9P clunk flush, and now this join — behave as a live thread's would.
The join brackets itself in that same flag, saving and restoring rather than
clearing, because on the at-exit path the close already owns it and a bare
clear would re-arm the death legs for every later descriptor in the table.

That inherits the flag's own residual rather than escaping it: a poll thread
that never reaches its terminal parks the dying process unreapably instead of
burning a CPU. It is the better failure, and it is reachable — the
frame-boundary deadline does **not** bound a mid-frame receive, because the
body must complete or the shared stream desyncs, so a stalled server mid-frame
delays the stop until the frame ends. Termination rests on the servers being
trusted and prompt. That is a trust assumption, not a mechanism, and it is the
same one the clunk flush already rests on.

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

## The ring is a user-pool allocation (2026-09-23; B-1a' round-1 close)

`loom_create(sq, cq, exempt)` builds its ring through
`burrow_create_anon(size, exempt)`, so the ring's pages come from the physical
user pool with the creating Proc's exemption (`sys_loom_setup` passes
`proc_resource_exempt(p)`) and return at `free_pages` whoever frees them
([[sub-kernel-mm-phys]]); the eager charge record it stamps is keyed on the
paying ADDRESS SPACE's id, not the pid, so a non-CLOEXEC ring's close after
an exec never refunds against the successor's space (the audit's F4;
[[sub-kernel-burrow]]).

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
- **A spin inside a syscall body can never wait on a thread.** Syscall bodies
  run with interrupts on but are not preemptible, so an interrupt arriving
  mid-spin changes nothing about which thread holds the CPU; with one CPU
  nothing else can run at all. Any wait here whose writer is a *thread* — as
  opposed to an interrupt handler — must be a sleep. The distinction is the
  one this subsystem's teardown got wrong for three months.

  The operational form, because "is it bounded?" is the wrong first question:
  **ask who WRITES the value awaited.** A thread's write is unsafe here; an
  interrupt handler's, or an in-flight hardware transition's (`on_cpu`), is
  not. Boundedness is what you conclude *after* that answer, not what you
  check instead of it — the old spin was bounded in every author's head and
  hung 100% of uniprocessor boots for nineteen days.
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

[[chg-2026-08-02-async-sweep]], [[chg-2026-08-16-loom-charge-ledger]],
[[chg-2026-08-16-loom-backstop-closed]] (the thread-ledger backstop, and the
one-line fix that would have leaked). 2026-09-23 (L): the identity cape's two
Loom rules (the caped `GETATTR` copy; the caped create gid).

## Provided-buffer contract, not yet enabled (October 4)

The operator selected explicit pools for private streaming READ. The contract
in docs/ASYNC-SERVICE-BUFFERS.md reserves receipt/return ownership independently
of CQ consumption, preserving64/16-byte descriptors and correlation. Pool members
and service slots stay bounded; empty-pool backpressure cannot block cancellation.
No handlers or new mirrors are implemented by this documentation checkpoint.
AS-R8 in the async status also owns the raw Rust registration safety correction.

The supporting owner/admission/request engine at7571ad4e4 passed50/50 clean
CPU1/4/8 and kernel-UBSanCPU4/8 boots, with exact source/draft checks. Those
prerequisite results do not qualify the future private-ring or pool consumer.

## Provided-buffer mirrors compiled (October 4)

The three service headers/modules now include the five pool records and eight
new constants specified in docs/ASYNC-SERVICE-BUFFERS.md. The ABI gate checks
30 constants/10 records, actual serialized bytes, every asserted field offset
and new-record alignment, plus the full kernel header's unchanged64/16/88-byte
envelope and disabled private masks. Three intended mirror mutations are detected.
This supersedes the earlier mirrors-pending statement only; no pool handler is
enabled and the ownership model/implementation are still owed.

### Provided-buffer model gate (October 4)

[[spec-loom-service-buffers]] separates CQ acknowledgement from payload return
and source-local retirement from retained pool completions. Two bounded clean
runs (464/6416 states) and eleven named counterexamples pass. This is a design
model, with no enabled kernel pool handler or claim of actual payload safety yet.

The dormant pool transition module and shared native fixture are documented in
[[sub-kernel-loom-pools]]. Actual Loom buffer tests exercise it, but the private
setup flags and ring dispatch remain disabled pending owner integration.

## AS-2h: paired payload receipts and foundation qualification (October 4)

The combined foundation at26c21df87 passed50/50 clean boots across default
CPU1/4/8 and UBSan CPU4/8, ten each. Every failure category, including timing,
was zero. Source/index and four protected drafts matched; the runner released
Mac. This covers AS-2e pool core, AS-R8 Rust borrow correction, AS-2f worker
tickets and AS-2g preallocated protocol storage. It supersedes their earlier
broad-gate debt, not the incomplete private runtime. Evidence:
work/oct4-async-service/pool-foundation-matrix/verified.json.

New internal receipt geometry preserves legacy layouts and adds32bytes per CQ
slot only to the optional constructor. Maximum geometry is675840 page-rounded
bytes; allocator occupancy and charge belong to the private owner. Paired
publication uses private geometry/tail, copies CQE and receipt, makes the member
LEASED, then release-publishes the tail. Full CQ leaves PENDING intact. Ordinary
CQEs clear old receipts; the untyped producer cannot mint SERVICE_BUFFER. CQ
acknowledgement and terminal delivery never return a payload.

Actual-source host ASan/UBSan and eight intended assertion failures pass. The
release-store observer checks paired state at publication; it is not an ARM
weak-memory proof. The shared native fixture checks minimum/maximum geometry,
full CQ, corrupt mirrors, explicit return, clearing and counter wrap. Fresh CI
build and CPU1 boot1830/1830 pass. Pool model clean cases464/6416states and all
11 named counterexamples pass. Evidence:
work/oct4-async-service/owner-integration/receipt-host-passed.json and
receipt-native-passed.json. The earlier matrix predates this receipt change;
combined broad qualification of the new private owner remains an activation gate.

These internal helpers do not establish caller identity, pool-to-ring binding,
charges or MORE-before-terminal scheduling. Their caller must retain the ring
and pool, serialize all pool mutations with the same ring lock and enforce
request order. Close/exec/reaper, private slot/protocol integration and safe
owned clients remain. Public feature masks stay disabled; no clipboard, Pi or
fresh graphical qualification is claimed.

## The private-owner fixture, and the boundary it states about itself (2026-10-07)

`kernel/test/loom_private_fixture.h` was UNOWNED and is claimed here, beside
`loom_receipt_fixture.h` which it is built like. One entry point,
`loom_private_fixture()`, returns an error STRING or NULL, so a failure names its
own check (48 `LP_CHECK` call sites) instead of a line number a later edit
re-points. The count is the measured one: an earlier revision of this paragraph
said 45, which was the grep's answer including the macro's own `#define` line.

What it drives: the private owner's admission and retirement -- a refused
geometry leaving neither charge nor guard, exclusive image ownership at
admission, public setup and legacy execution staying refused on a private owner,
a returned BORROW not counting as a close, and the final/nonfinal retirement
discrimination.

Two details are load-bearing and easy to lose in a reformat:
- `lp_wait` spins on `loom_private_retired()`, a MONOTONIC counter, under a 5s
  deadline -- the retirer is a separate thread, so a retirement is OBSERVED
  rather than assumed complete on return. A gauge read as zero would otherwise
  be satisfied by "it never started".
- `lp_detach_settling` encodes the settling discipline `vma_detach_range_in`
  requires: `as->lock` held across the call, and the returned chain of Burrows
  whose last mapping went handed to `burrow_free_deferred` AFTER the unlock,
  because a FILE Burrow's free may sleep. Passing `payer` is what makes it settle
  at all -- NULL settles nothing and leaves an eager region charged, which is the
  safe direction and the wrong one for this fixture.

THE ONE LEG WHOSE WITNESS IS A MUTANT (2026-10-07). Retirement through creator
death was already covered -- the leg that calls `test_proc_drop` and then checks
the image's final charge -- but it keeps its own `addrspace_pin` across the whole
window, because inspecting the charge after the reap requires one. That pin is
the SAME `addrspace_lifetime_get` that `addrspace_private_begin` takes, so it
masks the property `loom_private_destroy` actually depends on: with a second
lifetime reference held, a ring that took none would still find its image
addressable. The added "unpinned-reap" leg holds none, asserts NOTHING about the
image (touching it would reintroduce the reference under test), and observes the
retirement only through the monotonic counter.

Its discrimination lives in a mutant, and the mutant's expected outcome is a
NAMED invariant failure rather than an arbitrary crash: remove the
`lifetime_get` in `addrspace_private_begin` and the matching put in `_end` while
keeping `++private_rings`, and the owner's drop inside `proc_free` becomes the
FINAL lifetime drop with a private ring still guarded -- which
`addrspace_lifetime_put` extincts on by name, "AddrSpace final lifetime drop with
private rings". The guard fires in the DYING PROC, before the retirer could reach
a freed descriptor, so the evidence is deterministic and attributable instead of
being whatever a use-after-free happens to do. (The first version of this
paragraph predicted the UAF; astra corrected it against the source on 0161 t53,
and the correction makes the experiment better, not weaker.)

That also states what the leg's content really is: it is the ONLY leg where the
owner's drop IS the final lifetime drop while a ring is outstanding. Under the
pinned leg, the fixture's own reference makes that drop non-final, so the guard
cannot fire there at all.

Two claims are kept apart in the leg because `lp_wait` cannot tell them apart:
it waits for `>= goal`, which is EVENTUAL retirement, so exactly-once is asserted
separately as a counter DELTA, with its precondition (nothing in flight at the
snapshot) asserted rather than assumed. Neither reads the dead image.

RELEASE IS NOT WITNESSED HERE, and how that was settled is worth more to a
reader than the conclusion, because three positions were held on it in one day.
First: "no witness without a production counter", a leaked `struct AddrSpace`
being one slab object nothing in this tree counts. Then: "page-granular after
all" -- the final lifetime drop also runs `proc_pgtable_destroy`, so a reference
never released strands PAGE TABLES, whole pages that `phys_free_pages` sees once
`magazines_drain_all` has run. Then OUT AGAIN, which is where it stands, because
the instrument cannot be made sound HERE. `phys_free_pages` reports
`g_zone0.total_free_pages` alone and an order-0 free goes to a per-CPU magazine
with its flags cleared -- "magazine ownership, not free list" -- so the reading
exists only after a drain, and `magazines_drain_all` walks EVERY CPU's magazine
with no lock and no IRQ mask. The tree's other 24 page-accounting call sites
free on the CPU they measure from, so for them that cross-CPU pass is a hazard
only. This leg cannot: the retirer frees the dying image on whatever CPU it ran
on, which makes the cross-CPU pass LOAD-BEARING for the reading, in a suite that
runs after `smp_init` with kthreads runnable. A measurement whose instrument
needs a quiescence the fixture cannot establish is a DIFFERENT claim, not a
weaker one.

astra named both halves on 0161 t55: normalise the snapshots under the drain's
required quiescence, and keep the claim narrow until an omitted-put-only mutant
shows the gauge can redden at all -- the balanced mutant tests ACQUISITION, not
release. The asymmetric baseline was a real defect in the first version (sample,
then drain only at the end, so earlier legs' magazine residue could fabricate a
positive delta or cancel a leaked page); the symmetric discipline the tree
already encodes is `test_cow.c`, which drains BEFORE and after and states its
claim as a delta between two runs one variable apart, and which records in its
own header that an order-0 free never reaches the buddy. The drain's cross-CPU
exposure is enqueued as a tracked bug against `mm/magazines.c` rather than left
in this prose: for the other call sites it is a latent SMP hazard, not an
instrument question.

RELEASE IS PAIRED IN THE SOURCE, which is a weaker claim than witnessed and is
recorded because it tells a reader chasing a guard leak where NOT to look. The
production set is small enough to enumerate, and the enumeration is the whole
value -- a negative over an uncounted set is a guess. `addrspace_private_begin`
has ONE production call site, `loom_create_private`; `addrspace_private_end` has
three, two of them that function's failure exits and one in
`loom_private_destroy`. After a successful begin the creator has exactly three
exits: the charge refusal, the layout-allocation failure, and success, after
which no further early return exists. `l->service_as` is written ONCE and never
cleared, so `loom_is_private` -- the sole routing discriminator, with nine
production readers -- cannot go stale. The only last-ref path is `loom_unref`,
which sends a private ring to the retire queue and everything else to
`loom_free`; `loom_free` is static with exactly that one call site, guarded by
`!loom_is_private`, so a private ring cannot reach the `kfree` that would skip
the release. (Every other mention of `loom_free` in the tree is a COMMENT, which
is why a bare grep reads alarmingly: count call sites, not mentions.) The retirer
pops and destroys unconditionally, and the destroy releases the guard LAST, after
the uncharge, with the retired counter bumped after that so a waiter's acquire
orders the frees. Every path that takes the guard releases it exactly once.

What that argument is blind to is the residue worth carrying. It shows the code
CONTAINS a release on every path and says nothing about one having EXECUTED. A
ring whose refcount never reaches zero never enqueues, so a reference leak would
strand the guard with this pairing fully intact -- and that reduces to ONE named
dependency rather than an open worry: the private path adds no ref-taker of its
own, `loom_ref` having exactly one production call site, inside
`handle_acquire_obj`, paired with `handle_release_obj`'s drop. Guard release
therefore inherits the handle layer's get/put balance, a separately owned
invariant, and nothing more. The consumer side is narrower still than it reads:
the retirer thread is created UNCONDITIONALLY at boot (the braces around it are a
bare scope, not a condition) and an allocation failure extincts, after
`loom_retire_init` and before the suite runs -- so "no consumer" is unreachable
and what remains is I-8 liveness, not a private-ring premise. The admission gate
is worth one caution: `service_retire_ready` is set by the queue's init, which
attests the queue's FIELDS and not the consumer's existence. Today the window
between the two is empty, so nothing can be admitted into it; if private
admission ever moves earlier than the thread -- the syscall enabling is where
that would happen -- the gate has to cover both halves or a ring will enqueue to
a list nobody drains. The page-accounting release is a separate claim: the uncharge arithmetic
in the destroy is not covered by any of this. And the two `extinction()` arms in
the destroy precede the release, so a private ring in legacy state dies LOUDLY
rather than leaking -- those arms are not leak paths.

RUN AT LAST, AND BOTH HALVES NOW HOLD (2026-10-07, across two leases). The leg
EXECUTES AND PASSES: a full suite on the gate image reported `tests: 1836/1836
PASS` with `[test] loom.private_owner_lifecycle ... PASS` by name, `test.sh`
exit 0 and no extinction, on the pinned Stratum, with the control kernel
preserved beside its nine boot inputs. So the reachability this dossier called
unproven is now demonstrated: a private ring created on a Proc that is then
reaped does retire on the ring's own image reference, through the real retirer,
with no fixture pin holding the image up.

THE MUTANT HALF DOES NOT HOLD, and the reason is a property of the TREE rather
than of the leg. The balanced mutation of `addrspace_private_begin`/`_end` is
LETHAL IN AN EARLIER TEST: `test_addrspace.c`'s `private_ring_sharing_failure()`
-- the first statement of `addrspace.proc_alloc_in_shares` -- ends with
`addrspace_private_begin(as); addrspace_unref(as);` and asserts the space
SURVIVES with zero owners precisely because the guard pins it. Strip the guard's
reference and that unref becomes the final lifetime drop with `private_rings ==
1`, so the named extinction fires there, about ninety suite lines before
`loom.private_owner_lifecycle` is reached. The boot never ran the leg.

TWO CONSEQUENCES, both worth more than the failed run. First, the property the
mutant was built to witness -- that the guard's lifetime reference is
load-bearing -- is ALREADY witnessed in-tree, and witnessed POSITIVELY by that
`ownerless` assertion rather than by a mutation, which is the stronger shape.
Second, a mutation of a SHARED primitive cannot discriminate one caller's leg
while an earlier test exercises the same primitive; the mutant has to be confined
to the CALLER's use. The confined form leaves `addrspace.c` untouched, so every
addrspace test behaves normally and the boot reaches the leg, and it is BALANCED
over two sites: `addrspace_unpin` at `loom_create_private`'s SUCCESSFUL return --
after the charge and the layout have both succeeded, so the two rollback exits
stay balanced and the mutant rests on no assumption about allocation failure --
and a direct `--as->private_rings` under `as->lock` in the destroy, replacing
`addrspace_private_end`. Balance is the load-bearing word: the obvious confined
form cancels the create's get and leaves the destroy's put, so every ring cycle
nets -1 on the refcount and an earlier leg dies with a DIFFERENT message. The
outcome is deterministic rather than racy, and the source settles that rather
than the hope: `proc_free` releases the address space BEFORE it calls
`handle_table_free`, so at the lifetime drop the handle table is
intact, `loom_unref` has not run, nothing is enqueued and `private_rings` is 1 --
the retirer never gets a turn.

AND THAT FORM HAS NOW RUN: THE ACQUISITION WITNESS IS CLOSED (2026-10-07
20:46:46-20:49:43Z, under a second lease, three minutes on a warm `build/`).
A FRESH control first, because the fixture had changed: `tests: 1836/1836 PASS`
against the DERIVED expectation of 1836 registrations, the leg announced with
its arrival marker, the NORMAL teardown marker, a PASS verdict in its own block,
`test.sh` exit 0, control kernel `dd0c4e67c7306ae0`. Then the mutant, both halves
verified present, kernel `92f1dbb1c7de3778` and therefore not the control's:
`test.sh` exit 1 and ONE extinction, by name --

    [test] loom.private_owner_lifecycle ... [lp-mark] unpinned-reap-owner-drop
    EXTINCTION: AddrSpace final lifetime drop with private rings

-- with the arrival marker present, NO cleanup marker, and NO verdict, which
together say the boot died AT the drop under test rather than anywhere else in
the same test. The Halls dump then attributes it independently of any of that
instrumentation: frame #4 is `test_loom_private_owner_lifecycle+0xb14`. So the
guard fires in this leg because the ring's own lifetime reference is gone, and
the reference is load-bearing for a private ring outliving its creator's Proc.
ACQUISITION ONLY: the release half still has no execution witness, and the
structural pairing recorded above remains its whole basis.
Recovery ran on the same exit: two mutant images quarantined out of `build/`,
`kernel/loom.c` restored and hash-verified to its pristine `cbdd71f6f5ee4c74`,
and the clean rebuild BYTE-IDENTICAL to the control. The whole run -- two
`--config ci` bakes and a third for recovery, plus two full boots -- cost the
volume 499 MiB (11359 -> 10860 MiB), which is the figure a peer had asked for
and nothing had retained until this run kept its own readings.

The runner's own oracle was the thing that called the failed run a success, and
the defect is instructive: it asked only that the leg not report PASS, which a
leg that never ran satisfies just as well. It now asks POSITIVELY which test the
boot was inside when it died, derived from the log's last announcement, and
refuses when that is not this leg -- driven against the real failed log, where it
rejects.

THAT IS NECESSARY AND NOT SUFFICIENT, which is why the fixture now carries
ARRIVAL MARKERS. `LP_CHECK` is `goto done`, and the cleanup at `done:` unrefs the
ring and drops the owner as well -- so under any mutant that strips the ring's
image reference, an earlier check failure reaches an owner drop with a ring
outstanding and raises the SAME named extinction inside the SAME test, while the
check that actually failed never reaches the log because the boot ends before the
suite can report it. Line order is not execution order, and a per-leg argument
from line numbers does not survive a `goto`. The fixture therefore prints one
marker immediately before the target leg's owner drop and a different one before
the cleanup's, the mutant stage requires the first and refuses on the second, and
the control stage requires the first too -- a control whose leg never reached that
drop would pass without exercising the operation under test, which is the
quietest way for a comparison to mean nothing. The cleanup marker also prints the
failing check's message, so the hidden failure becomes visible rather than being
replaced by its own consequence.

AND THEN THE MARKERS BROKE THE ORACLE THEY WERE ADDED TO SERVE (astra, yip 0161
t63), recorded here because the defect lives in the INTERACTION and not in either
piece. `done:` is ALSO the normal fallthrough from the leg's last check, so a
PASSING run prints a cleanup marker -- and an oracle refusing on ANY cleanup
marker refuses every healthy run. The marker now names which arrival it is,
`normal-fallthrough` or `after-check-failure: <msg>`, so neither case is inferred
from the absence of the other. And `test.c` prints `    [test] <name> ... `
WITHOUT a newline, runs the test, and prints the verdict afterwards
(`test_run_all`), so a marker's own newline moves
the verdict onto a later line: `<name> ... PASS` no longer exists on one line in
an instrumented healthy run, and the mutant stage's completion check -- which
looked for PASS or FAIL on the announcement line -- had become a check that could
not fire. Both stages now read the LEG'S OWN BLOCK, its announcement to the next
announcement, and treat the verdict as a STATE in that block: PASS, FAIL, or
NONE, where NONE is the lethal mutant's expected state and a different thing from
FAIL. Neither defect was reachable by the marker-only synthetic arms that
preceded them, because both live in the interaction with the suite's own output
and those arms were built by hand from PRE-INSTRUMENTATION logs -- so the accept
case could not show that the instrumented healthy run no longer matched. The arms
are now built by editing REAL boot logs at the leg's own line, every gate in both
oracles has one, and two arms are real logs unedited, including the one the first
oracle called discrimination.

THE ADMISSION REFUSALS, EDGE BY EDGE (2026-10-08, authored and UNRUN). Checkpoint
1 asks for every refusal and unwind edge before publication, and the fixture drove
only two of `loom_create_private`'s: invalid geometry (refused before guard and
charge) and a shared image (the guard is never taken). The charge refusal is now a
leg. It runs on a fresh space whose cap cannot cover the admission, and asserts the
preconditions that route the call to that branch: single owner, non-exempt, the
geometry admitted just above on the default cap, and a bound derived from that
admission's measured charge, not hand-counted. It then asserts that the charge, the
private-ring count AND the lifetime reference all return to baseline. The reference
is checked on its own because a leaked guard also holds one: the final drop's
private-ring check never runs, the leak is otherwise silent, and a split defect
could clear the count and keep the reference (astra, yip 0161 t67). Its RED is a
confined one-site mutant, deleting `addrspace_private_end` from the `!charged`
branch only. It is attributed to the leg's OWN assertion: the exact verdict, and
the fixture's `after-check-failure` marker naming the same check. The resulting
`kernel test suite failed` is required to be the ONLY extinction and is never
accepted on, because any FAIL produces it. The fixture's cleanup releases a whole
leaked guard after the assertion records the failure, so the mutant cannot carry
the leak into later tests. The oracle's 18 arms pass off-lease, built from real
guest logs including a retained real FAIL of this fixture. The guest run is owed.
The layout-allocation unwind (`loom_create_layout` failing after the charge:
uncharge, then `addrspace_private_end`) is STRUCTURAL ONLY and its runtime
obligation stays OPEN. `kernel/` has no allocation fault seam. A minimal,
test-only, locally scoped one is owed for review, not assumed. Neither edge bears
on the retirement's release half, which stays open.

THE BOUNDARY, which the header states and this dossier repeats because a reader
of the vault may never open the header: scheduling is FORCED here. Handles are
opened and closed directly and the fixture waits on a counter, so nothing in it
demonstrates reachability from a syscall pair, and no such claim is made. The
checks are the reviewed set from the paused owner-integration draft (astra,
yip 0161) plus the final/nonfinal discrimination that set could not cover,
because every retirement in it ends the ring's occupancy and so refunds the whole
charge. The private runtime remains gated -- no syscall reaches
`loom_create_private` -- so this fixture is currently its only driver.
