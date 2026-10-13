# Private service Loom ABI: AS-0 reservation

October 4, 2026. Implements the encoding task under approved scripture4722f34e8.
Reservation only until the private runtime is qualified. Existing rings retain
all encodings and behavior. No new syscall, broad authority bit or raw-fd escape.

## Registry allocation

Loom uses syscalls66/67/68 unchanged. Reserve setup PRIVATE_SERVICE=1<<2;
REGISTER_SERVICE_TARGET=2, REGISTER_SERVICE_SLOT=3, ABORT_SCOPE=4,
QUERY_SERVICE_SLOT=5, REAP_SERVICE_SLOT=6; and SQE SERVICE_CONNECT=20.
Opcode19 remains reserved wire passthrough. Keep legacy LOOM_OP_COUNT and
LOOM_SETUP_VALID unchanged until activation; defining a constant is not support.
The C and Rust service ABI headers explicitly say this is unavailable initially.

Private mode initially accepts CONNECT20, WALK1, LOPEN2, READ4, WRITE5, CLUNK10.
SQE flags are zero except READ may use BUFFER_SELECT and, with it, MULTISHOT
as specified in ASYNC-SERVICE-BUFFERS.md; LINK, DRAIN and
CQE_SKIP are refused, preserving a terminal completion obligation for every
accepted request. Existing64-byte SQE and16-byte CQE sizes/offsets do not change.

## Slot identities

A reference is16bytes: slot:u32 at0, reserved-zero:u32 at4, incarnation:u64 at8.
Slots range0..63 across one shared target/scope/fid/pool table. Incarnation0 is
invalid; a ring-global monotone counter mints each reservation, stops before wrap,
and never reuses an incarnation. User-provided indices cannot choose generations.

