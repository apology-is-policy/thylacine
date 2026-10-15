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
  - usr/lib/libt/include/thyla/loom_client/adapter.h
  - usr/lib/libt/src/loom_client/adapter.c
  - usr/lib/libt/src/loom_client/native_kernel.c
  - usr/lib/libt/tests/loom_client/test_adapter.c
  - usr/lib/libt/tests/loom_client/sim_kernel.h
  - usr/lib/libt/tests/loom_client/sim_kernel.c
audit: hard
guarded-by: [inv-i29, inv-i30, inv-i7]
validated-by: [prose]
locks: []
hazards: []
abis: [abi-loom-service, abi-loom-ring]
design:
  - "docs/ASYNC-SERVICE-ABI.md \"Native adapter (October 12)\""
created: 2026-10-12
updated: 2026-10-15
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

The C twin (NA-4c) is libt's `thyla/loom_client/adapter.h`. It composes the C
cores ([[sub-libt-loom-client]]) the same way, over `struct t_loom_kernel`, a
table of the same raw calls; `t_loom_kernel_native()` (`native_kernel.c`) fills
it with libt's system-call stubs. Its host tests run the Rust tests' 55
scenarios, one for one, against a C port of the simulated kernel. Where the
two differ, the sections below say which side they describe.

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
- **The C interface** (ASYNC-SERVICE-LIFECYCLE section 7: opaque handles,
  submit, poll, cancel, reap):
  - `T_LOOM_RING_STORAGE(entries)` declares one ring's storage: the ring, one
    request-book entry and one operation handle per CQ slot (`2 * entries` of
    each). It is zero before its first use and reused only once `t_loom_idle`
    answers 1. `T_LOOM_RING_SETUP` passes the counts; setup refuses a count
    that is not the CQ's (-EINVAL) and a ring whose kernel geometry differs
    (-EIO).
  - Targets, scopes, fids and pools are cells of the ring's 64-entry tables;
    operations are `struct t_loom_op` cells. A submission returns its
    operation, `t_loom_turn` progresses the ring, and the kind's completion
    call answers 0 while pending, 1 once done (the handle is consumed, and a
    CONNECT's scope or a WALK's fid is handed over), or a negative errno with
    the handle kept: -ECANCELED for a closed ring, -EIO for a failed one,
    -EINVAL for another kind's handle. `t_loom_op_drop` of a pending operation
    aborts its scope. A stream yields its shots through `t_loom_stream_next`
    and is dropped explicitly; payloads already delivered stay valid.
  - A full table refuses before the kernel: no free operation handle, SQ slot
    or pool cell answers -ENOSPC. A client core's code becomes an errno:
    CLOSING and a ledger STATE are -ECANCELED, a full book or exhausted
    cookies -ENOSPC, a busy lease -EBUSY, geometry and receipt codes -EINVAL;
    kernel errnos pass through and anything else is -EIO.
  - A misused handle (a stale one, another ring's, a drop twice) ends the
    process through `T_LOOM_CHECK`, as in the cores.
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
  POLLERR, which the kernel answers only for a Loom object it no longer
  recognises, fails the ring (-EIO). poll's -EINTR is a spurious wake. An
  ENTER refusal that is not a close (-EINVAL, -EACCES) would repeat on every
  turn, so it fails the ring too.
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
  shot routed once retirement has begun is refused by its pool, checked by the
  ledger (`Pool::refuse`; a record naming a member a live payload still reads
  fails the ring) and RETURNed at once, so REAP can free the pool; a RETURN that finds the descriptor gone
  forgets it as any REGISTER does, and the turn answers -ECANCELED.
- **Teardown.** `close` (and dropping the `Ring`) aborts every scope and closes
  the descriptor. `Shared` detaches the mapping, then the buffers, when its last
  owner goes: after the CQ reader and every payload lease.
- **The C release levels** stand for `Inner` and `Shared`. `handles` counts the
  caller's handles; once the ring handle and the last of them are gone, the
  fixed driver and the mapping are dropped and the ring gives up its own hold.
  `holders` counts that hold, the core backend (kept alive by read results and
  payload leases) and the mapping backend. At zero the descriptor is closed,
  the mapping (only once validated) and every region are detached, and the
  storage is zeroed, which is what `t_loom_idle` reads. Setup's every refusal
  goes through the same release, so a failed setup leaves the storage idle.
- **C stream routes.** A stream dropped before its end keeps its operation
  cell as a route (handle cleared), holding its pool, so the shots still
  arriving are routed and their leases returned; the cell is freed with the
  stream's final completion, or when the ring's last handle goes. Delivered
  payloads wait in a 64-cell queue ordered by arrival; each holds one of the
  bank's 64 members, so a full queue is a broken invariant.

## Data structures

`Shared`: the kernel, the descriptor, the validated mapping, the regions and a
`[Option<SlotEntry>; 64]`. `Inner`: the `OwnedMapping`, the SQ `Producer`, the
`FixedDriver`, a waker table and a stream-route table of `cq_entries` cells
each, and a background-CLUNK table of 64.

C: `struct t_loom_ring` holds the kernel table, the descriptor and ring state,
the 64 regions, the SQ producer's pointers, `slots[64]` (each with its embedded
handle and background CLUNK book), `pools[64]`, the payload queue `shots[64]`,
the `t_owned_mapping`, the cores' `t_fixed_storage`, and pointers to the
caller's book entries and operation cells. Nothing is allocated.

