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
