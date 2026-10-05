# Async service implementation

Approved scripture4722f34e8. Single-agent on Astra; protected authority/settings
drafts preserved. ASYNC-SERVICE-LIFECYCLE.md owns the contract; memory-accounting
implementation follows this facility as approved.

| Stage | State | Evidence / remaining work |
| --- | --- | --- |
| AS-0 contract | Approved | Scripture4722f34e8; concrete numeric/layout reservation in ASYNC-SERVICE-ABI.md. |
| AS-0 ABI/model | Verified | Three compiled mirrors, 22 constants, five records and three intended source-mirror mismatches; model 5,828 states and seven named counterexamples. Private setup remains rejected. |
| AS-1 progress | Qualified | Resumable framing/native-root handshake and real SrvConn adapter; byte-boundary host fixture, 12 mutants, ASan/UBSan, CPU1 boot1830/1830. 50/50 clean default/SMP/kernel-UBSan boots after TLS repair; AS-2 must bind helpers to private ownership. |
| AS-2 scopes | In progress | Descriptor lifetime, native admission and sharing/COW guards verified; Request progress is verified; private table, provided pools, retirement, deadlines and completion obligations remain. |
| AS-3 clients | Pending | C/Rust APIs plus native stalled/malformed-peer and repeated-reconnect fixtures. |
| AS-4 adoption | Pending | Clipboard/MODE clients, SAK cancellation at every phase, responsive graphics and logout. |

No private scope, new account or completed clipboard claim follows from an ABI
reservation. The earlier hidden-storage implementation8b2212c0e has its own
50/50 boot qualification; those results do not qualify this new facility.

Evidence directory: work/oct4-async-service. Main/Aux informed through existing
Yip calls0116/0108. No Main landing; existing limits/default-off clipboard remain.

## AS-R1: version negotiation validation (October 4)

During AS-1 inspection, the shared session dispatcher accepts any syntactically
valid Rversion dialect and a zero negotiated msize. This lets VERSIONED claim
9P2000.L readiness without agreement on that protocol. Reproduce against the
actual session source, then reject unsupported dialects and framing-impossible
sizes before changing session state. The AS-1 handshake and legacy callers must
share the corrected validation. Owned by Astra; blocks this transport checkpoint.

## AS-R2: host sanitizer timeout (October 4)

The first combined ASan/UBSan framing fixture exceeded its ten-second timeout
before returning a verdict. Unsanitized checks pass; this is not evidence of a
sanitizer pass. Preserve the failed run and compare an instrumented fixture with
a minimal sanitizer executable, sampling any live owned process before deciding
the cause. Owned by Astra; blocks broad verification until diagnosed.

AS-R1 correction is in the shared dispatcher, with native and actual-source
regressions; unsupported dialects and msize0..6 are refused before VERSIONED.
Fresh guest verification is pending.

AS-R2 diagnosed: both the minimal executable and an instrumented full fixture
stall before main in Apple Clang17 ASan initialization. Samples show recursive
AsanInitInternal through get_dyld_hdr -> dyld_shared_cache_iterate_text_swift ->
Block_copy -> malloc -> AsanInitFromRtl -> StaticSpinMutex::LockSlow. No guest
code runs in that failure. The installed Homebrew Clang22.1.4 passes the minimal
control, detects an intentional heap-buffer-overflow, and passes the actual
framing/handshake fixture with ASan+UBSan. Its stale configured SDK path required
an explicit per-invocation -isysroot from xcrun; no global host configuration was
changed. Evidence: work/oct4-async-service/sanitizer-diagnosis and
framing-sanitizers. Use that scoped compiler for host sanitizer checks here.


AS-1 checkpoint: fresh CI build and CPU1 boot1830/1830 pass, including the real
SrvConn byte-at-a-time version/attach, busy-reader refusal, captured principal,
post-ready abort and retained endpoint-reference tests. AS-R1 guest regression
passes. Existing9p_client model197states and all five buggy configs pass their
expected verdicts (aggregate Invariants, not separately named subproperties).
Full SMP/kernel-UBSan matrix is next; no new graphical/Pi/min-display or live
clipboard claim. The helpers have no userspace activation yet. Build, guest log
and matched boot artifacts: work/oct4-async-service/build-1791126222276802000.

