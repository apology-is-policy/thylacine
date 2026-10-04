# Async service self-review

Single-agent review, as directed by the operator. This is not an independent audit.

## AS-0: reserved encodings and lifecycle model

Scripture4722f34e8 and numeric reservationd2362ec11 precede the consumers.
The kernel, native C and Rust records pin every field offset and total size.
`tools/check-loom-service-abi.py` compiles actual mirrors and compares their
encoded bytes with independently constructed vectors: 22 constants, five
records, signed fd/error fields and nontrivial 64-bit incarnations. The C
layouts also compile for ARM64. Mutating kernel opcode, native C scope kind
or Rust setup flag fails the expected mirror comparison in all three cases.
The full consuming kernel header compiles for ARM64; static assertions confirm
PRIVATE_SERVICE remains outside LOOM_SETUP_VALID and LOOM_OP_COUNT stays20.
There is no runtime dispatch or authorization change in this checkpoint.

`specs/check-loom-service.py` explores5,828 states, depth28. Seven mutants
violate their named properties: premature free, success committed after abort,
double terminal, stale incarnation, CQ overflow, credit refund while a peer
retains its endpoint, and retirement waiting for peer progress. Peer progress
has no fairness premise. Local execution and CQ draining are weakly fair;
closing the ring discards delivery obligations only after local borrows end.

Completion commit and CQ delivery are separate. A success committed before
abort can be delivered later without reviving the scope. The design now says
this explicitly; it does not permit deciding new success after the abort latch.

Evidence: work/oct4-async-service/{abi.log,as0-verification.json,model-gate/}.
The first full-header fixture missed arch/arm64 on its include path; the saved
failure is a compile-command defect, corrected before its successful check.
The earlier model precedence and Vault mirror-count fixture errors are retained
in abi-reservation-fixture-note.md. None counts as an intended mutant failure.

Limits: the abstract model does not prove actual C locks, byte framing, DAC,
usercopy, Proc/AddrSpace ownership or runtime retirement. Those are AS-1/2 gates.
This checkpoint adds declarations and verification only: no new boot, SMP,
sanitizer, graphical, Pi or minimum-display qualification is claimed.


## AS-1: resumable framing and native-root handshake

The extension lives in the existing transport module and reuses p9_session's
builders/dispatch. A separate try-vtable makes nonblocking semantics explicit;
legacy send/recv refuse PROGRESS transports. Legacy close cannot bypass the
abort latch. Each step makes at most one callback and copies at most one
negotiated msize. RX retains both cursor and declared frame length; TX retains
immutable kernel-owned frame storage until sent/aborted. No heap allocation,
namespace walk, peer wait, Tflush or Tclunk occurs in these helpers.

The enclosing client's lock must serialize progress and abort. These helpers
do not independently prove exclusivity, authority or reference ownership; AS-2
must provide those before userspace exposure. The SrvConn adapter reuses the
role-aware srvconn_io_nonblock and terminal srvconn_teardown. It neither takes
nor releases a reference. Actual owner retirement, rather than cancellation,
releases buffers and connection credits. A retained endpoint continues to hold
its object and charge. There is no alternate scheduler/executor here.

The native-root handshake copies the captured principal scalar; uname/aname
stay empty, matching devsrv's existing native attach. It sends version, receives
and validates version, sends attach, then publishes readiness only after the
matching full Rattach binds the root. Absolute deadline is checked before every
I/O step and never renewed by incoming bytes. Terminal reason and failed phase
are sticky; post-ready cancellation closes the session without peer cleanup.

AS-R1 reproduction accepted dialect unknown and msize0 into VERSIONED. The
shared dispatcher now rejects dialects other than 9P2000.L and framing-impossible
msize0..6 before state mutation. Host and guest regressions qualify this fix;
the private attach builder additionally obeys negotiated msize, refusing a
version result too small for even its actual Tattach frame.

Validation: actual C transport/wire/session fixture exercises each partial-byte
boundary, cancellation and deadline at every handshake boundary, malformed and
coalesced replies, EOF, duplicate abort, immutable terminal reason, captured
principal and hostile errno bounds. Twelve source mutants fail their expected
assertions. The complete fixture passes ASan+UBSan with Homebrew Clang22.1.4;
a deliberate heap-buffer-overflow proves that ASan is active. ARM64 syntax gate
compiles the transport and actual SrvConn adapter. Fresh CI CPU1 boot1830/1830
passes, including expanded srvconn.nonblocking_backpressure and
9p_session.version_handshake. Existing9p_client model197states plus five buggy
configs gives the expected aggregate Invariants counterexamples.

