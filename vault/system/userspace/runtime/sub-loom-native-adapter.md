---
id: sub-loom-native-adapter
type: sub
title: "the native Loom adapter -- a private service ring driven from EL0"
parent: moc-userspace-runtime
code:
  - usr/lib/loom-client/src/native/mod.rs
  - usr/lib/loom-client/src/native/kernel.rs
  - usr/lib/loom-client/src/native/handles.rs
  - usr/lib/loom-client/src/ring_records.rs
  - usr/lib/loom-client/src/tests/native.rs
  - usr/lib/loom-client/src/tests/native/sim.rs
audit: hard
guarded-by: [inv-i29, inv-i30, inv-i7]
validated-by: [prose]
locks: []
hazards: []
abis: [abi-loom-service, abi-loom-ring]
design:
  - "docs/ASYNC-SERVICE-ABI.md \"Native adapter (October 12)\""
created: 2026-10-12
updated: 2026-10-13
---
## Purpose

The native Loom adapter drives one private service ring from EL0. It composes
the Rust client cores ([[sub-loom-client]]) into a client a program can use:
connect to a strict 9P service named in a registry, walk and open its files,
read and write through registered buffers, and stream pooled reads whose
payloads are leases. It is a module of `loom-client`, written over a public
`Kernel` trait of raw calls, so all of its logic runs under `cargo test` against
a simulated kernel. libthyla-rs implements `Kernel` as `ThylaKernel` and names
the ring `ServiceRing` ([[sub-libthyla-rs]]). Setup succeeds only in the
qualification image; elsewhere the kernel answers -EOPNOTSUPP. Nothing is
enabled by it.

## Contract

- **`Kernel`** is an `unsafe trait`: each method is one system call whose
  arguments and result pass unchanged (setup, register, enter, poll on one
  descriptor, close, attach, detach, the monotonic clock). `attach` must return
  eager, physically contiguous, zeroed memory, the kind Loom registration
  resolves. The poll bits are checked against libthyla-rs's at compile time.
- **`Ring::setup(kernel, entries, buffers)`** attaches one region per buffer
  length (rounded to a page; at most 63) and a name arena, sets up the ring with
  PRIVATE_SERVICE and SERVICE_BUFFERS, validates the geometry through
  `OwnedMapping`, and registers every region. Any refusal releases what was
  taken.
- **Handles.** `Target` (a service name in a registry), `Scope` (one connection:
  a fresh 9P session), `Fid`, `Pool`, `Stream`, `Payload`. Operations are
  futures: `Connect`, `Walk`, `Open`, `Read`, `Write`, and `Stream::next`.
  `Ring::turn` drives the ring once; `Ring::block_on` runs one future.
- **Cancellation is scope-wide.** Dropping an unfinished future or stream aborts
  its whole scope, siblings included; `Scope::abort` does it explicitly. The
  kernel has no narrower cancel (ASYNC-SERVICE-LIFECYCLE section 7).
- **Errors are negative errnos.** Kernel errnos pass through; `Full` and a full
  SQ are -ENOSPC; a closed ring is -ECANCELED; a failed ring (a corrupt or stale
  CQE) is -EIO. A changed `dropped` count panics: the adapter never writes an
  out-of-range index, so it is a broken invariant.
- **A ring closed under the adapter is forgotten, never closed again.** ENTER
  answered -EBADF or -ECANCELED, any REGISTER answered -EBADF (`syscall.c`
  checks the descriptor before the record), or poll answering POLLHUP or
  POLLNVAL, means another thread closed the ring's only descriptor: a private
  ring is never duplicated (handle.c, `handle_dup`) or inherited across fork
  (`handle_slot_may_alias`), and exec latches it only past the point of no
  return (proc.c, `proc_exec_replace`). The number may already name another
  file, so `Shared` drops it unclosed and the ring answers -ECANCELED. A
  TARGET's -EBADF can name its registry descriptor instead, so there a QUERY
  of the zero reference decides: a live ring answers it -ENOENT. Detection
  waits for the adapter's next REGISTER, ENTER or poll. A number reused by a
  file of another kind is caught by the next REGISTER or ENTER, but a poll
  issued first waits on the new file. One reused by another Loom of the same
  program, legacy or private, is caught by none of the three: a REGISTER
  answers -EOPNOTSUPP or goes to the other ring, and ENTER and poll go to it.
  Closing the ring's only descriptor from another thread is the program's
  fault, and without descriptor generations EL0 cannot see a reuse sooner.