## AS-R3: TLS boot-probe handshake failure (October 4)

The AS-1 broad matrix stopped after default-smp1 boot3 failed tlsperf M4
with "tls handshake"; Joey exited1 and the kernel reported that failure.
Boots1/2 passed; all1830 kernel tests in boot3 passed before the userspace probe.
The diagnostic currently discards the underlying TLS error and iteration.
Preserved serial/harness logs are in work/oct4-async-service/as-r3. No50-boot
qualification is claimed. Diagnose this failure before continuing AS-2 or
restarting the gate. The stopped wrapper restored all protected drafts and
released Mac.

AS-R3 repaired: diagnostic stress on CPU1 recorded server iteration317 EOF/Io,
then client318 EOF/Io after the server exited. A deterministic real-driver
fixture combining client Finished and close_notify reproduced Io at every run.
The wrapper had latched establishment only on WriteTraffic; rustls can report
PeerClosed first when these records share a read. It now latches completion
from rustls::CommonState::is_handshaking after each successful processing step.
A close before completion fails without another peer read; a close after
completion preserves successful authentication and returns clean EOF.

Fresh CI CPU1 boot1830/1830, the coalesced/bytewise/early-close controls, the
untrusted-certificate control and1000 live TLS handshakes pass. Evidence:
work/oct4-async-service/as-r3/probe-1791127579804936000; pre-fix deterministic
failure: probe-1791127476255271000; live failure: probe-1791127314181498000.
The old matrix also completed boot4 before termination; its three passes and
one failure are not a qualification. Full default/SMP/UBSan restart is pending.

## AS-1 broad qualification (October 4)

The fresh AS-1/TLS matrix on8542dbb4b completed50/50 clean boots: ten each
at default CPU1/4/8 and kernel-UBSan CPU4/8. Every row records zero corruption,
external-kill, inject-miss, timing and other classifications. The wrapper exited0,
restored all four protected drafts, released Mac, and the separate log/pin
check verified every individual boot, five summaries, original source hashes
and empty index. Evidence: work/oct4-async-service/as-r3/matrix-fixed/verified.json
and smp.log. This qualifies the transport helpers and TLS correction, not private
Loom runtime, clipboard activation or a fresh graphical/Pi/min-display run.

## AS-2a: descriptor lifetime prerequisite (October 4)

AddrSpace now distinguishes process owners from kernel descriptor pins. The last
owner drains mappings; the final total reference frees descriptor/page tables.
Device and authority sharing predicates count owners, and delayed Burrow cleanup
can settle the exact original payer without a dead Proc pointer. Existing memory
limits and private-setup refusal remain unchanged. This is not the MM hierarchy.

Actual-source lifecycle ASan/UBSan plus seven mutants, ARM64 compilation, five
clean existing model configurations/twelve intended counterexamples and fresh
CI CPU1 boot pass. Guest tests cover true sharing versus kernel pins, last-owner
VMA-only device quiescence and exact-payer refund after Proc death. Evidence:
/Users/northkillpd/projects/thylacine-astra/work/oct4-async-service/as2a/check-1791130311682969000. This checkpoint has no new broad/graphical/Pi qualification.
AS-2 scope admission, sharing/exec guards and retirement worker are still pending.

## AS-R4: AS-2b native admission gate (October 4)

The first fresh AS-2b CPU1 gate reports 1829/1830 and kernel-test extinction.
Owned investigation blocks this admission checkpoint; do not rerun until its
concrete assertion is understood. Build and serial evidence are preserved in
work/oct4-async-service/as2b/check-1791131840841607000. All four protected
drafts were restored and Mac released by the runner. Host admission sanitizer
and ten intended mutants passed; they do not override a failing native gate.

AS-R4 diagnosis: the new test closed the listener then tried to repost while
the original poster was alive. The existing registry contract deliberately
keeps that service LIVE on descriptor close; poster death (or scoped authority
teardown) tombstones it. The listener-retention test in `kernel/test/test_devsrv.c` already
asserts this rule. Corrected the new fixture to assert that close preserves LIVE,
then run real Proc death and repost from a fresh marked Proc. Production source
is unchanged by this correction. The corrected native gate is pending.