AS-R2: Apple's Clang17 sanitizer runtime hangs before main even for a minimal
program. Both sampled stacks show recursive ASan initialization through dyld's
cache callback and malloc. Per-command LLVM22 plus the xcrun SDK path works;
no global compiler/SDK configuration changed. The first receive-offset mutant
hit the stronger exact-byte assertion before the originally expected label;
that evidence is preserved and the gate now checks the actual intended failure.

Limits: host fixtures do not prove kernel scheduling/locks. The native test runs
real channels but does not expose the future private Loom table, ABI handlers,
CQ obligations, ownership guards or asynchronous retirement queue. No completed
AS-2/3/4, clipboard activation, fresh graphical/Pi/min-display or full SMP gate
claim belongs to this implementation checkpoint. Matrix qualification follows.

## AS-R3: completed handshake followed by coalesced clean close

The single role macro now derives completion from rustls's authenticated
protocol state after successful processing, before interpreting the edge
notification. Error returns bypass the latch; peer closure alone never sets it.
This avoids both losing a valid completed handshake and treating an early
close as authentication. The transport still flushes staged records before
returning success. The early-close check precedes any next blocking fill.
Both roles share the correction; no verifier, certificate policy or netd close
semantics change.

The actual driver's deterministic fixture supplies Finished+close in one read,
then the same stream bytewise, and tests a plaintext close before ClientHello.
The coalesced arm failed Io before the repair and passes after it. Existing
untrusted-certificate rejection remains green. CPU1 live stress reproduced
server317/client318 EOFs before the repair and completes1000 after it. The
benchmark's retained diagnostics report role, iteration, TLS error and I/O
outcome without recording records, keys or certificate material.

Single-agent self-review, not an independent audit. These results establish
the regression and narrow runtime correction; full matrix is still pending.
General TLS blocking-I/O deadlines remain the existing dossier seam.

## AS-1 / AS-R3 broad qualification

The fresh AS-1/TLS matrix on8542dbb4b completed50/50 clean boots: ten each
at default CPU1/4/8 and kernel-UBSan CPU4/8. Every row records zero corruption,
external-kill, inject-miss, timing and other classifications. The wrapper exited0,
restored all four protected drafts, released Mac, and the separate log/pin
check verified every individual boot, five summaries, original source hashes
and empty index. Evidence: work/oct4-async-service/as-r3/matrix-fixed/verified.json
and smp.log. This qualifies the transport helpers and TLS correction, not private
Loom runtime, clipboard activation or a fresh graphical/Pi/min-display run.
Single-agent self-review remains the operator-approved staffing. The next AS-2a
draft is separate and has no guest qualification from this matrix.

## AS-2a: exact owner lifetime for asynchronous retirement

Implementation of approved ASYNC-SERVICE-LIFECYCLE sections 6 and 8. This is an
internal prerequisite; it neither enables private Loom nor replaces the current
shared-map budget. It is not the later MM account hierarchy.

An AddrSpace has process/constructor owners and kernel descriptor pins. `ref`
counts both; `owners` counts only the first. Allocation starts both at one.
Owner acquisition first takes a total reference, then increments owners; it
requires an existing owner and cannot resurrect an ownerless descriptor.
Kernel pin acquisition requires an already live owner or pin. Neither accessor
is a synchronization primitive or a way to acquire an unpinned pointer.

Owner release decrements owners. Only its transition from one to zero drains
VMAs, on the ordinary exit/exec path, retaining that owner's total reference
through the entire drain. FILE Burrow destruction can call a sleeping clunk;
that work must not move to the private service retirement worker. Only after
the drain finishes does the owner release its total reference. Kernel unpin
releases only a total reference. The single final total-reference transition
frees page tables and the descriptor, asserting there are no owners or VMAs.
No pair of independent zero tests decides destruction.

A kernel pin does not authorize VA translation or retain mappings. Existing
no-CPU-under-old-TTBR0 requirements still belong to final owner release. Loom's
registered writable contiguous ANON buffers have independent Burrow references
and stable direct-map addresses; those references, not the address-space VMAs,
keep pending I/O storage valid. The later private implementation must retain
both kinds of reference and retire them in that order.