- **Slot drops.** A failed WALK's reservation is reaped without a CLUNK (the
  kernel would refuse one: `loom_fid_begin_clunk` wants a READY fid). A REAP
  answered -EBUSY is retried on a later turn; any other refusal but -ENOENT
  drops the adapter's entry and leaves the slot for close to reclaim.

## Mechanism

- **One book, one dispatcher.** The fixed driver is the ring's request driver:
  its book holds control (LOPEN, CLUNK), slot (CONNECT, WALK), fixed and pooled
  requests. `route` accepts each CQE through the book and returns what it was:
  a fixed terminal (the owner's buffer use ends), a control terminal (left for
  its future), or a pooled shot, which the dispatcher hands to the pool its
  request names and queues on the stream.
- **Submission.** SQ space, then the request's publish, then the WALK name copy,
  the SQE write and the release store of `sq_tail`, then ENTER(NONBLOCK, 0).
  Nothing fallible follows the publish. A short ENTER leaves the rest queued;
  the next turn enters again.
- **WALK names** live in an extra registered buffer, one 256-byte cell per SQ
  slot, keyed by the slot the SQE takes. The kernel copies the component inside
  the ENTER that consumes the slot, and only the creator thread enters.
- **Waiting.** `RingWait` is the cores' `WaitSource`: arm samples `cq_tail`,
  ready is a non-empty CQ, wait is poll(2) with the time left rounded up to a
  millisecond. POLLHUP and POLLNVAL forget the descriptor and close the ring;
  POLLERR fails the wait with -EBADF.
- **Deadlines** go in each SQE; the kernel enforces them and aborts the scope.
- **Slots.** A scope's fids pin it, so `reap_pass` repeats while a pass frees
  a slot. `Shared` tracks every target, scope and fid the adapter minted:
  Live, Clunk (a dropped fid, clunked when the SQ has room), Clunking, Reap
  (REAP retried each turn, and before a reservation answers -ENOSPC). Pools are
  the cores' and are reaped through `destroy_pool`.
- **Retirement.** `retire` closes admission and aborts every scope. `retired()`
  holds once every SQE pushed has had its terminal CQE taken (`Shared`'s
  `submitted` and `ended`) and every scope's QUERY reports RETIRED. The private
  frontend ends every SQE it consumes in exactly one terminal (an admitted one
  by I-29, a refused one through its precharge); a dropped SQE ends in none,
  and the adapter panics on one. RETIRED alone is not
  proof: the engine retires a scope in the visit that cancels its last
  operation and posts that terminal on a later one (`loom_service_engine.c`,
  `engine_ready_visit`), and ENTER can precharge a refusal against a full CQ
  (`loom_service_frontend.c`) that no request record counts. Retirement discards
  only abandoned book entries, so a future held across it keeps its terminal. A
  shot routed once retirement has begun is refused by its pool and RETURNed at
  once, so REAP can free the pool; a RETURN that finds the descriptor gone
  forgets it as any REGISTER does, and the turn answers -ECANCELED.
- **Teardown.** `close` (and dropping the `Ring`) aborts every scope and closes
  the descriptor. `Shared` detaches the mapping, then the buffers, when its last
  owner goes: after the CQ reader and every payload lease.

## Data structures

`Shared`: the kernel, the descriptor, the validated mapping, the regions and a
`[Option<SlotEntry>; 64]`. `Inner`: the `OwnedMapping`, the SQ `Producer`, the
`FixedDriver`, a waker table and a stream-route table of `cq_entries` cells
each, and a background-CLUNK table of 64.

