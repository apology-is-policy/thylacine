---
id: sub-kernel-loom
type: sub
parent: moc-kernel-async
title: "Loom — the io_uring inversion over 9P"
code:
  - kernel/loom_service_fids.c
  - kernel/include/thylacine/loom_service_fids.h
  - kernel/loom_service_operation.c
  - kernel/include/thylacine/loom_service_operation.h
  - kernel/loom_service_reply.c
  - kernel/include/thylacine/loom_service_reply.h
  - kernel/loom_service_decode.c
  - kernel/include/thylacine/loom_service_decode.h
  - kernel/loom.c
  - kernel/loom_service_close_queue.c
  - kernel/include/thylacine/loom_service_close_queue.h
  - kernel/loom_service_slots.c
  - kernel/loom_service_requests.c
  - kernel/loom_service_connect.c
  - kernel/loom_service_control.c
  - kernel/include/thylacine/loom_service_control.h
  - kernel/loom_service_engine.c
  - kernel/include/thylacine/loom_service_engine.h
  - tools/check-loom-service-index.py
  - kernel/include/thylacine/loom_service_connect.h
  - kernel/loom_service_protocol.c
  - kernel/include/thylacine/loom_service_protocol.h
  - kernel/include/thylacine/loom_service_requests.h
  - kernel/test/loom_requests_fixture.h
  - tools/host-tests/loom-service-requests.c
  - tools/test-loom-service-requests.py
  - kernel/loom_service_nodes.c
  - kernel/include/thylacine/loom_service_nodes.h
  - kernel/test/loom_node_owner_fixture.h
  - kernel/test/loom_nodes_fixture.h
  - tools/host-tests/loom-service-nodes.c
  - tools/test-loom-service-nodes.py
  - kernel/include/thylacine/loom_service_slots.h
  - kernel/include/thylacine/loom_service_targets.h
  - kernel/test/loom_slots_fixture.h
  - tools/host-tests/loom-service-slots.c
  - tools/test-loom-service-slots.py
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
updated: 2026-10-10
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

