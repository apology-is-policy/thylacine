# Async service implementation

Approved scripture4722f34e8. Single-agent on Astra; protected authority/settings
drafts preserved. ASYNC-SERVICE-LIFECYCLE.md owns the contract; memory-accounting
implementation follows this facility as approved.

October 8 ownership update: the operator transferred Corona's remaining work
entirely to Astra when the Claude agents exhausted credits. Astra continues the
existing `corona/async-memory` checkout, with Yip/resource identity rooted only in
`thylacine-astra`. The current queue and exact verification state are in
`work/oct5-as-r9/astra-takeover.md`. T83's helper-timeout path is corrected in
source and the strengthened log checker passes 26 constructed-log arms; the
actual-source host control and new guest qualification remain pending resources.
No new runtime, integration, private activation or memory-limit removal is claimed.

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

## AS-R9: Burrow settlement races the final reference (repair written, UNRUN)

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

**SUPERSEDED October 5.** The operator authorised Corona to implement the
resumed async service lifecycle and production memory accounting, beginning with
AS-R9, and Astra gave the implementation handoff (Yip call 0161). The pause above
is recorded for history; it no longer governs. The preserved owner draft is
still unapplied, and private async, the replacement accounting and the clipboard
all remain gated on their own qualification.

## Corona AS-R9: charge settlement inside the drop (October 5-6 -- SOURCE COMPLETE, GUEST UNRUN)