## Concurrency

Creator thread only (`Rc`, `RefCell`). No other thread may close the ring's
descriptor; if one does, the adapter forgets the number rather than close a
file it may now name. No borrow is held across a waker's `wake` or a payload's
drop. The kernel's poll registers its waiter before it
samples the CQ, which gives the wait its atomic compare.

## Invariants enforced

- **I-29 / I-30 (client side):** the book bounds outstanding requests to
  `cq_entries` and takes each terminal once; each SQE is whole before the release
  store, and `user_data` is a cookie, not authority.
- **I-7:** no region is detached while a reader or lease can still use it.

## Error paths

A local refusal before publish leaves no state: the reserved slot goes to Reap
and the unpublished book entry is cancelled. After publish the operation always
reaches a terminal or the ring closes.

## Performance

One ENTER per submission and per turn that routed completions; one poll per
idle turn. Lookups scan at most 64 slots or `cq_entries` cells. Nothing on the
completion path allocates.

## Prosecution

- **The simulated kernel** (`tests/native/sim.rs`) ports the private SQE shape
  check and models ENTER, poll, REGISTER and close as the kernel's frontend,
  decode and loom.c define them; each test names the behaviour it pins. It
  panics on a second close of a descriptor. A WALK in flight borrows its
  destination (the kernel's claim pins it), so REAP refuses that slot until the
  WALK ends. A scope's QUERY reports its operations in flight and its
  terminals not yet posted; `hold_terminals` keeps an abort's terminals back
  after the scope retires, `hold_rejections` keeps a refusal back, as the kernel
  does against a full CQ (the sim holds it whatever the room) and consumes no
  further SQE meanwhile, and `engine_visit` runs the engine's retirement
  between two adapter calls. The sim's ENTER posts every deferred completion
  before it consumes, which the kernel's zero-submit ENTER does not. Its ENTER
  and REGISTER answer a dead number -EBADF, as the kernel does. Known divergence: an operation on a fid that is not READY, under a
  READY scope, answers -ECANCELED in the sim and -ENOENT in the kernel; no
  adapter path depends on the errno.
- **Sabotage.** `work/oct13-na4b/sabotage.py` turns each test red with one
  targeted adapter edit: 45 cases, every one red at its intended assertion
  in the run of October 13, 08:37Z (`work/oct13-na4b/sabotage-20261013T083704Z`),
  on the tree that carries audit round 5's fixes. A case whose anchor no
  longer matches reports ANCHOR-ROT and counts as not red.
- **The twins.** `tools/check-loom-service-abi.py` checks `ring_records`'
  `Submission`, `Header` and `BufferRange` field by field, and its constants,
  against the ring layout table.

## Seams

- **NA-4c**, the C adapter in libt, follows the same section.
- **NA-4d** runs this adapter in the guest against an EL0 strict-9P fixture.
- **AS-4** adopts it for the clipboard client.

## Caveats

1. No guest program uses the adapter yet; only the host runs it (NA-4d).
2. Retirement and REAP are not poll events. A caller waiting for `retired()`
   turns with a deadline.
3. The simulated kernel's REAP follows `loom_private_service_reap` as of
   October 13 (a walked fid reaps once clunked or once its scope has RETIRED;
   fids pin their scope). Until then no kernel path freed a walked fid while
   its ring lived, and the sim was more lenient than the kernel. The sim has no
   slot phase: the kernel's RETIRING FID is reachable by REAP only through
   `control_retiring_fid`, which only the guest test exercises.
4. Paths no host test reaches yet: `Fid::walk` from a fid that is not the
   root, a refused
   `Payload::try_return`, POLLERR and -EINTR (the sim returns neither), an ENTER
   error other than -ECANCELED and -EBADF, an explicit `Ring::close` with
   handles alive, and a closing pool's REAP (`destroy_pool`) answered
   -ECANCELED once the ring is lost.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