The three Proc ownership predicates (device quiescence, authority-image join,
image-flag stamping) and the spawn budget's raw-ref predicate all use owners.
A kernel cleanup pin must neither block sole-image elevation nor hide the last
driver from MMIO quiescence. A genuine second Proc must retain both effects.
The appended counter changes AddrSpace's asserted internal size72 to80 without
moving existing members. No userspace record changes.

Burrow charge claim/restore gain exact-AddrSpace forms. Existing Proc wrappers
delegate to them; their behavior and lock order stay the same. The caller owns
the exact descriptor and Burrow reference. Claim precedes Burrow release;
refund follows actual storage destruction; a nonfinal release restores the
claim. No dead Proc pointer or successor image is read. The existing conservative
claim/restore window and the legacy detach's shared-out policy are unchanged.
This does not claim a new globally exact memory ledger: the approved MM arc
will replace those semantics. An ownerless descriptor cannot admit new user
allocations while its pending charges settle.

Primary precedent: Linux separates mmgrab descriptor lifetime from mmget mapping
lifetime ([lifetime helpers](https://github.com/torvalds/linux/blob/master/include/linux/sched/mm.h)). Thylacine retains its own Proc, AddrSpace,
Burrow, namespace and existing accounting rules.

Verification:
actual lifecycle C slice + actual struct declaration, allocator/drain doubles,
serial two-owner/pin cases, final-owner drain paused while a pin releases, and
100 concurrent final-drop schedules. Clean ASan/UBSan and seven intended named
mutations pass. Six complete C translation units compile for ARM64. Existing COW/capacity/Burrow models pass five clean configurations and twelve intended counterexamples. Fresh CI CPU1 boot passes the guest tests: actual process table elevation/sharing; actual last-Proc
VMA-only virtio quiescence; exact-payer claim/refund after Proc death.

Self-review findings corrected before application: total pins cannot retain all
VMAs because their last release could then wait on a filesystem peer; raw-ref
spawn budget consumer was found by a full census; test fixture externs were
missing; device result assertions now follow cleanup. The additional mutation
reached an earlier protection than its first expected label, which was corrected
with original evidence retained. No independent review is claimed (single-agent).

Focused evidence: /Users/northkillpd/projects/thylacine-astra/work/oct4-async-service/as2a/check-1791130311682969000. No AS-2a broad matrix, graphical or Pi claim;
the preceding 50/50 matrix qualified AS-1/TLS before these changes. Private
setup remains rejected. The ownership engine and userspace clients are next.

## AS-2b: exact admission identity and native target publication

Ordinary open and private preparation now share one native registry admission
core. The target retains the navigation registry view, exact fixed service slot,
name and per-slot generation. Both slot and generation matter: recycling can
move a name into a different slot whose independent generation counter matches.
There is no global fallback and a 33..255-byte valid name cannot truncate to a
native 32-byte name. Routes retain the source through the view and charge the
view's SrvDomain; capability-posted providers cannot satisfy resident routes.
Strict private capture/preparation refuses byte, cape and remote-marked posts.
The legacy byte capability/self-post gate still runs before allocation.

Preparation allocates/charges the SrvConn and holds a registry reference without
publishing anything. Publication takes only the retained service's registry
lock, checks LIVE and exact generation again, and transfers a separate reference
to the backlog. It is single-owner, single-publication state; the future scope
owner must hold its abort/admission latch across this call. Publication never
allocates, waits, exchanges bytes or wakes callbacks. Separate wake runs after
caller locks are released; the admission reference keeps the wait-list storage
alive even if the poster exits. Release tears down an unpublished connection
locally; a published caller takes its own transport ref before dropping the
admission. Server-held references continue to retain storage/credits. This is
not proof of the not-yet-implemented scope cancellation linearization.

ProcAccessIdentity captures principal, primary/supplementary groups and atomic
caps by value. Ordinary permission helpers delegate to the same owner-first DAC
logic; no fake Proc and no worker authority. Private admission can match creator
stripes AND its pinned exact AddrSpace under the process-table lock, the same
lock as exec's image swap. ALIVE is required, only values escape, failure clears
output. Capture anew per operation; a ring is not a permanent grant cache.
Connection principal remains the CONNECT identity, and existing live peer checks
remain unchanged. Capturing successfully is not itself a continued liveness
proof: the scope's exit/exec/abort latch still has to serialize publication.

Actual-source host fixture passes ASan/UBSan and ten intended named mutations:
wrong image, dead creator, missing target generation, cross-slot ABA, replacement
at publication, raw endpoint escape, provider rather than consumer charging,
lost supplementary groups, owner fallthrough, and omitted cancellation teardown.
The fixture uses registry/connection/table doubles; native tests separately cover
real process-table values, exec-image discrimination, dead Proc refusal, actual
SrvConn references, prepare invisibility, duplicate publish, actual poster death
and rebind, explicit recapture, unsupported modes and long-name refusal. Fresh
CI CPU1 boot passes all1830 registered tests. Six complete C translation units
compile for ARM64. The three required Corvus kernel-connection mutants produce their expected
aggregate Invariants counterexamples. They are not a model of this new C engine.

AS-R4 was a new test error: closing a listener does not unpost a live service.
The corrected fixture asserts that invariant, then kills the fixture Proc through
its real cleanup and reposts from a new marked Proc. Original failure evidence
is preserved. The first model command used a relative configuration path that
TLC resolved from the module directory; it failed before model exploration.
The corrected invocation uses absolute paths without repeating the passed boot.
I also mistakenly started the clean Corvus configuration before reading its
dossier: that full run is suspended by earlier operator direction. Stopped only
our exact Java process, retained its incomplete log (no PASS), and removed its
temporary states. Ran the three required connection mutants; no model inputs
or bounds changed. See work/oct4-async-service/as2b/model-command-note.md.

This remains an internal prerequisite. No private setup/REGISTER/SQE activation,
new asynchronous cleanup worker, shared-AS guard, COW omission, exec abort or
clipboard activation is claimed. Broad integrated qualification follows those
consumers. All four protected drafts remain separate; review is single-agent.

## AS-2c: sharing exclusion and selective fork omission

Begin requires an existing owner and exactly one owner under the same AS lock
that try_ref uses to admit sharing. Therefore simultaneous setup and sharing
cannot both succeed. A last-owner drain may coexist with guard retention, but
cannot destroy the descriptor because each guard owns a total lifetime pin.
End drops that pin outside the AS lock. Its caller must wait for actual local
retirement, not just descriptor removal; wiring that caller is still owed.
proc_alloc_in's refusal leaves as NULL and uses normal unpublished rollback.
No policy path calls the checked void ref to convert a normal refusal into panic.

COW classification excludes the kernel-only private ring flag before every
mapping-kind arm; protection/splitting preserves the flag. This prevents both
writable and permanently read-only aliases in the child. Ordinary buffers are
not tagged, so existing lazy COW and eager writable refusal remain intact.
The tag consumes an unused state bit; size and user permission ceilings do not
change. There is still no userspace path to create a private ring.

The actual-source sanitizer fixture exercises serial and concurrent reference
schedules, including200 competing setup/share attempts and100 final-drop races;
eleven mutations fail. Native tests use real proc allocation/rollback and actual
protect splitting, clone, Burrow mapping counts and cleanup. Two source mutants
fail the inherited-ring and missing-buffer assertions; clean CPU1 boot1830/1830
passes. The existing COW model's three clean/seven buggy configurations give
their expected named results; it does not model private rings. Evidence is in
work/oct4-async-service/as2c. AS-R5 exposed nested TEST_ASSERT's local return:
the helper returned into a continuing test, allowing a second failure to replace
the first label. Explicit error propagation corrects this in both new fixtures.
The failed run is preserved, and both native mutants were rerun after correction.

Single-agent review, not independent audit. No new broad matrix or graphical
qualification is claimed; whole-system qualification is owed with the private
consumer. Four authority/settings drafts remain exact and unstaged.

## AS-2d: bounded private request driver

A caller binds only a fresh unpublished client, retaining its external progress
storage until destruction. Client.lock serializes cursor, immutable queued
out_buf, tag registration, reply parsing and abort. Submission may build one
frame but performs no transport I/O; while a partial TX owns out_buf, another
submission completes EAGAIN before building. Every progress visit does one
transport callback at most. Alternating TX and RX means backpressure cannot
strand a reply needed to drain the server. Scope deadlines after attachment
remain the enclosing private owner's responsibility; the handshake checks its
own absolute deadline, with zero meaning none as already approved.

The shared session builders and demux remain the sole protocol implementation.
An exclusive client has no ownerless flush/clunk flows: an unknown tag or a
reply for a not-yet-sent request is terminal. The dispatcher still validates
reply type and length. Terminal paths detach sending/transport borrows before
callbacks, which may immediately free their RPC. A completed success is removed
before callback, so later abort cannot complete it twice. Private abandon retains
its existing no-callback promise for that one RPC, then aborts the entire stream;
it never waits for peer acknowledgement. Normal private owners use explicit
abort so all accepted requests receive completion. Whole-ring detachment is the
only consumer allowed to discard completion delivery obligations.

Legacy shared clients have progressNULL and retain their existing paths. Every
blocking builder, elected reader pump and synchronous close refuses a bound
private client before modifying out_buf. No Spoor export or user setup is enabled.
Destroy requires exclusive ownership as before and completes local teardown
before freeing client storage. It does not release the enclosing scope or server
endpoint, whose lifetime/accounting remains with the AS-2 owner to be integrated.

Tests: full real client declarations and extracted actual functions, with real
wire/session/transport source, pass ASan/UBSan; eight deliberate mutations expose
TX overwrite, RX starvation, premature reply, missing sending guard, lost/double
terminal, retained TX borrow and blocking entry. The same fixture runs against
actual kernel locks/allocator in CPU1 boot1830/1830. It covers every request TX
and reply RX byte cancellation boundary and callbacks freeing their own storage.
Framing12mutants and zero-deadline control also pass. Existing9p_client197states
and five expected aggregate counterexamples pass. ARM64 compiles three full
units. These checks do not establish private Loom scheduling or usercopy safety.

Initial host fixture declared wakeup with the wrong return type: corrected to
the actual int declaration. A new zero-deadline test initially ran before old
framing tests, changing a mutant's first failure message; moving it after the
established tests retained both controls without weakening any oracle. Failed
and passing logs are retained in work/oct4-async-service/as2d. AS-R6's helper
contract mismatch is repaired; no prior userspace private exposure existed.
Single-agent review; broad integrated qualification and consumers remain owed.

## AS-2 prerequisite matrix and provided-buffer contract

The full50-boot matrix on7571ad4e4 is verified across all five rows and failure
categories; draft/source/index checks and resource cleanup passed. It does not
exercise an enabled private handler. Evidence: work/oct4-async-service/as2-matrix.

AS-R7 review follows actual payload rejection/scalar rearm and Rust CQ release.
Operator-selected C separates payload return from CQ-head acknowledgement via
full64-bit pool/lease identities and per-slot receipts. Reviewed stale returns,
slot reuse, prior success delayed across abort, EOF without lease, empty-pool
progress, provisional overlap exclusion, full-CQ terminal ordering, exact-payer
retention and close/exec. No new code or runtime result is claimed. AS-R8 records
the raw Rust registration safety correction required before client qualification.
This is single-agent self-review, not independent audit.

## Provided-buffer ABI mirror checkpoint

All three actual mirrors agree with manually packed independent vectors for
30 constants/10 records. New fields include full-width pool incarnation and lease
identities; companion receipt does not overload user_data. Static alignment/offset
checks and full kernel header compilation preserve SQE64/CQE16/Params88, distinct
MORE/F_NOTIF/receipt flags, distinct selection flag and disabled private setup.
Three independent constant mutants fail by their expected mirror labels.
CFLAGS affects only host compilation; freestanding ARM64 checks remain separate.

This proves declaration/layout consistency, not decoder validation, copyout
rollback, payload ownership, cancellation, or native runtime behavior. The model
and consuming implementation must supply those proofs. First SDK invocation
failure and corrected logs remain in work/oct4-async-service/buffer-pools.

## Explicit payload model (October 4)

The focused loom_service_buffers model passes464 states (one member/two leases)
and6416 (two members/three leases), both with two pool generations. Eleven
intentional mutations fail their named safety/temporal properties. Local
retirement needs no peer, CQ delivery/ack or payload return fairness. Single
stream; physical aliasing, real bytes, locks and multi-stream fairness remain
actual-source/runtime requirements. Evidence: work/oct4-async-service/buffer-pools/model-passed.json.
No private activation, new native boot or graphical qualification in this step.

Self-review only: verified that the older service model's fair CQ draining is
not imported into the new retirement proof. Successful pending payloads survive
Retire; pool re-creation requires all leases gone. Model enumerates a single
stream and abstracts bytes as unique nonces; implementation gates must not use
this result as proof of physical lifetime or cross-stream scheduling.

## AS-2e: bounded payload pool core (October 4)

The dormant kernel pool module implements transactional provisional registration,
combined64-member admission, canonical extent exclusion, BUSY/PENDING/LEASED
transitions, full-incarnation/nonce returns and retained results independent of
source refs. No CQ-head hook, peer I/O, allocations, callbacks or private setup
activation. Caller lock, exact backing pins/charges, fixed-I/O exclusion, shared
CQ publication and owner cancellation remain explicit integration obligations.

Actual-source host ASan/UBSan and eleven intended semantic mutations pass. The
shared native fixture runs inside loom.register_buffers; fresh CI image boots
1830/1830. Metadata ledger: bank5128, descriptor32, cell80, copied result48bytes;
these are sizes, not allocation-charge measurements. Relevant broad SMP/UBSan
qualification remains owed for the new core; earlier50/50 was the AS-2a-d base.
No graphical/Pi/runtime-client result is claimed.

First fixture compile failed on POSIX errno names: corrected to canonical T_E_*.
Shared-fixture extraction then produced two wrong member names: compiler caught
both; fixed before execution. First native wrapper treated quaestor's expected
unowned-path return1 as a fatal error, before any build; corrected and adopted
new surfaces into a pool dossier and existing Loom tests into the Loom dossier.
Logs and diagnoses remain in work/oct4-async-service/buffer-pools.

Host evidence: /Users/northkillpd/projects/thylacine-astra/work/oct4-async-service/buffer-pools/core-1791139023201923000
Native evidence: /Users/northkillpd/projects/thylacine-astra/work/oct4-async-service/buffer-pools/native-1791139106929169000

Self-review, not independent audit: all prepare validation precedes mutation;
provisional quota/exclusion is immediately visible; no pin/callback occurs here;
exact returns inspect private cells; byte-count maximum is signed-CQE bounded;
nonce exhaustion precedes empty-pool wait; snapshot clears ABI reserved fields;
stream release cannot clear pending/leased cells. Rotation is only among pool
members: inter-stream fairness is a required owner policy still to implement.
Code carries no completion publication/final ordering claim beyond its stored
result. Native helper returns errors to outer assertion. Reap has no teardown
shortcut that would falsely acknowledge consumer leases.

## AS-R8 raw boundary and caller audit (October 4)

The raw Rust registration boundary is explicitly unsafe. Caller review found
three concrete reference hazards: whole EventRing staging borrowed while other
surface writes are pending; Ordered taking RX on a WRITE CQE with READ pending;
and Weft getters accepting borrows after an unresolved enter/reap error. Fixes
use direct completed ranges, exact read tags and inflight refusal respectively.
Admission/seat also filter stale/write CQEs before borrowing. Symlink-probe now
ends on unresolved wait errors rather than reusing storage in an expected-error
leg. Full caller reasoning is in work/oct4-async-service/asr8/caller-audit.md.

Compile-fail checks use the actual Loom module, not a signature imitation.
Weft controlled failures use actual extracted getters and wait; the extractor's
first unscoped selection failed compilation and was corrected to impl WeftFlow.
No guest bug claim is inferred from that fixture setup failure. Four Tapestry
mutants violate the exact named borrow tests. Review remains single-agent;
concurrent native transport, graphics and broad gates are recorded separately.
Safe provided-pool storage/leases remain to implement; this boundary repair does
not make the existing raw Ring API automatically own all future submissions.

## AS-2f: creator worker accounting through retirement (October 4)

Private worker tickets contain permanent creator stripes, not a retained Proc
pointer. Admission resolves a live, nonterminating creator and its exact pinned
image under the process-table lock, checks the shared thread budget and fills
a previously empty ticket. Refund consumes the same ticket under that lock and
resolves by stripes alone: exec preserves the counter, a reaped creator has no
remaining counter, and a replacement process never receives the refund. Exempt
creators still count workers and cannot overflow the signed counter.

Actual-source host checks under ASan/UBSan pass, including100 two-thread
admission/release schedules and eight intended mutation failures. Native
proc.stripes_smoke extends its real table fixture with cap/refund/image-identity
and actual descriptor destruction; fresh CPU1 boot1830/1830 passes. The image
change is a controlled fixture, not private-ring exec qualification. Its final
version unlinks synthetic Procs before mutating image/count fields.

This internal helper is for the pending private owner; legacy Loom behavior and
public feature masks are unchanged. Private close/exec/reaper, metadata charging,
protocol/receipt integration and safe owned clients remain. Broad matrix debt
now covers AS-2e, AS-R8 and this helper; earlier50/50 covers AS-2a-d only.
Evidence: work/oct4-async-service/owner/{ticket-host-final,native-passed.json}.

Single-agent self-review: no saved process address, no process lookup outside
the lifecycle lock, no allocator/callback/wait under that lock; duplicate release
of one retained ticket serializes, and occupied admission cannot overwrite it.
The ticket is kernel-owned and noncopyable by contract; callers must not reuse
its storage while an operation still references it, and must retain the input AS
descriptor across admission. The unchanged legacy charge API cannot be used by
the future detached private owner. No Loom lock may enclose ticket operations.
The first identity mutation failed an earlier state assertion; the revised
missing-creator case identifies that failure directly. Original logs retained.

## AS-2g: preallocated protocol storage (October 4)

p9_client_init_preallocated accepts the owning scope's charged RX/TX storage
without hidden bulk-TX allocation. It rejects null/short storage before client
publication. The explicit out_buf_owned flag makes destroy free only the legacy
heap-owned bulk buffer; external storage survives abort/destroy and remains the
owner's responsibility. Reinitialization resets ownership. The original init
retains its inline/default and heap-or-inline bulk behavior.

The shared native/host private-client fixture now uses provided buffers for
real framing/request progress and checks bulk boundaries, ownership and reinit.
Actual-source ASan/UBSan passes with13 intended mutants (five storage, eight
progress); allocator instrumentation catches foreign free and leaked storage.
The original wrong-owner mutant triggered ASan bad-free during fixture cleanup
before the assertion could print; a bounded host allocation tracker now reports
that same violation directly. Existing9p_client model197states and five intended
aggregate Invariants counterexamples pass; native CPU1 boot1830/1830 passes.

This supplies storage ownership, not its charge transaction: the private ring
owner must reserve and retain the enclosing allocations. Caller storage must be
disjoint and exclusive until destroy completes. No private ABI or clipboard
activation; full owner/retirement integration and combined broad matrix remain.
Evidence: work/oct4-async-service/owner-storage/{host-final,checked.json}.

Single-agent self-review: the shared initializer validates provided capacities
before creating session/transport state; the existing init passes provided=false.
No allocator is called by the provided initialization branch or its downstream
initializers. Session construction failure and transport failure do not adopt
external storage. Destroy aborts any bound progress before clearing the client
and never frees an external TX. Read/write/handshake protocol and lock order are
unchanged; the source-mapped model does not prove allocation or alias ownership.

## AS-2h: paired receipt publication self-review (October 4)

Single-agent review of construction, publication and failure paths: old layout
calls the same factored constructor with receipts=false; bounded power-of-two
inputs keep all arithmetic within u32. The internal true constructor supplies
geometry only and is not exposed through setup. Kernel-private offsets/tail
select the write; hostile shared head can stall or lose the caller's observations
but cannot widen the allocation. Receipt, CQE and LEASED transition occur under
one Loom lock before the release tail. No allocator, callback or peer wait runs
inside that lock. Poll wake follows unlock while the caller retains the ring.
Full CQ changes no pool state and returns EAGAIN. An impossible failed deliver
leaves tail unchanged. Nonpool publication clears stale receipts and refuses the
buffer flag. MORE/final ordering remains a request-owner obligation.

Host publication observation is deterministic same-thread instrumentation, not
concurrent ARM execution. Native test uses real constructors, pool and cleanup;
its static pool is fixture-owned, not a demonstration of syscall registration.
The first draft host compile redundantly defined inline rendez_init; the stub
was removed before the passing actual-source run. Eight named mutations,
ASan/UBSan, both clean pool models and11 named counterexamples pass; native
1830/1830 passes. Foundation50/50 at26c21df87 predates this change. No independent
audit or completed private-mode qualification is claimed.