## AS-2b: native admission prerequisite (October 4)

Immutable actual-creator access snapshots and exact-AddrSpace lookup now share
ordinary DAC semantics. Native targets retain the actual registry, exact service
slot and generation. Common preparation/publication serves both legacy opens and
future private scopes, preserving route/domain/byte authority and weighted
connection accounting. No peer I/O occurs before publication or in the new
private preparation API. The future scope abort latch must serialize publish.

Host ASan/UBSan and ten deliberate source mutants pass. Six ARM64 translation
units compile. Corrected fresh CI CPU1 boot passes1830/1830; actual Proc death,
repost, server reference retention and no-peer preparation are exercised.
Evidence: work/oct4-async-service/as2b/host-1791131759653901000 and
work/oct4-async-service/as2b/check-1791131963071346000. AS-R4's incorrect listener
close assumption is corrected and its failing run preserved. The model runner's
relative config lookup failed before exploration; the three required connection
mutants then passed with absolute paths. The mistakenly launched suspended clean
Corvus run was stopped; its partial log is not success. See model-command-note.md.
No new broad/SMP/graphical/Pi result; private runtime and clipboard remain off.

AS-R4 closed: corrected native gate passes1830/1830. AS-2b required model
counterexamples are observed for post_without_marker, identity_cached_on_fid
and dead_proc_stale (aggregate Invariants). The clean Corvus gate remains
suspended by its earlier operator decision. All four draft hashes and source
pins match after verification; matching boot artifacts are retained with hashes.

## AS-R5: nested native assertion attribution (October 4)

The AS-2c omit-ordinary source mutation triggered the intended buffer-clone
assertion, but the new void helper returned only to its caller. A later parent
assertion overwrote the harness's one failure message. The gate correctly
refused to accept the wrong final label. Both new nested fixtures now return
an error to their registered test, which stops on that first failure. No
production correction follows from this test-harness attribution defect.
Evidence: work/oct4-async-service/as2c/check-1791132814198835000. The corrected
native mutants and fresh clean gate are pending; the prior mutant build is
not a runnable clean image.

## AS-2c: private ring sharing and fork guards (October 4)

Private-ring guard admission and new process-owner acquisition serialize under
AddrSpace.lock. A guard holds a descriptor pin until local retirement; it does
not keep mappings or a dead process alive. proc_alloc_in refuses sharing and
rolls back normally while any guard exists. AddrSpace remains80bytes.
Kernel-only private ring VMA state survives protect/split and is omitted from
COW children, including read-only pieces. Ordinary registered buffers retain
their existing clone rules; their registration never stamps the private tag.

Actual-source lifecycle ASan/UBSan passes eleven intended mutations, 100 final
reference races and200 guard-versus-sharing races. Fresh CI CPU1 boot1830/1830
passes. Two real guest source mutations separately fail the named inherited-ring
and missing-ordinary-buffer assertions. Existing COW models pass three clean
configurations (580/10636/2996states) and seven intended counterexamples.
Evidence: work/oct4-async-service/as2c/check-1791133210590134000 and
host-1791132695331926000. AS-R5 is closed: helpers propagate the first error to
the registered test, preventing a later failure from replacing its message.
All source pins and four original draft hashes match; paired clean boot artifacts
are retained. No new full matrix, graphical/Pi qualification, private setup or
clipboard activation. Next: private request progress, close/exec/exit abort and
bounded local retirement, then clients and end-to-end adoption.

## AS-R6: zero-deadline handshake contract (October 4)

While connecting AS-1 to the private request engine, its handshake helper was
found to reject deadline0 although the approved ABI specifies no deadline for0.
Correct the helper and add a never-expiring handshake control; retain absolute
nonzero deadline tests. No private userspace activation has occurred. Owned by
Astra; blocks the request-progress checkpoint.

## AS-2d: private request progress (October 4)

