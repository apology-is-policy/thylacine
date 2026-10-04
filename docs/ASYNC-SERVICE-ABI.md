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
Stale or absent references return-ENOENT; busy reap returns-EBUSY; unsupported
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
operation borrows end. Any operation timeout terminally aborts its whole scope.

CONNECT success result is its already-reserved destination index; WALK success
likewise. LOPEN/CLUNK return0, reads/writes byte count, failures negative errno.
Destinations never become usable from a success-looking malformed/stale CQE:
only the kernel-held slot state authorizes a following request. MULTISHOT uses
existing MORE and exactly one final completion. No new private CQE flag is needed.

## Mirrors and validation

Kernel: kernel/include/thylacine/loom_service_abi.h, included by loom.h.
Native C: usr/lib/libt/include/thyla/loom_service.h.
Rust: usr/lib/libthyla-rs/src/loom/service_abi.rs, exported by loom.rs.
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
