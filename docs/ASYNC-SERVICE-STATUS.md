# Async service implementation

Approved scripture4722f34e8. Single-agent on Astra; protected authority/settings
drafts preserved. ASYNC-SERVICE-LIFECYCLE.md owns the contract; memory-accounting
implementation follows this facility as approved.

| Stage | State | Evidence / remaining work |
| --- | --- | --- |
| AS-0 contract | Approved | Scripture4722f34e8; concrete numeric/layout reservation in ASYNC-SERVICE-ABI.md. |
| AS-0 ABI/model | Verified | Three compiled mirrors, 22 constants, five records and three intended source-mirror mismatches; model 5,828 states and seven named counterexamples. Private setup remains rejected. |
| AS-1 progress | Implemented; broad gate pending | Resumable framing/native-root handshake and real SrvConn adapter; byte-boundary host fixture, 12 mutants, ASan/UBSan, CPU1 boot1830/1830. AS-2 must bind these helpers to private ownership. |
| AS-2 scopes | Pending | Private table/owner guards, accounting, asynchronous retirement, deadlines and completion obligations. |
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
