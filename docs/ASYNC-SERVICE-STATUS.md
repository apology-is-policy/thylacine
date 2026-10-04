# Async service implementation

Approved scripture4722f34e8. Single-agent on Astra; protected authority/settings
drafts preserved. ASYNC-SERVICE-LIFECYCLE.md owns the contract; memory-accounting
implementation follows this facility as approved.

| Stage | State | Evidence / remaining work |
| --- | --- | --- |
| AS-0 contract | Approved | Scripture4722f34e8; concrete numeric/layout reservation in ASYNC-SERVICE-ABI.md. |
| AS-0 ABI/model | Verified | Three compiled mirrors, 22 constants, five records and three intended source-mirror mismatches; model 5,828 states and seven named counterexamples. Private setup remains rejected. |
| AS-1 progress | Qualified | Resumable framing/native-root handshake and real SrvConn adapter; byte-boundary host fixture, 12 mutants, ASan/UBSan, CPU1 boot1830/1830. 50/50 clean default/SMP/kernel-UBSan boots after TLS repair; AS-2 must bind helpers to private ownership. |
| AS-2 scopes | In progress | Descriptor lifetime, native admission and sharing/COW guards verified; private table, request progress, retirement, deadlines and completion obligations remain. |
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
