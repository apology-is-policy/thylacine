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
validated-by: [spec-loom, spec-loom-multishot, spec-loom-order, spec-loom-devgone, spec-loom-role, gate-smp]
locks: []
abis: []
design:
  - "docs/LOOM.md"
  - "docs/reference/107-loom.md"
created: 2026-08-02
updated: 2026-10-07
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

That condition reads the completion ring without the ring lock and counts only
posted completions, which is sound only while nothing is in flight: no
completion can post concurrently, and no slot is reserved for an operation still
out. So the thread parks on it only when nothing is in flight. With operations
in flight it waits the way an enter does (below): it reads for every client it
has work on, over a ready stream only, and with nothing to read it hooks every
one of them and the completion list, sets the enter-needed flag, and parks until
a hook fires, a completion lands, the user produces a submission or reaps a
completion since its loop began, or it is told to stop. Each of those is
somebody's event, so a completion ring that cannot admit does not make it spin.
Until 2026-10-06 it pumped one client with a 10 ms frame-boundary deadline and
yielded in a loop while another thread held that client's role.

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
The fan-in below takes one such reference per distinct client and holds it
across the pumps, the hooks and the sleep, until every hook is off.

**The pump budget.** One ENTER's wait pumps at most `submitted + P9_TAG_LIMIT
+ 1` frames (`loom_wait_for_completions`, the Loom-3 audit's F4), so a server
flooding frames cannot hold a CPU inside one syscall. The frames ahead of this
ring's own answer tags already in flight, and a session holds at most
`P9_TAG_LIMIT` of those since its tag table grows (ARCH 21.11; the bound was the
64-tag pool's before 2026-10-07), so a server that answers what it was sent
never meets the budget.

**Waiting for the reader role.** The reader role belongs to the 9P client, and a
dev9p client is shared with other processes' synchronous calls. A synchronous
reader hands the role on only to another synchronous call, because an async
operation has no thread to read for it, so it can leave with this ring's reply
unread and nobody reading. An ENTER whose pump finds the role held therefore
hooks the client's role-waiter list as well as the completion wait-list, both on
its one rendezvous, and sleeps until either fires. A handoff that leaves the role
free with nobody designated wakes the list, and so does the session's death; the
ENTER then pumps itself. The hook is registered under the client lock against a
sample of the role taken there, so a release before it is seen and one after it
finds the hook. The borrow guard's reference is kept while the hook is on the
client's list and dropped after it comes off (LOOM.md 8.6 item 2;
`9p_client.loom_enter_wakes_when_role_frees`).

**The fan-in (2026-10-06, operator vote "waiters fan in").** That hook served
one client: the client of the ring's newest in-flight operation. A ring's
operations can span clients -- an event loop over a socket and files is the
canonical use -- and a reply on any other client stayed unread while the picked
one was held or slow. The picked client's pump also blocked in the receive
whether or not anything was due. So the waiter now reads for **every** client it
has an operation in flight on, and only over a ready stream. It collects the
distinct clients under the ring lock, skipping terminal operations and ones
parked for a re-arm, which have nothing on the wire, and pins each. It pumps
each once with the engine's readiness-gated pump, which takes a client's role
only when the role is free and the transport says a receive would not block.
With nothing to read it hooks every client -- a held role on the role-waiter
list, a free one on the transport's readiness list, never both -- then the
completion list, and sleeps on one rendezvous over all of them. A dead client
ends nothing: its death already posted an error completion for each of its
operations. Only the waiter's own death or stop unwinds it. The ENTER and the
poll thread share this set; the dev9p poll pump runs the same shape
([[sub-kernel-ninep-dev9p-poll]]).

The set lives on the stack: sixty-four entries, because operations ride
registered handles and one table names at most sixty-four. A re-registration
with operations still in flight can leave more clients than that in flight, so
the set can be partial. A partial set rotates its starting point over the
in-flight list and bounds its sleep with a 10 ms rescan, since a client left out
has no hook to wake the waiter. The set costs about 3.5 KiB of stack, the
precedent being the poll system call's sixty-four waiters. Witnesses:
`9p_client.loom_enter_reads_every_client` (two clients, the newest held: the
older one's reply ends the wait), `.loom_enter_partial_set_rescans` (a set
capped at one client finds the other by the rescan) and
`.loom_sqpoll_parks_on_a_held_role` (the poll thread sleeps, and does not run,
while its only client's role is held).

**The generation.** A completion that another thread reads posts its CQE before
it records what the completion changes for the ring's driver: a multishot
operation's re-arm, or a chain successor's gate. A waiter woken by that CQE
could re-check before the record, find nothing to re-arm or admit, and sleep
again with nothing left to wake it. And a sibling thread's submit can put an
operation on a client the waiter collected before it, between its client hooks
and its completion hook. Each ring keeps a generation, bumped under the ring
lock (`loom_drive_moved_locked`) by the post, by the completion's state update,
and by every operation that goes in flight: a submit's link, a re-arm claimed.
The
enter's sleep and the poll thread's in-flight park sample it at the top of the
loop and sleep only if it has not moved, re-reading it under the ring lock after
the completion hook is filed, so a completion after that read flags the hook.
The completion window needs a completion on another CPU between its post and
its record, and no test reproduces it deterministically (OPEN-BUGS 2026-10-06
16:20Z). The submit window is the fan-in's own, found by its self-audit; a test
knob (`g_loom_fanin_test_stall`) parks the waiter inside it while a sibling
submits (`9p_client.loom_enter_sees_a_sibling_submit`). A pump never sleeps in a
receive (the transport's `recv_now`), so a waiter's death or stop always finds
it in its sleep; a frame found in part stays with the client.

`specs/loom_role.tla` models the wait over any number of clients, its spec note
`spec-loom-role`: `NoMissedWake` (never asleep over a readable frame on a free,
undesignated role), `NoBlindRecv` (never in a receive with nothing due) and
`EnterReturns` (a reply on one client ends the wait while another's is held
forever). It does not model the generation.

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

So the join is made uninterruptible by a flag of its own,
`kthread_join_active`, set with save and restore around the sleep: while it is
set no death reaches the joiner's sleeps. Until 2026-10-07 the join borrowed
the at-exit close's `exit_close_active` instead; part B of the exit-close
design (ARCH 7.9.1) lets a second kill lift that flag's hold so the final
close stops waiting on its server, and a join riding it would then have
returned from every sleep at once and spun in a non-preemptible syscall body
-- forever at `-smp 1`, where the CPU it spins on is the one the poll thread
needs. The join's own flag holds every death, forced or not
(`loom.sqpoll_join_held_through_forced_close` catches the joiner inside the
join, through a test-only hold on the poll thread's terminal, and requires it
asleep).

What bounds the join is the poll thread's own work, never a server. Its pumps
never wait inside a frame (a frame found in part stays with the client), the
ops that would wait on a wire RPC are refused on an SQPOLL ring
(`loom_dir_mutation_gate`), and its reap's last close never waits either: the
thread marks itself `closes_never_wait` at entry, so its Tclunk goes to the
closer where it would wait for a tag or ring space (part A) and a staged
write-behind run goes to the closer as a close job (part C)
([[sub-kernel-ninep-dev9p]]). This closes the residual
[[seam-close-flush-unbounded]] recorded here before.

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
  borrowed client, and across every hook filed on it until the hook is off.
- **A waiter may sleep only over hooks on every client it waits on**, each
  filed under the lock its list's wakers hold, and with the generation unmoved
  since its loop top. A client left out of the hooks (a partial set) needs the
  rescan timer; a sleep without it strands that client's reply.
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