Target registration allocates a TARGET slot. Reserving a SCOPE allocates the
root/fid slot later populated by CONNECT. Reserving a FID binds it to an existing
live SCOPE; WALK consumes that exact reservation, not any empty array entry.
A caller cannot race a stale completion against a new reservation: validate both
source and destination incarnation on the copied SQE, then hold their pinned
records until completion. REAP refuses live/busy slots. A terminal scope is
reapable only once local retirement and terminal-completion publication finish.
An unused reservation and a target with no local operation borrow can be reaped.
A walked FID is reapable once it is retired: after its CLUNK completes, or once
its scope has locally retired (RETIRED, the engine's proof of abort quiescence)
and no operation claims or borrows it. Each FID pins its scope, so a scope is
reapable only after its FIDs (operator vote, October 13).
CQs may still contain an old terminal result, but it refers to the old request;
incarnations never repeat and the library never reuses a live correlation ID.

## Versioned register records

All records begin size:u32 at0, version:u16 at4 (=1), flags:u16 at6 (=0).
SYS_LOOM_REGISTER nargs is1 for each of these records. Copy the input once into
kernel memory before validation. All unused input/output-only fields must be
zero. Output copy failure rolls back publication and owned references; no hidden
slot is left occupied. Version/size/unknown-flag errors return-EINVAL.

| Record | Size | Fields following common header |
| --- | --- | --- |
| target |288| registry_fd:i32@8, name_len:u32@12, name:u8[256]@16, result:ref@272 |
| reserve |48| kind:u32@8, reserved:u32@12, scope:ref@16, result:ref@32 |
| control |32| object:ref@8, reserved:u64@24 |
| snapshot |64| object:ref@8, kind:u32@24, state:u32@28, scope:ref@32, active_ops:u32@48, pending_terminals:u32@52, reason:i32@56, flags:u32@60 |

Target accepts name_len1..255, no slash/NUL, neither dot nor dot-dot, and a zero
unused name tail. Descriptor must reference the actual native registry; ordinary
service permissions/incarnation checks remain required at connect admission.
Reserve kind is SCOPE2 or FID3; TARGET1 is minted only by target registration.
SCOPE reservation takes an all-zero scope ref; FID takes a nonzero live scope.
Control is used by ABORT and REAP. QUERY uses snapshot, with only the header and
object set on input; everything else zero. Success returns0 and fills the record.
ABORT returns0 for a newly accepted abort,1 if already terminal/aborting.
Stale or absent references return-ENOENT; QUERY reads LIVE slots only, so it
answers -ENOENT for a clunked FID, which REAP still frees. Busy reap returns-EBUSY; unsupported
private mode returns-EOPNOTSUPP; exhaustion-ENOSPC; allocation failure-ENOMEM.
This adds no errno number. Protocol failure uses-EIO plus a recorded phase;
timeout-ETIMEDOUT and cancellation-ECANCELED retain their current numeric values.

States: EMPTY0, RESERVED1, ADMITTED2, VERSION3, ATTACH4, READY5, ABORTING6,
RETIRED7. TARGET uses READY; unused SCOPE/FID uses RESERVED; a walked/opened fid
uses READY. Snapshot scope is self for SCOPE and parent for FID, zero for TARGET.
Flags1 means local retirement complete; flags2 means request bytes may have
escaped. Neither means server rollback or refund of an endpoint retained by it.
Fields report a consistent locked snapshot; state is diagnostic, not authority
to bypass a later live admission check. No payload bytes are copied here.

## Private SQE field meanings

Every SQE is copied once, preserving I-30. Common private meanings:
handle_idx=source slot; _resv1[1]=source incarnation; _resv1[3]=absolute deadline
nanoseconds (0 means no deadline). _resv0 must be0. user_data is user correlation,
not authority. The kernel creates its own monotone request identity internally.
_resv1[0] remains registered-buffer byte offset; buffer index/len retain meanings.

| Operation | offset | len/buffer | _resv1[2] |
| --- | --- | --- | --- |
| CONNECT | destination SCOPE slot (upper32bits zero) | all zero | reserved destination incarnation |
| WALK | destination FID slot (upper32bits zero) | one path component1..255, or empty clone with all buffer fields zero | destination incarnation |
| LOPEN | read/write mode0,1,2 only | all zero |0|
| READ/WRITE | file byte offset | registered buffer slice |0|
| CLUNK |0| all zero |0|

For pooled READ only, ASYNC-SERVICE-BUFFERS.md overrides the fixed-buffer
fields above with a pool slot/incarnation; it does not change source/deadline.

Initial WALK disallows slash/NUL/dot/dot-dot; a root clone is empty. This is a
relative service fid walk, not general namespace traversal. A client may walk
components sequentially without a new path decoder. LOPEN rejects create/truncate
and every unallocated mode flag; apply the ordinary9P DAC/stat checks before
publishing access. A scope root cannot be CLUNKed to circumvent full scope
retirement: use ABORT then REAP; derived FID CLUNK retires that fid after pending
operation borrows end, and REAP then frees its slot. Any operation timeout terminally aborts its whole scope.

CONNECT success result is its already-reserved destination index; WALK success
likewise. LOPEN/CLUNK return0, reads/writes byte count, failures negative errno.
Destinations never become usable from a success-looking malformed/stale CQE:
only the kernel-held slot state authorizes a following request. MULTISHOT uses
existing MORE and exactly one final completion. No new private CQE flag is needed.

## Mirrors and validation

Kernel: kernel/include/thylacine/loom_service_abi.h, included by loom.h.
Native C: usr/lib/libt/include/thyla/loom_service.h.
Rust: usr/lib/loom-client/src/service_abi.rs, re-exported by libthyla-rs as loom::service_abi.
The ring records these records travel with (params, SQE, CQE, ring header) are
mirrored for native C in usr/lib/libt/include/thyla/loom.h, beside the three
Loom syscall stubs. Each field offset is pinned there, and the EL0 probe checks
the mirror against what the kernel actually writes.
Pouch/Go currently have no Loom mirror; adding one later requires extending
abi-loom-service's mirror set. They must not be invented as existing consumers.

AS-0 compiles actual headers/modules, checks every size/offset/constant and
compares emitted record bytes across C and Rust. Runtime tests must additionally
reject malformed versions/fields, stale incarnations, destination reuse, wrong
kinds, foreign scopes and live-slot reap. Layout tests alone cannot prove those
semantic checks. No runtime support or clipboard activation follows from AS-0.

## Provided-buffer pool extension (October 4)

The operator selected explicit pools (option C) to close AS-R7 before private
runtime activation. ASYNC-SERVICE-BUFFERS.md owns the concrete companion ABI:
setup SERVICE_BUFFERS8 requiring PRIVATE_SERVICE4; register7/8/9 for pool
creation, exact lease return and pool query; kindPOOL4 in the existing64-slot
table; SQE BUFFER_SELECT16; CQE SERVICE_BUFFER4. F_NOTIF2 remains reserved.
Private MULTISHOT READ now requires BUFFER_SELECT and a pool incarnation,
not repeated writes into one fixed slice. Ordinary fixed single-shot READ
remains available under its original field interpretation.

SQE64, CQE16 and user_data remain fixed. Pool-enabled setup exposes32-byte
per-CQ-slot receipts through loom_params._resv1[0..2], and still returns88bytes.
Five new layouts (member24, create1568, receipt32, return40, snapshot64) and their
exact offsets/zero rules are in the companion. The three compiled mirrors now pin all30 constants/10 records against
independent byte vectors and full ARM64 envelope guards. Runtime qualification
and pool ownership model/consumers follow; valid masks remain off.

## Qualification bridge (October 12)

Two operator votes of October 12, 2026 (Corona) settle how userspace reaches the
reserved ABI before activation. Neither changes an encoding above.

**A compile lever, off by default.** The build-config symbol
`PRIVATE_ASYNC_QUAL` (default n, in no preset) sets the kernel CMake option
`THYLACINE_PRIVATE_ASYNC_QUALIFICATION`, which defines
`LOOM_PRIVATE_QUALIFICATION`. The precedent is the Halcyon
interaction-qualification feature and the `KERNEL_TESTS` and tickful levers. The
`KERNEL_TESTS` gate was rejected: the everyday dev image runs the in-kernel
tests, so it would turn the feature on in practice. Without the lever, setup
with PRIVATE_SERVICE or SERVICE_BUFFERS and register ops 2..9 return
-EOPNOTSUPP. No private ring can then exist, so ENTER and poll never see one. A
test in the default image pins this. The lever is a qualification vehicle, not
activation; whole-arc activation remains a separate gate.

With the lever, the syscalls route to the existing internal private stack:

- **SETUP.** flags must be PRIVATE_SERVICE, alone or with SERVICE_BUFFERS.
  SERVICE_BUFFERS alone, SQPOLL, CQSIZE or any unknown bit returns -EINVAL. The
  private executor is implicit. entries is a power of two in 1..4096, and
  cq_entries is twice entries. Setup creates the ring and maps it with the
  private-ring VMA tag, so fork omits it and exec closes it. It then starts the
  executor (-EAGAIN on the thread budget) and installs the handle. params echoes
  the granted flags; with SERVICE_BUFFERS it also carries the receipt geometry
  described in ASYNC-SERVICE-BUFFERS.md. Every failure unwinds completely. A
  fault while copying params out closes the handle and unmaps the ring.
- **REGISTER.** BUFFERS on a private ring is private registration. HANDLES
  returns -EOPNOTSUPP, because targets replace it. Ops 2..9 take nargs 1 and the
  record's address. The kernel copies the record exactly once, at the size its
  operation fixes; a short size or a header mismatch returns -EINVAL. TARGET,
  SLOT and POOL publish only after the result has been copied back. A fault
  rolls the reservation back, burns its incarnation and returns -EFAULT. QUERY
  and POOL QUERY write output only on success. ABORT returns 0 or 1; RETURN
  returns 0.
- **ENTER never sleeps.** to_submit is capped at sq_entries. min_complete must be
  0, and flags may hold only NONBLOCK; GETEVENTS returns -EINVAL. ENTER copies
  each SQE before it publishes sq_head (SQ-1), kicks queued CQ work, and returns
  the count consumed, or -ECANCELED once the ring is closing. A caller outside
  the creator image gets -EACCES.
- **poll parks.** POLLIN means a CQE is ready; POLLHUP means the ring is closing.
  A client parks in poll(2) with its nearest absolute deadline as the timeout and
  rechecks its work predicate after every wake. That poll registers before it
  rechecks, so a CQE posted between the client's check and its sleep is not
  lost. There is no second park path to qualify. The legacy GETEVENTS wait was
  rejected because ENTER has no timeout argument and so cannot honour an
  absolute deadline.

-EFAULT and the other codes are existing registry values; no errno is added.
The flags that choose the route live inside params, so a params block the kernel
cannot validate or read returns the legacy -1 before any route is chosen.

**The EL0 probe (NA-2).** `loom-bridge-probe`, a native C program, ships in every
image's ramfs and reaches these syscalls from EL0 through the real argument and
copy paths. The kernel test `loom.bridge_probe` spawns it and requires the exit
status that its own build implies: one value for a kernel with the lever, another
for a kernel without it. A probe that infers its mode from what it sees cannot
pass the wrong image. The test may skip only a kernel booted without userspace;
a ramfs that carries init but not the probe fails it. In the default image the
probe checks every private route's -EOPNOTSUPP, and that a refusal leaves no
handle, no mapping and no partial params write. In the qualification image it
checks:
- each refusal above;
- the params copyout against a read-only page and against params that run into one;
- the record copyin (one loop serves every op) against a record that runs into an
  unreadable page;
- the result copyout of SLOT, QUERY, POOL and POOL QUERY against a read-only
  record (TARGET shares SLOT's and POOL's publish path and is faulted in-kernel,
  since it needs a service registry);
- the rollback, by the identity of the next fd and ring address;
- rejected SQEs' CQEs, reaped through poll(2), with one SQE that only a correctly
  read opcode byte can reject as it is.

Every successful setup runs over params pre-filled with a non-zero pattern, so a
zero read back was written by the kernel. The test then requires that the probe
created exactly the expected number of private rings, none in the default image
and four in the qualification image, and that every one of them retired.

## Native C client storage (October 12)

Operator vote, 2026-10-12 (NA-2b): the native C owned-client cores take
caller-provided storage. Native C links libt, which is freestanding: there is no
heap, no memcpy and no assert. The cores therefore never allocate. They are
installed in libt as `thyla/loom_client/` (headers) and `src/loom_client/`.

A ring client's storage is one object, `T_FIXED_STORAGE(entries)`, sized at
compile time. `entries` is the ring's CQ size: a power of two of at most 8192
(twice LOOM_MAX_ENTRIES), one request-book entry per CQ slot. The cores do not
check it against the mapping's `cq_entries`; the native adapter that composes
the mapping and the client sets it. It bounds outstanding requests, not
completions: a multishot pooled read yields many CQEs for one entry, so never
overfilling the CQ stays the kernel's half of I-29. Every other table
in it is sized by the ABI's own limits:
- one fixed cell and one request cell per registered buffer, 64 of each;
- 64 pool-member cells shared by every pool of the ring, as the kernel's own
  pool bank is shared, and so at most 64 pool headers;
- the 64-slot descriptor table that names the ring's pools.

The owned mapping (the paired CQ reader over the kernel's ring) and the
retirement executor take caller storage of their own, until a native adapter
composes them with the client. Storage is zero before its first use (static
storage already is). It is reused only once it is idle (`t_ring_idle`,
`t_fixed_storage_idle`, a destroyed mapping, an executor with no references).
Reusing live storage is a broken invariant.

A pool's header and member cells are claimed before the client registers the
pool with the kernel. Once the 64-member envelope admits the pool, the claim
cannot fail. So a local claim never fails after the kernel has reserved
something, and the obligation to pre-reserve local metadata holds by
construction rather than by discipline.

- A full table in front of the kernel refuses before the kernel is touched,
  with -ENOSPC (`T_LOOM_CLIENT_ENOSPC`). This covers the request book
  (`T_REQ_FULL`) and the 64-member envelope. The kernel's slot table, pool bank
  and request table answer the same errno when they are full.
  `tools/check-loom-service-abi.py` pins both sides to 28. A pool registration
  tells the two apart by its native rc, which is zero when the refusal is local.
- The retirement executor's full table keeps `T_RETIREMENT_FULL` (-1001). Admit
  also returns the factory's own errors, and a factory over the kernel can
  answer -ENOSPC. Only FULL says the factory never ran, so the caller still owns
  what the factory would have consumed.
- A pool registration's uncertain outcome is `T_RING_UNCERTAIN` (-1003), outside
  the POSIX errno range. A definite rejection returns the kernel's errno, and
  its native rc is that same errno. An uncertain code inside the range (the
  uninstalled core's -100, which is ENETDOWN's value) could not be told apart
  from a rejection with that errno, and the two need different cleanup.
- The cores copy with libt's byte helpers (`thyla/bytes.h`), not memcpy or
  memset. The userspace toolchain builds them at -O2, where clang inlines every
  aggregate copy. At -O0 it would call memcpy for a struct passed by value. A
  native link runs with --no-undefined, so it refuses that call rather than
  carrying it.
- A broken internal invariant ends the process through `t_loom_client_fatal`,
  not assert or abort. libt's definition writes the site to fd 2 and then calls
  `t_exit_group`. It is never ignored.
- The ring mapping stays the kernel's, mapped by setup. The storage object holds
  only the client's own bookkeeping.
- `make test-loom-client` (`tools/test-loom-client.sh`) runs the cores' host
  tests under ASan and UBSan and at -O2. It also builds the cores for aarch64
  with libt's flags and links every public function under --no-undefined.

Rejected:
- A libt heap. This is Plan 9's answer, where libc mallocs over the data
  segment. Here it would add a new audit-bearing allocator to a library that
  today serves only probes and fixtures. It would also have to sit on the eager
  SYS_BURROW_ATTACH, because RESERVE charges at first touch.
- Rust-only clients. This would amend AS-3's "C/Rust clients".
- The C cores on Pouch. Pouch has no Loom mirror, and no ported program is
  planned to use private async.

Prior art: liburing's no-libc build, where the caller owns struct io_uring;
seL4 and Genode, which pass allocators explicitly; and Fuchsia's
caller-allocated FIDL.

## Native Rust client cores (October 12)

The Rust twins of the owned-client cores (NA-3) keep the heap. libthyla-rs has
`ThylaAlloc`, and the caller-storage vote concerned freestanding C only.

They are installed as the crate `loom-client` (`usr/lib/loom-client`),
`no_std` with `alloc`, not as modules of libthyla-rs. libthyla-rs cannot be
built for the host, because its `_start` is ELF assembly, so a test inside it
would run nowhere. The cores make no syscall. Like `thyla-heap`, they live in a
crate the host can test, and libthyla-rs supplies the kernel calls through the
native adapter (NA-4). Until the adapter exists the cores are crate-private, and
only their tests use them.
- `service_abi` moves into the crate, because the cores use its records and a
  host-tested crate cannot depend on libthyla-rs. libthyla-rs re-exports it as
  `loom::service_abi`.
- The crate is edition 2024, the edition the twins were written and tested in,
  so the install builds exactly what was tested. The NA-3 audit enumerated every
  `RefCell` borrow in the cores and found none whose end differs under 2021: the
  pin keeps the derivation exact, and no borrow depends on it.
- Under `cargo test` the crate is std, as the twins' own test binaries were, so
  the tests keep `Vec`, `Box` and `thread_local!` as written. Every other build,
  the guest's included, is `no_std`.
- `make test-rust` runs the tests with `cargo test`. `make test-loom-client`
  runs them again in the release profile, which is the one the guest builds
  (opt-level "z", overflow checks on). A test build still unwinds on a panic
  where the guest aborts.
- The tests count allocations. A pool's state is allocated before the kernel
  admits it, so the count runs from the moment the backend's `register_pool`
  returns until the client's does, and any allocation there fails the test.
  Bounded progress, a ready future and a wait turn are counted the same way.

The Rust cores return typed errors, so no two outcomes share a value. The
full-table outcomes match C:
- the request book's `Error::Full` is C's `T_REQ_FULL`, which is -ENOSPC;
- the 64-member envelope answers `ring_payload::Error::Full`, which C answers
  with -ENOSPC. The uninstalled twin answered `Busy`, which also means that a
  fixed range or a pool slot is in use;
- the executor's full table stays `Error::Full`, apart from a factory's error,
  as C keeps -1001 apart from -ENOSPC;
- a future driver's 64-registration table, which has no C twin, answers
  `Storage(Full)`. The uninstalled twin answered `Busy` there too.

When the native adapter maps one of these to an errno, `Full` becomes ENOSPC.

### C local codes

A C core's return channel can also carry a kernel errno or another family's
code. So no local code lies in the errno range, except `T_REQ_FULL`, which is
-ENOSPC on purpose. Each family has its own block at or below -1000. The
hundreds digit names the family, and the last digit keeps the old value:
- `T_FIXED_CLOSING`, `T_RETIREMENT_FULL`, `T_RETIREMENT_CAPACITY` and
  `T_RING_UNCERTAIN`: -1000 to -1003, unchanged;
- the lease ledger's `T_LEASE_*`: -1101 to -1106;
- the request book's `T_REQ_*`: -1201 to -1206;
- the owned mapping's `T_MAP_*`: -1301 to -1304;
- the paired CQ reader's `T_CQ_GEOMETRY` and `T_CQ_CORRUPT`: -1401 and -1402.
  It returned bare -1 and -2 before.

Two collisions forced this. `t_fixed_read` and `t_fixed_write` returned -3
both for `T_REQ_EXHAUSTED` and for `T_LEASE_BUSY`. The first is permanent (the
cookie counter is spent) and the second is transient. `t_ring_fixed_bytes`
returned `T_LEASE_BUSY` (-3) in the channel that carries a remote -ESRCH.
Static assertions keep every code inside its block.

### Ring records

The Rust ring records (`Params`, `Sqe`, `Cqe`, `BufReg`, and a `RingHdr` that
`loom.rs` used to read only by byte offset) move into libthyla-rs's
`loom/ring_abi.rs`. That file depends on no crate, so
`tools/check-loom-service-abi.py` compiles it and checks it against the layout
table the kernel and libt headers already answer to. The ring's header offsets
in `loom.rs` are derived from `RingHdr`. The checker also checks the cores' own
copies of the setup record and the CQE, field by field: `t_owned_setup` and
`t_owned_cqe` in C, `Setup` and `Completion` in Rust.

## Native adapter (October 12)

NA-4 composes the owned-client cores into a client that drives a private ring
from EL0. It runs only in the qualification image, because outside it setup
returns -EOPNOTSUPP; it activates nothing. The cores make no syscall and never
write an SQE. Their `RingBackend`, `Mapping` and `WaitSource` contracts name
what an integrator owes, and this section says how the adapter pays it.

**Where it lives.** The Rust adapter is a module of `loom-client`, written over
a public trait of raw kernel calls (setup, register, enter, poll, close, detach,
the monotonic clock). libthyla-rs implements that trait over its stubs and
re-exports the adapter. This is `thyla-heap`'s shape: libthyla-rs implements its
`Backend`. So the adapter's logic runs under `cargo test` against a simulated
kernel, and the cores stay crate-private. The C adapter is
`thyla/loom_client/adapter.h` and `src/loom_client/adapter.c` in libt, over a
table of the same calls that libt fills with its `svc` stubs and the host tests
fill with a simulation. Following the October 12 vote, its storage is one
caller-provided object that holds the mapping, the client storage and the
executor. The Rust adapter comes first, because the clipboard client that AS-4
adopts is Rust.

**One book per ring.** Every request takes its cookie from one request book of
`cq_entries` entries (NA-2b F3): control operations (CONNECT, WALK, LOPEN,
CLUNK), fixed reads and writes, and pooled reads. One dispatcher reads each CQE
and its receipt through the paired reader and routes it by the book entry's
kind:
- a control completion goes to the operation's waiter;
- a fixed completion goes to the fixed owner, which ends that buffer's use;
- a pooled completion goes to its pool, which yields an owned payload lease.
The fixed driver becomes the ring's request driver: its book is the ring's one
book, and control, slot and pooled requests are reserved through it beside the
fixed ones. CONNECT and WALK succeed with their reserved destination slot
(loom_service_connect.c, loom_service_admission.c), so the book accepts exactly
that value for them and nothing positive for LOPEN or CLUNK. That edits Astra's
cores, so it ends NA-3's derived equivalence, as NA-3's dossier says the first
edit will.

**Submission.** The adapter is the SQ's only producer. It writes the SQE, with
`user_data` set to the book's cookie, and the SQ index. It publishes the book
entry, and only then release-stores `sq_tail`. Nothing fallible follows that
store. The private executor never drains the SQ, so every submission ends with
ENTER(NONBLOCK, `min_complete` 0). ENTER may consume fewer SQEs than are
queued: when the CQ is full, when a rejection is pending, or while another ENTER
is submitting. A ring that starts closing mid-call returns the count consumed so
far, or -ECANCELED if it is zero. The adapter submits the rest on a later turn
and never discards one. It never writes an out-of-range index, so
a change in the header's `dropped` count is a broken invariant, and it ends the
process through the client's fatal path.

**WALK names.** A WALK's component travels in a registered buffer. The adapter
registers one more buffer after the caller's, a name arena of one 256-byte cell
per SQ slot, and a WALK uses the cell of the slot its SQE takes. The kernel
releases `sq_head` before it copies the name, but it copies it inside the ENTER
that consumed the slot (loom_service_frontend.c, loom_service_io.c), and only the
creator thread enters. So a cell is free again before its slot is.

**Deadlines.** Each operation's absolute deadline travels in its SQE, where the
kernel enforces it, and a timeout aborts the whole scope. The adapter keeps no
timer of its own; a wait's timeout is the caller's deadline.

**Waiting.** The `WaitSource` is poll(2) on the ring fd. `arm` samples
`cq_tail`, and `wait` polls for POLLIN with a timeout taken from the nearest
absolute deadline. The kernel registers the waiter before it samples the CQ, and
that is what discharges the trait's obligation to compare the armed state
atomically with parking. poll's timeout is in milliseconds, so the adapter
rounds the remaining time up: it never wakes before a deadline, and it expires
the deadline on the recheck. POLLHUP means the ring is closing. A private ring
has exactly one descriptor: it is never duplicated or inherited across fork, and
exec latches it only past the point of no return. So a ring that closes under the
adapter had that descriptor closed by another thread, whether poll reports it
(POLLHUP or POLLNVAL) or ENTER does (-ECANCELED or -EBADF). The number may already
name another file, so the adapter forgets it without closing it. A pool RETURN, a
REAP and a scope's retirement are synchronous REGISTER results, not poll events.
The loop turn rechecks them as local progress. ENTER never sleeps, by the
October 12 vote.

**Scopes and cancellation.** The adapter creates scopes. It reserves the slot
(REGISTER SLOT, kind SCOPE), submits CONNECT with an absolute deadline, and
hands out a scope only after CONNECT's completion. FID slots for WALK work the
same way. Cancellation is ABORT_SCOPE, the only cancel the kernel has: it ends
the whole connection, siblings included. Every cancelling API says so, and the
application uses separate scopes for independent work (ASYNC-SERVICE-LIFECYCLE
section 7). A scope lives while its handle, a fid of it or an operation on it
does; when the last goes, the adapter aborts it and REAPs it once it reports
RETIRED. Dropping a fid clunks it in the background, when the SQ has room, and
REAPs it after the CLUNK's completion. A fid of an aborted scope is not clunked:
the abort ends its session, and the fid is reaped once the scope reports RETIRED.
The scope is reaped after its fids.
REAP is retried on every turn, and once more before a reservation answers
-ENOSPC. Retirement and REAP are not poll events, so a turn that waits for them
needs a deadline.

**Pools and streams.** A pooled read is a typed stream handle that holds its
pool. Dropping the handle aborts its scope, as dropping a fixed request does.
A pool is retired locally: no new read may name it. The kernel has no pool
retire, and REAP refuses a pool while any stream, busy, pending or leased
member remains. So each progress turn retries REAP until it succeeds. The
backend's whole-ring calls map as follows:
- `begin_retire` closes local admission and aborts every scope the adapter owns;
- `retired` holds once every SQE the adapter pushed has had its terminal CQE
  taken (each SQE the kernel consumes ends in exactly one, I-29), each of those
  scopes reports RETIRED, and no fixed or pool reference remains. RETIRED alone
  is not enough: the engine retires a scope in the visit that cancels its last
  operation and posts that terminal on a later one, and ENTER can hold a refusal
  back against a full CQ that no request record (and so no QUERY) counts.
  Retirement then discards only abandoned request entries; a future still held
  keeps its terminal for its owner. A shot that arrives once retirement has begun
  goes straight back by RETURN, or its member would stay leased and REAP would
  refuse the pool;
- `close` closes the ring fd. The mapping and the buffers are detached when
  their last owner is gone, after the paired reader and every payload lease,
  because closing does not unmap and those read them until the end. A mapping
  whose setup geometry fails validation is never detached. Exec closes the ring,
  and fork omits its mapping (NA-1).

**Errors.** The client's `Full` and a full SQ become -ENOSPC. Kernel errnos pass
through unchanged. A closed ring answers -ECANCELED, the kernel's own code for
it. A corrupt or stale CQE fails the ring, which closes it, and every operation
then answers -EIO. The adapter adds no errno.

**Sub-chunks.**
- NA-4a: this section.
- NA-4b: the Rust adapter and libthyla-rs's implementation of the call trait,
  tested on the host against a simulated kernel.
- NA-4c: the C adapter, tested the same way.
- NA-4d: the guest qualification. No EL0 program can reach a private CONNECT
  today, because the kernel's tests serve from kernel threads on registries
  they create themselves. NA-4d adds an EL0 fixture, a strict 9P server posted
  in a registry the probe can name, and a qualification-image probe. The probe
  drives both adapters through: connect, walk, open, fixed reads and writes, a
  multishot pooled stream with out-of-order returns, a deadline that aborts its
  scope, cancellation by drop, a full CQ, and closing the whole ring with leases
  still held. A kernel test pins the probe's exit status by image, as
  `loom.bridge_probe` does.

Each sub-chunk is audit-bearing, and NA-4b adds the trigger row for the
adapter.