Corona checkout `/Users/northkillpd/projects/thylacine-corona`, branch
`corona/async-memory`, base `5ff62b78809846af4780ec41f82d1676e7584e80` (verified
equal to Astra's tip; all six of the handoff `base.json` source hashes match).

**Verification posture, measured on this tree.** The host double HAS now run and
AS-R9 is **reproduced**; everything requiring the guest has not run. The Mac lease
is held by Main for its signal7 landing gates and Corona is queued behind it, so
no kernel build, native test, model or boot has happened. The operator cleared the
host double specifically (a single-file clang compile with no QEMU, CMake or
parallelism cannot change a gate's verdict), and running it early was what caught
two build faults in seconds instead of on contended hardware -- see below.

**The defect, widened from the original write-up.** AS-R9 was recorded above as a
pattern "in legacy Loom". It is not confined there: the claim/drop/restore
sequence has **six** instances, five of them in live production paths
independent of the dormant private owner --
`kernel/loom.c:322` (`loom_drop_pin_settling`), `kernel/loom.c:706` (displaced
registered-buffer pins), `kernel/weft.c:388` (share unregister),
`kernel/weft.c:445` (owner orphan sweep), `kernel/vma.c:386` (eager-ANON detach
in `vma_detach_range_in`) -- plus `kernel/syscall.c:7320` (JIT destroy), which is
sound and is treated separately below.

**Severity, measured rather than asserted.** `burrow_free_internal` clobbers
`magic` and returns the slot to SLUB, which its own comment notes does not zero
the slot. So a stale restore lands on a slot that is either still free
(`magic == 0`, so `burrow_charge_restore_in` extincts -- a whole-system kill
reachable from an ordinary pair of concurrent closes, and the dominant outcome),
reissued with no charge recorded (the payer's charge is planted on an unrelated
region; it becomes a wrong refund -- an I-32 under-count -- only if that region
is never `burrow_charge_record`'d, since that call overwrites unconditionally,
and is later settled against the same AddrSpace id: reachable for backings that
take no record, but **not** the likely case), or reissued with a charge (the
`charge_pages != 0` arm extincts with "re-charged mid-settle", a **fabricated**
fault whose own comment asserts the case cannot happen). The write can also race
a concurrent `burrow_create` initialising that slot. Independently, the holder
that actually frees the region reads the momentarily-cleared record and refunds
nothing, so the payer stays charged for pages that no longer exist. `g_vmo_cache`
being Burrow-specific bounds a reissued slot to being a Burrow.

**The repair.** `kernel/burrow.c` + `burrow.h`:
`burrow_charge_claim_locked` (the claim with `v->lock` already held, so there is
one claim implementation rather than two); `burrow_unref_settled_in` /
`burrow_unref_settled`; `burrow_release_mapping_settled_deferred`. The
decrement, the `{0,0}` dual-counter decision and the charge claim run in one hold
of the lock. A non-qualifying drop leaves the record alone, so the holder that
does qualify still finds it; a qualifying drop takes it under the lock, so
settlement is exactly-once. Neither form touches the Burrow after the reference
it dropped is gone, so no caller needs a surviving reference. The refund returns
as a scalar so the caller applies it outside the leaf lock. `payer` is the exact
AddrSpace incarnation; `NULL` settles nothing. The mapping form keeps the
deferred contract and qualifies on `freed || shared_out`, reading `shared_out`
under the same lock -- monotonic false -> true, so a later observation can only
add a reason to settle. `kernel/vma.c` gained `vma_free_settled_deferred`, of
which `vma_free_deferred` is now the no-payer wrapper, so the Vma validation is
not duplicated. All five unsafe callers migrated.

**JIT destroy: proven sound, kept, premise named.** Its restore-path reference is
guaranteed by three facts, now written at the site instead of inherited from a
comment that asserted the conclusion: every failure return in
`burrow_unmap_reporting` precedes that function's first mutation, so a nonzero rc
is no teardown rather than a partial one and leaves its alias attached; the
restore arm runs only when one of the two unmaps failed, so at least one alias
still holds a mapping ref; and both aliases live in `p->as`, whose lock is held
across the whole interval, while refs from any other address space only add to
the counts. Premise one is the fragile half -- a failure return added below the
mutation point would silently make the site a use-after-free write -- so
`burrow.unmap_failure_leaves_mapping_attached` pins it rather than a comment.

**Three false claims deleted, not softened.** `burrow.h`, the Burrow dossier and
the Loom dossier each described this window as benign over-charging, the Loom
dossier calling its failure mode "deliberately chosen". It was neither benign nor
chosen: the descriptions omitted the use-after-free write entirely, and
"never a refund to a Proc that did not pay" is false in the reissued-slot case.
`burrow.h`'s claim/restore contract now states that the API is legal only for a
caller holding an independent reference across the interval, and names JIT as the
only such caller.

**Tests written, none executed.** Native: `burrow.settled_drop_retains_nonfinal_charge`,
`burrow.settled_drop_exact_payer`, `burrow.settled_mapping_drop_defers_free`,
`burrow.unmap_failure_leaves_mapping_attached` (each with a positive control one
variable away where a negative assertion would otherwise be satisfiable by a
broken fixture). `kernel/test/test_addrspace.c`'s async-owner settle -- the case
the exact-payer form exists for, where only an AddrSpace pin names the payer --
now settles through the drop. Host double `work/oct5-as-r9/asr9-fixture.py`, 12
legs: three pre-fix schedules (handle/handle, mapping/handle, handle/mapping),
the reissued-clean and reissued-charged outcomes, a **positive control** that
runs the identical sequence with no racer and requires the restore to complete
(without it an `extinction()` miswired to always exit 42 would "reproduce" the
bug on any input), four repaired schedules, and a shared_out discrimination pair
one variable apart. The pre-fix functions are extracted with
`git show 5ff62b788:kernel/burrow.c` rather than from the working tree, so the
repair cannot launder the premise; `work/oct5-as-r9/verify-verbatim.py` separately
proves the handoff fixture's four functions are byte-identical to shipped source
and prints its denominator so a zero-block run cannot pass as agreement.

**AS-R9 IS REPRODUCED (host double, 2026-10-05).** `work/oct5-as-r9/asr9-fixture.py`,
`CC=llvm@22` with a scoped `-isysroot $(xcrun --show-sdk-path)`. Evidence:
`work/oct5-as-r9/asr9-fixture.json` + `leg-*.log` + `mutant-*.log`.
**12/12 legs and 5/5 mutants behaved exactly as predicted**, each leg carrying a
distinct exit code so it cannot pass by dying the wrong way:

| leg | exit | what it witnesses |
|---|---|---|
| `old-handle-handle` | 42 | non-final handle drop, racing final handle drop -> restore on a clobbered descriptor |
| `old-mapping-handle` | 42 | non-final MAPPING drop, racing final handle drop (mixed holders) |
| `old-handle-mapping` | 42 | non-final handle drop, racing final MAPPING drop (the other drop order) |
| `old-recycled-clean` | 44 | slot reissued empty -> charge PLANTED on an unrelated region, then refunded: the I-32 under-count |
| `old-recycled-charged` | 43 | slot reissued charged -> FABRICATED `re-charged mid-settle` |
| `control-no-race` | 1 | **positive control**: identical sequence, no racer -> the restore COMPLETES |
| `new-*` (6 legs) | 0 | repaired: settled once under one lock; non-final drops retain the record; exact-payer holds; shared_out pair discriminates |

The pre-fix functions are extracted with `git show 5ff62b788:kernel/burrow.c`, not
from the working tree, so the repair cannot launder the premise; every extracted
body is asserted to be a verbatim substring of its source. The five mutants each
reddened their **named** leg with its **named** exit code (M1 claim-on-every-drop
-> 70; M2 ignore-shared-out -> 45; M3 always-settle-mapping -> 70; M4
exact-payer-ignored -> 73; M5 claim-does-not-clear -> 75), which is what makes
them discriminating rather than merely detecting.

**Boundary of that evidence, stated rather than left implied.** The double is a
cooperative single-threaded schedule: its lock is a counter, its allocator a
static slot whose state is rewritten, and no freed host memory is ever
dereferenced. It establishes that the shipped pre-fix functions and the repaired
ones behave differently under one named interleaving with the implementation as
the single changed variable. It does **not** establish ARM weak-memory behaviour,
real SLUB timing, or that the interleaving is reachable from any particular
syscall pair. Those need the guest.

**Class sweep (2026-10-05).** Fixing six known sites proves nothing about whether
the same shape lives elsewhere, so `work/oct5-as-r9/uaf-class-sweep.py` looks for
AS-R9's CLASS: any use of a Burrow pointer after the reference keeping it alive
was dropped, across all 211 kernel/mm `.c` files. 318 drop call sites; it refuses
to report success if it finds zero, since a sweep that never fired would "pass"
vacuously. It over-reports by design and every hit is triaged by hand.
**No instance of the class survives in production code.** The first cut reported
99 mentions, including an alarming-looking `weft.c:312` that unrefs `v` and then
stores `v` into the global share registry -- a FALSE POSITIVE: the scanner walked
past a `return`, and those are mutually exclusive branches (the `burrow_ref` is
taken before the lock; the table-full path unrefs and returns). Teaching it to
stop at a `return`/`goto`/`break` at or left of the drop's indentation, and at any
dedent past it, cut the list to 62. What remains is two benign shapes: tests that
read `burrow_handle_count` after an unref precisely to assert a SURVIVING pin
still holds the region, and `x->burrow = NULL` stores that clear the container
field rather than touch the dead object (`vma.c:150`, `vma.c:179`, `weft.c:542`).

**Still owed, all lease-blocked:** `tools/build.sh kernel --config ci`; the four
new `burrow.*` tests plus the burrow/vma/weft/loom/capacity/resource/addrspace
suites; the burrow and capacity models; and `tools/ci-smp-gate.sh` -- this is an
SMP race fix, so a single-CPU green proves little, and the October 1-2 waiver has
expired. Then the audit round: the `kernel/burrow.c` + `burrow.h` row in
`docs/AUDIT-TRIGGERS.md` (VMO / BURROW) is triggered, and now carries an AS-R9
addendum with a PROSECUTE list. Astra holds review; per AGENTS.md's single-agent
rule Corona does not spawn reviewer subagents.

**Pre-lease syntax check (2026-10-05).** `work/oct5-as-r9/syntax-check.sh`, a
reproducible `-fsyntax-only` pass over all eight edited files with the toolchain's
own flags. Not a build: no linking, no artifacts, no mutation of `build/`. Its job
is to keep the contended lease for real gates rather than spend it on a typo.
All eight parse. It compares the warning COUNT against the base commit instead of
requiring zero, because `weft.c` and `syscall.c` carry pre-existing warnings -- a
matching count is the discriminating result, where "no warnings" would also be
satisfied by an invocation that never looked: `burrow.c` 0/0, `vma.c` 0/0,
`loom.c` 0/0, `weft.c` 1/1, `syscall.c` 68/68, and the three test files clean.
**No warning or error is attributable to this change.** Two findings were
correctly attributed AWAY from it in the process: `weft.c:550`'s sign-compare and
`syscall.c`'s 68 missing-prototypes are all pre-existing (verified by running the
same check against `git show 5ff62b788:`), and a `test_burrow.c:545` implicit
declaration was an artifact of the ad-hoc invocation, not a defect -- the symbol
is declared at `burrow.h:987` behind `#ifdef KERNEL_TESTS`, the base file
reproduces it identically, and line 545 is not in the appended region (the AS-R9
tests start at line 613). This proves the files PARSE and nothing about
behaviour; `tools/test.sh` remains owed.

**Two build faults the early run caught**, both of which would otherwise have
burned contended lease time: llvm@22 defaults to a sysroot that does not exist
(`MacOSX26.sdk`), fixed with a scoped `-isysroot` -- the same stale-SDK trap the
October 4 journal entry already records, so it has now recurred and belongs in
the recipe rather than in each author's memory; and `-Werror` rejected two of the
mutations for leaving `shared_out` unreferenced, which is a property of the
mutation, not of the repair.

**Dossiers co-staged:** `sub-kernel-burrow` (the AS-R9 section and the corrected
contract), `sub-kernel-vma`, `sub-kernel-loom`, `sub-kernel-weft`. `quaestor lint`
reports 0 failures; its 2 warnings (`sub-kernel-loom-pools` section order, 47
stale dossiers) are pre-existing and not introduced here.

### October 6: the blocker was mis-measured, and the requirement collapsed

Superseding, not rewriting, the October 5 record above. Everything it states about
the defect and the repair still holds; two things it states about the OBSTACLE were
wrong, and the measured corrections change the plan.

**The 21G reclaim was wrong twice over.** The October 5 posture was "blocked until
Main frees `thylacine-s7ci/build` (21G)". Both halves failed on measurement. Main's
declared work showed their legs had NOT ended -- that worktree was the input to the
next step of their landing, not residue -- so the request was withdrawn before they
acted on it. And the tree is itself an APFS clone of their primary's `build/`, so
removing it frees only what its bake wrote, never 21G. That is the same clone-
accounting error made earlier the same day on another peer's trees and already
written down; it recurred because it had been stored as a fact about those trees
instead of as a rule about clone families. On this volume no `du` figure is
reclaimable space: only what a tree UNIQUELY wrote is, and `du` cannot show that.

**The requirement collapsed from 14-21G to a kernel build.** `git diff --name-only
5ff62b788..HEAD` touches `kernel/`, `docs/` and `vault/` ONLY -- zero files under
`usr/`. The kernel ELF is loaded separately from the ramfs, so nothing in this
change can invalidate a userspace artifact, and a from-zero `--config ci` bake was
never the requirement. The binding constraint was always the Mac LEASE, not disk.

**Astra approved an incremental cache (Yip 0169) under provenance conditions**, and
one of those conditions found something this write-up had missed. Her caveat was
that unchanged `usr/` does not by itself prove unchanged generated headers or ABI
dependencies. Checked rather than re-asserted: `kernel/include/thylacine/vma.h` IS
in the diff and three files under `usr/` reference it -- but all three are COMMENTS,
and an anchored grep for a real `#include` directive across `usr/` and `lib/`
returns nothing, so no userspace translation unit compiles against it. The change
to that header is a single added declaration (`vma_free_settled_deferred`) with no
struct, constant or enum touched, and `burrow.h` is referenced by zero userspace
files. The only generated header is `corvus_system_recovery_phrase.h`, which no
kernel header feeds. The inherited objects are therefore sound as a CACHE, and are
treated as cache only -- never as evidence.

**That check produced a finding worth more than itself.** The three references are a
hand-maintained MIRROR: `VMA_PROT_READ/WRITE/EXEC` (`vma.h:31-33`) are duplicated as
`T_PROT_READ/WRITE/EXEC` (`libt/include/thyla/syscall.h:602-604`) and restated in
libthyla-rs, each under a comment reading "MUST mirror" -- and that comment is the
whole enforcement. No `_Static_assert` ties the two sets and no `tools/` script
compares them. The values agree today, so there is no live defect; a drift would make
userspace and the kernel disagree SILENTLY about memory-protection bits, which is
W^X-adjacent (I-12). Enqueued as a P3 hazard, pre-existing and not introduced here;
the fix is a derived check, because userspace cannot include the kernel header and a
name-pinned guard is re-pointed by hand.

**Three further commits, all host-free.** `burrow.settled_drop_exact_payer` was given
a positive control it lacked: both its arms were zero-assertions (a NULL payer settles
nothing; a non-payer settles nothing) and nothing in the fixture established that a
charge was ever present to refuse, so a `burrow_charge_record` that recorded nothing
would have satisfied both. The mutation set and a sibling witness caught that case
anyway, so the suite was never vacuous -- but a test whose discrimination lives in a
sibling is one deletion from proving nothing. `burrow_is_shared_out` was deleted after
establishing that THIS repair removed its last caller (the eager-ANON arm of
`vma_detach_range_in` at base), making it the repair's own residue and the same drift
hazard the earlier self-audit removed one layer down. And the host double is now
committed as evidence rather than left untracked, since it is the only thing that
reproduces AS-R9 and it is cited in the queue, the dossier and three peer calls.

**A flaw in the verification harness, recorded because it bears on the figures above.**
The pre-lease syntax check extracted base `.c` files but compiled them with
`-I kernel/include` -- the CURRENT tree's headers. Deleting a declaration from
`burrow.h` therefore moved the BASE count from 0 to 1 and the check reported
REGRESSED on a change that REMOVES a warning. A baseline that moves when the thing
under test changes is not a baseline. It now extracts the whole base tree with
`git archive` and compiles against the base's own headers, and a count below base
reports IMPROVED rather than failing. Under the corrected baseline the other four
files' counts are byte-identical, so every figure recorded in the October 5 section
stands.

**Exact-current guest verdict: there is none.** No build, no boot, no native test, no
model, no SMP gate, on any host, ever. The four new `burrow.*` tests are registered
and have never executed. The only evidence is the off-guest host double, whose stated
boundary establishes nothing about ARM weak memory, real SLUB timing, or syscall
reachability.

**The lease window is a prepared script**, `work/oct5-as-r9/lease-runbook.sh`, so a
contended resource is spent executing rather than exploring. Stage 0 re-runs the spec
obligation -- `burrow.tla`'s three buggy cfgs must still violate `NoUseAfterFree`, and
`capacity.tla`'s two must violate `NoOrphan` with `ChargeConserved` holding ahead of
it, `capacity_buggy_detach_no_refund` being literally AS-R9's second arm -- and needs
no image or artifacts at all, so it runs regardless of the cache. Stage 3 verifies the
image by CONTENT, since the bake-trap class fails as absent content behind a green
ledger: the suite total must rise by exactly 4 and the four witness names must appear
in the ELF, gated behind a denominator control (a base-era test name) so a broken
search cannot be misread as missing tests. Stage 5 is `ci-smp-gate`, required because
this is an SMP race fix and a single-CPU green proves little. Stage 6 is a
second-silicon pass on thyla-pi, the only non-Apple ARM64 in the loop, because a race
fix green on one memory model is one reading and only a second axis separates two
causes. The runbook carries its own free-space floor, set at Main's 6 GiB plus the
expected delta, because this base predates `disk_floor_check` and an unguarded bake
here dies on ENOSPC instead of refusing -- the failure mode that broke every agent's
shell on October 5.

### October 6, later: the first guest run, and D7's cause

**The repair's witnesses executed for the first time and PASSED.** `tests: 1834/1834`
against the base tree's `1830/1830` -- exactly +4 -- with a PASS record for each of
the four by name, read from the boot log rather than `test.sh`'s stdout, because an
ELF name shows inclusion and not execution. Stable across two boots. Stage 0's five
buggy cfgs all still violate (`burrow_*` x3 report `Invariant Invariants`;
`capacity_buggy_detach_no_refund` and `replace_orphans` report `NoOrphan` violated
with `ChargeConserved` HOLDING -- the first of those being AS-R9's second arm in
model form).

Paired-image hashes at post-build: `.config 4fcc788d6be38b80` (identical by hash to
the cache source's), `thylacine.elf 1fe1ba3a46219dc1`, `ramfs.cpio 817320cea4ccd9b6`,
`pool.img a3ae8a265b3355e4`. Disk 13 GiB before the clone, 12 after the full rebuild
and re-bake -- about 1 GiB of real growth against the 14-21G a from-zero bake costs.

**The boot then extincted, and it was not the tests.** `joey: D7 overlapping login
probe FAILED` -> `EXTINCTION: joey exited non-zero 1`, proximate cause
`install-dek uid=4294967294 dataset=2 result=err:eaccess` under three simultaneous
sessions -- after that same dataset installed `result=ok` earlier in the same boot.
Deterministic, 2/2 on the identical image, re-run deliberately to test determinism.

**The cause is an unequal EXTERNAL input, not the repair.** Found by Astra on
read-only inspection and verified here against primary sources:
`build/pouch/stratumd-cmake/CMakeCache.txt` names `stratum-v2_SOURCE_DIR` as the
SHARED `projects/stratum/v2` (HEAD `ac519fc`) in this tree and
`projects/stratum-astra/v2` in hers, whose HEAD is `61dde37` ("Support independently
proven session leases for home keys", directly on `ac519fc`);
`merge-base --is-ancestor 61dde37 HEAD` is FALSE in the shared tree.
`SRV-SESSION-REGISTRY-DESIGN.md:285-290` states the requirement outright -- one
authenticated DEK lease per connection/dataset pair, each new connection proving
UNWRAP even when another session has installed the key, implementation isolated in
`stratum-astra`. Without it the overlapping session cannot prove UNWRAP, which is
exactly the observed refusal and exactly the tell (ok first, refused on the overlap).

The runbook now pins `STRATUM_SRC` to `stratum-astra` and ASSERTS
`STRATUM_PIN=61dde37` before building. Which half of that guard discriminates is
recorded in the script: `git rev-parse 61dde37` SUCCEEDS in the shared tree, because
the object is present there without the commit being in its history, so only
`merge-base --is-ancestor` can refuse. Verified both ways against both trees.

**The generalisable failure, enqueued as a tooling item rather than fixed in this
slice.** The two images shared an identical `.config` hash and base commit, and four
recorded artifact hashes matched -- yet differed in the one input that decided the
outcome, because `build.sh` consumes `$STRATUM_SRC` (build.sh:301, :3281) read-only,
bakes the result into the ramfs, and records its identity in no ledger, no config and
no hash. Four matching hashes stood in for equal inputs, which is the same shape as
quoting `du` for reclaimable space: a proxy accepted in place of the thing. The probe
compounds it -- `git grep "overlapping login"` finds `usr/joey/joey.c` on this base
line and NOTHING on `main` or `9dc80bb37`, so the D7 ladder and its Stratum support
are both Astra-line features split across two repositories, and the handoff pinned
one tree and not the other.

**Still owed, unchanged: `ci-smp-gate` has never run.** This is an SMP race fix, so
that is the gate that matters most, and four witnesses passing on a single 4-CPU boot
is evidence rather than qualification. The confirmation run (pinned Stratum, same
AS-R9 source and config, full matching pair, trace comparison, then the SMP gate)
waits for the next owned lease; aux held the Mac for their served-link close and was
not displaced for a red that is explained and enqueued.

### October 6, evening: the verification runbook audited as code

No lease was available all evening (main held the Mac for the devno and loom-mc
gates, with aux ahead in the queue), so this slice is entirely off-lease: nothing
was built, booted or gated, and no kernel source changed. The work was auditing
`work/oct5-as-r9/lease-runbook.sh` against the TOOLS it calls instead of against
the memory of having written it. Thirteen defects, five of them found by Astra
reviewing it read-only on Yip 0161. The runbook decides whether a scarce lease
produces evidence or noise, and it had not been audited the way the code it
verifies is audited.

**What the eventual result will mean, which is what changed.** Three of the
defects would have produced a MISLEADING verdict rather than a failed run:

- **Stage 5 captured its gate and asserted nothing.** `tools/ci-smp-gate.sh | tee`
  yields tee's status, so `set -e` was blind and no later line examined it: a RED
  `ci-smp-gate` would have run through to the script's closing "the qualifying
  verdict" line, on the one gate AS-R9 is blocked on. The gate's own status now
  returns through a subshell sentinel, the `ci-smp-gate: PASS` line must be
  present, and all five matrix rows are enumerated rather than counted.
- **Five PASS rows are not fifty clean boots.** `tools/smp-multiboot.sh:347`
  returns `corrupt==0 && extkill==0 && other==0`, so TIMING and INJECT-MISS boots
  can be nonzero inside a row that PASSES -- and the timing arm is labelled
  "benign host-fragility" in the tool, which is the non-explanation CLAUDE.md
  forbids. Stage 5 now requires `pass == N` for every label; because each boot
  carries exactly one classification, that single check is equivalent to "every
  other category zero". A narrowed `SMP_GATE_CONFIGS` or an `SMP_GATE_N` below 10
  is refused outright: narrowing the matrix on an SMP race fix is verifying around
  the hazard, and the subset still prints a PASS.
- **The Stratum pin checked the input; nothing checked the output.** `build_stratumd`
  runs in the all-flow (build.sh:394), copies to `$progs_out` (build.sh:3347) and
  is installed into the ramfs (build.sh:780-784). This tree held four stale
  `stratumd` binaries, two of them in those staging paths, which stage 1 did not
  clear -- so a run that skipped or failed that build would have baked the OLD
  daemon in while the pin guard reported success. Stage 1 now deletes the staged
  daemon (a loud absence beats a silent stale binary) and stage 2 asserts both
  that the stratumd `CMakeCache.txt` NAMES `$STRATUM_SRC` and that each staged
  binary postdates a pre-build timestamp.

**Two further items of record.** The D7 red-boot exception now requires exit code
20 exactly -- the *unchanged* known red, byte-identical to the recorded baseline
-- where `!= 0` had also admitted "changed shape" (a second cause in play),
"control failed" (a vacuous experiment) and any shell error; code 22 was split out
for a refusal whose trace differs. And Astra's build-cache clone approval is
discharged (acknowledged on 0169 t5/t6, confirmed on 0161 t17), so the runbook no
longer clones her tree on `CLONE_APPROVED=1`: an existing `build/` is used as-is,
a re-clone requires `CLONE_RECHECKED=1` after a fresh coordination check, and the
cache invalidation -- which previously sat inside the clone branch and therefore
never ran on the own-cache path -- is unconditional.

Every stage is now audited against the tool it calls: stage 0 ran on Oct 6,
stages 1/2/4/5/6 were repaired, and stage 3's denominator control was verified
against the real ELF (`test.c:2328` registers the literal
`burrow.refcount_lifecycle`, so the control discriminates instead of aborting
every run). Every new assertion was exercised BOTH ways before being trusted --
thirty cases, including control breakers, and for the Stratum check a red arm on
the live stale `CMakeCache.txt` rather than a synthetic one.

**Posture unchanged: AS-R9 is UNQUALIFIED, because `ci-smp-gate` has never run.**
None of this brings the gate closer to running; it only makes the run's verdict
mean what it says.

### October 6, night: the run reached the guest, and D7 is cured with the kernel exonerated

What had aborted both earlier runs was stage 4's witness assertion, not D7: it
read `work/oct5-as-r9/guest-test.log`, which is test.sh's stdout (29 lines, zero
`[test]` lines), while the suite's 1835 `[test]` lines live only in the boot log.
It had never passed since the day it was written, so the October 6 run's
"aborted" was mine and not the guest's. The witness, tally and skip reads now use
the preserved boot log behind a denominator control.

With `STRATUM_SRC` pinned to stratum-astra @61dde37 (exact head equality, a
dirty-tree refusal, and the pin asserted in the OUTPUT: the stratumd
`CMakeCache.txt` names the pinned tree, both staged binaries newer than a
pre-build stamp):

- `joey: D7 overlapping login probe PASS` -- `work/oct5-as-r9/boot-logs/boot-confirm-232503Z.log:3495`
- `joey: D7 twenty distinct login/logout cycles PASS` -- `:3776`
- `Thylacine boot OK` -- `:3914`; EXTINCTION lines: 0
- tests 1834/1834, all four burrow witnesses with PASS records

Paired-image hashes of that run, preserved under
`work/oct5-as-r9/cpu1-green-pair/` with a manifest before stage 5 can re-bake
them (Astra, 0161 t21): `.config 4fcc788d6be38b80`, `thylacine.elf
1fe1ba3a46219dc1`, `ramfs.cpio cf650c39d99b8716`, `pool.img f01c00189218b9e0`.
Against the red run's hashes recorded above, the ELF and the `.config` are
BYTE-IDENTICAL and only the Stratum-derived half moved. The kernel was held fixed
as the control variable and D7 flipped, so no part of the charge-settlement
repair can be the cause.

D7's queue entry nevertheless stays OPEN until the gate. The red was only called
deterministic after 2/2 on an identical image; a cure resting on 1/1 does not
meet that bar. The close condition is per-boot D7 PASS witnesses in the retained
logs, NOT an inference from five green rows (Astra, 0161 note 17) -- and the
retention had to be built, because `smp-multiboot.sh` overwrites
`build/test-boot.log` every boot and keeps a copy only for a non-PASS
classification, so the passing boots' evidence was being discarded. It now takes
`SMP_KEEP_LOGS=1` (default off; a prior run's logs are archived, not deleted),
and stage 5 counts per label how many boots reached the overlapping-login ladder
and how many reported PASS, refusing when a label's retained-log count is not N.

**Posture unchanged: AS-R9 is UNQUALIFIED, because `ci-smp-gate` has never run.**

### October 7: the second axis is unavailable, and the residual is recorded rather than silent

thyla-pi cannot be reached by either route, measured on both rather than inferred
from a status line: `ssh thyla-pi` fails to resolve `thyla-pi.local` (its ssh
config's HostName is an mDNS name), and `ssh thyla-pi-cf`, whose ProxyCommand is
`cloudflared access ssh --hostname thyla-pi-ssh.treeso.net`, answers `websocket:
bad handshake` with no local `cloudflared` process either -- so the far end of the
tunnel is down. Meanwhile `yip resources` lists `pi FREE`, which is a LEASE state
and never a reachability measurement; stage 6 of the lease runbook says exactly
that in a comment (Astra, 0161 note 8) and now has the measurement behind it.

This matters because AS-R9 is an SMP race fix, and thyla-pi (4x Cortex-A72, real
KVM) is the only non-Apple memory model in the fleet -- the axis that separates
two causes sharing one reading, and the host #214 was closed on. With it down,
every SMP result in flight rests on Apple M2 under HVF alone.

**Operator decision, 2026-10-07 (do not re-ask): the mac gate alone, with this
residual recorded.** AS-R9 qualifies on M2/HVF from the 50-boot `ci-smp-gate`
matrix plus the per-boot D7 witnesses the retained logs now carry, and the
missing A72/KVM axis stays an explicit, owned, queued residual -- named in the
review rather than absent from it -- with the pi leg to run when the host comes
back. `PI_AXIS=0` is therefore the correct setting for this run, not a dodge, and
stage 6 refuses out loud if anyone sets it otherwise without the lease.

What this does NOT license: reading the mac gate's green as a two-axis
qualification, or closing the residual by argument. The residual is owned in
OPEN-BUGS by the thyla-pi entry, which also carries the vote.

### October 7, later: the gate qualified AS-R9 on the mac axis

`ci-smp-gate: PASS -- 0 corruption across all configs` on run log
`work/oct5-as-r9/run-1007T061510Z.log`, verdict tree
`e673db5b941fb71123cfb5a2d38e3e163b38aba2` as recorded in that run's
post-ci-smp-gate provenance block. Gate exit 0, `runbook exited 0`.

- **50/50 CLEAN boots**, not five PASS rows: every label reports
  `10 PASS / 0 CORRUPTION / 0 external-kill / 0 inject-miss / 0 timing / 0 other (N=10)`.
  Each boot carries exactly one classification (`smp-multiboot.sh` `rc_ok` arm
  increments `pass` then `continue`s; all other paths fall through a single-match
  `case`), which is what licenses reading `pass == N` as "all six others zero".
- **Two kernels**: 30 boots on the default ELF `1fe1ba3a46219dc1`, 20 on the
  sanitizer ELF `8fdd7f1f5695b4ff` (absent before the gate built it).
- **D7, reported separately**: ladder reached 10/10 per label, 50 total, probe
  PASS 50, FAILED 0, measured off the 50 retained logs behind the denominator
  control. The OPEN-BUGS D7 entry is CLOSED on this evidence.
- **Suite**: `tests: 1834/1834 PASS`, `[skip] lines: 0` on the gate image, and the
  four burrow witnesses each RAN+PASSED by name:
  `settled_drop_retains_nonfinal_charge`, `settled_drop_exact_payer`,
  `settled_mapping_drop_defers_free`, `unmap_failure_leaves_mapping_attached`.
- **Exposure (#200)**: per-label means 42 / 45 / 47 / 46 / 49 s per boot, rising
  monotonically with CPU count and the sanitizer, 50 boots with no outlier.
  Astra's disclosed verification window (06:25:00-06:28:30Z) overlapped
  `default-smp1` boots 8-10 and `default-smp4` boots 1-2; those boots read 42,
  42, 44 and 45, 46 against label means 42 and 45, so no perturbation is visible.
  Recorded as a negative result, which excuses nothing because nothing failed.
- **Provenance note**: post-gate `.config` (`cd0200373d03e647`) is the sanitizer
  configuration the gate left behind; the default build's pin is the post-build
  block at 06:17:02Z (`4fcc788d6be38b80`), and the default kernel ELF is
  byte-identical across all three blocks. `STRATUM_SRC` stayed
  `stratum-astra/v2 @61dde3727921e70e2c72fbd3c9e2044a192f4a54`, clean.

**STILL ONE AXIS.** All 50 boots ran on Apple M2 + HVF. The A72/KVM leg never ran
(thyla-pi unreachable on both routes), per the operator's "mac gate alone,
residual recorded". This does not license reading the mac green as two axes; only
the pi booting this repair closes it. Astra reviews with the residual named.

Nothing pushed, nothing landed on main. The 128 MiB protection, private async and
clipboard are unchanged; the paused private-owner draft stays shut until Astra's
review.

### October 7, later still: the private-owner port, with the retirement settled

Astra closed the scoped AS-R9 review at `ba0c8f60f` and cleared obligation 1's
shape on yip 0161 turn 29, prescribing the exact retirement form. The approved
empty-private-owner infrastructure is now authored into this checkout against
the repaired base: `213b695f8` (the port) and `f05aefbff` (its off-lease
evidence).

**The correction the port exists to make.** The draft's `loom_private_destroy`
settled its ring charge with claim / drop / restore -- the shape AS-R9 removed,
written anew on a path that postdates the repair, and absent from both the
draft's base and this branch's repaired HEAD. A textual rebase would have
conflicted on nothing and left exactly that site defective while fixing the two
around it. It is replaced by the settled form: one
`burrow_unref_settled_in(l->ring, as, &refund)`, one uncharge of
`metadata + refund` under `as->lock`, and `addrspace_private_end` LAST, because
it may drop the final lifetime reference and free the descriptor. `as` and
`metadata` are snapshotted above the `kfree`. Verified after the port:
`kernel/loom.c` holds zero claim/restore sequences and three settled sites.

**Reachability is not claimed.** The window needs two holders of one Burrow, and
the retirement guards may exclude that by construction. The settled form costs
nothing here, so soundness does not rest on an exclusivity argument nobody has
verified.

**The fixture gap was larger than a missing case.** Every retirement in the
draft's fixture ends the ring's occupancy, so all of them refund the whole
charge -- an implementation that refunded UNCONDITIONALLY satisfies all 96 lines
of it, which means neither the draft's conditional nor the new `refund` was
exercised by it. The added leg maps the ring into the creator image so the
handle drop is NONFINAL: the refund must be zero, only the metadata may come
back, and the mapping teardown settles the rest. Its baseline is measured after
the map rather than predicted.

**That assertion is no longer a prediction.** A host double
(`work/oct5-as-r9/private-retire-fixture.py`) extracts
`burrow_charge_claim_locked`, `burrow_unref_settled_in` and
`burrow_release_mapping_settled_deferred` VERBATIM from `kernel/burrow.c` by
brace matching, each asserted a substring of its source, and doubles only the
environment. Five rows, all as predicted
(`work/oct5-as-r9/private-owner-logs/private-retire-matrix.log`):

| impl | leg | rc | want | |
|---|---|---|---|---|
| settled | final | 0 | 0 | refunds metadata + ring |
| settled | nonfinal | 0 | 0 | metadata only; tail settles the ring |
| settled | exact-payer | 0 | 0 | another image's record is not refunded here |
| unconditional | final | 0 | 0 | **BLIND SPOT** demonstrated, not asserted |
| unconditional | nonfinal | 11 | 11 | **DISCRIMINATION**, `page_count 0, want 3` |

Requiring the mutant to PASS the final leg is the row that carries the argument:
it shows directly why none of the draft's legs could have caught it.

Boundary, as the AS-R9 double stated it: this is the charge ARITHMETIC under a
forced single-threaded schedule. Nothing about ARM weak memory, real locking,
SLUB timing, or syscall reachability. The interleaving is fixture-forced and
labelled as such. It does not replace the native fixture or the SMP matrix.

**Three defects in the draft fixed in passing**, all one class -- something true
about the wrong subject: `main.c` moved the `vdso_init` comment onto the new
`loom.h` include where it is false (it belongs on `vdso.h`, where that function
is declared); the retirer block landed between the G-3 weave-reaper comment and
the weft code that comment describes; and the fixture rode on
`test_loom_create_geometry`, so a lifecycle failure would have reported under a
ring-layout name. It is registered as `loom.private_owner_lifecycle`.

**Reviewed as code, not waved through on an unchanged base.** The release
wrapper covers four of six `handle_release_obj` sites; the two it leaves alone
are correct, because `handle_put` returns a BORROWED reference while the handle
still exists and the dup-rollback site releases an acquire that never became a
handle. Latching either would mark a live ring closing on a mere borrow.

**NOT BUILT, NOT BOOTED.** No kernel compile, no boot, no native test: the mac
is held by main for the tag-pool gate and this branch is queue-head behind it.
The native leg and `ci-smp-gate` are owed, and the interior-unmap and
nonfinal-retirement fixtures must each be shown RED against their mutants before
they count as witnesses. Also owed: the five owning dossiers' pass, deliberately
deferred because writing it now would record charge arithmetic in the vault that
the guest has not yet confirmed.

Gates unchanged: 128 MiB protection retained; private async, replacement memory
accounting and clipboard all non-default; no syscall reaches
`loom_create_private`. The pi A72/KVM axis remains the named unrun residual --
re-measured today, unreachable by both routes. Nothing pushed, nothing landed.

### October 7, afternoon: the port compiles, boots, and both fixtures turn RED on command

The lease landed 09:46Z. The three claims the section above called owed are now
measured, and one belief it rested on turned out to be wrong.

**The port compiles.** First whole-kernel build of the private-owner work: zero
errors. The off-lease syntax check had predicted exactly that and it held, so
the remaining pre-build risk really was link-time and behavioural rather than
syntactic. The build did FAIL, but downstream and not in the kernel -- see the
Stratum provenance note below.

**The suite is green at the derived number.** 1836/1836 PASS, 0 FAIL, 0
`[skip]`, 0 `EXTINCTION:`, boot banner present. Both new witnesses ran and
passed, and the three `burrow.settled_*` AS-R9 witnesses are still green beside
them. 1836 was not written down and then met: it was DERIVED from the
registration table in `kernel/test/test.c` before the boot, and the suite's own
tally came back equal to it. Two routes to one figure.

**Both fixtures are witnesses.** Evidence
`work/oct5-as-r9/private-owner-logs/red-legs/20261007T103115Z`, against HEAD
`2666645b7`:

| leg | mutation | verdict | reason | attribution |
|---|---|---|---|---|
| interior-unmap | the `vaddr_start` guard deleted from `burrow_unmap_reporting` | `burrow.unmap_interior_start_refused` FAIL | "an interior start must be refused -- v1.0 has no partial unmap" | only failure; kernel's own tally 1835/1836 FAIL |
| uncond-refund | the ring refunded unconditionally in the ACTUAL `loom_private_destroy` | `loom.private_owner_lifecycle` FAIL | "a nonfinal ring drop refunds the metadata only" | only failure; tally 1835/1836 FAIL |
| green control | sources restored, kernel rebuilt | 1836/1836 PASS | boot completed, 0 skips, no extinction | both fixtures PASS |

Leg 2 is astra's PO-R2 discriminating test of the real destructor rather than of
a transcription of it: the mutation is applied to the shipped function, and the
assertion that breaks is the nonfinal one -- the exact arithmetic the retirement
exists to get right. The all-final legs pass under that same mutant, which is
why the nonfinal leg is the one that carries the argument. The green control's
kernel is byte-identical to the pre-legs kernel (`thylacine.bin` sha256[0:16]
`5ced18c43ae8302a` both times), so the mutate/build/restore/rebuild cycle
returns both the tree and the artifact to where they started.

**A VERDICT IS A STATE IN THE SERIAL LOG, NOT A LINE IN IT.** The first real run
of the red legs REFUSED, reporting the reddened test as ABSENT, and it was right
to refuse: the fixture and the mutation were both fine and the parser was not.
`test.c` prints `    [test] NAME ... ` BEFORE running the test, so anything the
test prints lands between the name and its verdict -- and on failure
`test_fail()` calls `sched_dump_runnable()`, so `FAIL: <msg>` arrives two lines
later. This is not a failure-path curiosity: 87 of the 1836 PASSING verdicts in
the green boot are split the same way by ordinary kernel output, and the serial
log is CRLF, so every `$`-anchored pattern fails silently. A single-line regex
read 1749 of 1836 verdicts, and the green control's "no failing test" check had
been blind to 87 tests all along -- it only looked sound because the tally check
sat beside it. The parser is now one normalisation pass validated against two
REAL logs before being trusted, it refuses below 1000 resolved verdicts or on any
unresolved one, and `expect_red` cross-checks the KERNEL'S OWN tally against it.

**A belief in two handoff notes and one script header was wrong.** "A kernel
test FAIL extincts the boot, so a red leg carries no tally." Measured on a real
mutant boot: the suite runs every test, prints `tests: 1835/1836 FAIL`, and
`boot_main` extincts on THAT. A red leg carries a full tally, which is why the
tally is now load-bearing evidence instead of something the runner was told not
to expect.

**The wrapper itself is now tested, not just its parsers.** Astra's note 34
found six defects in the red-legs runner that a parser replica could not have
reached: cleanup could report success on a failed restore, signals carried the
interrupted command's status, there was no owned-child wait, the unqualified
marker was cleared before the green checks ran, and the tally was not pinned to
an expected count. `work/oct5-as-r9/red-legs-wrapper-test.sh` runs 16 scenarios
and 77 checks against the real runner in throwaway git trees -- signal, build
failure, stale serial log, wrong fail reason, unattributed red, a concurrent
third-party edit, a short green tally, an unreaped build descendant, an
unprovable quiescence, a moved HEAD, and two scenarios driven by REAL recorded
boot logs. It discriminates: 77/77 against the corrected runner, 11 wrong
against the previous one, including the old runner CREDITING a red leg from a
stale log after `test.sh` never booted and silently overwriting a third party's
edit to `loom.c`. Logs:
`private-owner-logs/wrapper-test-po-r5-{fixed,prefix}-20261007T114942Z.log`.

**A FIGURE OF MINE WAS WRONG, and the retained evidence is what caught it.**
This passage previously said "11 scenarios and 57 checks ... 57/57 against the
corrected runner, 26 wrong against the previous one". The 57 run was real -- it
ran after S11 and S12 were added -- but its log was never retained, so the only
retained evidence was `wrapper-test-new.log` at **53 passed**, from before S12
existed (it does contain S11 -- astra corrected my chronology in 0161 t35), and
`wrapper-test-old-runner.log` at 26 passed / 26 wrong of 52 checks. Astra read the evidence rather than the claim and found the gap
(0161 t33). A number quoted from a run whose log was not kept is a recollection,
not a measurement, however real the run was; the figures above are the retained
logs' own last lines.

**PO-R5, the reusable-wrapper repair** (astra 0161 t33; stub coverage only, no
guest re-run requested or taken). Three defects, each now a scenario that fails
against the pre-fix runner:
1. `reap_owned` waited on the immediate build child only. A build's real
   descendants are its compilers, which outlive the shell that launched them, so
   source restoration could race a live writer. Every owned command now runs as
   its OWN PROCESS GROUP (`set -m`), because ancestry cannot be used -- a
   grandchild is reparented to init the moment its parent exits, which is
   exactly the case that matters -- and a group is reaped and then PROVEN empty
   by pgid. A peer's build has its own group and is never named.
2. A surviving QEMU after KILL only printed `WARNING`. Quiescence is now a
   state: unproven quiescence REFUSES the restore, names the live pids and the
   pristine originals, and exits nonzero. Leaving a loudly mutated tree whose
   originals are named is recoverable; a half-restored one is not.
3. The unqualified marker was removed whenever the suite was green, even when
   the run's final source/HEAD checks had failed. It now requires BOTH -- the
   suite says the IMAGE is sound, the final checks say the EVIDENCE is
   attributable -- and the marker, being the durable artifact a later session
   reads, outranks the exit status.
The refusal branch cannot be driven by a real process, because nothing survives
SIGKILL. S15 drives it by shadowing the INSTRUMENT instead -- a fake `ps` that
keeps reporting a member of the build's group, which is what an unreapable
process looks like to the runner. The runner itself is never given a seam.

**AND THAT FIRST FIX HAD THE HOLE MY OWN INDEX WARNS ABOUT** (astra, 0161 t35).
`group_members` piped `ps` into `awk`, which exits with awk's status and prints
nothing when ps dies -- so a FAILED OBSERVATION was indistinguishable from an
empty group, in the one function whose whole job is to prove a negative. She
exercised the extracted body against a ps stub returning 7: status 0, zero
bytes, and the first emptiness check would have succeeded. `my_qemu_pids` had
the same pipeline. Both now take a SNAPSHOT whose status is kept separately, and
the snapshot is CONTROLLED -- a process table that does not contain this shell
did not observe this machine, so its silence is not evidence. An unobservable
group is UNKNOWN, never empty: the run refuses, keeps the marker, and does not
restore source. Coverage is two shapes, because a status check alone catches
only the first: S17 a ps that FAILS (exit 7, no output) and S18 a ps that ANSWERS
WITHOUT LOOKING (exit 0, a plausible table lacking this shell), plus S19 as the
positive control -- with a working ps the same predicate must PROVE the group
empty, or S17/S18 would also pass against a runner that simply always refuses.

Measured three ways at one harness version: **92/92** against the fix, **84/8**
against the first PO-R5 attempt (whose runs RESTORED SOURCE with a dead ps), and
**73/19** against the pre-PO-R5 runner. Logs
`private-owner-logs/wrapper-test-obs-{fixed,vs-po-r5-v1,vs-prefix}-20261007T120712Z.log`.
Worth noting what does NOT discriminate: the exit status. All three runners exit
3 on S17/S18, the older two for an unrelated reason, so only the message and
tree-state checks carry the finding.

**STRATUM PROVENANCE: D7's residue is in this tree's `build/`, and it blocks the
gate script.** `tools/build.sh kernel --config ci` refuses at the stratumd step:
`build/pouch/stratumd-cmake/CMakeCache.txt` and `build/host-stratum/CMakeCache.txt`
were both generated from a PEER's source tree, while
`build/host-stratum-pre-d7/CMakeCache.txt` -- whose name records when it was
displaced -- holds the canonical one. Inherited through the APFS clone of a
peer's `build/` that their now-spent approval covered. The half that matters:
`build/pouch/stratumd-cmake` is the GUEST stratumd baked into the ramfs, so the
AS-R9 green image's stratumd came from a peer's working tree. **The AS-R9 KERNEL
verdict stands** -- the kernel ELF was the controlled variable and was
byte-identical across the red/green pair -- **but that image is not reproducible
from canonical Stratum, which is a weaker statement than this document has been
making.** Enqueued in OPEN-BUGS, owned, not worked around. Main checked their own
tree: both caches there name canonical, so the asymmetry is this tree's alone.

Two claims of mine around it have been withdrawn rather than left standing: that
a rebuild from canonical would bake "54 peer-uncommitted files" into the image
(all 54 are `.md` or `.tla`, none a build input -- main measured it and I
confirm), and that canonical "lacks the session-DEK lease work" (`install-dek`
appears in 3 files in BOTH trees; that grep establishes no difference, and the
claim was inherited from the D7 write-up and never measured). D7's cure WAS a
stratumd from the peer tree -- that is measured -- but the mechanism attached to
it is not.

**SETTLED 2026-10-07 by the source-level comparison astra asked for, and it
changes the remedy.** The two Stratum "trees" are ONE repository with two
worktrees: `~/projects/stratum` on `main` at `ac519fc`, and
`~/projects/stratum-astra` on the local branch `codex/astra-session-dek` at
`61dde37`. `61dde37` is canonical `main` plus EXACTLY ONE commit, "Support
independently proven session leases for home keys" -- 10 files, +269/-96,
centred on the ctl DEK path, adding a per-session lease predicate and a lease
test tool. The live remote was read with `ls-remote`, not from a cached ref, and
holds `ac519fc` for `refs/heads/main`; the commit is on no remote-tracking
branch, so it is local and unpushed. (The `origin/main` tracking ref reads
`976cb6f`, which is stale and divergent -- zero commits ahead of `ac519fc` --
which is exactly why a remote is judged with `ls-remote`.)

So the withdrawn claim was wrong in BOTH directions, not merely unevidenced.
Canonical `main` is not missing "the session-DEK lease work": it already carries
a DEK lease table (its own `TLY-A5b` connection-binding). What it lacks is that
ONE commit, which REWORKS that mechanism so an overlapping same-user session
proves UNWRAP independently. The correct statement is therefore narrow and
checkable: *this image's stratumd is canonical plus one unpushed commit*.

The consequence for the enqueued provenance question is the part that matters:
**"rebuild from canonical" is not the remedy.** It would drop `61dde37` and, on
the D7 attribution already recorded here, re-redden the overlapping-login probe.
The remedy is for that commit to land in Stratum `main` -- which is astra's
change and the operator's push, not something this branch can do -- or for the
pin to be recorded, which is the build-ledger line already landed on main at
`cb7194c10`. Until one of those, an image built here is reproducible only with
`STRATUM_SRC` pointed at that worktree, and saying so is the honest form of the
claim.

**ASTRA'S ANSWER (0161 t39) settles the disposition, and it is not the one the
paragraph above assumed.** There is no Stratum-main landing schedule and she
declines to invent one; she authorizes no merge or push, and a future delivery
to Stratum `main` is a separate coordinated review and qualification plus an
operator publishing step, which she is surfacing to the operator herself. So
this is a **retained external dependency on her branch, not a corona-owned
missing implementation**, and the prescription is hers: keep the exact clean pin
explicit in the build ledger and in `STRATUM_SRC` for compatible builds, because
a default canonical checkout would omit the independent-session rework. Her
commit already carried scoped self-review, all 73 host CTest targets,
actual-function and four-mutant checks and D7 integration, and it records its
own ASan/UBSan waiver and a best-effort disconnect eviction on storage failure.

That drops the canonical-bake experiment: its outcome is known by construction,
she requested no new build, and the controlled red/green pair already supports
the prediction -- spending a shared host to confirm a prediction is a ceremony,
not a measurement. What remains owed here is narrower and is mine: the two
poisoned caches in this `build/`, generated from her tree at an unrecorded
state, which is why ANY full build refuses in this tree and therefore why
`ci-smp-gate.sh` has never run FOR THIS PRIVATE-OWNER CHECKPOINT. Scope that
precisely, because the broader wording erases evidence that stands: the earlier
AS-R9 gate DID run in this tree through the same runbook -- the 5x10 matrix at
`run-1007T061510Z`, which built the sanitizer kernel that had not existed before
it, and which was reviewed separately (astra, 0161 t41). What is unqualified by
a gate script is the private-owner kernel, not AS-R9. Queued for the mac on that
basis -- clear
the caches, bake with the explicit pin, run the real gate -- which is what would
retire the matrix-stage substitution described next. The full build/cache
qualification stays distinct from the already-accepted image-specific matrix
result.

**AND THE BLOCKER ABOVE IS OVERSTATED -- the correction is a method failure, not
a tooling fact.** `work/oct5-as-r9/lease-runbook.sh` ALREADY removes both
poisoned caches: its invalidation stage is unconditional, it deletes the stratumd
CMake tree and the host-stratum cache (and the staged daemons) before building,
and it then calls the REAL `ci-smp-gate.sh` with retained logs and counts D7
witnesses per boot. It ran that way on October 6 and produced the preserved green
pair. So the true statement is narrow: a BARE `build.sh --config ci`, and
therefore a bare `ci-smp-gate.sh`, refuses in this tree. "The gate script cannot
run here" and "no full bake can succeed here" were both too strong. The gate was
runnable through my own runbook the whole time, and when the bare build refused I
reached for the matrix-stage substitute instead of the path written the day
before. The substitution was honestly labelled; it was also avoidable, which is
worse than a tooling limit, because nothing but my own attention was in the way.

**The actual precondition is DISK, and it is a shared one.** The runbook's floor
is 8 GiB and this volume reports 7.7-7.9 GiB free, so it would refuse at stage
`start` -- correctly, since lowering that floor is not a fix. The figure MOVES:
three readings minutes apart ran 7934 -> 7823 -> 7931 MiB, because a peer is
mid-gate on the same volume with an ls-ci bake queued behind it. Everything of
mine that is genuinely expendable -- the invalidated build subset, then the
re-mintable ramfs and disk images -- yielded 108 MiB in total, with the du-vs-df
gap behaving exactly as the APFS-clone lesson predicts. The remaining levers are
not unilaterally mine: ~771 MiB is the preserved green pair that astra asked be
kept and only she should release, and the volume carries six worktrees each with
a multi-GiB `build/`, which is an operator-level prune.

One gap closed while looking: today's qualified kernel is now preserved at
`work/oct5-as-r9/private-owner-qualified-kernel/` with its hashes and what
qualified it. It had existed ONLY in `build/kernel`, which that runbook deletes
unconditionally -- so every verdict recorded above was pointing at a directory
one run away from being erased, while the `cpu1-green-pair/` set preserved beside
it is the October 6 D7 kernel, a different binary.

THAT PRESERVATION WAS INCOMPLETE, and astra caught it (0161 t43): a boot reads
FOUR inputs and the first revision held two. What the matrix actually consumed is
recorded by the run itself at `private-owner-logs/smp-matrix-1007T1040Z.log:3-6`,
and against that oracle the set now stands as:

- both kernel flavours, `.elf` and `.bin`. QEMU boots the FLAT binary, never the
  ELF, and `build/kernel-undefined` was already deleted when the gap was named --
  so `thylacine-undefined.bin` is a RECONSTRUCTION, labelled as one. It is a pure
  function of the retained ELF (`kernel/CMakeLists.txt:343` is a bare
  `objcopy -O binary`), and the regenerated bytes hash to `5193ee5f914f96ae`,
  EQUAL to what the matrix recorded before the loss. Positive control, since a
  reconstruction checked only against itself proves nothing: the same recipe
  reproduces the default `thylacine.bin` exactly, and two `llvm-objcopy` installs
  agree on both. The match is also proof the retained ELF is the matrix's ELF.
- the PRE-BOOT pool and its key twin. `tools/smp-multiboot.sh:315` calls
  `pool_restore` as the first statement of the per-boot loop, so all 50 boots
  started from `pool.img.baked-snapshot` (`9384c245b6f1cb5b`). This also corrects
  a provenance defect of my own: the matrix header hashed the LIVE `pool.img` at
  matrix start (`f948430ed9665b14`), which `pool_restore` overwrote before boot 1
  -- it recorded an input no boot ever read. The script now hashes the snapshot,
  and says so loudly when the snapshot is absent, because that is the one state
  in which boots mutate the pool cumulatively and `pool_restore` is a silent
  no-op.
- `ramfs.cpio` `63d781afe4a6e5f5` -- BYTES LOST, hash-only provenance, stated
  rather than omitted. It is not reconstructible off-lease: the archive is
  assembled from `build/ramfs-src` plus the userspace binaries, and `build/usr`
  went with the same invalidation. A re-bake would be a substitution, not a
  preservation. CONSEQUENCE: the preserved set pins what the kernel and pool
  were, but cannot be booted as a set to reproduce the 10-07 matrix.

The cure is structural rather than remembered: `lease-runbook.sh` now clones all
four inputs out of `build/` right after the build, when they exist together, and
refuses when it finds an input it cannot preserve. It is bounded to two
generations on purpose -- `cp -c` costs no blocks today, but a clone's shared
blocks become real when the original is rebaked (~283 MiB for the sparse pool),
so an unbounded history would leak into the very floor the script guards.
`work/oct5-as-r9/preserve-inputs-test.sh` drives the step as EXTRACTED from the
live runbook (22 checks, 5 scenarios) and is sabotage-tested both ways: a flat
copy that lets one kernel flavour overwrite the other goes 6 WRONG, and removing
the failed-copy refusal goes 2 WRONG.

Because `ci-smp-gate.sh:140` opens with an unconditional `build.sh kernel`, a
BARE invocation of the gate refuses here. This sentence originally read "the gate
cannot run as a script here", which is too strong and is corrected in full
above -- the runbook's unconditional invalidation clears both poisoned caches and
then calls the real gate. The wording is narrowed rather than deleted, but note
the reading order: the correction sits ABOVE this paragraph, so a reader going
top-down used to meet the withdrawn claim last, restated as fact with no marker.
`work/oct5-as-r9/smp-matrix-on-qualified-image.sh`
runs its MATRIX STAGE instead -- the same five rows, the same N=10, the same
per-row timeouts, classifier and pass condition, with the rows and N DERIVED out
of `ci-smp-gate.sh` rather than retyped -- on the image the suite and both legs
already qualified. It is **not** `ci-smp-gate.sh` and is not reported as that
script having passed; what it omits is the gate's own userspace rebuild, which a
bare invocation cannot reach. For a kernel change it is arguably the better
experiment: the image under test is bit-for-bit the one the green suite and both
credited legs ran on, so the kernel is the single variable across all three
bodies of evidence.

Gates unchanged: 128 MiB protection retained; private async, replacement memory
accounting and clipboard all non-default; no syscall reaches
`loom_create_private`. The pi A72/KVM axis remains the named unrun residual.

THE BRANCH IS PUSHED, and nothing is landed. `corona/async-memory` is on both
mirrors by operator decision; `main` reads `cb7194c10` on both, untouched.
Branch only, never `main`, no force, verified per URL with `ls-remote` rather
than from the push's own report, and independently confirmed by astra (0161
t41). Nothing is merged, landed or activated, and the delivery's merge
obligations are unchanged. Earlier sections of this document say "nothing
pushed" and are left as written: they were true when written, and a status
document that edits its own history stops being evidence of anything.


## October 8, 19:58: inner unwind discriminated, bounded Mac scope

Run `inner-leg-20261008T195301Z` completed with exit 0 on `2bf48f7ad`, clean
external Stratum `61dde3727921e70e2c72fbd3c9e2044a192f4a54`. Control passed
1836/1836 with normal fallthrough. Independent pristine-source mutants each
produced exactly one expected failure, 1835/1836 and test.sh rc1:
M1 missing inner kfree -> "the inner ring failure frees the unpublished Loom";
M2 disabled observer -> "a watched large free fires the watch";
M3 removed thread guard -> "a watched free on another thread leaves the watch quiet".
M2 and M3 stop before the inner fault is armed. M1 intentionally leaks one large
allocation until its disposable VM exits; the observer proves free-site ENTRY,
not buddy reclamation.

KERNEL_TESTS-off comparison: no test symbols, identical names; kfree 43
instructions/15 relocations, layout 130/10, create 3/0, receipts 3/0, private
127/13, all byte-identical disassemblies including relocations. Recovery restored
both sources to pristine == HEAD, and ELF/bin match the saved control exactly
(bin ae48cf3dcc59c047, ELF d16bcf72a632f7ff). Mac lease released.

Control inputs moved outside pruning to `inner-leg-control-PINNED-20261008T195301Z`;
all nine hashes verified. The undefined pair in that preserved set is older
856ecae evidence, not new-source sanitizer coverage. Full SHA256s, raw result
checks and shape counts: `astra-takeover/inner-run-20261008T195301Z-result.json`.
Self-review reread all four raw boot logs, asserted sole diagnostics and verdicts,
compared saved disassemblies and verified restored sources and artifacts. This
is single-agent self-review, not an independent audit. Scope is one Mac default
boot per side. Older full gates still cover only their recorded source.

Disk was unblocked by reclaiming unused reproducible native Cargo debug caches
in Main and b2c under the operator resource grant; no tracked/source files,
release outputs, raw evidence or pinned generations were removed. Inventories:
astra-takeover/main-cargo-cache-cleanup.json and b2c-cargo-cache-cleanup.json.
APFS sharing made the first apparent 2.6 GiB deletion reclaim little; removing the
other cache brought actual free space to 9.7 GiB. This run stayed above the 8 GiB
floor, 9943 -> 9468 MiB. Prior floor-refusal evidence remains intact.

NEXT: release-half sensitivity and remaining structural/engine obligations in
checkpoint1-coverage.md, then new-source full gates and approved async-first
engine work before MM0-MM4. No Main landing, private async/clipboard activation,
or removal of the 128 MiB protection is implied by this bounded result.


## October 8, 20:08: lifetime release decrement discriminated

Run release-leg-20261008T200530Z on 2a4e501c7 exited 0. Control 1836/1836,
normal fallthrough, test.sh 0. The successful-retirement omitted-put-only mutant
failed ONLY "private retirement releases its image lifetime reference":
1835/1836, test.sh 1, sole matching diagnostic and suite-failure extinction.
Its diff expands the current private_end body at loom_private_destroy only,
retaining the guard decrement and locking; the shared helper and all rollback
callers remain untouched. The leaked lifetime reference is intentionally left
until disposable VM teardown. This discriminates the decrement while a creator
owner remains. FINAL descriptor/page-table reclamation remains OPEN and separate.

Self-review verified both raw logs, exact attribution, mutant diff, restored
source == pristine == HEAD, and rebuilt ELF/bin == pinned control (bin
 aa7b16dc8fdfad3b). Moved nine verified inputs outside pruning to
release-leg-control-PINNED-20261008T200530Z. Full hashes and scope are in
astra-takeover/release-run-20261008T200530Z-result.json; its undefined pair is
older-source evidence only. Disk 9418 -> 9021 MiB, 8 GiB floor retained. Lease
released. This is single-agent self-review, no independent audit or full-gate
claim. Next: final reclamation witness/remaining checkpoint obligations and
new-source full gates, then the approved engine and MM work. No activation,
Main integration or removal of the 128 MiB protection.


## October 8 Astra takeover: dormant shared slot identity ledger

The approved shared 64-slot identity/reservation/borrow foundation is implemented
in kernel/loom_service_slots.c, with one shared host/guest fixture. Host ASan/UBSan
control and 13 named negative controls pass; the existing lifecycle model clean
case (5828 states) and seven buggy cases pass their expected verdicts. Native
runtime qualification is pending for this changed source. Scope and caller
obligations are in the Loom dossier and work/oct5-as-r9/astra-takeover.md.
The ledger is not yet owned/charged by a private Loom and enables no handler.
Async engine integration still precedes MM0-MM4; all activation protections stay.


October8 22:14UTC: the dormant slot-ledger fixture on ceac7f380 passed the full
Mac matrix,50/50 clean (default smp1/4/8, UBSan smp4/8), each1837/1837 with
separate ledger and private-owner verdicts and complete D7 evidence. Ten captured
input hashes and100 logs verified; minimum recorded admission10757MiB. Detailed
provenance and remaining caller/engine/guest-allocator obligations are in the
latest pickup and astra-takeover/current-gate-20261008T213148Z-result.json.
No activation or Main landing; retain128MiB protection. Self-review only.


Charged slot-owner storage now implemented after the ledger qualification: one
private-only buddy page, admission rollback, lock-serialized close and guarded
retirement. Host controls and native linking pass; guest runtime for this changed
source is PENDING. See the latest pickup for exact evidence and remaining engine
obligations. No service handler or private setup mode has been activated.


October8 23:06UTC: charged slot-owner integration on7c184b3ef completed50/50
clean Mac boots,1838/1838 plus both slot tests, private-owner and complete D7
checks in each. Ten captured inputs and100 log hashes verified; minimum admission
10930MiB. See latest pickup and astra-takeover/current-gate-20261008T222442Z-result.json.
Future pool/protocol ownership and guest final-allocator/Pi gaps remain open.
Self-review only; no public activation, Main landing or128MiB removal.


## October 8, 23:22 UTC: joint pool registration and charged owner storage

The private owner now allocates a zeroed7176-byte pool table (one64-cell bank
plus64 pool descriptors indexed by the existing shared service slot). Joint
prepare reserves both identity and member capacity before output copyout.
Publish rechecks both provisional phases and rejects close without half commit.
Rollback releases both exclusions; reap refuses borrowers, streams, BUSY,
PENDING and LEASED members before modifying either ledger. Exact payload return
permits reap; neither CQ consumption nor slot removal returns a payload. The
ring-global incarnation/nonce counters never reset. All calls require the same
Loom lock and retained owner reference; backing pins are released after unlock.

Construction precharges two additional buddy pages and frees pool storage before
refund on every downstream failure. Retirement requires both tables empty and
frees them before ring settlement, metadata refund and exact image-pin release.
Native linked DWARF measures Loom2872bytes (one buddy page), pool table7176bytes
(two pages), plus the deliberately whole slot page: four metadata pages total.
Ring backing and conservative Burrow descriptor attribution remain separate.
See astra-takeover/pool-table-layout.json and retained native link/DWARF output.

Self-review: under the common lock, pool_publish cannot fail after slot_publish
succeeds because its RESERVED precondition was already checked; rollback and
reap likewise validate all fallible prerequisites before either ledger is freed.
Close blocks new registration/publication without destroying retained payloads.
The existing pre-abort committed-result delivery semantics remain intact.

Bounded ASan/UBSan host evidence: pool-table-20261008T2320Z control plus7 targeted
transaction mutants; slot-owner-20261008T231734Z nine controls plus10 charge/free/
close mutants; final-reclaim-20261008T231743Z both owner-drop orders plus6 exact
omission mutants. Every mutant failed its named assertion. The owner/final-drop
fixtures use checked allocator, geometry, image, lock and VMA doubles, so these
are call/free/order witnesses, not guest physical reclamation or race proofs.
Lifecycle model clean5828 plus7 named counterexamples and buffer models clean464/
6416 plus11 named counterexamples passed in pool-table-*-model-20261008T2312Z.

Native build/link passes. New loom.service_pool_table shares the transaction
fixture with host; loom.private_slot_owner now covers pool-allocation refusal,
downstream unwind and the full metadata charge. Expected suite count1839; the
full Mac gate is pending for this source. Prior50/50 applies only to7c184b3ef.

Still owed before activation: actual authority/resolution and canonical Burrow
pins, accepted fixed-I/O exclusion integration, parent scopes, protocol/worker/
terminal retirement and full client payload lifecycle. These helpers do not
authorize service syscalls. Guest final allocator reclamation and Pi/A72/KVM
remain open. Self-review only, no independent audit. Continue async before
MM0-MM4; no Main landing, private async/clipboard activation or128MiB removal.


## October 9, 00:01 UTC: pool-owner full Mac gate verified

current-gate-20261008T232028Z finished with gate.rc=runner.rc=0 on exact
1295a25da8b546498661c359523917187c01d832, tracked source clean throughout.
All five rows passed10/10 (default smp1/4/8, UBSan smp4/8). Raw-log verification
confirmed1839/1839 on all50, including service_pool_table, private_slot_owner,
service_slots and private-owner normal fallthrough. Every boot passed D7 overlap
and twenty distinct d7user00..19 completed cycles plus the boot banner.
Ten captured input hashes verified and100 serial/harness logs retained with
hashes; wrapper exit also matched live immutable build inputs to capture.
External Stratum remained clean at exact61dde37. Minimum recorded admission
10974MiB exceeds the unchanged8192MiB floor. Mac lease released; inputs PINNED.
Optional Venus/Vulkan programs were absent from this ramfs, not qualified.

Evidence: astra-takeover/current-gate-20261008T232028Z-result.json and the run's
verify.py, verified-logs and input manifest. This source-specific Mac result
qualifies the pool/slot storage and transaction fixtures, not the remaining
private authority, buffer-pin/fixed-I/O exclusion, scope/protocol/worker/terminal
integration. Guest final allocator reclamation and Pi/A72/KVM remain open.
Next: registered-buffer ownership using the approved canonical Burrow and exact
creator/image rules. Async work still precedes MM0-MM4. Self-review only; no
Main landing, private async/clipboard activation or128MiB protection removal.


## October 9: internal private registered-buffer ownership (runtime pending)

loom_private_register_buffers is an internal-only admission path. It requires a
retained private Loom and creator Proc in its stable syscall image, checking both
Proc incarnation and exact retained AddrSpace before resolution. Input is an
immutable kernel snapshot; there is no new usercopy or public dispatch. Existing
ANON/writable/contiguous single-VMA resolution takes actual Burrow pins. The
ring's own backing is refused. The complete new set resolves before installation;
partial refusal leaves the prior table intact. Under the Loom lock, close or any
provisional/live pool refuses replacement. Success swaps the whole bounded table.
Speculative or displaced pins settle outside locks against the retained image,
never by rediscovering a Proc or following its post-exec address space.

Final private retirement still requires empty service/pool ledgers and no legacy
worker/operation/fid state. It now clears and settles registered-buffer pins
before freeing tables, settling the ring and dropping the retained image last.
No new allocation/charge is needed for this table: it already resides in Loom.
Pool-member canonical resolution and independent pin adoption, accepted fixed-I/O
exclusion and protocol/worker retirement remain separate unfinished integration.
Public private setup, registration and I/O remain disabled.

Self-review: resolution holds the exact image lock, installation the Loom lock,
and neither encloses pin destruction. Exec's sole-thread swap latches private
rings before replacing the image; internal callers must obey the documented
stable-syscall-image requirement. A held Loom reference prevents final table
retirement during admission. Existing AS-R9 settled drops determine refunds
inside the Burrow lifetime lock; no claim/drop/restore window is introduced.

Host ASan/UBSan private-buffers-20261009T000936Z passed control plus7 exact
authority/ring/pool/close/drop/refund mutants using checked resolution, image,
Burrow-settlement and lock doubles. The first harness attempt's wrong-image
mutant hit its resolver-lock assertion before its intended authority assertion;
that evidence is retained in private-buffers-20261009T000859Z. The corrected leg
uses an empty-table request to isolate authority. See the correction JSON.
Updated slot-owner-20261009T000729Z passes nine controls plus10 mutants;
final-reclaim-20261009T000739Z passes both owner-drop orders plus6 mutants.
Those destructor controls exercise empty buffer tables, with real buffer pins
covered separately by the new native fixture. Lifecycle model clean5828 and
all7 named counterexamples pass (private-buffers-model-20261009T0008Z).

Native compile/link passes. New loom.private_buffers covers real pin counts,
partial-resolution rollback, exact creator/image rejection, own-ring rejection,
provisional/live-pool replacement refusal, unmap-then-clear, foreign-payer final
pin release, close refusal and final retirement refund. Expected count1840;
runtime/full Mac qualification is pending for this changed source. Previous
50/50 applies only to1295a25da. This is self-review, not independent audit.
Guest final allocator reclamation and Pi/A72/KVM remain open. Continue async
before MM0-MM4; no Main landing, private async/clipboard activation or128MiB removal.


## October9: private-buffer native fixture failure and correction

The3dcff4cb9 full matrix was stopped after six completed default-smp1 boots
reproduced the same sole failure: loom.private_buffers / foreign mappings
detached,1839/1840. No source qualified. Astra stopped its own process group;
wrapper exit143 retained clean-source and input checks and released Mac. Twelve
raw serial/harness logs, hashes and exact failed inputs are retained under
current-gate-20261009T001138Z, marked PINNED as FAILED.

Cause: the new fixture mapped foreign backing at0x60000000, below the burrow
window accepted by SYS_BURROW_DETACH. The existing legacy foreign-payer fixture
explicitly documents this constraint and uses0x140000000. The correction uses
that in-window address and holds the destination image lock around the share,
as required by the mapping API. Production buffer ownership code is unchanged.
This was a fixture defect, not evidence of a platform timing problem.

A fresh rebuilt default SMP1 diagnostic passed1840/1840, including private_buffers,
pool/slot/owner fixtures, D7 overlap and twenty distinct completed login cycles,
and boot banner. Recorded source is3dcff4cb9 plus the exact fixture-only diff;
ELF/bin and input hashes are retained in
astra-takeover/private-buffers-diagnostic-20261009T001526Z. Build/input/source
stability checks passed. This one boot does not replace the full Mac matrix;
a new clean-source matrix is still required. Prior pool-owner50/50 remains
bounded to1295a25da. Self-review only; no private activation, Main landing or
128MiB removal. Continue remaining async ownership/protocol work before MM0-MM4.


## October 9: corrected private-buffer full Mac gate verified

Exact source b010ace93fae59f5bb28856d7b5456200aa8e60c passed the full
50-boot Mac HVF matrix in current-gate-20261009T001717Z: ten boots each at
default SMP1/4/8 and UBSan SMP4/8, all 1840/1840. Every boot includes
loom.private_buffers, the pool/slot/owner fixtures, D7 overlap, twenty distinct
completed d7user00..19 login cycles and the boot banner. Both runner and gate
exit zero. All 100 copied serial/harness logs and ten captured inputs were
hash-verified; see astra-takeover/current-gate-20261009T001717Z-result.json and
the run's verify.py. Captured inputs are PINNED. External Stratum is exact clean
61dde3727921e70e2c72fbd3c9e2044a192f4a54. Minimum recorded admission free space
is 10399 MiB, above the 8192 MiB floor; the runner released Astra's Mac lease.

This qualifies the corrected private registered-buffer fixture and recorded
source on Mac HVF. The failed 3dcff4cb9 run remains retained as failed evidence.
Optional Venus/Vulkan probes were absent from this ramfs; no graphics claim.
Guest final allocator reclamation and Pi/A72/KVM remain open. Canonical pool
member pin ownership, fixed-I/O exclusion and the remaining async protocol/worker
integration precede MM0-MM4. Self-review only, not an independent audit. No Main
landing, private async/clipboard activation or removal of the 128 MiB protection.


## October 9: canonical private pool member pins (internal only)

The internal pool front-end now validates the complete immutable create record,
creator incarnation/image and receipt geometry, then resolves registered slices
to canonical Burrow identities and physical byte offsets under the Loom lock.
It uses subtraction bounds before addition, rejects ring backing and nonzero
unused/output fields, and lets the joint ledger reject overlap even through
different registered aliases or provisional pools. Every accepted member owns
one independent Burrow pin before the lock is released. No allocation or
usercopy occurs inside that lock. The buffer table remains stable while pooled.

Publish is a retained-owner continuation after future copyout. Rollback/reap
snapshot member pins under the same lock as the ledger transition; refusals
drop none. Successful removal drops the saved pins outside the lock using the
retained creator image's settled-drop primitive, safe even if buffer-table
replacement wins the unlocked gap. These wrappers must only remove pools they
created; raw ledger-only fixtures do not transfer pins to this front-end.

Self-review: source and table pins keep canonical addresses alive; no Proc is
rediscovered during removal, and no Burrow is read after dropping its pin.
One pin is released per member, including disjoint members sharing a Burrow.
Close rejects prepare and publication, but permits rollback/reap by a retained
kernel owner. The unexpected-legacy-work guard rejects inflight, admitting,
held-chain and rearm state. This is NOT the future exact fixed-I/O exclusion
implementation: private dispatch stays disabled, and accepted fixed extents
must be integrated under the same lock before activation. User authority at
continuation syscalls, copyout wiring, close/worker cleanup, protocol admission
and guest final allocator reclamation remain open. Self-review, not an audit.

Host private-pool-pins-20261009T011324Z passes ASan/UBSan control plus ten exact
mutants for image/creator, unused tail, canonical alias displacement, ring
backing, fixed admission, member pin acquisition, rollback drops, refusal drops
and final refund. Checked image/Burrow/lock doubles include table replacement
after unlock; they are sequential witnesses, not a concurrent allocator proof.
Initial harness attempts are retained: 011209Z had a misspelled ledger-close
function; 011240Z's ring-rejection mutant hit the double's zero-pin precondition
before the intended assertion. Giving the fake ring its real-world owner pin
made that mutant reach the intended assertion. Neither correction changed
production code or weakens its checks.

Native loom.private_pool_pins checks actual alias overlap, canonical offsets,
independent pin counts, provisional overlap/rebinding refusal, rollback identity
burning, stale removal, busy reap, unmap before reap, exact final table refund
and close before publication. Fresh default SMP1 diagnostic passed 1841/1841,
D7 overlap, all twenty distinct cycles and boot banner. Evidence is
astra-takeover/private-pool-pins-diagnostic-20261009T011326Z: source 1e06cc3d3
plus captured exact diff, stable rebuilt ELF/bin and six input hashes, raw logs.
Native link passed. Existing lifecycle/payload models and generic ledgers are
unchanged; their prior model results are not a proof of this new C front-end.

Full Mac qualification remains pending for the new source. The prior 50/50
remains bounded to b010ace93. Continue remaining async before MM0-MM4; no Main
landing, private async/clipboard activation or removal of the 128 MiB protection.


## October 9: canonical pool-pin full Mac qualification

Exact source 6200c9de0121abbfa08bea719f904f20ae5a9ac6 passed all 50 Mac HVF
boots in current-gate-20261009T011552Z. Default SMP1/4/8 and UBSan SMP4/8
each passed ten boots, every boot 1841/1841 including loom.private_pool_pins,
private buffers, pool/slot/owner rows, D7 overlap, twenty distinct d7user00..19
cycles and boot banner. Gate and runner exit zero. The verifier checked all
100 copied raw serial/harness logs and ten captured input hashes; inputs are
PINNED. Stratum remains exact clean 61dde37. Minimum recorded admission free
space was 9961 MiB, above the 8192 MiB floor. Astra's Mac lease was released.
See astra-takeover/current-gate-20261009T011552Z-result.json and run verify.py.

This is source-specific Mac qualification of internal canonical pool pins.
Exact fixed-I/O extent reservations, usercopy/authority dispatch and scope/
protocol/worker integration remain pending. Pi/A72/KVM, guest final allocator
reclamation and absent optional Venus/Vulkan probes remain unqualified.
Self-review only. Async remains ahead of MM0-MM4; no Main landing, private
async/clipboard activation or removal of the 128 MiB protection.


## October 9: internal fixed-buffer reservations and pool exclusion

Private fixed-buffer borrows now use a 40-byte node embedded in a caller-owned,
precharged request. The caller must supply an immutable kernel member snapshot
from a retained creator in its stable syscall image. Admission resolves through
the same canonical registered-buffer helper as pools, checks even provisional
pool extents under the Loom lock, and takes one Burrow pin and one Loom reference.
Pool preparation checks these accepted fixed extents under that same lock.
Touching/disjoint intervals remain legal; different registered aliases cannot
bypass exclusion. Duplicate nodes and reservations exceeding existing CQ
terminal-obligation capacity are refused. This bounds list traversal without
raising any existing limit. Actual shared terminal admission remains the future
request engine's obligation before dispatch; this helper does not dispatch.

Release requires all held/running/cancelled local readers and writers to end.
It verifies exact node membership, unlinks/clears under the Loom lock, then drops
backing and settles against the retained image before releasing the Loom ref.
Wrong-owner/double release changes nothing. Rebinding the registered table does
not lose accepted fixed pins or extents; final close cannot retire the owner
while any fixed borrower remains. The destructor fails closed if the ledger is
nonempty despite reaching zero references. No allocation, usercopy, worker wait
or final object destruction occurs inside the Loom lock.

Self-review: list/node ownership is kernel-only and exclusive to the retaining
request; no user token is trusted. Backing/owner refs are acquired before list
publication and released only after unlink. The removal path never uses the node
or Loom after its final unref. The common extent resolver avoids divergent alias
arithmetic; overlap uses half-open physical intervals and rejects invalid ranges.
Worst-case pool preparation scans at most 64 members times existing CQ capacity;
progress/latency qualification with the actual request engine remains owed.
Native DWARF reports Loom 2888 bytes (0xb48), still one charged buddy page; fixed
nodes must be included in future request-storage charges, not separately allocated
by this helper. Existing four owner metadata pages remain unchanged.

ASan/UBSan private-fixed-20261009T020251Z passes control plus ten exact mutations
for image/creator, backing/owner pins, both exclusion directions, capacity, both
release obligations and close refusal. Refreshed canonical pool owner controls
(private-pool-pins-20261009T020300Z) pass control plus ten exact mutants. Refreshed
slot-owner-20261009T020340Z passes nine controls and ten mutants, and
final-reclaim-20261009T020349Z passes both owner-drop orders and six mutants.
These use checked host doubles and do not establish concurrent protocol or
guest allocator correctness. Existing formal models are unchanged; prior model
results are not presented as a proof of the new C admission/retention path.

Fresh native default SMP1 passed 1842/1842, including loom.private_fixed, pool
pins/buffers/owner fixtures, D7 overlap, all twenty distinct cycles and banner.
Evidence: astra-takeover/private-fixed-diagnostic-20261009T020341Z captures
93db78ecd plus exact tested diff, rebuilt ELF/bin, six stable input hashes and
raw logs. The final header change only clarifies the immutable/stable-image
caller precondition. Native link passed; full clean-source Mac matrix remains
pending for this new source. Previous 50/50 is limited to 6200c9de0.

Remaining async work includes actual request/CQ admission and local-user
release wiring, target/scope/fid ownership, copyout/authority dispatch and bounded
protocol/worker retirement. No public private-service activation, Main landing,
clipboard enablement or removal of 128 MiB protection. Pi/A72/KVM and guest final
allocator reclamation remain open. Continue async before MM0-MM4; self-review
only, not an independent audit.


## October 9: fixed-reservation full Mac qualification

Exact source d3563f828652709ceb34cc3a94cddf9b42b6d9e0 passed all 50 Mac HVF
boots in current-gate-20261009T020608Z. Default SMP1/4/8 and UBSan SMP4/8
each passed ten boots, every boot 1842/1842 including loom.private_fixed,
pool pins/buffers/slot/owner rows, D7 overlap, twenty distinct d7user00..19
cycles and boot banner. Both runner and gate exit zero. The verifier checked
100 copied raw serial/harness logs and ten captured input hashes; inputs are
PINNED. Stratum remains exact clean 61dde37. Minimum recorded admission free
space was 9355 MiB, above the 8192 MiB floor. Astra's Mac lease was released.
See astra-takeover/current-gate-20261009T020608Z-result.json and run verify.py.

This qualifies the internal fixed-reservation checkpoint on Mac. Actual request/
CQ admission, target/scope/fid ownership and protocol/worker integration remain
open. Pi/A72/KVM, final guest allocator reclamation and absent optional graphics
probes remain unqualified. Self-review only. Continue async before MM0-MM4; no
Main landing, private async/clipboard activation or 128 MiB protection removal.


## October 9: native target ownership in private rings

Internal target preparation now validates one immutable 288-byte kernel snapshot,
checks the retained creator and exact image, reserves from the shared 64-slot
ledger, then resolves the actual registry fd with handle_get. The existing native
capture API enforces CWALKONLY native roots, strict 9P posts, exact registry view,
service slot and post generation; no fallback registry, peer I/O or connect
permission is introduced. Full input tails and output fields must be zero.
The retained registration transaction survives an unlocked capture/close race:
close winning before publication rolls back the slot and drops captured refs.
Provisional targets consume quota but cannot be borrowed. Copyout failure burns
the incarnation and rolls back; publish remains an internal continuation.

A 4096-byte precharged table holds 64 native 64-byte targets, indexed by the
shared identity slots rather than adding another quota. Loom is now 2896 bytes
(0xb50), still one buddy page; total owner metadata is five pages (Loom one,
slots one, pool table two, target table one). A one-shot target allocation refusal
is consumed even on invalid geometry. Every construction failure frees prior
storage before refunding the exact retained image.

Target get pins the shared live slot and Loom under the same lock. Reap refuses
borrowers and stale identities before changing either payload or slot. Successful
rollback/reap detaches the native reference under lock and destroys it outside.
Put unpins before dropping the Loom outside the lock, with no subsequent owner
access. Zero owner references therefore imply no target borrowers or retained
registration transactions. Final retirement clears remaining live/provisional
targets outside locks; scopes, fids and pools still require their own retirement.
CONNECT must still capture actual caller identity and use native admission checks.
No syscall entry or private runtime activation is included.

Self-review covered close during unlocked capture, publication, stale reference
removal, quota consumption, borrowed payload stability, final put, exact-image
charge retention and all allocation unwinds. The native target fixture uses a
real posted registry and navigation fd without a responder: closes the fd/root
and poster, proves the registry remains retained, then proves the last borrower
retires the closed ring and destroys the registry exactly once. This is an
internal owner fixture, not protocol or public usercopy qualification.

Validation: native default SMP1 passed 1843/1843, D7 overlap, twenty distinct
cycles and banner; source 0efb3c9bb plus captured diff and new target header in
astra-takeover/private-targets-diagnostic-20261009T025959Z, rebuilt ELF/bin and six
stable input hashes. Final declaration-only edits reorder fault declarations, add the test forward
prototype and clarify continuation retention. ASan/UBSan private-targets-20261009T030105Z passes
control plus twelve exact mutations; slot-owner-20261009T025921Z passes eleven
controls and thirteen mutants; final-reclaim-20261009T025933Z passes both owner
drop orders and seven mutants. Host boundaries use checked doubles and establish
neither concurrent protocol execution nor guest allocator reclamation. Two host
harness corrections and the diagnostic marker-parser correction are recorded
with untouched failed/raw logs. Full current-source Mac matrix is pending;
prior 50/50 remains limited to d3563f828.

Next: actual scope/fid/request/CQ admission, authority/usercopy dispatch, and
bounded protocol/worker integration before MM0-MM4. Pi/A72/KVM and final guest
allocator reclamation remain open. Self-review only, no independent audit.
No Main landing, private async/clipboard activation or 128 MiB protection removal.

The existing lifecycle model was rerun in private-targets-model-20261009T0303Z:
clean 5828 states and all seven named counterexamples passed. This validates the
existing finite abstraction, not the new C target authority/locking path.


## October 9: native target full Mac qualification

Exact source 4ac2790caa82653cf3a5bcf0f77aa5367c59535b passed all 50 Mac HVF
boots in current-gate-20261009T030505Z: default SMP1/4/8 and UBSan SMP4/8,
ten each. Every boot passed 1843/1843, including native target ownership,
private fixed/pool/buffer/owner fixtures, D7 overlap, twenty distinct d7user00..19
cycles and banner. Both gate and runner exit zero. Verification retained and
hashed 100 raw serial/harness logs and all ten captured inputs; inputs PINNED.
Minimum recorded admission free space 8786 MiB exceeds the 8192 MiB floor.
Stratum remained exact clean 61dde37; Astra's Mac lease was released. Evidence:
astra-takeover/current-gate-20261009T030505Z-result.json and run verify.py.

This is Mac qualification of internal target ownership, not complete private
async support. Continue scope/fid/request/CQ admission, usercopy/authority and
bounded protocol/worker retirement before MM0-MM4. Pi/A72/KVM, final guest
allocator reclamation and absent optional graphics probes remain unqualified.
Self-review only. No Main landing, private async/clipboard activation or removal
of the 128 MiB protection. Preserve all pinned artifact generations; current
free disk is about 8.5 GiB, so recheck admission before further heavy work.


## October 9: shared scope/fid reservation metadata

The new internal loom_service_nodes module joins SCOPE/FID reservation metadata
to the existing shared 64-slot ledger. SCOPE requires a zero parent; FID requires
an exact published READY SCOPE. A child pins that parent before reserving its own
slot, including while provisional across copyout. Capacity refusal unwinds the
pin and leaves output unchanged. Rollback burns identity, clears both ledgers
and unpins the parent. Publication rechecks parent READY, so abort during copyout
wins without silently publishing a child. Ring close also rejects publication.

Only unused RESERVED nodes with zero own borrows can use reap_unused. A parent's
child pins prevent reuse, stale child refs cannot clear replacements, and child
cleanup can release its parent after close/abort. READY/ADMITTED/ABORTING/RETIRED
objects require future engine retirement with protocol/local/terminal proofs;
this helper cannot bypass them. The module does no allocation, usercopy, callbacks
or locking internally: all calls require the owner's shared ring lock and a
retained owner. Each node is 24 bytes, the table 1536 bytes. It is compiled and
fixture-tested but NOT YET allocated, charged or attached to a private Loom;
no new runtime owner storage or private syscall activation is claimed.

Self-review checked parent pin acquisition before slot publication, quota-failure
unwind, exact parent/child incarnations, provisional invisibility, parent abort
versus child publish, and refusal-before-mutation on reap. The shared native/host
fixture explicitly simulates parent READY/ABORTING/RETIRED: it is not a handshake,
authority, concurrent-lock or final-protocol-retirement witness.

ASan/UBSan nodes-20261009T0355Z passes control plus eleven exact mutants; its input
hashes include both the new module and actual shared slot source/header. The
initial nodes-20261009T0352Z also passed, before expanding provenance coverage.
Existing lifecycle model nodes-model-20261009T0355Z passes clean 5828 states and
seven named counterexamples; it is the existing abstraction, not a C proof.
Fresh default SMP1 passed 1844/1844, D7 overlap, twenty distinct cycles and banner.
Evidence nodes-diagnostic-20261009T035230Z retains source579329177 plus exact diff,
rebuilt ELF/bin, six stable input hashes and raw logs. Full current-source Mac
matrix remains pending; previous 50/50 is limited to target source4ac2790ca.

Next: integrate charged node storage and unused-node close cleanup into the
private Loom, then scope/fid protocol resources, request/CQ admission, usercopy
and bounded worker progress; continue async before MM0-MM4. Preserve the 128 MiB
protection and disabled private async/clipboard; no Main landing. Self-review
only, no independent audit. Pi/A72/KVM and final guest allocator reclamation
remain open. To preserve disk headroom, 700 duplicate build archive logs were
removed only after byte/hash equality with retained canonical verified logs;
log-cache-reclaim-20261009T0355Z.json records every pair. No pinned input or
canonical raw evidence was removed.


## October 9: scope/fid metadata Mac gate verified

Exact source c5abd02647e9744eb200afe65e4530b19b5cd5af passed all 50 Mac HVF
boots in current-gate-20261009T035529Z: default SMP1/4/8 and UBSan SMP4/8,
ten each. Every boot passed 1844/1844, including loom.service_nodes and the
existing target/fixed/pool/buffer/owner fixtures, D7 overlap, twenty distinct
d7user00..19 cycles and banner. Gate and runner exited zero; raw verification
retained and hashed 100 serial/harness logs and all ten captured inputs. Inputs
are PINNED. Minimum recorded admission free space was 8282 MiB, above 8192.
Astra's Mac lease was released. Result: astra-takeover/
current-gate-20261009T035529Z-result.json and the run's verify.py.

This verifies metadata with explicitly simulated parent protocol states. Charged
node owner storage, actual scope/fid protocol resources, request/CQ admission,
usercopy/authority and worker retirement remain next, before MM0-MM4. Pi/A72/KVM,
final guest allocator reclamation and absent optional graphics probes remain
unqualified. Self-review only; no Main landing, private async/clipboard activation
or removal of the 128 MiB protection. Preserve pinned artifact generations.


## October 9: charged node owner and unused-node retirement

Private Loom now owns the 1536-byte SCOPE/FID metadata table in a separately
allocated, zeroed 4096-byte buddy page. Charge is reserved before allocation;
constructor failures free node/target/pool/slot storage before refunding and
dropping the exact image pin. The node allocation fault is consumed even by
early geometry refusal. Loom is 2904 bytes (native DWARF 0xb58), still one buddy
page: metadata is now six pages including slot, pool, target and node storage.
No syscall ABI or private activation changes.

At zero owner references, the retire worker has exclusive access. It cleans
unused FIDs before unused SCOPEs, using exact rollback for provisional nodes
and unused reap for live nodes; child cleanup releases its parent pin first.
Only RESERVED nodes with no own borrows qualify. READY/ADMITTED/ABORTING/RETIRED,
invalid identities and orphan payloads fail closed: actual protocol retirement
is still the engine's obligation. Node storage is freed before the final image
refund/private_end, outside locks. There is no peer I/O, wait or usercopy here.

Self-review checked whole-page charging against allocation size, all unwind
edges, last-owner exclusivity, child-before-parent ordering, stale/borrow/state
guards and final image lifetime. The native owner fixture checks node allocation
refusal, zeroed storage, complete charge, automatic provisional/live unused-scope
cleanup and restoration of image charge/refcount. Host control explicitly
simulates parent states for child cleanup; this is not a protocol witness.

Evidence: slot-owner-20261009T044245Z passes 19 controls and 18 exact ASan/UBSan
mutants, including reversed cleanup order, omitted cleanup, charge/free/unwind
and orphan guards. Initial slot-owner-20261009T043850Z passed 19 controls and
16 mutants before the two extra sensitivity checks. final-reclaim-20261009T043907Z
passes both owner-drop orders and eight omission mutants at the host allocator
boundary. Native link succeeded; node-owner-diagnostic-20261009T044103Z passed
1844/1844, D7 overlap, twenty distinct cycles and banner, source cfa1c5c63 plus
its captured diff with six stable input hashes and retained ELF/bin. Existing
lifecycle model node-owner-model-20261009T0442Z passes clean 5828 states and
seven named counterexamples. Full Mac coverage for these owner changes is
pending; prior 50/50 belongs to c5abd0264. No independent audit is claimed.

Next: actual scope/fid protocol resources and retirement, request/CQ admission,
usercopy/authority and bounded worker progress, then MM0-MM4. Private async and
clipboard stay disabled; keep the 128 MiB protection and do not land Main.
Pi/A72/KVM, final guest allocator reclamation and optional graphics remain open.

Disk admission headroom was restored by reclaiming five idle, ignored, untracked
native Cargo debug caches under the operator resource grant, from Main and old
Aux checkouts. No source, release outputs, run evidence or pinned generation was
removed. native-debug-cache-reclaim-20261009T0443Z.json inventories exact paths
and files; measured free space rose from 8164 to 8737 MiB. Full gate admission
will independently enforce the unchanged 8192 MiB floor.


## October 9: charged node owner full Mac gate verified

Exact source 262ff29d3fe306dd700481f36d1fa4dd40aaede0 passed 50/50 Mac HVF
boots in current-gate-20261009T044430Z: default SMP1/4/8 and UBSan SMP4/8, ten
each. Every boot passed 1844/1844, including charged private owner/unused-node
cleanup and existing node/target/fixed/pool/buffer fixtures, D7 overlap, twenty
distinct login cycles and banner. Both exits were zero; verified 100 raw logs
and all ten input hashes, and marked inputs PINNED. Minimum recorded admission
free space was 8208 MiB, above the unchanged 8192 floor. Mac lease released.
Result: astra-takeover/current-gate-20261009T044430Z-result.json.

This is internal owner/metadata coverage, not complete private async support.
Actual scope/fid protocol resources and retirement, request/CQ admission,
usercopy/authority and bounded worker progress remain next before MM0-MM4.
Pi/A72/KVM, final guest allocator reclamation and absent optional graphics probes
remain unqualified. Self-review only. Keep private async/clipboard disabled,
128 MiB protection and all pinned evidence; no Main landing. Current free disk
is only about 8 GiB: restore headroom before the next build/gate without lowering
admission or removing canonical evidence.

## October 9 05:42 UTC: scope/FID owner transactions

Implemented internal creator/image-bound prepare/publish/rollback/unused-reap
entry points over the existing charged tables. Immutable 48-byte record
validation and same-Loom-lock publication preserve provisional visibility,
parent pins and burned identities across copyout failure, abort and close.
Caller retains the Loom across every continuation; no protocol or syscall
activation. Self-review performed (not independent).

Verification: extracted actual-C ASan/UBSan control plus 13 expected named mutant
failures (private-nodes-20261009T053849Z); native diagnostic
node-transactions-diagnostic-20261009T053938Z passes 1845/1845, D7 overlap,
twenty distinct d7user00..19 cycles and boot banner, with stable source diff,
added fixture hash and six input hashes. Lifecycle model clean5828 and all seven
named counterexamples pass (node-transactions-model). Full current-source Mac
matrix remains pending; prior 50/50 applies only to262ff29d3.

Operator cleanup removed work/oct5-as-r9 while this turn started. With explicit
operator permission, all2698 tracked files were restored exactly from HEAD;
large untracked artifacts were not restored. Historical logs/results remain,
but old untracked pinned inputs and flight JSON files are absent locally; do
not claim those historical binaries can currently be rechecked. Current build
inputs survived and a new diagnostic retains fresh ELF/bin clones. Disk is now
above16GiB; floor remains8192MiB. Astra's six pending files and four protected
drafts verified unchanged; Stratum remains clean61dde37.

Next: complete the current-source gate, then charged request/terminal records
before native connection publication, actual scope/protocol ownership, bounded
progress and local retirement. AS-3/4 and then MM0-MM4 follow. Main landing,
private async/clipboard activation and128MiB removal remain unauthorized here.

## October 9 06:28 UTC: scope/FID transactions full Mac gate verified

Exact source `d4825ae1e106924d67d3eb405180da7f8d5882d6` passed
`current-gate-20261009T054208Z`: 50/50 boots, default SMP1/4/8 and kernel
UBSan SMP4/8, ten each. All boots passed1845/1845 including the new
`loom.private_node_owner` fixture, D7 overlap, twenty distinct d7user00..19
login/logout cycles and the boot banner. Both gate.rc and runner.rc are0.
The verifier retained and hashed100 raw serial/harness logs and checked all ten
captured input hashes against the clean source/end-source and Stratum61dde37
provenance. Lowest recorded admission headroom16194MiB;8192MiB floor intact.
The runner released Astra's Mac lease. Result:
`astra-takeover/current-gate-20261009T054208Z-result.json`; verifier beside run.
Astra's pending files and four protected drafts, plus clean Stratum pin, were
rechecked unchanged after the run. This is self-verification, not an independent
audit. Optional graphics probes remain absent from this exact ramfs; Pi/A72/KVM
and final guest allocator reclamation remain unqualified.

This qualifies the internal reservation owner checkpoint only. Next: charged,
bounded request records and terminal-completion obligations before native
backlog publication, then actual scope/protocol binding, caller authority,
bounded progress and local retirement. Private service registration/CONNECT and
clipboard remain disabled, Main untouched,128MiB protection retained. MM0-MM4
still follow completion of async integration and its client/consumer gates.

## October 9 06:53 UTC: request/terminal owner checkpoint

Continue autonomously through checkpoints, per the operator's latest explicit
direction; no handoff/confirmation between verified implementation steps.

Added the bounded request ledger (56-byte records,32-byte table descriptor),
CQ-sized precharged buddy storage on private Loom, allocation-fault unwind and
unused-only zero-owner cleanup. Each record owns an exact scope pin and monotone
request identity. Terminal commit requires local borrow count zero, rejects
fresh success after abort/close, and preserves prior committed success across
CQ pressure. Delivery and storage reap are separate. Detach/discard checks do
not replace the engine's actual local-quiescence proof. Actual private terminal
posting uses kernel CQ geometry, retains full-CQ obligations, clears receipts
and wakes after unlock. Private syscall activation remains off.

Native diagnostic request-owner-diagnostic-20261009T064808Z:1847/1847, new ledger
and owner/CQ fixtures, D7 overlap +20 distinct cycles +banner; exact source diff,
three added-source hashes and six inputs stable. Native Loom size2936(0xb78),
one page; small-ring total metadata now7pages. ASan/UBSan: ledger1+13mutants,
post1+6mutants, owner27controls+23mutants including128/8192-entry geometry and
budget refusal, final-reclaim both orders+9mutants (host boundary only).
Lifecycle clean5828/seven expected counterexamples pass. Initial harness-only
unused-parameter/stale-free-count failures preserved and corrected. Self-review
performed; no independent audit. Full current-source gate pending; previous
50/50 belongs only to d4825ae1e.

Next continue directly: reserve real per-scope protocol storage and caller
identity, bind native devsrv prepare/publish + preallocated p9 progress to these
request/terminal owners, wire bounded progress and retirement, then AS-3/4 and
MM0-MM4. No Main landing, private activation, clipboard activation or128MiB
removal. Keep8192MiB floor and clean external Stratum61dde37; preserve protected
Astra files and operator cleanup. All earlier completed monitors stay paused.


## October 9 07:46 UTC: request gate verified; charged protocol preparation

The request/terminal owner source45d26893bd8b5e16a3934eed008ae4804f5bff38
passed all50 Mac HVF boots in current-gate-20261009T065820Z: default SMP1/4/8
and kernel UBSan SMP4/8, ten each. All50 passed1847/1847 including request
ledger/private CQ posting, D7 overlap, twenty distinct d7user00..19 cycles and
banner. Both rc0; verifier retained100 raw logs and verified ten captured input
hashes, clean source/end-source and clean Stratum61dde37. Minimum recorded
admission15572MiB, floor8192 intact. Result:
astra-takeover/current-gate-20261009T065820Z-result.json. Mac lease released.

Continued directly into real prepublication protocol ownership. The new internal
loom_service_protocol owner borrows an exact registered target, snapshots the
live creator's stripes plus exact pinned image, prepares a fresh native SrvConn
with ordinary weighted credits, and replaces the target borrow with its own
Loom reference. It reserves the complete image page charge before allocation,
then initializes the existing allocation-free client and exclusive native try
adapter. Tversion is queued locally; no backlog publication or byte I/O occurs.
Close is rechecked before returning ownership. Future CONNECT publication must
still reserve the request/destination first and recheck deadline/abort under Ll.

Native owner size42208 bytes (DWARF0xa4e0), rounded to16 buddy pages. Both RX and
TX are separate class-sized allocations, disjoint from the client as required
by the existing API: normal32KiB pays32 pages total, bulk128KiB pays80. This is
additional to ring metadata and SrvConn's weighted credits, not MM0-MM4 global
physical accounting. No bulk degradation or uncharged cache allocation at init.
Exclusive unused release refuses a published connection unchanged. Otherwise it
aborts/destroys the local cursor, releases native ownership, frees TX/RX/owner,
refunds the retained exact image and drops Loom LAST, all final drops outside
Ll/as locks. This cannot certify retirement of an admitted scope or live borrows.

Self-review (not independent) is preserved in protocol-draft/self-review.txt.
It caught and corrected an initial inline-TX alias against the preallocated
API's disjoint-buffer contract before integration. Final actual-owner-C host
ASan/UBSan evidence control-5 passes20 controls and11 named mutants, including
that alias, all charge/free/ref edges, late close and published-cleanup refusal.
Its boundary doubles are not a wire or guest physical-reclamation proof.

Fresh native diagnostic protocol-owner-diagnostic-20261009T074413Z passes
1848/1848, the actual native protocol-owner fixture on both buffer sizes,
allocation/charge refusal, actual creator capture, absence of peer effects,
unused cleanup and final Loom/image retention, plus D7 overlap/twenty distinct
cycles/banner. Exact HEAD+diff, new source/header hashes, six input hashes and
rebuilt ELF/bin retained and stable. This newer implementation still owes its
full matrix; prior50/50 is limited to45d26893b.

Failures retained and resolved: the initial native link emitted a freestanding
memset libcall for the large fixture initializer; explicit byte-zeroing repaired
it. Native diagnostic074015Z then failed1847/1848 because a bare proc_alloc
fixture is absent from the kproc-rooted production identity walk. The fixture
now uses existing proc_test_link/unlink and checks a positive identity snapshot
before negative cases; production authority checks were not weakened. Initial
Apple clang17 host ASan startup stalled before main; a retained process sample
shows recursive sanitizer/dyld/malloc initialization, and the established LLVM22
plus explicit SDK toolchain passes. No failed evidence was discarded.

Continue autonomously through the new full gate and then bind prepared protocol
ownership to exact scope/request publication, bounded executor progress and
actual local retirement, followed by AS-3/4 and MM0-MM4. Do not rerun the completed
protocol-draft installer/supervisor: their historical failure and manual recovery
are recorded; canonical implementation is now in kernel/. Protected Astra files
and clean Stratum pin rechecked. Private activation/clipboard remain disabled,
Main untouched,128MiB protection retained. Pi/A72/KVM, final guest allocator
reclamation and absent optional graphics remain unqualified. Self-review only.

## October 9: full protocol-preparation gate verified after TUI migration

Source fa7e6c5bcdf0d8bdffecdbca7720f95e3cd1d20f passed all 50 Mac HVF boots
in current-gate-20261009T075147Z: default SMP1/4/8 and kernel UBSan SMP4/8,
ten each. Every boot passed 1848/1848 including loom.private_protocol,
D7 overlap, twenty distinct login/logout cycles and boot banner. Both runner
and gate exited zero. Verified 100 raw serial/harness logs and ten captured
input hashes; complete generation now PINNED. Minimum recorded admission
14708 MiB, above the unchanged 8192 MiB floor. Results are in
astra-takeover/current-gate-20261009T075147Z-result.json.

The TUI initially permitted only read-only access to these project checkouts;
operator restored full access and the same verifier has now retained the
canonical evidence and completed flight record. No new implementation or gate
ran during that access interruption. The CONNECT draft remains uninstalled
and unqualified. Continue scope/request publication, bounded progress and
local retirement, then AS-3/4 and MM0-MM4. This gate qualifies charged
unpublished preparation only, not active private runtime. No Main landing,
private async/clipboard activation or 128 MiB protection change. Self-review
only; no independent audit.

## October 9: CONNECT ownership, publication and local retirement

Astra bound charged protocol ownership to an exact RESERVED scope and a
precharged CONNECT terminal obligation. The request and protocol hold separate
scope pins before native allocation; duplicate scope preparation is refused.
Native backlog publication and request admission share the Loom lock with
cancellation and success. No bytes are sent before publication. One driver
uses the real nonblocking 9P client outside that lock, retaining a parser borrow.
READY requires actual version/attach completion. The CONNECT deadline ends at
committed success; later cancellation preserves that success for CQ delivery.

Abort acknowledgement latches transport shutdown; local retirement additionally
requires cursor abort and no concurrent teardown users. A full CQ retains the
terminal and protocol storage. The CONNECT owner alone reaps its delivered
request, drops the other scope pin, destroys local storage, refunds the exact
image and drops Loom last. Explicit metadata reap still requires no child pins.
A server-retained endpoint keeps its own native lifetime and weighted credits.
Protocol failures use EIO and record the attempted handshake phase as required
by the existing ABI; timeout/cancellation preserve their distinct reasons.

Final actual-C host ASan/UBSan evidence connect-control-20261009T170708Z passes
11 controls and 12 named mutations, including abort during progress, a second
driver, a teardown user delaying retirement, full CQ, and recorded failure phase.
These use checked native/protocol doubles, not a wire or SMP stress proof.
Existing protocol owner controls pass20+11 mutations, and the request ledger
passes its control plus13 mutations after factoring the nonmutating admission
check. Lifecycle model passes5828 states and seven expected counterexamples.
Self-review is in connect-draft/self-review.txt; no independent audit. It caught
and repaired the missing protocol failure phase before this checkpoint. Initial
host-runner mutation-count and unused-stub failures are retained; production
controls passed, the harness was corrected and all named mutations now fail at
their intended assertions.

Fresh native connect-diagnostic-20261009T170730Z passes1849/1849 including
loom.private_connect, real normal/bulk version+attach, error/cancel/timeout,
CQ backpressure and server-retained endpoint cleanup; D7 overlap, twenty distinct
login/logout cycles and banner also pass. Exact source diff/new-source hashes,
six input hashes and rebuilt ELF/bin are retained and stable. The earlier
170510Z diagnostic passed before adding failure-phase coverage and stays bounded
to that earlier source. The new full Mac matrix is pending; previous50/50 still
qualifies only fa7e6c5bc protocol preparation.

Continue autonomously: per-ring CONNECT lookup/entry lifetime and bounded fair
progress, close/detach retirement queue and query/reap integration, then remaining
private ops/provided buffers, AS-3/4 and MM0-MM4. This internal engine still has
an explicit caller-lifetime contract and permits only the handshake; no external
RPC/fid/payload borrow may escape it. No syscall/ring dispatch or private async/
clipboard activation, Main landing, or128MiB protection change. Preserve clean
Stratum61dde37, Astra pending/protected work, and8192MiB runner floor.

## October 9: CONNECT full Mac gate verified

Source b2b263e3f250ecaa42c7975a86beec7b65128f12 passed50/50 Mac HVF boots
in current-gate-20261009T170944Z: default SMP1/4/8 and kernel UBSan SMP4/8,
ten each. All boots passed1849/1849 including loom.private_connect, D7 overlap,
twenty distinct login/logout cycles and boot banner. Gate/runner rc0; verified
100 raw serial/harness logs and ten input hashes, with the complete generation
PINNED. Minimum recorded admission 13185MiB exceeds the8192MiB floor.
Result: work/oct5-as-r9/astra-takeover/current-gate-20261009T170944Z-result.json.

This qualifies internal CONNECT publication/handshake/local retirement at that
source. The separate owner-index draft remains native-unqualified until installed
and tested. Pi/A72/KVM, guest allocator reclamation and absent optional graphics
remain unqualified. Self-review only; no independent audit, Main integration,
private async/clipboard activation or128MiB protection change. Continue the
per-ring index checkpoint, then close queue/executor and AS-3/4/MM0-MM4.

## October 9: private per-ring CONNECT ownership index

Astra added a bounded 64-entry protocol-owner index under the existing Loom
lock. Each 24-byte entry holds an exact scope incarnation, protocol pointer,
entry pin count and INSTALLING/LIVE phase. The 1544-byte index occupies one
separately charged private-only 4096-byte buddy allocation, including a consumed
one-shot allocation-failure witness and complete unwind/final-free coverage.
ARM64 Loom remains2944 bytes (one buddy page); protocol remains42264 bytes
(16 buddy pages). Legacy rings do not allocate an index page. This is exact
per-image metadata charging, not the pending MM0-MM4 physical accounting.

Adoption transfers exclusive prepared ownership on return0, with an installation
pin spanning native publication AND its unlocked accept wake. Ordinary exact
lookup returns AGAIN during installation; abort can pin and cancel that state.
Publication failure retains ownership and a negative terminal obligation: the
actual cursor is aborted before PREPARED becomes TERMINAL. Closing/refused
adoption before indexing leaves cleanup to the caller. Lookup+entry pin share
Ll with removal, so no raw pointer gap opens; stale incarnations leave output
untouched. Raw CONNECT/protocol release refuses indexed ownership.

A visit advances a round-robin cursor, runs at most one real bounded client step
and one CQ post, then releases its entry. Holding a lookup entry prevents free,
not progress by other scopes. Collection requires LIVE, zero entry pins, actual
local retirement, no driver/teardown users, and a delivered/discarded terminal
with no parser borrows. It unlinks under Ll and releases/refunds outside Ll,
dropping the protocol's Loom reference last. The caller retains Loom throughout.
Scope metadata remains until explicit reap; final owner cleanup refuses a
nonempty index. A prior successful CONNECT result remains immutable on abort.

Whole-ring discard requires close, no fixed borrows, every indexed owner locally
quiescent and every occupied request matched to its exact retired indexed owner
and terminal/delivered. An unindexed constructor is therefore a blocker rather
than evidence that the ring has no work. Consumer detach, terminal discard and
collection then require neither CQ space nor a peer reply. A lookup racing the
unlocked collection can delay it safely; the caller retries after the pin ends.

Self-review (not independent) is in engine-draft/self-review.txt. It caught the
initial inline-index cost doubling all Loom allocations; the separately charged
private page repairs that cost. It also closed an unindexed-constructor detach
gap. Raw syntax-log inspection caught a misplaced native allocation-fault block
(kind/PO were undeclared in the request fixture), moved it into the protocol
fixture, and retained the failure. Corrected configured ARM64 syntax passes all
seven files. Earlier host assertion/mutation-expectation errors are preserved
with their corrections; no failed runtime result is dismissed.

Host evidence uses actual owner/CONNECT/request/slot/node/CQ C with checked
boundary doubles: engine11 controls+9 mutants, CONNECT11+12, protocol20+11,
private constructor29+26 and final destruction both drop orders+10 mutants.
Shared request rejection fixture passes with17 named mutants. The lifecycle
model passes5828 states and seven expected counterexamples; the new index
model checks89 states and six targeted lifetime/publication counterexamples.
These establish bounded local mechanisms, not native wire or SMP qualification.
The source-matched native diagnostic and full matrix are recorded separately.

Still internal, handshake-only and caller-driven. No private ring/syscall/SQPOLL
activation, general RPC/fid/payload borrows, clipboard, Main landing or128MiB
protection change. Visit's return is not a readiness/sleep predicate. A distinct
reference-owning close queue is still required because indexed owners retain
Loom and its zero-ref destruction cannot initiate their abort. Next: that queue,
actual last-handle/exec cleanup and wake races, then readiness-aware executor,
remaining private operations/provided buffers, AS-3/4 and MM0-MM4. Keep external
Stratum clean61dde37, Astra pending/protected work, and8192MiB runner floor.

Native engine-diagnostic-20261009T1752Z passes1850/1850 including the actual
index fixture: expired indexed admission, normal/bulk independent peers with a
stalled first peer, entry-pin lifetime, full-CQ close discard, retained server
endpoints and exact protocol refunds/reap. D7 overlap, twenty distinct cycles
and banner pass; HEAD+diff/new-source hashes and six input hashes are stable,
and rebuilt ELF/bin retained. Initial1751Z failed1849/1850: the old slot-owner
fixture counted node+request pages but omitted the new index page. Corrected
expected physical metadata and added nonnull/zero index check; failed logs and
artifacts retained. The full Mac matrix for this newer source remains pending;
previous50/50 qualifies b2b263e3f internal CONNECT only.


## October 9: full owner-index Mac gate verified

Source a978d19c388f512a645cdcbaeea60a81a0a06727 passed50/50 Mac HVF boots in
current-gate-20261009T175812Z: default SMP1/4/8 and kernel UBSan SMP4/8,
ten each. Every boot passed1850/1850 including loom.private_engine, D7
overlap, twenty distinct login/logout cycles and the banner. Both rc0;
100 serial/harness logs and ten input hashes verified and retained/PINNED.
Minimum recorded admission 11875MiB, above8192MiB.
Result: work/oct5-as-r9/astra-takeover/current-gate-20261009T175812Z-result.json.

This qualifies the internal charged index and explicit progress at that source.
The separate close queue draft still owes native and full-matrix verification.
Self-review only; no independent audit, Main landing, private async/clipboard
activation or128MiB protection change. Continue close queue, active executor,
remaining private ops/provided buffers, AS-3/4 and MM0-MM4.


## October 9: reference-owning private close queue

Astra added an intrusive close node to each private Loom. Actual handle close
latches admission and starts the node exactly once while holding the caller's
live reference. Under the retire lock it takes one distinct queue reference,
then wakes outside locks. Exec's earlier commit latch remains leaf Ll-only;
forced descriptor removal starts the queue after process/handle locks are gone.
Zero-reference final destruction calls a bare latch/wake path and cannot
resurrect a close root. Loom is2968 bytes on ARM64, still one buddy allocation;
its already charged setup storage contains the queue link/state and diagnostic
visit counter. No stack, allocation or thread is created per close.

The existing retirement worker pops one FIFO close quantum and one zero-reference
final destruction per iteration. Q never encloses Ll, native work, allocator
calls, scheduler wakes or final ref drops. One close quantum aborts at most one
nonbusy indexed cursor and attempts bounded quiescent consumer detach/collection.
READY success remains immutable even if close cancels later. Full CQ and stalled
server endpoints do not prevent actual local cursor abort. A server endpoint
retains its own transport reference independently after the ring is gone.

A blocked ring parks off the runnable queue while retaining its root. Installer,
external driver/teardown, entry-pin, raw-constructor and fixed-buffer returns
notify after their ownership transitions. A return racing RUNNING sets dirty;
a return after PARKED enqueues once. Internal retirement steps/puts/collection
are quiet, avoiding perpetual self-wake loops. Completion drops the queue root
outside Q once; DONE cannot restart. Final destruction is still serviced during
busy close work, and scope visitation retains the existing round-robin cursor.

Fixed-buffer release unlinks its reservation under Ll but increments a separate
settling count before dropping the physical buffer pin outside Ll. Both active
and settling counts consume admission capacity; either prevents consumer detach.
Only after physical settlement does release decrement settling, notify close,
and drop its retained Loom reference. Raw CONNECT rollback/release takes a
temporary Loom root around consuming physical cleanup and the subsequent wake.
All public engine calls still require their caller to retain Loom.

Close collection leaves scope metadata while provisional FID callers can own
parent pins. Once the last Loom reference is gone, final cleanup removes unused
FID reservations before retired SCOPE metadata. A retired scope must be LIVE,
unpinned, with empty parent/reserved fields. READY, ABORTING and invalid owners
still fail closed. This does not permit freeing an in-progress constructor or
pretending abort acknowledgement proves protocol quiescence.

The native loom.private_close fixture includes actual final handle close, forced
private exec with CLOEXEC cleared, a full CQ, independent normal/bulk peers,
entry and fixed-buffer borrows, an unindexed constructor, unused child metadata,
retained server endpoints and exact image refunds. Test-only unlocked boundary
hooks also pause real INSTALLING, driving and teardown owners while the real
retirer must park; returning those owners must wake it to completion. These are
deterministic interleavings, not an exhaustive scheduler proof. Native and full
matrix qualification are recorded with their exact source and artifacts below.

Still internal CONNECT-only. No private syscall/ring dispatch, general RPC/fid
operations, readiness-aware active executor, private userspace/clipboard
activation, Main landing or128MiB protection change. AS-3/4 and MM0-MM4 remain.
Self-review is recorded in close-draft/self-review.txt; no independent audit.

Draft ASan/UBSan controls and ARM64 syntax pass; native qualification follows.


Fresh native close-diagnostic-20261009T183912Z passes1851/1851, including
all seven close cases and second-ring progress while the first is parked. D7
overlap, twenty distinct login/logout cycles and boot banner also pass. Rebuilt
ELF/bin, six input hashes, exact HEAD+diff and new queue source/header hashes
are retained and stable. Real close/exec, full CQ, retained entry/fixed/constructor
owners and installer/driver/teardown interleavings finish local retirement with
exact image refunds while server endpoints remain retained. Source-matched Mac
SMP/UBSan matrix for this new code is still pending; the earlier50/50 qualifies
a978d19c3 index-only source. Pi/A72/KVM, final guest allocator reclamation and
optional graphics remain unqualified. No private activation/Main/128MiB change.