The existing p9_client now accepts an exclusive progress cursor on a fresh
unpublished client. Handshake and requests reuse existing builders, tags and
reply dispatch. Submission reserves the outgoing frame until its partial send
finishes; another submit receives EAGAIN before touching it. Alternating TX/RX
visits use at most one backend callback and one complete reply. Abort detaches
TX/parser borrows and completes remaining requests without peer flush/clunk.
Private ownerless/premature replies fail closed; legacy blocking entry points
refuse these private clients before touching shared frame storage.

AS-R6 is closed: zero deadline now means none, including a handshake driven at
UINT64_MAX time; existing absolute nonzero deadlines retain their tests.
Actual-source client ASan/UBSan and eight named mutations pass, including
success/abort callbacks that immediately free RPC storage. Framing ASan/UBSan
and12mutations pass. The same private fixture passes in fresh native CPU1
boot1830/1830. Existing9p_client model197states and five expected aggregate
Invariants counterexamples pass. Evidence: work/oct4-async-service/as2d,
check-1791134120223514000, host-1791134082058710000 and
host-1791133991553906000. Source pins and all four protected hashes verified.
No private Loom table, close/exec owner hooks, retirement queue, completed
clipboard or fresh broad/graphical/Pi qualification is claimed. Those consumers
remain the next implementation; the 128MiB protection remains unchanged.

## AS-2 prerequisites: broad qualification (October 4)

Committed lifetime/admission/sharing/request-progress helpers at7571ad4e4 passed
50/50 clean boots: defaultCPU1/4/8 and kernel-UBSanCPU4/8, ten each. Every row has
zero corruption, external-kill, inject-miss, timing and other classifications.
The runner exited0, restored all four original drafts and released Mac; verify.py
checked all50 individual records, five rows, pinned source/HEAD/index and hashes.
Evidence: work/oct4-async-service/as2-matrix/{verified.json,smp.log}. This qualifies
the prerequisites, not private Loom scopes or future pool code. No new graphical,
Pi, minimum-display or completed clipboard claim.

## AS-R7: streaming payload ownership (operator selected C)

The private ABI reservation allowed MULTISHOT READ without a per-shot buffer
handoff. Legacy payload MULTISHOT is already rejected; no shipped private READ
corruption is claimed. Scalar rearm only checks CQ space, and Rust Ring::reap
releases its CQ slot before the caller processes separate payload bytes.
The operator chose explicit provided-buffer pools now. Contract/encodings are
ASYNC-SERVICE-BUFFERS.md; source evidence/review/qualification plan are under
work/oct4-async-service/buffer-pools. Design is complete; mirror/implementation
and actual runtime proof remain required before closing this activation gap.

## AS-R8: raw Rust registration safety boundary (owned AS-3 repair)

Ring::register_buffers currently accepts integer VA descriptors through a safe
Rust method without owning the backing or requiring an unsafe lifetime/exclusivity
obligation. Safe callers can submit kernel I/O while retaining ordinary mutable
buffer slices. Pins keep storage alive but cannot establish Rust aliasing rules.
No new guest crash is claimed; this is a verified source API defect. Repair the
raw boundary and all callers, then build pool APIs that take ownership and return
borrow-checked payload leases. It blocks safe-client qualification; tests include
compile-fail ownership cases and native byte/lifetime schedules.

## Provided-buffer ABI mirrors (October 4)

Under scripture30695b43e, kernel C, native C and Rust now declare30 constants
and10 records, including all five pool/receipt controls. All new sizes, field
offsets and eight-byte alignment are asserted. Independent serialized vectors,
ARM64 header compilation and the full kernel-envelope gate pass; three deliberate
kernel/C/Rust constant mutations fail the intended mirror comparison. Existing
64/16/88-byte records and valid-mask refusal remain pinned. The pool model and
runtime implementation remain next, with AS-R7 open until ownership is qualified.

Evidence: work/oct4-async-service/buffer-pools/abi-passed.json and its logs.
The first attempt stopped at the compiler's stale default MacOSX26 SDK path,
before source validation; the host gate now honors CFLAGS and was rerun using
the actual xcrun SDK. No machine-wide compiler setting changed. No sanitizer,
new boot or graphical result is claimed for these declarations.

