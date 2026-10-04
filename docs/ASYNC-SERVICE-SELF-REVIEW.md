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
