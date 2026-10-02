# Dedicated seat owners: implementation review

October 2, 2026, Astra. Operator-approved single-agent self-review, not an
independent adversarial audit. Base 1ba2b3354. The public clipboard endpoint is
still disabled. This checkpoint installs the cancellation/progress foundation
and migrates the existing session media service.

## Authority and ordering

HSR reservation requires the existing declared normal renderer connection and
fresh kernel peer stripes. HSC Join binds that reservation to one physical
connection from the same kernel peer. Tokens decoded from a message cannot
supply peer identity. Fixed eight-member state and two unbound handshakes refuse
capacity exhaustion. One opened control fid per physical HSC lane and exact
monotone transaction caching prevent competing aliases or replay.

The coordinator alone samples Lictor and serializes phase transitions against
registration/cancellation under a short mutex. No GPU/RPC/allocation/join occurs
under that mutex. Per-member suspension revisions stop another participant's
ACK from staling a frozen obligation. EOF, failed episodes and lost renderer
declarations retain a live potential obligation. Only exact Cancelled, orderly
Retire or a fresh kernel-confirmed death discharges it. Counters fail closed at
exhaustion. Zero members still require a fresh QUIESCING observation.

Lictor rechecks the designated compositor's kernel PID and stripes on opcode 66.
The aggregate is conjunctive with hardware retirement, private scanout and key
release. It cannot authorize a stale or non-QUIESCING generation. Cursor remains
opcode 65. No hardware request completes early to release a producer's slot.
There is no new kernel ABI, capability, role or syscall.

## Ownership and lifetime

Each ServiceWorker owns shared state, four nonblocking pipe FDs and a guarded
stack. A child borrows the state only until kernel-confirmed clear-tid join.
Construction unwinds partial ownership. A failed join retains storage until
process exit. Wake hints coalesce on EAGAIN; shared state retains the work, and
bounded drains leave remaining bytes readable. Stop/status atomics are distinct
from metadata notifications. No error/drop diagnostic writes the ordinary
console from a control owner during EXCLUSIVE.

The Halcyon owner holds all media parsing and connection state, Broker, HIA and
HSC. Setup moves File ownership, never Ring/EventRing via an unsafe Send cast.
The UI shares only bounded route/budget metadata and moved image results. The
service polls independent HIA/HSC descriptors and its listener/clients. A posted
owner's fatal error ends its Proc without depending on a blocked UI; native
repost checks prove registry cleanup. Normal service teardown is used only at
process/session exit. A published service must not be dropped and its Proc kept
as a continuing host.

No public clipboard requests exist yet, so HIA is idle and the broker has no
application replies. Future adapter code must retire unsent/partial replies
before cancellation ACK and drain/retain outstanding HIA storage. That work and
the complete activation resource ledger remain open, not implicitly approved
by these empty-broker lifecycle tests.

## Findings closed in this implementation

- Wire review found opcode 65 already names Cursor; new ACK uses 66.
- A root-owned mode 0600 control file would deny ordinary users before kernel
  peer authentication; mode 0666 plus server-side identity checks is required.
- Tflush of a parked State must keep request history; it caches EINTR instead
  of permitting reused IDs. Idle State returns within 250ms for orderly stop.
- The physical probe originally logged after ACK; normal console output parked
  in EXCLUSIVE. Witnesses now print after restoration, with the harness waiting
  on the kernel's episode marker. Failed evidence is retained.
- The forced GPU fixture initially looked up the HSR request's zero placeholder;
  it now uses the returned registration incarnation. The failed run is retained.
- Media completion now publishes before Rwrite, so a client cannot exit before
  the result is available to the UI. Its exact route token is rechecked at
  publication; a reused leaf under another token cannot receive stale data.
  The leaf-only mutation fails the named replacement-tile test.
- Listener poll/accept errors explicitly fail the owner. Runtime and
  post-publication failure controls moved from PollWorker to the new executor;
  the old readiness-only fault hooks would not exercise this implementation.
- A surviving publication flag classifies constructor failure without borrowing
  released worker storage; pre-publication failure still leaves no service.

## Measured evidence and limits

Evidence is retained under `work/oct2-hi-seat/`, including source/artifact pins,
failed attempts, raw console logs and scripts. Thirteen coordinator plus two
Lictor gate tests, seventeen transport tests, and the migration's 524-test host
set pass. Eight named barrier mutants fail as intended. Two actual-source route
schedules pass, and their leaf-only mutant fails. Native schedules include
independent owner progress, FD rollback, joined stop and 16,384 wake hints.

The 1830/1830 ordinary CPU1 boot passes. Native physical F10 proves both a real
parked compositor GPU RPC and an application present stay pending through
EXCLUSIVE and resume after restoration. The forced RPC is submitted after the
coordinator observes QUIESCING; no separate pre-chord queued-request timing
experiment is claimed. A stalled participant never reaches EXCLUSIVE, with
refusal/restoration in 6,660ms including the 1,500ms failure notice. Media clients
finish with the UI deliberately idle. Published failure ends the Proc and allows
repost. This is not graphical crash recovery.

Final graphical session media (72.54s) and F10 authorization (90.65s) pass at
1280x800. View, PNG/JPEG Gallery, manual history/theme, Gallery SAK restoration,
confer/use/abdicate, wrong-key and cancellation remain working. The manual-history
workspace and trusted prompt screenshots were visually inspected. Final focused
images use GOROOT bake disabled for disk capacity; default foundation images
passed earlier, but no new Go qualification is claimed. SMP/50-boot/ASan/UBSan
are waived by the operator for October 2, not passed. Pi, minimum mode, total
idle CPU cost and full activation allocation accounting remain unqualified.

The existing present model's slot/recycle/share actions are unchanged; this
addition gates trusted input in userspace and its normal rendering schedule is
covered by the physical parked-response probe. No new TLA+ model is claimed;
the new-spec suspension applies, with executable production-core counterexamples
for the new handshake. The original present lifecycle is not weakened.