## Explicit payload model (October 4)

The focused loom_service_buffers model passes464 states (one member/two leases)
and6416 (two members/three leases), both with two pool generations. Eleven
intentional mutations fail their named safety/temporal properties. Local
retirement needs no peer, CQ delivery/ack or payload return fairness. Single
stream; physical aliasing, real bytes, locks and multi-stream fairness remain
actual-source/runtime requirements. Evidence: work/oct4-async-service/buffer-pools/model-passed.json.
No private activation, new native boot or graphical qualification in this step.

AS-R8 caller-audit note: WeftFlow::wait can leave an operation unresolved after
enter/reap failure, while tx_buf/rx_buf currently expose slices without checking
inflight. The existing AS-R8 safety work must cover that error path as well as
marking raw registration unsafe; a safety comment alone will not discharge it.
No new runtime corruption observed; this is source-level API ownership evidence.

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

## AS-R8: raw Rust payload ownership corrected (October 4)

Raw register_buffers is unsafe with the full asynchronous alias/lifetime
contract. All ten native call sites now acknowledge and enforce it. Registered
storage exposes checked direct range views. EventRing borrows only a validated
completed slot, Ordered never borrows pending RX for a WRITE completion, and
admission/seat filter tag/phase/length before making any view. Weft getters return
WouldBlock while an operation remains unresolved, including enter/reap failure.
The symlink probe terminates on lost completion rather than reusing its payload.

Actual full Loom module: range runtime checks plus three intended compiler
rejections (unsafe call, alias and premature drop). Tapestry27/27 host tests;
four exact source-mutant failures. Actual Weft getters/wait: controlled enter
and reap failures plus two missing-guard mutants. Fresh native caller build and
CPU1 boot1830/1830 pass. Existing on-wire structs and kernel ABI are unchanged.

Graphical paired-artifact runs: session media71.98s and physical F10 SAK88.98s,
both exit0 with their scenario PASS assertions. Gallery-restored and SAK prompt
1280x800 captures inspected. No new minimum-display or Pi qualification; no
clipboard activation. The old wrapper's removed registration-log assertion
stopped after passing media; source history proves removal in49a574b09, so the
media result was retained and only unrun SAK resumed on hash-verified artifacts.

AS-R8's raw boundary/caller correction is implemented with these focused results.
The owned provided-pool wrapper and broad qualification remain activation gates.
AS-2e's new kernel core likewise still owes its broad SMP/UBSan matrix; earlier
50/50 covers AS-2a-d only. Next: private ring owner, accounting, close/exec/reaper,
then scope/protocol/receipt integration and safe pool clients before clipboard.

Native evidence: /Users/northkillpd/projects/thylacine-astra/work/oct4-async-service/asr8/native-1791139902448434000
Graphics: /Users/northkillpd/projects/thylacine-astra/work/oct4-async-service/asr8/graphics-1791140090219845000

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

## AS-R9: Burrow settlement races the final reference (open)

Review of private retirement found the existing claim/drop/restore sequence in
legacy Loom and full eager VMA detach. After a non-final drop returns false,
another holder may perform the final drop before restore touches the descriptor.
The Burrow lock protects each operation individually, not the interval. Private
retirement must not adopt this pattern. Reproduce the precise interleaving, then
make charge settlement atomic with the drop decision for both refs and mappings.
No runtime failure is claimed yet; source-level lifetime defect under investigation.
Owner edits are uncommitted and private setup is disabled; no gate is active.

## Operator pause: kernel lifetime work (October 5)

At the operator's request, leave the just-reviewed kernel lifetime/refund work
parked and continue independent UI work. AS-R9 remains open, source-level only;
no reproducer, repair or runtime qualification is claimed. Do not resume it or
the dependent private-owner activation without a later operator direction.
The unfinished owner changes are preserved byte-for-byte in
work/oct4-async-service/owner-integration/paused-owner, with base files, patch
and pin.json. They are not applied to source. HEAD remains c822021a2. Four
authority/settings drafts are untouched; no resource lease or job is active.
Continue the approved modal visuals/status work without enabling private async
services or the clipboard endpoint.
