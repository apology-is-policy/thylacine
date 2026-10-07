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
an expected count. `work/oct5-as-r9/red-legs-wrapper-test.sh` runs 11 scenarios
and 57 checks against the real runner in throwaway git trees -- signal, build
failure, stale serial log, wrong fail reason, unattributed red, a concurrent
third-party edit, a short green tally, and two scenarios driven by REAL recorded
boot logs. It discriminates: 57/57 against the corrected runner, 26 wrong against
the previous one, including the old runner CREDITING a red leg from a stale log
after `test.sh` never booted, exiting 0 while REMOVING the unqualified marker on
a 1835-of-1836 suite, and silently overwriting a third party's edit to `loom.c`.

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

Because `ci-smp-gate.sh:140` opens with an unconditional `build.sh kernel`, the
gate cannot run as a script here. `work/oct5-as-r9/smp-matrix-on-qualified-image.sh`
runs its MATRIX STAGE instead -- the same five rows, the same N=10, the same
per-row timeouts, classifier and pass condition, with the rows and N DERIVED out
of `ci-smp-gate.sh` rather than retyped -- on the image the suite and both legs
already qualified. It is **not** `ci-smp-gate.sh` and is not reported as that
script having passed; what it omits is the gate's own userspace rebuild, which is
the part that cannot run. For a kernel change it is arguably the better
experiment: the image under test is bit-for-bit the one the green suite and both
credited legs ran on, so the kernel is the single variable across all three
bodies of evidence.

Gates unchanged: 128 MiB protection retained; private async, replacement memory
accounting and clipboard all non-default; no syscall reaches
`loom_create_private`. The pi A72/KVM axis remains the named unrun residual.
Nothing pushed, nothing landed.