THE ADMISSION REFUSALS, EDGE BY EDGE (2026-10-08; the charge refusal RAN, below). Checkpoint
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
accepted on, because any FAIL produces it, and `test.sh`'s own status must be
nonzero too: a guest FAIL that the harness reports as success is a harness
inconsistency, not a pass (astra, t69). The fixture's cleanup releases a whole
leaked guard after the assertion records the failure, so the mutant cannot carry
the leak into later tests. An admission that wrongly SUCCEEDS is counted into the
expected retirements before its check fails, so cleanup waits for that ring too
and no asynchronous work escapes the leg. The oracle's 18 arms pass off-lease, built from real
guest logs including a retained real FAIL of this fixture. AND IT HAS RUN
(2026-10-08 08:35:38-08:38:54Z, under lease, on source 6f2bd6226): the fresh control
`tests: 1836/1836 PASS`, the leg PASS in its own block with the normal-fallthrough
marker, `test.sh` exit 0, kernel `dd0a56361d1e7b9d`. The mutant, kernel
`e883d57ffdb5ba46`: the leg's block carries `after-check-failure: refused charge
leaves no guard, reference or charge` and the identical `FAIL:` verdict, it is the
ONLY FAIL in the boot, `tests: 1835/1836 FAIL`, the only extinction is `kernel test
suite failed`, and `test.sh` exit 1. So the call does route through the charge
branch, and the assertion does see a leaked guard. `kernel/loom.c` was restored to
its pristine hash and `build/` was rebuilt byte-identical to the control. That is one
boot per side on the Mac axis. It is not an SMP qualification, and nothing is
activated by it.
THE LAYOUT-FAILURE UNWIND (2026-10-08; it RAN, see the end of this paragraph). This edge is
`loom_create_layout` failing AFTER the charge, which should uncharge and then call
`addrspace_private_end`. Nothing in `kernel/` could make an allocation fail, so a
test-only seam was added, with its scope reviewed first (astra, t71). It is a
`KERNEL_TESTS` one-shot in `loom.c`: arm, disarm and armed accessors, taken as the
FIRST statement of `loom_create_private`, so an early refusal cannot leave it armed.
It is compiled out of the production shape. It is sound only because the fixture
is that function's only caller, and it must be rescoped before any concurrent or
engine caller exists.

The leg checks the seam itself:
- the shot is clear before arming;
- an armed call refused EARLY still spends it;
- the next admission is not faulted.

It then takes its baselines before arming, checks the cap against the admission
measured above so the charge succeeds and the layout is what fails, and asserts
the CHARGE and the GUARD (count, reference, owner) SEPARATELY. Two mutants, each
applied to pristine source, delete one half each, the uncharge (M1) and the
`private_end` (M2), and each must fail the leg at its OWN message. The oracle's
cross-arms show neither prediction is satisfied by the other's log. The runner
then compiles `loom.c` with `KERNEL_TESTS` off against the seam's parent: no seam
symbol may survive, and `loom_create_private`'s machine code must be identical.
`loom_create_layout`'s OWN inner failure path (its `kfree` when
`burrow_create_anon` fails) is a separate obligation, with its own leg below.
Neither edge bears on the retirement's release half, which stays open.

AND IT RAN (2026-10-08, 11:52:13-11:56:19Z, under lease, on source 9bb7e6037).
- Control: `tests: 1836/1836 PASS`, the leg PASS in its own block, `test.sh` exit 0,
  kernel `2b56a62ae6cc92ae`. That PASS includes every seam-contract check.
- M1 (the uncharge deleted, on pristine source, kernel `5a8a5ac93fb18e32`): the leg
  failed at `layout failure returns the charge`.
- M2 (the `private_end` deleted, on pristine source, kernel `1458457252f31eaa`): it
  failed at `layout failure releases the guard, reference and owner`.
- Each failure is named twice (the verdict and the `after-check-failure` marker) and
  is the ONLY FAIL in its boot: `tests: 1835/1836`, `test.sh` exit 1, and only the
  suite's consequential extinction.
- Shape, with `KERNEL_TESTS` off against the seam's parent: no seam symbol,
  identical symbol names, and `loom_create_private` identical in all 127
  instructions AND all 13 relocations. The comparison was shown to refuse a
  relocation-only change, by an object-level control that retargeted one CALL26
  without touching a byte of code.
- `kernel/loom.c` was restored and `build/` rebuilt byte-identical to the control.
That is one boot per side on the Mac axis: not SMP, not activation.

THE INNER UNWIND (2026-10-08; AUTHORED, NOT YET RUN). `loom_create_layout` allocates
the Loom metadata and then the ring Burrow, and when the Burrow is refused it must
`kfree` the metadata itself, before the caller's unwind runs. That path is shared
by every Loom constructor, so the fault cannot lean on the fixture-only argument
the outer seam uses (astra, t75). It is a `KERNEL_TESTS` one-shot KEYED TO THE
ARMING THREAD, taken only after the metadata allocation succeeded, and consumed
by a compare-and-swap from the current thread to NULL. A NULL current, before
`thread_init`, is refused explicitly, or the swap would match a disarmed slot and
fault an unarmed boot-time caller. The only production caller of the
constructors is `SYS_LOOM_SETUP` on a user thread, which can never match.

The observer is the hard part. `struct Loom` is larger than 2048 bytes (a
`_Static_assert` in `loom_create_private`), so it is a LARGE kmalloc, and `kfree`
returns it through `free_pages` directly, never through `kmem_cache_free`. A
freelist walk after the call races with reuse by another CPU, and no gauge is
specific to one object, so slub gained a single-slot `KERNEL_TESTS` watch on the
validated large-free branch, described in [[sub-kernel-mm-slub]]. The take arms
it on the metadata while that is live and unpublished. If `kfree` runs, the watch
fires at the call. If `kfree` is omitted, the pages stay allocated, so nothing can
fire it. It records ENTRY to the free site, not the buddy outcome. The first
version keyed the notifier to the POINTER only, and astra found a stale-notifier
interleaving in review (t81). The self-check's freed object is reused on another
CPU, and that CPU's `kfree` loads the still-armed watch and stalls. Meanwhile the
fixture re-arms on the Loom, and the stalled store then lands on the new arming,
so a `kfree`-deleted mutant would PASS. The notifier is now keyed to the arming
thread as well. A cross-thread control drives the property that closes it: a
helper kthread frees a watched object, and the watch must stay quiet.

The fixture waits for an explicit terminal publication before calling the shared
join helper. That helper returns `void` even after its bounded wait times out;
returning from it alone cannot prove completion or reaping. A fixture-local
timeout disarms the watch and stops before the inner leg. The transferred object
stays the helper's and is never speculatively freed. Its static handoff storage
outlives the failed fixture. A late publication does not overwrite the captured
timeout. The host control extracts this wait and control block from the source;
its scheduler/allocation doubles do not qualify native scheduling.

The leg first checks the watch on a same-class pair: an unwatched free stays quiet
and the watched one fires. Each object is freed exactly once before any verdict
is read. Then the leg admits with the ring fault armed and asserts, in this
order: the refusal, the spent shot, the watched metadata, the charge, and the
guard/reference/owner; the ORACLE (the watch fired) comes last in the leg. A
`LP_CHECK` failure jumps to cleanup, so a lone FAIL at the oracle also shows that
every earlier check passed. A free that went unobserved is UNKNOWN, not owned:
the leaked metadata of the `kfree`-deleted mutant is never reclaimed, and stays
an intended, bounded leak until its disposable VM ends (astra, t77).

The runner (`work/oct5-as-r9/inner-leg-run.sh`) has three mutants, each applied to
pristine copies of both files:
- `kfree(l)` deleted: must fail ONLY the oracle.
- the watch's hook deleted: must fail ONLY the self-check, which precedes the
  arm, so the inner leg does not run under that mutant.
- the watch's thread check deleted: must fail ONLY the cross-thread control,
  which also precedes the arm.
It requires exactly one matching failure diagnostic in addition to the named
verdict. `test_fail` overwrites a test's final message, so a lone expected verdict
otherwise could hide an earlier helper timeout. A constructed-log negative
control covers that masking case; it is not guest evidence.
The shape stage compiles `loom.c` and `slub.c` with `KERNEL_TESTS` off and compares
`kfree` and every emitted Loom constructor, with relocations. The kmalloc-NULL
edge returns before anything is allocated, and its caller-side NULL is the one
the outer leg already discriminated. It is recorded as its own STRUCTURAL row:
a bounded coverage statement, not runtime closure of every allocation failure.

THE BOUNDARY, which the header states and this dossier repeats because a reader
of the vault may never open the header: scheduling is FORCED here. Handles are
opened and closed directly and the fixture waits on a counter, so nothing in it
demonstrates reachability from a syscall pair, and no such claim is made. The
checks are the reviewed set from the paused owner-integration draft (astra,
yip 0161) plus the final/nonfinal discrimination that set could not cover,
because every retirement in it ends the ring's occupancy and so refunds the whole
charge. The private runtime remains gated -- no syscall reaches
`loom_create_private` -- so this fixture is currently its only driver.


October 8 runtime evidence: `inner-leg-20261008T195301Z` on 2bf48f7ad
discriminated all three pristine-source mutants at their distinct assertions,
with one failure each (1835/1836) and a green 1836/1836 control. Test-off kfree
and four emitted Loom constructors match the baseline including relocations;
no test symbols survive. Sources and clean control ELF/bin restored exactly.
This closes the inner-unwind witness at one Mac default boot per side only.
The observer records free-site entry, not buddy completion. Release sensitivity,
new-source full gates and engine admission/retirement remain separate. Full
self-review results and pins: work/oct5-as-r9/astra-takeover/inner-run-20261008T195301Z-result.json.


The next release-sensitivity probe retains the creator's original owner and
compares its exact lifetime count after the retirer publishes completion. An
omitted-put-only mutation at successful retirement must fail that count while
leaving charge/guard/owner assertions satisfied. This targets the decrement;
final descriptor/page-table reclamation is still separate. No production
observer or magazine drain is added. Prepared, guest UNRUN; runner and checker
are work/oct5-as-r9/release-leg-run.sh and release-oracle-arms.sh.


Release decrement now discriminated in release-leg-20261008T200530Z on
2a4e501c7: 1836/1836 control and 1835/1836 omitted-put-only mutant, sole failure
at the exact ref-count check. Self-review verified source and control ELF/bin
restoration, nine pinned inputs and raw diagnostics. The retained creator owner
makes this a decrement witness only; final image reclamation stays open.


Current fixture full Mac qualification, October 8: actual ci-smp-gate.sh on
98c50e844 completed all 50 default-smp1/4/8 and UBSan-smp4/8 boots cleanly.
Every boot reports 1836/1836 and private-owner normal fallthrough, D7 overlap
and twenty distinct user cycles. Ten exact input hashes and 100 raw logs
preserved in work/oct5-as-r9/current-gate-20261008T201816Z; self-review result
in astra-takeover/current-gate-20261008T201816Z-result.json. This is control
coverage on Mac HVF, not new mutant or Pi evidence. Final image reclamation
and the private engine remain open; no activation or Main landing.


October 8 host boundary evidence: final-reclaim-control.py extracts private
retirement, AddrSpace lifecycle and the MMU table destructors. Both owner-drop
orders free one descriptor and four table levels under ASan/UBSan; omissions
of private put, descriptor free, table destruction and root free are each
detected. Raw sources/diffs/results: work/oct5-as-r9/astra-takeover/
final-reclaim-20261008T211401Z/. Allocator, lock, VMA and ring doubles bound this
to sequential host destruction-call sensitivity. Guest reclamation and the
private engine remain open; production and guest fixture bytes are unchanged.


## Shared private slot ledger (October 8, Astra)

`loom_service_slots.c` implements the approved ring-global 64-slot identity and
borrow ledger as a dormant engine component. One 1552-byte table holds 64
24-byte records. This is an internal layout, not a QUERY protocol state or wire
ABI. All calls require the owning ring lock. Reservation occupies capacity and
burns a nonzero monotonically increasing incarnation; rollback releases capacity
but cannot rewind it. Exhaustion refuses instead of wrapping. Only exact live
references of the requested kind borrow; retirement stops new borrows, and reap
requires zero existing borrowers. Close freezes reserve, publish and pin while
allowing rollback, unpin and reap to finish. Usercopy can occur between calls;
publication rechecks close under the same lock.

The ledger does not itself enforce native authority, FID parent-scope binding,
terminal-completion publication, payload retention or protocol retirement. Those
are caller obligations before retire/reap and remain implementation work. It is
not yet allocated/charged inside Loom or reachable from a syscall. No new public
mode is enabled. In particular, delivery of a result already committed before
abort is permitted by the approved lifecycle: do not add a blanket close/abort
rejection to completion delivery in place of engine lifetime integration.

The shared host/guest fixture tests rollback/stale references, provisional
visibility, mixed-kind capacity, pinned retirement, exhaustion and close versus
publication. Host ASan/UBSan passed and 13 exact-site mutations each failed its
named assertion; the existing lifecycle model passed 5828 states and all seven
buggy configurations. This is single-agent self-review, not an independent
review. Native guest and full current-source gates are pending at this checkpoint;
the 50-boot result on 98c50e844 applies only to that older source. Exact host
inputs and raw evidence: work/oct5-as-r9/astra-takeover/slots-20261008T2130Z/.


October 8, native link correction: current-gate-20261008T213009Z stopped
before any boot (gate.rc=runner.rc=1). The fixture's 1552-byte aggregate zero
assignments lowered to an undefined libc memset despite freestanding flags.
Replaced those two resets with the byte-loop convention used by existing pool
and receipt fixtures. No production ledger behavior changed. Native kernel
link now succeeds; all 14 actual-module ASan/UBSan host outcomes were rerun
successfully with the corrected shared fixture (slots-20261008T2132Z/).
The original failure and original host evidence remain preserved. Full native
matrix remains pending until a new, exact-source run completes.


## October 8, 22:14 UTC: slot-ledger full Mac gate verified

current-gate-20261008T213148Z completed on exact clean source
ceac7f380334a272cbc8e37f3ac9b1923e28f761 with gate.rc=runner.rc=0.
All five standard rows (default smp1/4/8, UBSan smp4/8) passed 10/10, zero
other classifications. Each of the fifty retained serial logs independently
shows 1837/1837, loom.service_slots PASS, private-owner normal fallthrough PASS,
D7 overlap and twenty distinct d7user00..19 completed cycles, plus boot banner.
No extinction or kernel-test failure. The two optional graphics diagnostics
remain explicitly absent programs in the exact captured ramfs, not passes.

The retained verifier checked all ten captured config/kernel/ramfs/pool/key
hashes, source identity and clean end diff, and saved 100 serial/harness logs
with per-file hashes. Wrapper checks also matched live immutable build inputs
to capture. Stratum remained clean at exact 61dde37. Minimum recorded admission
space was 10757 MiB, above the unchanged8192MiB floor; Mac lease released.
Result: astra-takeover/current-gate-20261008T213148Z-result.json. The captured
input generation is PINNED. This qualifies the dormant ledger's shared fixture
on the recorded Mac source. Caller locking, native authority, scope/payload/
terminal integration and owner charging remain unimplemented here; sequential
fixture success does not close them. Guest final allocator reclamation and
Pi/A72/KVM remain open; host destruction controls remain separately bounded.
Self-review only, no independent audit. Continue private owner integration before
MM0-MM4; no Main landing, private async/clipboard activation or128MiB removal.


## October 8: charged slot storage attached to the private owner (runtime pending)

Private construction now precharges one additional buddy page, allocates it zeroed
for the1552-byte slot ledger, and publishes it only with a fully constructed Loom.
Allocation failure and downstream layout failure free acquired storage before
refunding and dropping the exact image pin. Close freezes ledger admission under
the existing Loom lock. Final retirement requires all records FREE and unpinned,
frees the page outside locks, then follows the existing ring settlement and
metadata refund ordering. Internal users must retain their Loom reference across
slot calls/borrows. No service handler is enabled: the future engine must retire
its records before releasing its last owner reference, or the guard fails closed.
Pool storage, authority, parent scopes, terminal/payload gates and workers remain
next integration work; this is not a complete service engine.

Native linked DWARF measures Loom2864bytes (one buddy page) and the slot ledger
1552bytes (one deliberately whole-page allocation). Base private metadata is now
two pages; ring backing plus conservative Burrow descriptor attribution stays
separate. Legacy Loom storage gains one pointer but stays in its existing buddy
order. See astra-takeover/slot-owner-layout.json and retained DWARF output.

Self-review and actual-source host ASan/UBSan: slot-owner-20261008T222200Z has
8 controls (success, six refusals, busy-retirement guard) and7 exact-site mutants
for charge, unwind free/refund/pin, retirement free, close freeze and busy guard.
Every mutant fails precisely its named assertion. final-reclaim-20261008T222206Z
passes both owner-drop orders and five omitted-release controls, now including
slot-page free. Host geometry/Loom/image/lock/allocator doubles bound these
results; they are not guest physical accounting or concurrency proofs. Intentional
mutant leaks are process-bounded with LSan disabled, ASan/UBSan still enabled.

Existing lifecycle model passed5828 states and all7 named buggy verdicts
(slot-owner-model-20261008T2220Z). Real ARM syntax and native kernel linking pass.
New separate loom.private_slot_owner fixture covers allocation refusal, downstream
rollback, exact charge, close with held borrow and final refund; expected suite
count1838. Runtime/full Mac gate is still pending on this changed source; the
50-boot1837 result applies only to ceac7f380. This is self-review only. Preserve
all activation guards, Main boundary, guest final-allocator/Pi residuals and128MiB.


## October 8, 23:06 UTC: charged slot-owner full Mac gate verified

current-gate-20261008T222442Z completed with gate.rc=runner.rc=0 on exact
7c184b3ef2a1c23dbc28268abd6c12b45a3dab25, tracked source clean throughout.
All five standard rows passed10/10 (default smp1/4/8, UBSan smp4/8), with no
other classifications. The verifier independently checked all50 serial logs:
1838/1838, loom.private_slot_owner PASS, loom.service_slots PASS, private-owner
normal fallthrough PASS, D7 overlap and twenty distinct d7user00..19 completed
cycles, plus boot banner. Optional graphics programs were absent from the exact
captured ramfs; their two nonfatal diagnostics do not count as graphics coverage.

Ten captured input hashes verified, live immutable build inputs matched capture
at wrapper exit, and100 serial/harness logs were retained with hashes. External
Stratum remained clean at exact61dde37. Minimum recorded admission10930MiB was
above the unchanged8192MiB floor. Mac lease released; input generation marked
PINNED. Full result: astra-takeover/current-gate-20261008T222442Z-result.json.

This qualifies the recorded slot-page constructor/refusal/close/retirement
integration on Mac. It does not qualify future pool storage, authority, parent
scope/protocol/terminal/worker integration, guest final allocator reclamation,
or Pi/A72/KVM. The host omitted-free witnesses stay at their documented boundary.
Next is the approved pool table/member ownership and publication integration;
async lifecycle still precedes MM0-MM4. Self-review only, not independent audit.
No Main landing, private async/clipboard activation or128MiB protection removal.


## October 8, 23:22 UTC: joint pool registration and charged owner storage

The private owner now allocates a zeroed7176-byte pool table (one64-cell bank
plus64 pool descriptors indexed by the existing shared service slot). Joint
prepare reserves both identity and member capacity before output copyout.
Publish rechecks both provisional phases and rejects close without half commit.
Rollback releases both exclusions; reap refuses borrowers, streams, BUSY,
PENDING and LEASED members before modifying either ledger. Exact payload return
permits reap; neither CQ consumption nor slot removal returns a payload. The
ring-global incarnation/nonce counters never reset. All calls require the same
Loom lock and retained owner reference; backing pins are released after unlock.

Construction precharges two additional buddy pages and frees pool storage before
refund on every downstream failure. Retirement requires both tables empty and
frees them before ring settlement, metadata refund and exact image-pin release.
Native linked DWARF measures Loom2872bytes (one buddy page), pool table7176bytes
(two pages), plus the deliberately whole slot page: four metadata pages total.
Ring backing and conservative Burrow descriptor attribution remain separate.
See astra-takeover/pool-table-layout.json and retained native link/DWARF output.

Self-review: under the common lock, pool_publish cannot fail after slot_publish
succeeds because its RESERVED precondition was already checked; rollback and
reap likewise validate all fallible prerequisites before either ledger is freed.
Close blocks new registration/publication without destroying retained payloads.
The existing pre-abort committed-result delivery semantics remain intact.

Bounded ASan/UBSan host evidence: pool-table-20261008T2320Z control plus7 targeted
transaction mutants; slot-owner-20261008T231734Z nine controls plus10 charge/free/
close mutants; final-reclaim-20261008T231743Z both owner-drop orders plus6 exact
omission mutants. Every mutant failed its named assertion. The owner/final-drop
fixtures use checked allocator, geometry, image, lock and VMA doubles, so these
are call/free/order witnesses, not guest physical reclamation or race proofs.
Lifecycle model clean5828 plus7 named counterexamples and buffer models clean464/
6416 plus11 named counterexamples passed in pool-table-*-model-20261008T2312Z.

Native build/link passes. New loom.service_pool_table shares the transaction
fixture with host; loom.private_slot_owner now covers pool-allocation refusal,
downstream unwind and the full metadata charge. Expected suite count1839; the
full Mac gate is pending for this source. Prior50/50 applies only to7c184b3ef.

Still owed before activation: actual authority/resolution and canonical Burrow
pins, accepted fixed-I/O exclusion integration, parent scopes, protocol/worker/
terminal retirement and full client payload lifecycle. These helpers do not
authorize service syscalls. Guest final allocator reclamation and Pi/A72/KVM
remain open. Self-review only, no independent audit. Continue async before
MM0-MM4; no Main landing, private async/clipboard activation or128MiB removal.


## October 9, 00:01 UTC: pool-owner full Mac gate verified

current-gate-20261008T232028Z finished with gate.rc=runner.rc=0 on exact
1295a25da8b546498661c359523917187c01d832, tracked source clean throughout.
All five rows passed10/10 (default smp1/4/8, UBSan smp4/8). Raw-log verification
confirmed1839/1839 on all50, including service_pool_table, private_slot_owner,
service_slots and private-owner normal fallthrough. Every boot passed D7 overlap
and twenty distinct d7user00..19 completed cycles plus the boot banner.
Ten captured input hashes verified and100 serial/harness logs retained with
hashes; wrapper exit also matched live immutable build inputs to capture.
External Stratum remained clean at exact61dde37. Minimum recorded admission
10974MiB exceeds the unchanged8192MiB floor. Mac lease released; inputs PINNED.
Optional Venus/Vulkan programs were absent from this ramfs, not qualified.

Evidence: astra-takeover/current-gate-20261008T232028Z-result.json and the run's
verify.py, verified-logs and input manifest. This source-specific Mac result
qualifies the pool/slot storage and transaction fixtures, not the remaining
private authority, buffer-pin/fixed-I/O exclusion, scope/protocol/worker/terminal
integration. Guest final allocator reclamation and Pi/A72/KVM remain open.
Next: registered-buffer ownership using the approved canonical Burrow and exact
creator/image rules. Async work still precedes MM0-MM4. Self-review only; no
Main landing, private async/clipboard activation or128MiB protection removal.


## October 9: internal private registered-buffer ownership (runtime pending)

loom_private_register_buffers is an internal-only admission path. It requires a
retained private Loom and creator Proc in its stable syscall image, checking both
Proc incarnation and exact retained AddrSpace before resolution. Input is an
immutable kernel snapshot; there is no new usercopy or public dispatch. Existing
ANON/writable/contiguous single-VMA resolution takes actual Burrow pins. The
ring's own backing is refused. The complete new set resolves before installation;
partial refusal leaves the prior table intact. Under the Loom lock, close or any
provisional/live pool refuses replacement. Success swaps the whole bounded table.
Speculative or displaced pins settle outside locks against the retained image,
never by rediscovering a Proc or following its post-exec address space.

Final private retirement still requires empty service/pool ledgers and no legacy
worker/operation/fid state. It now clears and settles registered-buffer pins
before freeing tables, settling the ring and dropping the retained image last.
No new allocation/charge is needed for this table: it already resides in Loom.
Pool-member canonical resolution and independent pin adoption, accepted fixed-I/O
exclusion and protocol/worker retirement remain separate unfinished integration.
Public private setup, registration and I/O remain disabled.

Self-review: resolution holds the exact image lock, installation the Loom lock,
and neither encloses pin destruction. Exec's sole-thread swap latches private
rings before replacing the image; internal callers must obey the documented
stable-syscall-image requirement. A held Loom reference prevents final table
retirement during admission. Existing AS-R9 settled drops determine refunds
inside the Burrow lifetime lock; no claim/drop/restore window is introduced.

Host ASan/UBSan private-buffers-20261009T000936Z passed control plus7 exact
authority/ring/pool/close/drop/refund mutants using checked resolution, image,
Burrow-settlement and lock doubles. The first harness attempt's wrong-image
mutant hit its resolver-lock assertion before its intended authority assertion;
that evidence is retained in private-buffers-20261009T000859Z. The corrected leg
uses an empty-table request to isolate authority. See the correction JSON.
Updated slot-owner-20261009T000729Z passes nine controls plus10 mutants;
final-reclaim-20261009T000739Z passes both owner-drop orders plus6 mutants.
Those destructor controls exercise empty buffer tables, with real buffer pins
covered separately by the new native fixture. Lifecycle model clean5828 and
all7 named counterexamples pass (private-buffers-model-20261009T0008Z).

Native compile/link passes. New loom.private_buffers covers real pin counts,
partial-resolution rollback, exact creator/image rejection, own-ring rejection,
provisional/live-pool replacement refusal, unmap-then-clear, foreign-payer final
pin release, close refusal and final retirement refund. Expected count1840;
runtime/full Mac qualification is pending for this changed source. Previous
50/50 applies only to1295a25da. This is self-review, not independent audit.
Guest final allocator reclamation and Pi/A72/KVM remain open. Continue async
before MM0-MM4; no Main landing, private async/clipboard activation or128MiB removal.


## October9: private-buffer native fixture failure and correction

The3dcff4cb9 full matrix was stopped after six completed default-smp1 boots
reproduced the same sole failure: loom.private_buffers / foreign mappings
detached,1839/1840. No source qualified. Astra stopped its own process group;
wrapper exit143 retained clean-source and input checks and released Mac. Twelve
raw serial/harness logs, hashes and exact failed inputs are retained under
current-gate-20261009T001138Z, marked PINNED as FAILED.

Cause: the new fixture mapped foreign backing at0x60000000, below the burrow
window accepted by SYS_BURROW_DETACH. The existing legacy foreign-payer fixture
explicitly documents this constraint and uses0x140000000. The correction uses
that in-window address and holds the destination image lock around the share,
as required by the mapping API. Production buffer ownership code is unchanged.
This was a fixture defect, not evidence of a platform timing problem.

A fresh rebuilt default SMP1 diagnostic passed1840/1840, including private_buffers,
pool/slot/owner fixtures, D7 overlap and twenty distinct completed login cycles,
and boot banner. Recorded source is3dcff4cb9 plus the exact fixture-only diff;
ELF/bin and input hashes are retained in
astra-takeover/private-buffers-diagnostic-20261009T001526Z. Build/input/source
stability checks passed. This one boot does not replace the full Mac matrix;
a new clean-source matrix is still required. Prior pool-owner50/50 remains
bounded to1295a25da. Self-review only; no private activation, Main landing or
128MiB removal. Continue remaining async ownership/protocol work before MM0-MM4.


## October 9: corrected private-buffer full Mac gate verified

Exact source b010ace93fae59f5bb28856d7b5456200aa8e60c passed the full
50-boot Mac HVF matrix in current-gate-20261009T001717Z: ten boots each at
default SMP1/4/8 and UBSan SMP4/8, all 1840/1840. Every boot includes
loom.private_buffers, the pool/slot/owner fixtures, D7 overlap, twenty distinct
completed d7user00..19 login cycles and the boot banner. Both runner and gate
exit zero. All 100 copied serial/harness logs and ten captured inputs were
hash-verified; see astra-takeover/current-gate-20261009T001717Z-result.json and
the run's verify.py. Captured inputs are PINNED. External Stratum is exact clean
61dde3727921e70e2c72fbd3c9e2044a192f4a54. Minimum recorded admission free space
is 10399 MiB, above the 8192 MiB floor; the runner released Astra's Mac lease.

This qualifies the corrected private registered-buffer fixture and recorded
source on Mac HVF. The failed 3dcff4cb9 run remains retained as failed evidence.
Optional Venus/Vulkan probes were absent from this ramfs; no graphics claim.
Guest final allocator reclamation and Pi/A72/KVM remain open. Canonical pool
member pin ownership, fixed-I/O exclusion and the remaining async protocol/worker
integration precede MM0-MM4. Self-review only, not an independent audit. No Main
landing, private async/clipboard activation or removal of the 128 MiB protection.


## October 9: canonical private pool member pins (internal only)

The internal pool front-end now validates the complete immutable create record,
creator incarnation/image and receipt geometry, then resolves registered slices
to canonical Burrow identities and physical byte offsets under the Loom lock.
It uses subtraction bounds before addition, rejects ring backing and nonzero
unused/output fields, and lets the joint ledger reject overlap even through
different registered aliases or provisional pools. Every accepted member owns
one independent Burrow pin before the lock is released. No allocation or
usercopy occurs inside that lock. The buffer table remains stable while pooled.

Publish is a retained-owner continuation after future copyout. Rollback/reap
snapshot member pins under the same lock as the ledger transition; refusals
drop none. Successful removal drops the saved pins outside the lock using the
retained creator image's settled-drop primitive, safe even if buffer-table
replacement wins the unlocked gap. These wrappers must only remove pools they
created; raw ledger-only fixtures do not transfer pins to this front-end.

Self-review: source and table pins keep canonical addresses alive; no Proc is
rediscovered during removal, and no Burrow is read after dropping its pin.
One pin is released per member, including disjoint members sharing a Burrow.
Close rejects prepare and publication, but permits rollback/reap by a retained
kernel owner. The unexpected-legacy-work guard rejects inflight, admitting,
held-chain and rearm state. This is NOT the future exact fixed-I/O exclusion
implementation: private dispatch stays disabled, and accepted fixed extents
must be integrated under the same lock before activation. User authority at
continuation syscalls, copyout wiring, close/worker cleanup, protocol admission
and guest final allocator reclamation remain open. Self-review, not an audit.

Host private-pool-pins-20261009T011324Z passes ASan/UBSan control plus ten exact
mutants for image/creator, unused tail, canonical alias displacement, ring
backing, fixed admission, member pin acquisition, rollback drops, refusal drops
and final refund. Checked image/Burrow/lock doubles include table replacement
after unlock; they are sequential witnesses, not a concurrent allocator proof.
Initial harness attempts are retained: 011209Z had a misspelled ledger-close
function; 011240Z's ring-rejection mutant hit the double's zero-pin precondition
before the intended assertion. Giving the fake ring its real-world owner pin
made that mutant reach the intended assertion. Neither correction changed
production code or weakens its checks.

Native loom.private_pool_pins checks actual alias overlap, canonical offsets,
independent pin counts, provisional overlap/rebinding refusal, rollback identity
burning, stale removal, busy reap, unmap before reap, exact final table refund
and close before publication. Fresh default SMP1 diagnostic passed 1841/1841,
D7 overlap, all twenty distinct cycles and boot banner. Evidence is
astra-takeover/private-pool-pins-diagnostic-20261009T011326Z: source 1e06cc3d3
plus captured exact diff, stable rebuilt ELF/bin and six input hashes, raw logs.
Native link passed. Existing lifecycle/payload models and generic ledgers are
unchanged; their prior model results are not a proof of this new C front-end.

Full Mac qualification remains pending for the new source. The prior 50/50
remains bounded to b010ace93. Continue remaining async before MM0-MM4; no Main
landing, private async/clipboard activation or removal of the 128 MiB protection.


## October 9: canonical pool-pin full Mac qualification

Exact source 6200c9de0121abbfa08bea719f904f20ae5a9ac6 passed all 50 Mac HVF
boots in current-gate-20261009T011552Z. Default SMP1/4/8 and UBSan SMP4/8
each passed ten boots, every boot 1841/1841 including loom.private_pool_pins,
private buffers, pool/slot/owner rows, D7 overlap, twenty distinct d7user00..19
cycles and boot banner. Gate and runner exit zero. The verifier checked all
100 copied raw serial/harness logs and ten captured input hashes; inputs are
PINNED. Stratum remains exact clean 61dde37. Minimum recorded admission free
space was 9961 MiB, above the 8192 MiB floor. Astra's Mac lease was released.
See astra-takeover/current-gate-20261009T011552Z-result.json and run verify.py.

This is source-specific Mac qualification of internal canonical pool pins.
Exact fixed-I/O extent reservations, usercopy/authority dispatch and scope/
protocol/worker integration remain pending. Pi/A72/KVM, guest final allocator
reclamation and absent optional Venus/Vulkan probes remain unqualified.
Self-review only. Async remains ahead of MM0-MM4; no Main landing, private
async/clipboard activation or removal of the 128 MiB protection.


## October 9: internal fixed-buffer reservations and pool exclusion

Private fixed-buffer borrows now use a 40-byte node embedded in a caller-owned,
precharged request. The caller must supply an immutable kernel member snapshot
from a retained creator in its stable syscall image. Admission resolves through
the same canonical registered-buffer helper as pools, checks even provisional
pool extents under the Loom lock, and takes one Burrow pin and one Loom reference.
Pool preparation checks these accepted fixed extents under that same lock.
Touching/disjoint intervals remain legal; different registered aliases cannot
bypass exclusion. Duplicate nodes and reservations exceeding existing CQ
terminal-obligation capacity are refused. This bounds list traversal without
raising any existing limit. Actual shared terminal admission remains the future
request engine's obligation before dispatch; this helper does not dispatch.

Release requires all held/running/cancelled local readers and writers to end.
It verifies exact node membership, unlinks/clears under the Loom lock, then drops
backing and settles against the retained image before releasing the Loom ref.
Wrong-owner/double release changes nothing. Rebinding the registered table does
not lose accepted fixed pins or extents; final close cannot retire the owner
while any fixed borrower remains. The destructor fails closed if the ledger is
nonempty despite reaching zero references. No allocation, usercopy, worker wait
or final object destruction occurs inside the Loom lock.

Self-review: list/node ownership is kernel-only and exclusive to the retaining
request; no user token is trusted. Backing/owner refs are acquired before list
publication and released only after unlink. The removal path never uses the node
or Loom after its final unref. The common extent resolver avoids divergent alias
arithmetic; overlap uses half-open physical intervals and rejects invalid ranges.
Worst-case pool preparation scans at most 64 members times existing CQ capacity;
progress/latency qualification with the actual request engine remains owed.
Native DWARF reports Loom 2888 bytes (0xb48), still one charged buddy page; fixed
nodes must be included in future request-storage charges, not separately allocated
by this helper. Existing four owner metadata pages remain unchanged.

ASan/UBSan private-fixed-20261009T020251Z passes control plus ten exact mutations
for image/creator, backing/owner pins, both exclusion directions, capacity, both
release obligations and close refusal. Refreshed canonical pool owner controls
(private-pool-pins-20261009T020300Z) pass control plus ten exact mutants. Refreshed
slot-owner-20261009T020340Z passes nine controls and ten mutants, and
final-reclaim-20261009T020349Z passes both owner-drop orders and six mutants.
These use checked host doubles and do not establish concurrent protocol or
guest allocator correctness. Existing formal models are unchanged; prior model
results are not presented as a proof of the new C admission/retention path.

Fresh native default SMP1 passed 1842/1842, including loom.private_fixed, pool
pins/buffers/owner fixtures, D7 overlap, all twenty distinct cycles and banner.
Evidence: astra-takeover/private-fixed-diagnostic-20261009T020341Z captures
93db78ecd plus exact tested diff, rebuilt ELF/bin, six stable input hashes and
raw logs. The final header change only clarifies the immutable/stable-image
caller precondition. Native link passed; full clean-source Mac matrix remains
pending for this new source. Previous 50/50 is limited to 6200c9de0.

Remaining async work includes actual request/CQ admission and local-user
release wiring, target/scope/fid ownership, copyout/authority dispatch and bounded
protocol/worker retirement. No public private-service activation, Main landing,
clipboard enablement or removal of 128 MiB protection. Pi/A72/KVM and guest final
allocator reclamation remain open. Continue async before MM0-MM4; self-review
only, not an independent audit.


## October 9: fixed-reservation full Mac qualification

Exact source d3563f828652709ceb34cc3a94cddf9b42b6d9e0 passed all 50 Mac HVF
boots in current-gate-20261009T020608Z. Default SMP1/4/8 and UBSan SMP4/8
each passed ten boots, every boot 1842/1842 including loom.private_fixed,
pool pins/buffers/slot/owner rows, D7 overlap, twenty distinct d7user00..19
cycles and boot banner. Both runner and gate exit zero. The verifier checked
100 copied raw serial/harness logs and ten captured input hashes; inputs are
PINNED. Stratum remains exact clean 61dde37. Minimum recorded admission free
space was 9355 MiB, above the 8192 MiB floor. Astra's Mac lease was released.
See astra-takeover/current-gate-20261009T020608Z-result.json and run verify.py.

This qualifies the internal fixed-reservation checkpoint on Mac. Actual request/
CQ admission, target/scope/fid ownership and protocol/worker integration remain
open. Pi/A72/KVM, final guest allocator reclamation and absent optional graphics
probes remain unqualified. Self-review only. Continue async before MM0-MM4; no
Main landing, private async/clipboard activation or 128 MiB protection removal.


## October 9: native target ownership in private rings

Internal target preparation now validates one immutable 288-byte kernel snapshot,
checks the retained creator and exact image, reserves from the shared 64-slot
ledger, then resolves the actual registry fd with handle_get. The existing native
capture API enforces CWALKONLY native roots, strict 9P posts, exact registry view,
service slot and post generation; no fallback registry, peer I/O or connect
permission is introduced. Full input tails and output fields must be zero.
The retained registration transaction survives an unlocked capture/close race:
close winning before publication rolls back the slot and drops captured refs.
Provisional targets consume quota but cannot be borrowed. Copyout failure burns
the incarnation and rolls back; publish remains an internal continuation.

A 4096-byte precharged table holds 64 native 64-byte targets, indexed by the
shared identity slots rather than adding another quota. Loom is now 2896 bytes
(0xb50), still one buddy page; total owner metadata is five pages (Loom one,
slots one, pool table two, target table one). A one-shot target allocation refusal
is consumed even on invalid geometry. Every construction failure frees prior
storage before refunding the exact retained image.

Target get pins the shared live slot and Loom under the same lock. Reap refuses
borrowers and stale identities before changing either payload or slot. Successful
rollback/reap detaches the native reference under lock and destroys it outside.
Put unpins before dropping the Loom outside the lock, with no subsequent owner
access. Zero owner references therefore imply no target borrowers or retained
registration transactions. Final retirement clears remaining live/provisional
targets outside locks; scopes, fids and pools still require their own retirement.
CONNECT must still capture actual caller identity and use native admission checks.
No syscall entry or private runtime activation is included.

Self-review covered close during unlocked capture, publication, stale reference
removal, quota consumption, borrowed payload stability, final put, exact-image
charge retention and all allocation unwinds. The native target fixture uses a
real posted registry and navigation fd without a responder: closes the fd/root
and poster, proves the registry remains retained, then proves the last borrower
retires the closed ring and destroys the registry exactly once. This is an
internal owner fixture, not protocol or public usercopy qualification.

Validation: native default SMP1 passed 1843/1843, D7 overlap, twenty distinct
cycles and banner; source 0efb3c9bb plus captured diff and new target header in
astra-takeover/private-targets-diagnostic-20261009T025959Z, rebuilt ELF/bin and six
stable input hashes. Final declaration-only edits reorder fault declarations, add the test forward
prototype and clarify continuation retention. ASan/UBSan private-targets-20261009T030105Z passes
control plus twelve exact mutations; slot-owner-20261009T025921Z passes eleven
controls and thirteen mutants; final-reclaim-20261009T025933Z passes both owner
drop orders and seven mutants. Host boundaries use checked doubles and establish
neither concurrent protocol execution nor guest allocator reclamation. Two host
harness corrections and the diagnostic marker-parser correction are recorded
with untouched failed/raw logs. Full current-source Mac matrix is pending;
prior 50/50 remains limited to d3563f828.

Next: actual scope/fid/request/CQ admission, authority/usercopy dispatch, and
bounded protocol/worker integration before MM0-MM4. Pi/A72/KVM and final guest
allocator reclamation remain open. Self-review only, no independent audit.
No Main landing, private async/clipboard activation or 128 MiB protection removal.

The existing lifecycle model was rerun in private-targets-model-20261009T0303Z:
clean 5828 states and all seven named counterexamples passed. This validates the
existing finite abstraction, not the new C target authority/locking path.


## October 9: native target full Mac qualification

Exact source 4ac2790caa82653cf3a5bcf0f77aa5367c59535b passed all 50 Mac HVF
boots in current-gate-20261009T030505Z: default SMP1/4/8 and UBSan SMP4/8,
ten each. Every boot passed 1843/1843, including native target ownership,
private fixed/pool/buffer/owner fixtures, D7 overlap, twenty distinct d7user00..19
cycles and banner. Both gate and runner exit zero. Verification retained and
hashed 100 raw serial/harness logs and all ten captured inputs; inputs PINNED.
Minimum recorded admission free space 8786 MiB exceeds the 8192 MiB floor.
Stratum remained exact clean 61dde37; Astra's Mac lease was released. Evidence:
astra-takeover/current-gate-20261009T030505Z-result.json and run verify.py.

This is Mac qualification of internal target ownership, not complete private
async support. Continue scope/fid/request/CQ admission, usercopy/authority and
bounded protocol/worker retirement before MM0-MM4. Pi/A72/KVM, final guest
allocator reclamation and absent optional graphics probes remain unqualified.
Self-review only. No Main landing, private async/clipboard activation or removal
of the 128 MiB protection. Preserve all pinned artifact generations; current
free disk is about 8.5 GiB, so recheck admission before further heavy work.


## October 9: shared scope/fid reservation metadata

The new internal loom_service_nodes module joins SCOPE/FID reservation metadata
to the existing shared 64-slot ledger. SCOPE requires a zero parent; FID requires
an exact published READY SCOPE. A child pins that parent before reserving its own
slot, including while provisional across copyout. Capacity refusal unwinds the
pin and leaves output unchanged. Rollback burns identity, clears both ledgers
and unpins the parent. Publication rechecks parent READY, so abort during copyout
wins without silently publishing a child. Ring close also rejects publication.

Only unused RESERVED nodes with zero own borrows can use reap_unused. A parent's
child pins prevent reuse, stale child refs cannot clear replacements, and child
cleanup can release its parent after close/abort. READY/ADMITTED/ABORTING/RETIRED
objects require future engine retirement with protocol/local/terminal proofs;
this helper cannot bypass them. The module does no allocation, usercopy, callbacks
or locking internally: all calls require the owner's shared ring lock and a
retained owner. Each node is 24 bytes, the table 1536 bytes. It is compiled and
fixture-tested but NOT YET allocated, charged or attached to a private Loom;
no new runtime owner storage or private syscall activation is claimed.

Self-review checked parent pin acquisition before slot publication, quota-failure
unwind, exact parent/child incarnations, provisional invisibility, parent abort
versus child publish, and refusal-before-mutation on reap. The shared native/host
fixture explicitly simulates parent READY/ABORTING/RETIRED: it is not a handshake,
authority, concurrent-lock or final-protocol-retirement witness.

ASan/UBSan nodes-20261009T0355Z passes control plus eleven exact mutants; its input
hashes include both the new module and actual shared slot source/header. The
initial nodes-20261009T0352Z also passed, before expanding provenance coverage.
Existing lifecycle model nodes-model-20261009T0355Z passes clean 5828 states and
seven named counterexamples; it is the existing abstraction, not a C proof.
Fresh default SMP1 passed 1844/1844, D7 overlap, twenty distinct cycles and banner.
Evidence nodes-diagnostic-20261009T035230Z retains source579329177 plus exact diff,
rebuilt ELF/bin, six stable input hashes and raw logs. Full current-source Mac
matrix remains pending; previous 50/50 is limited to target source4ac2790ca.

Next: integrate charged node storage and unused-node close cleanup into the
private Loom, then scope/fid protocol resources, request/CQ admission, usercopy
and bounded worker progress; continue async before MM0-MM4. Preserve the 128 MiB
protection and disabled private async/clipboard; no Main landing. Self-review
only, no independent audit. Pi/A72/KVM and final guest allocator reclamation
remain open. To preserve disk headroom, 700 duplicate build archive logs were
removed only after byte/hash equality with retained canonical verified logs;
log-cache-reclaim-20261009T0355Z.json records every pair. No pinned input or
canonical raw evidence was removed.


## October 9: scope/fid metadata Mac gate verified

Exact source c5abd02647e9744eb200afe65e4530b19b5cd5af passed all 50 Mac HVF
boots in current-gate-20261009T035529Z: default SMP1/4/8 and UBSan SMP4/8,
ten each. Every boot passed 1844/1844, including loom.service_nodes and the
existing target/fixed/pool/buffer/owner fixtures, D7 overlap, twenty distinct
d7user00..19 cycles and banner. Gate and runner exited zero; raw verification
retained and hashed 100 serial/harness logs and all ten captured inputs. Inputs
are PINNED. Minimum recorded admission free space was 8282 MiB, above 8192.
Astra's Mac lease was released. Result: astra-takeover/
current-gate-20261009T035529Z-result.json and the run's verify.py.

This verifies metadata with explicitly simulated parent protocol states. Charged
node owner storage, actual scope/fid protocol resources, request/CQ admission,
usercopy/authority and worker retirement remain next, before MM0-MM4. Pi/A72/KVM,
final guest allocator reclamation and absent optional graphics probes remain
unqualified. Self-review only; no Main landing, private async/clipboard activation
or removal of the 128 MiB protection. Preserve pinned artifact generations.


## October 9: charged node owner and unused-node retirement

Private Loom now owns the 1536-byte SCOPE/FID metadata table in a separately
allocated, zeroed 4096-byte buddy page. Charge is reserved before allocation;
constructor failures free node/target/pool/slot storage before refunding and
dropping the exact image pin. The node allocation fault is consumed even by
early geometry refusal. Loom is 2904 bytes (native DWARF 0xb58), still one buddy
page: metadata is now six pages including slot, pool, target and node storage.
No syscall ABI or private activation changes.

At zero owner references, the retire worker has exclusive access. It cleans
unused FIDs before unused SCOPEs, using exact rollback for provisional nodes
and unused reap for live nodes; child cleanup releases its parent pin first.
Only RESERVED nodes with no own borrows qualify. READY/ADMITTED/ABORTING/RETIRED,
invalid identities and orphan payloads fail closed: actual protocol retirement
is still the engine's obligation. Node storage is freed before the final image
refund/private_end, outside locks. There is no peer I/O, wait or usercopy here.

Self-review checked whole-page charging against allocation size, all unwind
edges, last-owner exclusivity, child-before-parent ordering, stale/borrow/state
guards and final image lifetime. The native owner fixture checks node allocation
refusal, zeroed storage, complete charge, automatic provisional/live unused-scope
cleanup and restoration of image charge/refcount. Host control explicitly
simulates parent states for child cleanup; this is not a protocol witness.

Evidence: slot-owner-20261009T044245Z passes 19 controls and 18 exact ASan/UBSan
mutants, including reversed cleanup order, omitted cleanup, charge/free/unwind
and orphan guards. Initial slot-owner-20261009T043850Z passed 19 controls and
16 mutants before the two extra sensitivity checks. final-reclaim-20261009T043907Z
passes both owner-drop orders and eight omission mutants at the host allocator
boundary. Native link succeeded; node-owner-diagnostic-20261009T044103Z passed
1844/1844, D7 overlap, twenty distinct cycles and banner, source cfa1c5c63 plus
its captured diff with six stable input hashes and retained ELF/bin. Existing
lifecycle model node-owner-model-20261009T0442Z passes clean 5828 states and
seven named counterexamples. Full Mac coverage for these owner changes is
pending; prior 50/50 belongs to c5abd0264. No independent audit is claimed.

Next: actual scope/fid protocol resources and retirement, request/CQ admission,
usercopy/authority and bounded worker progress, then MM0-MM4. Private async and
clipboard stay disabled; keep the 128 MiB protection and do not land Main.
Pi/A72/KVM, final guest allocator reclamation and optional graphics remain open.

Disk admission headroom was restored by reclaiming five idle, ignored, untracked
native Cargo debug caches under the operator resource grant, from Main and old
Aux checkouts. No source, release outputs, run evidence or pinned generation was
removed. native-debug-cache-reclaim-20261009T0443Z.json inventories exact paths
and files; measured free space rose from 8164 to 8737 MiB. Full gate admission
will independently enforce the unchanged 8192 MiB floor.


## October 9: charged node owner full Mac gate verified

Exact source 262ff29d3fe306dd700481f36d1fa4dd40aaede0 passed 50/50 Mac HVF
boots in current-gate-20261009T044430Z: default SMP1/4/8 and UBSan SMP4/8, ten
each. Every boot passed 1844/1844, including charged private owner/unused-node
cleanup and existing node/target/fixed/pool/buffer fixtures, D7 overlap, twenty
distinct login cycles and banner. Both exits were zero; verified 100 raw logs
and all ten input hashes, and marked inputs PINNED. Minimum recorded admission
free space was 8208 MiB, above the unchanged 8192 floor. Mac lease released.
Result: astra-takeover/current-gate-20261009T044430Z-result.json.

This is internal owner/metadata coverage, not complete private async support.
Actual scope/fid protocol resources and retirement, request/CQ admission,
usercopy/authority and bounded worker progress remain next before MM0-MM4.
Pi/A72/KVM, final guest allocator reclamation and absent optional graphics probes
remain unqualified. Self-review only. Keep private async/clipboard disabled,
128 MiB protection and all pinned evidence; no Main landing. Current free disk
is only about 8 GiB: restore headroom before the next build/gate without lowering
admission or removing canonical evidence.

## October 9: creator-bound scope/FID reservation transactions

Internal `loom_private_node_prepare/publish/rollback/reap_unused` now front the
charged node and shared-slot tables. Prepare accepts a complete immutable
48-byte ABI snapshot, requires the exact creator incarnation AND address space,
and rejects nonzero result/reserved fields, wrong version/size/flags, invalid
kind or parent before publication. Failure leaves caller output unchanged.
The caller owns a Loom reference and stable syscall image across prepare,
copyout and publish-or-rollback; the helpers do not add a hidden reference or
retain a Proc pointer. Both ledgers and close/parent-state checks use the same
Loom lock, with no allocation, callback, usercopy or resource drop under it.
Provisional nodes stay invisible; publication rechecks an exact READY parent.
Rollback works after abort/close and never restores a burned incarnation.
Only unused RESERVED nodes are reapable here; active and RETIRED protocol nodes
still require engine retirement and completion publication proof.

Self-review checked immutable-record validation, both identity comparisons,
lock coverage of both tables, close versus copyout continuation, stale-ref
rejection and parent-pin cleanup. This is self-review, not an independent audit.
The shared native/host fixture explicitly injects READY/ABORTING; those states
are not a handshake or concurrent-scheduling witness. The actual extracted
wrapper host control passed under ASan/UBSan and killed 13 named authority,
record-validation and lock-omission mutants in private-nodes-20261009T053849Z.
The native diagnostic node-transactions-diagnostic-20261009T053938Z passed
1845/1845, D7 overlap and all twenty distinct cycle users; source diff, added
fixture hash and six inputs were stable. Lifecycle model clean5828 and all seven
named counterexamples passed in node-transactions-model. The only subsequent
source edit moved a header comment beside the correct declaration.

This checkpoint does not activate service registration or CONNECT. Charged
request/terminal capacity, actual protocol-owner binding, caller-authorized
native admission, progress scheduling and local retirement remain the next
integration work; the full current-source SMP/UBSan gate is pending.

## October 9 06:28 UTC: scope/FID transactions full Mac gate verified

Exact source `d4825ae1e106924d67d3eb405180da7f8d5882d6` passed
`current-gate-20261009T054208Z`: 50/50 boots, default SMP1/4/8 and kernel
UBSan SMP4/8, ten each. All boots passed1845/1845 including the new
`loom.private_node_owner` fixture, D7 overlap, twenty distinct d7user00..19
login/logout cycles and the boot banner. Both gate.rc and runner.rc are0.
The verifier retained and hashed100 raw serial/harness logs and checked all ten
captured input hashes against the clean source/end-source and Stratum61dde37
provenance. Lowest recorded admission headroom16194MiB;8192MiB floor intact.
The runner released Astra's Mac lease. Result:
`astra-takeover/current-gate-20261009T054208Z-result.json`; verifier beside run.
Astra's pending files and four protected drafts, plus clean Stratum pin, were
rechecked unchanged after the run. This is self-verification, not an independent
audit. Optional graphics probes remain absent from this exact ramfs; Pi/A72/KVM
and final guest allocator reclamation remain unqualified.

This qualifies the internal reservation owner checkpoint only. Next: charged,
bounded request records and terminal-completion obligations before native
backlog publication, then actual scope/protocol binding, caller authority,
bounded progress and local retirement. Private service registration/CONNECT and
clipboard remain disabled, Main untouched,128MiB protection retained. MM0-MM4
still follow completion of async integration and its client/consumer gates.

## October 9: charged request records and terminal posting

Each private owner now precharges a CQ-sized array of56-byte request records.
Its32-byte descriptor is embedded in Loom (native DWARF size2936/0xb78,
unchanged one-page buddy class). Records allocate at least a whole buddy page,
then round through the actual buddy charge for larger arrays. Small-ring base
metadata is seven pages;128 CQ entries use two record pages and8192 use128
record pages. Geometry bounds multiplication before allocation. The request
allocation fault is consumed before all refusals; failure frees requests and
prior tables before refund and exact image-pin release. Zero-reference cleanup
rolls back only PREPARED records before node cleanup, rejects admitted or orphan
payloads, and frees record storage before the final refund. It does not silently
throw away accepted obligations.

The request ledger burns monotone kernel identities unrelated to user_data,
retains an exact scope-slot pin, reserves bounded capacity before acceptance,
and rechecks absolute deadlines at admission. ACTIVE borrowers prevent terminal
commit/free; terminal commits are immutable and reject fresh success after
scope abort or ring close. Prior committed success survives abort. CQ delivery
and local storage release are separate transitions. Rollback/reap release the
parent only after their exact phase/identity/borrow checks; wrap is refused.
Consumer detach requires a closing ledger and no recorded borrowers, plus the
engine's external proof that all protocol/local users have ended. Discard then
replaces delivery for a locally quiescent terminal, never ordinary CQ pressure.

`loom_private_post_terminal` posts a committed non-pooled final result. The
same owner lock covers terminal peek, private-tail/mask CQ capacity check,
CQE/zeroed-receipt copy, delivered transition and release tail publication.
A full CQ returns AGAIN unchanged; wake occurs after unlocking. It deliberately
does not reject an earlier success just because close has since latched.
Provided-buffer receipts and intermediate multishot results remain in the pool
ledger; their binding to final request obligations is still engine work.

Self-review covered charge/free/refund order, bounded multiplication, shared
scope-pin ownership, stale refs, deadline gaps, abort versus commit, borrower
retirement, consumer detach, hostile CQ mirrors and wake ordering. No independent
audit is claimed. Operation-specific state/DAC checks, native publication,
actual parser/callback pinning, post-admission deadline scheduling and ownership
of request pointers must still be wired by the engine; these helpers do not
activate private syscalls or claim protocol quiescence from metadata alone.

Evidence: request-ledger-20261009T0647Z control plus13 named mutants;
request-post-20261009T064719Z actual posting function plus6 mutants;
slot-owner-20261009T065009Z actual constructor/destructor27 controls plus23
mutants, including small/128/8192-entry geometry, refusal and unused cleanup;
final-reclaim-20261009T064547Z both drop orders plus9 mutants (host allocator
boundary only). All host runs use ASan/UBSan. Initial test-scaffolding failures
are retained: removing deadline checks made the mutant's now parameter unused,
and the old owner test still expected five frees after adding the sixth
allocation. Both harness defects were corrected; no red production result was
suppressed. Native request-owner-diagnostic-20261009T064808Z passed1847/1847,
D7 overlap, twenty distinct cycles and banner, with stable source/extra-source
hashes and six inputs. The diagnostic's inherited `node_owner` JSON label checks
`loom.private_requests` in its saved verifier. Lifecycle model clean5828 and
seven named mutants pass in request-owner-model-20261009T0650Z. Full source
SMP/UBSan qualification follows this checkpoint.


## October 9 07:46 UTC: request gate verified; charged protocol preparation

The request/terminal owner source45d26893bd8b5e16a3934eed008ae4804f5bff38
passed all50 Mac HVF boots in current-gate-20261009T065820Z: default SMP1/4/8
and kernel UBSan SMP4/8, ten each. All50 passed1847/1847 including request
ledger/private CQ posting, D7 overlap, twenty distinct d7user00..19 cycles and
banner. Both rc0; verifier retained100 raw logs and verified ten captured input
hashes, clean source/end-source and clean Stratum61dde37. Minimum recorded
admission15572MiB, floor8192 intact. Result:
astra-takeover/current-gate-20261009T065820Z-result.json. Mac lease released.

Continued directly into real prepublication protocol ownership. The new internal
loom_service_protocol owner borrows an exact registered target, snapshots the
live creator's stripes plus exact pinned image, prepares a fresh native SrvConn
with ordinary weighted credits, and replaces the target borrow with its own
Loom reference. It reserves the complete image page charge before allocation,
then initializes the existing allocation-free client and exclusive native try
adapter. Tversion is queued locally; no backlog publication or byte I/O occurs.
Close is rechecked before returning ownership. Future CONNECT publication must
still reserve the request/destination first and recheck deadline/abort under Ll.

Native owner size42208 bytes (DWARF0xa4e0), rounded to16 buddy pages. Both RX and
TX are separate class-sized allocations, disjoint from the client as required
by the existing API: normal32KiB pays32 pages total, bulk128KiB pays80. This is
additional to ring metadata and SrvConn's weighted credits, not MM0-MM4 global
physical accounting. No bulk degradation or uncharged cache allocation at init.
Exclusive unused release refuses a published connection unchanged. Otherwise it
aborts/destroys the local cursor, releases native ownership, frees TX/RX/owner,
refunds the retained exact image and drops Loom LAST, all final drops outside
Ll/as locks. This cannot certify retirement of an admitted scope or live borrows.

Self-review (not independent) is preserved in protocol-draft/self-review.txt.
It caught and corrected an initial inline-TX alias against the preallocated
API's disjoint-buffer contract before integration. Final actual-owner-C host
ASan/UBSan evidence control-5 passes20 controls and11 named mutants, including
that alias, all charge/free/ref edges, late close and published-cleanup refusal.
Its boundary doubles are not a wire or guest physical-reclamation proof.

Fresh native diagnostic protocol-owner-diagnostic-20261009T074413Z passes
1848/1848, the actual native protocol-owner fixture on both buffer sizes,
allocation/charge refusal, actual creator capture, absence of peer effects,
unused cleanup and final Loom/image retention, plus D7 overlap/twenty distinct
cycles/banner. Exact HEAD+diff, new source/header hashes, six input hashes and
rebuilt ELF/bin retained and stable. This newer implementation still owes its
full matrix; prior50/50 is limited to45d26893b.

Failures retained and resolved: the initial native link emitted a freestanding
memset libcall for the large fixture initializer; explicit byte-zeroing repaired
it. Native diagnostic074015Z then failed1847/1848 because a bare proc_alloc
fixture is absent from the kproc-rooted production identity walk. The fixture
now uses existing proc_test_link/unlink and checks a positive identity snapshot
before negative cases; production authority checks were not weakened. Initial
Apple clang17 host ASan startup stalled before main; a retained process sample
shows recursive sanitizer/dyld/malloc initialization, and the established LLVM22
plus explicit SDK toolchain passes. No failed evidence was discarded.

Continue autonomously through the new full gate and then bind prepared protocol
ownership to exact scope/request publication, bounded executor progress and
actual local retirement, followed by AS-3/4 and MM0-MM4. Do not rerun the completed
protocol-draft installer/supervisor: their historical failure and manual recovery
are recorded; canonical implementation is now in kernel/. Protected Astra files
and clean Stratum pin rechecked. Private activation/clipboard remain disabled,
Main untouched,128MiB protection retained. Pi/A72/KVM, final guest allocator
reclamation and absent optional graphics remain unqualified. Self-review only.

## October 9: CONNECT ownership, publication and local retirement

Astra bound charged protocol ownership to an exact RESERVED scope and a
precharged CONNECT terminal obligation. The request and protocol hold separate
scope pins before native allocation; duplicate scope preparation is refused.
Native backlog publication and request admission share the Loom lock with
cancellation and success. No bytes are sent before publication. One driver
uses the real nonblocking 9P client outside that lock, retaining a parser borrow.
READY requires actual version/attach completion. The CONNECT deadline ends at
committed success; later cancellation preserves that success for CQ delivery.

Abort acknowledgement latches transport shutdown; local retirement additionally
requires cursor abort and no concurrent teardown users. A full CQ retains the
terminal and protocol storage. The CONNECT owner alone reaps its delivered
request, drops the other scope pin, destroys local storage, refunds the exact
image and drops Loom last. Explicit metadata reap still requires no child pins.
A server-retained endpoint keeps its own native lifetime and weighted credits.
Protocol failures use EIO and record the attempted handshake phase as required
by the existing ABI; timeout/cancellation preserve their distinct reasons.

Final actual-C host ASan/UBSan evidence connect-control-20261009T170708Z passes
11 controls and 12 named mutations, including abort during progress, a second
driver, a teardown user delaying retirement, full CQ, and recorded failure phase.
These use checked native/protocol doubles, not a wire or SMP stress proof.
Existing protocol owner controls pass20+11 mutations, and the request ledger
passes its control plus13 mutations after factoring the nonmutating admission
check. Lifecycle model passes5828 states and seven expected counterexamples.
Self-review is in connect-draft/self-review.txt; no independent audit. It caught
and repaired the missing protocol failure phase before this checkpoint. Initial
host-runner mutation-count and unused-stub failures are retained; production
controls passed, the harness was corrected and all named mutations now fail at
their intended assertions.

Fresh native connect-diagnostic-20261009T170730Z passes1849/1849 including
loom.private_connect, real normal/bulk version+attach, error/cancel/timeout,
CQ backpressure and server-retained endpoint cleanup; D7 overlap, twenty distinct
login/logout cycles and banner also pass. Exact source diff/new-source hashes,
six input hashes and rebuilt ELF/bin are retained and stable. The earlier
170510Z diagnostic passed before adding failure-phase coverage and stays bounded
to that earlier source. The new full Mac matrix is pending; previous50/50 still
qualifies only fa7e6c5bc protocol preparation.

Continue autonomously: per-ring CONNECT lookup/entry lifetime and bounded fair
progress, close/detach retirement queue and query/reap integration, then remaining
private ops/provided buffers, AS-3/4 and MM0-MM4. This internal engine still has
an explicit caller-lifetime contract and permits only the handshake; no external
RPC/fid/payload borrow may escape it. No syscall/ring dispatch or private async/
clipboard activation, Main landing, or128MiB protection change. Preserve clean
Stratum61dde37, Astra pending/protected work, and8192MiB runner floor.

## October 9: CONNECT full Mac gate verified

Source b2b263e3f250ecaa42c7975a86beec7b65128f12 passed50/50 Mac HVF boots
in current-gate-20261009T170944Z: default SMP1/4/8 and kernel UBSan SMP4/8,
ten each. All boots passed1849/1849 including loom.private_connect, D7 overlap,
twenty distinct login/logout cycles and boot banner. Gate/runner rc0; verified
100 raw serial/harness logs and ten input hashes, with the complete generation
PINNED. Minimum recorded admission 13185MiB exceeds the8192MiB floor.
Result: work/oct5-as-r9/astra-takeover/current-gate-20261009T170944Z-result.json.

This qualifies internal CONNECT publication/handshake/local retirement at that
source. The separate owner-index draft remains native-unqualified until installed
and tested. Pi/A72/KVM, guest allocator reclamation and absent optional graphics
remain unqualified. Self-review only; no independent audit, Main integration,
private async/clipboard activation or128MiB protection change. Continue the
per-ring index checkpoint, then close queue/executor and AS-3/4/MM0-MM4.

## October 9: private per-ring CONNECT ownership index

Astra added a bounded 64-entry protocol-owner index under the existing Loom
lock. Each 24-byte entry holds an exact scope incarnation, protocol pointer,
entry pin count and INSTALLING/LIVE phase. The 1544-byte index occupies one
separately charged private-only 4096-byte buddy allocation, including a consumed
one-shot allocation-failure witness and complete unwind/final-free coverage.
ARM64 Loom remains2944 bytes (one buddy page); protocol remains42264 bytes
(16 buddy pages). Legacy rings do not allocate an index page. This is exact
per-image metadata charging, not the pending MM0-MM4 physical accounting.

Adoption transfers exclusive prepared ownership on return0, with an installation
pin spanning native publication AND its unlocked accept wake. Ordinary exact
lookup returns AGAIN during installation; abort can pin and cancel that state.
Publication failure retains ownership and a negative terminal obligation: the
actual cursor is aborted before PREPARED becomes TERMINAL. Closing/refused
adoption before indexing leaves cleanup to the caller. Lookup+entry pin share
Ll with removal, so no raw pointer gap opens; stale incarnations leave output
untouched. Raw CONNECT/protocol release refuses indexed ownership.

A visit advances a round-robin cursor, runs at most one real bounded client step
and one CQ post, then releases its entry. Holding a lookup entry prevents free,
not progress by other scopes. Collection requires LIVE, zero entry pins, actual
local retirement, no driver/teardown users, and a delivered/discarded terminal
with no parser borrows. It unlinks under Ll and releases/refunds outside Ll,
dropping the protocol's Loom reference last. The caller retains Loom throughout.
Scope metadata remains until explicit reap; final owner cleanup refuses a
nonempty index. A prior successful CONNECT result remains immutable on abort.

Whole-ring discard requires close, no fixed borrows, every indexed owner locally
quiescent and every occupied request matched to its exact retired indexed owner
and terminal/delivered. An unindexed constructor is therefore a blocker rather
than evidence that the ring has no work. Consumer detach, terminal discard and
collection then require neither CQ space nor a peer reply. A lookup racing the
unlocked collection can delay it safely; the caller retries after the pin ends.

Self-review (not independent) is in engine-draft/self-review.txt. It caught the
initial inline-index cost doubling all Loom allocations; the separately charged
private page repairs that cost. It also closed an unindexed-constructor detach
gap. Raw syntax-log inspection caught a misplaced native allocation-fault block
(kind/PO were undeclared in the request fixture), moved it into the protocol
fixture, and retained the failure. Corrected configured ARM64 syntax passes all
seven files. Earlier host assertion/mutation-expectation errors are preserved
with their corrections; no failed runtime result is dismissed.

Host evidence uses actual owner/CONNECT/request/slot/node/CQ C with checked
boundary doubles: engine11 controls+9 mutants, CONNECT11+12, protocol20+11,
private constructor29+26 and final destruction both drop orders+10 mutants.
Shared request rejection fixture passes with17 named mutants. The lifecycle
model passes5828 states and seven expected counterexamples; the new index
model checks89 states and six targeted lifetime/publication counterexamples.
These establish bounded local mechanisms, not native wire or SMP qualification.
The source-matched native diagnostic and full matrix are recorded separately.

Still internal, handshake-only and caller-driven. No private ring/syscall/SQPOLL
activation, general RPC/fid/payload borrows, clipboard, Main landing or128MiB
protection change. Visit's return is not a readiness/sleep predicate. A distinct
reference-owning close queue is still required because indexed owners retain
Loom and its zero-ref destruction cannot initiate their abort. Next: that queue,
actual last-handle/exec cleanup and wake races, then readiness-aware executor,
remaining private operations/provided buffers, AS-3/4 and MM0-MM4. Keep external
Stratum clean61dde37, Astra pending/protected work, and8192MiB runner floor.

Native engine-diagnostic-20261009T1752Z passes1850/1850 including the actual
index fixture: expired indexed admission, normal/bulk independent peers with a
stalled first peer, entry-pin lifetime, full-CQ close discard, retained server
endpoints and exact protocol refunds/reap. D7 overlap, twenty distinct cycles
and banner pass; HEAD+diff/new-source hashes and six input hashes are stable,
and rebuilt ELF/bin retained. Initial1751Z failed1849/1850: the old slot-owner
fixture counted node+request pages but omitted the new index page. Corrected
expected physical metadata and added nonnull/zero index check; failed logs and
artifacts retained. The full Mac matrix for this newer source remains pending;
previous50/50 qualifies b2b263e3f internal CONNECT only.


## October 9: full owner-index Mac gate verified

Source a978d19c388f512a645cdcbaeea60a81a0a06727 passed50/50 Mac HVF boots in
current-gate-20261009T175812Z: default SMP1/4/8 and kernel UBSan SMP4/8,
ten each. Every boot passed1850/1850 including loom.private_engine, D7
overlap, twenty distinct login/logout cycles and the banner. Both rc0;
100 serial/harness logs and ten input hashes verified and retained/PINNED.
Minimum recorded admission 11875MiB, above8192MiB.
Result: work/oct5-as-r9/astra-takeover/current-gate-20261009T175812Z-result.json.

This qualifies the internal charged index and explicit progress at that source.
The separate close queue draft still owes native and full-matrix verification.
Self-review only; no independent audit, Main landing, private async/clipboard
activation or128MiB protection change. Continue close queue, active executor,
remaining private ops/provided buffers, AS-3/4 and MM0-MM4.


## October 9: reference-owning private close queue

Astra added an intrusive close node to each private Loom. Actual handle close
latches admission and starts the node exactly once while holding the caller's
live reference. Under the retire lock it takes one distinct queue reference,
then wakes outside locks. Exec's earlier commit latch remains leaf Ll-only;
forced descriptor removal starts the queue after process/handle locks are gone.
Zero-reference final destruction calls a bare latch/wake path and cannot
resurrect a close root. Loom is2968 bytes on ARM64, still one buddy allocation;
its already charged setup storage contains the queue link/state and diagnostic
visit counter. No stack, allocation or thread is created per close.

The existing retirement worker pops one FIFO close quantum and one zero-reference
final destruction per iteration. Q never encloses Ll, native work, allocator
calls, scheduler wakes or final ref drops. One close quantum aborts at most one
nonbusy indexed cursor and attempts bounded quiescent consumer detach/collection.
READY success remains immutable even if close cancels later. Full CQ and stalled
server endpoints do not prevent actual local cursor abort. A server endpoint
retains its own transport reference independently after the ring is gone.

A blocked ring parks off the runnable queue while retaining its root. Installer,
external driver/teardown, entry-pin, raw-constructor and fixed-buffer returns
notify after their ownership transitions. A return racing RUNNING sets dirty;
a return after PARKED enqueues once. Internal retirement steps/puts/collection
are quiet, avoiding perpetual self-wake loops. Completion drops the queue root
outside Q once; DONE cannot restart. Final destruction is still serviced during
busy close work, and scope visitation retains the existing round-robin cursor.

Fixed-buffer release unlinks its reservation under Ll but increments a separate
settling count before dropping the physical buffer pin outside Ll. Both active
and settling counts consume admission capacity; either prevents consumer detach.
Only after physical settlement does release decrement settling, notify close,
and drop its retained Loom reference. Raw CONNECT rollback/release takes a
temporary Loom root around consuming physical cleanup and the subsequent wake.
All public engine calls still require their caller to retain Loom.

Close collection leaves scope metadata while provisional FID callers can own
parent pins. Once the last Loom reference is gone, final cleanup removes unused
FID reservations before retired SCOPE metadata. A retired scope must be LIVE,
unpinned, with empty parent/reserved fields. READY, ABORTING and invalid owners
still fail closed. This does not permit freeing an in-progress constructor or
pretending abort acknowledgement proves protocol quiescence.

The native loom.private_close fixture includes actual final handle close, forced
private exec with CLOEXEC cleared, a full CQ, independent normal/bulk peers,
entry and fixed-buffer borrows, an unindexed constructor, unused child metadata,
retained server endpoints and exact image refunds. Test-only unlocked boundary
hooks also pause real INSTALLING, driving and teardown owners while the real
retirer must park; returning those owners must wake it to completion. These are
deterministic interleavings, not an exhaustive scheduler proof. Native and full
matrix qualification are recorded with their exact source and artifacts below.

Still internal CONNECT-only. No private syscall/ring dispatch, general RPC/fid
operations, readiness-aware active executor, private userspace/clipboard
activation, Main landing or128MiB protection change. AS-3/4 and MM0-MM4 remain.
Self-review is recorded in close-draft/self-review.txt; no independent audit.

Draft ASan/UBSan controls and ARM64 syntax pass; native qualification follows.


Fresh native close-diagnostic-20261009T183912Z passes1851/1851, including
all seven close cases and second-ring progress while the first is parked. D7
overlap, twenty distinct login/logout cycles and boot banner also pass. Rebuilt
ELF/bin, six input hashes, exact HEAD+diff and new queue source/header hashes
are retained and stable. Real close/exec, full CQ, retained entry/fixed/constructor
owners and installer/driver/teardown interleavings finish local retirement with
exact image refunds while server endpoints remain retained. Source-matched Mac
SMP/UBSan matrix for this new code is still pending; the earlier50/50 qualifies
a978d19c3 index-only source. Pi/A72/KVM, final guest allocator reclamation and
optional graphics remain unqualified. No private activation/Main/128MiB change.


## October 9: CLOSE-R1 physical rollback ordering repair

Self-review found and reproduced a premature close completion: unpublished
CONNECT rollback removed its PREPARED request and scope pins before physical
cursor/storage destruction. References prevented final free, but a concurrent
close worker could detach and declare DONE too soon. Rollback now retains the
request and both pins through destruction outside Ll, then drops them under Ll.
The public wrapper retains a temporary Loom root through cleanup and wake.

Actual-C ASan/UBSan reproduction rejects the old order and accepts the repair.
Canonical CONNECT11 controls/12 mutants and close-engine18 controls/7 mutants
pass. Native rollback-native-20261009T1854Z passes1851/1851, D7 overlap,20 distinct
login/logout cycles and banner, with source/input hashes and rebuilt ELF/bin.
The extended native close fixture forces a real retirer visit while unpublished
protocol destruction is suspended and requires the barrier still retained.
Astra deliberately stopped the5ab15c4d5 full matrix with SIGTERM(rc143), preserving
16 partial serial/harness logs. This was an intentional repair stop, not a full
qualification. The repaired source requires a fresh full matrix. Review and
raw evidence: astra-takeover/rollback-draft; self-review only, no independent
audit. No Main/private async/clipboard activation or128MiB protection change.


## October 9: repaired close lifecycle full Mac qualification

Exact source ba2ae6df3dd815a4853985cc81e9f981ca0d1bdb (CLOSE-R1 implementation
9b6672f7d) passed50/50 Mac HVF boots in current-gate-20261009T185605Z:
default SMP1/4/8 and UBSan SMP4/8, ten each. All50 pass1851/1851, including
actual close/exec, retained owners, fairness and the forced physical rollback
boundary, plus D7 overlap,20 distinct login/logout cycles and banner.
Both exits zero;100 raw serial/harness logs and ten input hashes verified and
retained/PINNED. Minimum recorded admission10930MiB, floor8192 preserved.
Mac lease released. Result: astra-takeover/current-gate-20261009T185605Z-result.json.

This qualifies that repaired internal close source. The separate query/reap
and retained-diagnostic draft passes nine host scenarios and fifteen mutations,
but is not installed or native-qualified. Continue those controls and active
readiness executor, remaining operations/buffers, AS-3/4, then MM0-MM4.
Pi/A72/KVM and final guest allocator reclamation remain unqualified. Self-review
only; no Main/private async/clipboard activation or128MiB protection change.


## October 9: internal service query, abort and reap controls

Creator stripes and exact address-space image authorize copied-record internal
QUERY/ABORT/REAP. Validation rejects malformed/output-only inputs and stale
incarnations without changing query output. A locked snapshot reports TARGET,
SCOPE or FID state and exact parent identity; POOL retains its dedicated query.
Reap dispatch carries only exact identity, with each consuming primitive
rechecking state and pins under Ll and making final drops outside Ll.

The existing charged node page now stores40-byte entries,2560bytes total within
its4096-byte allocation. First reason, attempted failure phase, sent-byte state
and actual local retirement survive protocol/index collection. QUERY counts
PREPARED/ACTIVE obligations in active_ops and unpublished TERMINAL obligations
in pending_terminals. DELIVERED storage awaiting collection counts neither;
its pins still prevent reap. Query never races an unlocked protocol cursor:
completed driver diagnostics publish under Ll, while an active driver or parser
borrow conservatively reports possible escaped bytes.

ABORT remains independent of CQ capacity and preserves a committed success.
An unindexed constructor gets an admission-closing node latch; its publication
and driver reentry observe it. Rollback retains CLOSE-R1's physical destruction
barrier before marking diagnostic retirement. Unused scopes retire immediately
only with no pins; indexed owners still require actual cursor/borrow quiescence.
Scope metadata survives collection until children and outstanding ownership are
gone and explicit exact reap succeeds. Published runtime ownership must use the
index; these copied-record helpers do not install syscall/ring dispatch or an
active executor. No general RPC/FID protocol or private userspace activation.

Nine actual-C host scenarios and15 named mutations pass under LLVM22 ASan/UBSan.
Canonical CONNECT11+12mutants, node transactions, constructor29+26mutants and
final-free both orders+10mutants pass after installation. Evidence records the
initial child-pin boundary-double correction, earlier expected-mutation-message
mismatch and missing draft test include path; production controls were preserved.
Self-review: astra-takeover/control-draft/self-review.txt, not independent audit.

Native control-native-20261009T2027Z passes1852/1852, D7 overlap,20 distinct cycles
and banner. Real normal/bulk handshakes behind full CQ preserve success through
abort; retained child pins block reap; diagnostics survive physical collection;
accepted server endpoints survive local refund. The actual unused-destruction
boundary still reports ABORTING until cleanup completes. Exact HEAD+diff, new
source hashes, six stable inputs and rebuilt ELF/bin retained. Full Mac matrix
for these changes is pending; prior50/50 qualifies ba2ae6df3 close-only source.
No Main/private async/clipboard activation or128MiB protection change.


## October 9: readiness-aware internal private executor

The existing per-ring SQPOLL thread now has an internal private handshake
executor. Creator stripes and the retained address-space image supply the
existing thread-budget ticket; no asynchronous raw Proc pointer is retained.
Ll serializes the start token across charge/allocation/publication or rollback.
A failed start frees its never-readied thread before refund and token release.
Ordinary private SQPOLL setup remains refused; no user dispatch is activated.

Readiness visits scan at most64 entries and drive one actionable owner with
the shared round-robin cursor. Cursor state is sampled under Ll only while no
driver/parser user is active; real protocol steps recheck state and never wait
for a peer. Native poll directions follow handshake phase. An exclusive wait
sweep retains entry pins, registers native interests before checking readiness,
then sleeps on atomic changes, CQ head, poll flags and the earliest absolute
ACTIVE deadline. READY discards its CONNECT timer; full-CQ terminals park.
The Rendez predicate never takes Ll or a device lock. Embedded64 poll hooks
fit within the existing charged4096-byte index page, checked on actual ARM64.
Every hook is unregistered before any wait pin is returned.

The executor borrows the ring lifetime. The zero-reference retirer wakes, joins
and frees the actual worker before ticket refund or table/storage destruction.
A last protocol unref from the worker queues this cleanup; it never self-joins.
Handle-close DONE includes start rollback and actual executor exit. Exit
publishes EXITING/exited in the existing IRQ-masked handshake before queue/join
wakes. Local completion is peer-independent but needs scheduler service.

Self-review found EX-W1: a silent readiness-scan pin return could strand a close
or armed collector. The actual-C witness rejects that draft; repair samples the
dependency and decrements its pin in the same Ll interval, then wakes unlocked.
Fourteen readiness cases/17mutants, five worker cases/8mutants, engine18/7,
CONNECT11/12, constructor29/26 and final-free both orders/10mutants pass.
Actual ARM64 syntax passes. Existing poll/lifecycle/index model controls and
counterexamples pass; they do not prove the new executor or native scheduler.
Self-review and failed witnesses: astra-takeover/executor-draft/self-review.txt.

Native executor-native-20261009T2116Z passes1853/1853, D7 overlap,20 distinct
login/logout cycles and banner with stable HEAD+diff, eight reviewed source
hashes and six input hashes. Real normal/bulk peers exercise registration-window
response injection, stalled-peer fairness, full CQ/drain, timeout without a new
edge, handle-close exit and exact ticket/image refund. An empty live parked
executor is joined by zero-reference cleanup. The2114Z capture attempt failed
before VM launch on an obsolete filename; repaired from the reviewed manifest,
with failure retained. Full new Mac matrix is next; ade9's50/50 qualifies only
the older1852 controls source. Self-review only, not independent audit.

General RPC/FID operations, provided-buffer dispatch and usercopy remain ahead
of AS3/4 and MM0-MM4. No Main/private user async/clipboard activation or128MiB
protection change. Pi/A72/KVM and final guest allocator reclamation remain open.


## October 10: private operation storage and callback capture prerequisites

The request ledger now separates its terminal obligation from physical RPC
storage ownership. Its56-byte record reuses a reserved field for a single owner:
Ll admits that owner only while PREPARED, and rollback/reap refuse reuse until
physical destruction. Completion delivery/discard does not release storage.
ACTIVE/borrowed, stale and duplicate owner drops fail; existing CONNECT uses0.

The internal operation constructor retains an exact indexed READY protocol and
the creator image through allocation and destruction. It reserves the terminal
and physical owner under Ll before charging and allocating one full buddy page.
Close/abort is rechecked after allocation. Refusal and exclusive unused cleanup
free storage, refund the image, release exact request/entry pins, then drop the
Loom last. No allocator, image/client lock, wake or final drop nests under Ll.
The constructor does not admit or submit an RPC and cannot free an active one.

Copied-SQE shape checks preserve all64-bit incarnations, reject reserved fields
and malformed fixed/pool combinations, and validate clone/component shape.
They grant no authority. Reply capture retains pointer-free metadata and copies
bounded payload before callback parser storage can be reused. It never allocates,
locks, publishes CQ or commits success; future driver admission and abort/deadline
checks remain mandatory. Malformed type/fid/count/walk/stat shape fails closed.

Host checks: shape1832cases/13mutants, reply capture13mutants, request physical
owner9mutants plus all17 existing request mutants, and charged constructor10
mutants pass under LLVM22 ASan/UBSan. ARM64 syntax and actual kernel link pass.
CAP-L1: native link first caught large struct assignments emitting unavailable
libc memcpy; explicit byte copying repaired it, and all capture mutants reran.
The first linker log and earlier harness failures remain in operations-draft.

Native operation-storage-native-20261009T215931Z passes1854/1854, D7 overlap,
20 distinct login/logout cycles and boot banner. Real normal/bulk READY peers
exercise expired admission, actual image quota and allocator refusals, exactly
one-page charge/refund, terminal-capacity refusal, monotone request identity,
and close PARKED until physical operation ownership is released, then DONE.
Eleven reviewed source hashes, six stable inputs, HEAD+diff and rebuilt ELF/bin
match that run. This is bounded CPU1 native storage evidence; f719ee4a3's50-boot
executor matrix does not qualify newer source. No new full matrix for unused
helpers. Self-review only, not independent audit.

Actual RPC list/admission/callback/commit, FID/DAC and fixed/provided-buffer
dispatch and readiness integration remain before AS3/4 and MM0-MM4. Canonical
source supersedes the installed draft; no historical installer may be rerun.
No Main/private async/clipboard activation or128MiB protection change.

Isolated next-operation prerequisite (Astra October9): fid-draft remains
uninstalled. Its exact WALK/open/CLUNK request claims and separate source-borrow
count address root lifetime-pin ambiguity; local abort/reap retains the parent
until derived identity is gone. Host89checks/24mutants, node11mutants and actual
ARM64 page geometry pass. These checks establish only sequential helper behavior;
root handshake binding, actual RPC/DAC/buffer/engine integration and native races
are outstanding. See fid-draft/self-review.txt for FID-F1 and test-witness repairs.

FID root/teardown integration (Astra October10): the canonical64-byte node now
carries exact request claims, scoped wire mapping, access flags and source
borrow count separately from aggregate lifetime pins. CONNECT binds only its
actual session root at successful handshake commit under Ll. Final zero-ref
cleanup retires mapped children only after parent protocol abort, no remaining
claim/borrow, then reaps child before dropping parent. Root reap checks both
ownership ledgers. Native1854 normal/bulk fixture holds root borrow and unused
child through actual close with charged operation storage. CONNECT14mutants,
metadata7mutants and kernel link pass; derived mapped metadata cases are host
simulation only. RPC consumers/readiness/DAC/payload integration remain ahead.
See fid-integration/self-review.txt; old uninstalled-draft note is superseded.

Next isolated layer (Astra October10): rpc-draft remains uninstalled. Its bounded
per-scope list retains one exact request/physical owner and borrow through queued,
callback and settlement states, then retains terminal storage behind CQ pressure.
RPC-R1 requires scope abort on malformed/duplicate capture. Host50checks/21mutants
and actual ARM64 object/layout pass; actual client submission, operation-specific
DAC/payload admission, readiness/fairness and close integration are not implemented
by this helper. Read rpc-draft/next-work.txt before installing any queue consumer.

Astra October 10 wire adapter remains uninstalled (`wire-draft`). Single-frame
builders use real session operations with negotiated message/output/iounit limits
and exact reply expectations. WIRE-R1 records the submit return separately from
callback status: a remote EAGAIN cannot license replay. Host 156 checks/14 mutants
and ARM64 compilation pass; real native engine, ordinary DAC, payload ownership
and close integration still require qualification. See wire-draft/self-review.txt
for checked host boundaries and next-work.txt for coherent integration duties.

Astra October 10 staged authority program remains uninstalled (`program-draft`).
GETATTR precedes component WALK search and LOPEN access; symlink opens refuse
before DAC, and owner-first permission uses a per-operation captured identity.
Program NEXT/TERMINAL/ABORT are tentative driver outcomes, not CQ or publication.
The real permission and wire path pass4608 permission combinations/15 mutants,
with explicit host boundaries and ARM64 compilation. Constructor identity
retention, actual FID/payload admission and READY/close integration remain ahead;
see program-draft/self-review.txt and next-work.txt. No native runtime claim.

Astra October10 combined owner remains uninstalled (`owner-program-draft`). Its
single program.wire owns builder/capture expectations and payload. Constructor
retains fresh creator identity; once-per-attempt submit/advance prevents replay
and a stat-only success from bypassing the effect. Positive finish requires the
completed program; scope poison cannot become an isolated terminal. Host160checks/
17mutants plus constructor14mutants and real ARM64776B owner pass. Actual FID/
canonical-buffer admission and READY/physical close integration remain ahead;
no native queue/progress-abort/concurrency qualification. Read its self-review
and next-work; older RPC draft working copies are superseded, evidence retained.

Astra October10 FID admission layer remains uninstalled (`admission-draft`). Pure
queue/request checks precede CLUNK retirement under one Ll transaction; source
access borrows and exact WALK/open claims precede transfer. CLUNK waits for earlier
borrowers; final metadata publication/hold return/terminal share abort/deadline
checks. Error CLUNK cleanup requires actual abort-quiescence proof from the future
driver.257hostchecks/26mutants plusconstructor16mutants and ARM64816B owner pass;
new FID program successes are synthetic. Canonical buffer/SQE frontend and native
READY/close remain outstanding. See self-review/next-work; no activation claim.

Astra October10 fixed-buffer preparation remains uninstalled (`fixed-io-draft`).
An immutable kernel SQE yields exact creator/storage ownership, retained canonical
fixed extent or copied WALK name and monotone wire-FID burn. Zero-byte I/O still
validates registration/offset, without a payload pin. Publication retains an
independent Loom wake root and never touches transferred storage. A physical-drop
barrier bars terminal/free until backing release outside Ll completes; payload
pointer/capacity then clear.268hostchecks/17mutants and ARM64864B owner pass.
Actual shared-ring/READY/close and provided-buffer lifecycle remain outstanding;
read its self-review/next-work for host boundaries. No native/activation claim.