## Concurrency

Creator thread only (`Rc`, `RefCell`; the C storage has no lock and the
header says so). No other thread may close the ring's
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
  and REGISTER answer a dead number -EBADF, as the kernel does. An operation
  on a FID that is not READY, under a READY scope, answers -ENOENT as
  `loom_fid_borrow` does (caveat 4).
- **The C twin's tests.** `tests/loom_client/sim_kernel.c` ports `sim.rs`
  knob for knob, and `test_adapter.c` runs the 55 scenarios of
  `tests/native.rs` under their Rust names (`future` reads `operation`); a
  `_Static_assert` pins the count. Each scenario also ends with the storage
  idle and every region detached. `tools/test-loom-client.sh` runs them
  sanitized (-O0, ASan and UBSan, no recovery) and optimized, and links
  `adapter.c` and `native_kernel.c` freestanding under `--no-undefined` with
  no mem\* import.
- **C sabotage.** `work/oct15-na4c/sabotage.py` turns each of the 55
  scenarios red with one targeted edit, after a control leg runs every named
  scenario green on the unedited tree. 63 cases (the 55 scenarios, four
  core cases and one Rust case) were every one red at its own assertion in
  the run of October 15, 00:22:48Z
  (`work/oct15-na4c/sabotage-20261015T002248Z.txt`). A case whose anchor no
  longer matches reports ANCHOR-ROT and counts as not red.
- **Sabotage.** `work/oct13-na4b/sabotage.py` turns each test red with one
  targeted adapter edit: 48 cases, every one red at its intended assertion
  in the run of October 13, 09:02Z (`work/oct13-na4b/sabotage-20261013T090219Z`),
  on the tree that carries vote 6 and audit round 6's fixes. A case whose anchor no
  longer matches reports ANCHOR-ROT and counts as not red.
- **The twins.** `tools/check-loom-service-abi.py` checks `ring_records`'
  `Submission`, `Header` and `BufferRange` field by field, and its constants,
  against the ring layout table.

## Seams

- **NA-4c**, the C adapter in libt, is this dossier's C half.
- **NA-4d** runs this adapter in the guest against an EL0 strict-9P fixture.
- **AS-4** adopts it for the clipboard client.

## Caveats

1. No guest program uses the adapter yet; only the host runs it (NA-4d).
   `native_kernel.c` is compiled and linked by the freestanding stage, but no
   boot executes it.
2. Retirement and REAP are not poll events. A caller waiting for `retired()`
   turns with a deadline.
3. The simulated kernel's REAP follows `loom_private_service_reap` as of
   October 13 (a walked fid reaps once clunked or once its scope has RETIRED;
   fids pin their scope). Until then no kernel path freed a walked fid while
   its ring lived, and the sim was more lenient than the kernel. The sim has no
   slot phase: the kernel's RETIRING FID is reachable by REAP only through
   `control_retiring_fid`, which only the guest test exercises.
4. The six paths round 1 left untested have host tests since 2026-10-13,
   each red under its own sabotage: `Fid::walk` from a file fid, a refused
   `Payload::try_return`, poll's -EINTR and POLLERR (sim knobs), an ENTER
   refusal that is not a close, an explicit `Ring::close` with handles alive,
   and `destroy_pool` answered -EBADF once the ring is lost. The sim answers
   -ENOENT for an operation on a FID that is not READY under a READY scope, as
   `loom_fid_borrow` does, and -EISDIR for a byte READ of the root.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
