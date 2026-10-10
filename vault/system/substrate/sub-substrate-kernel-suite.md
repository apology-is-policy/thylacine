---
id: sub-substrate-kernel-suite
type: sub
parent: moc-substrate
title: "The in-kernel suite — what the boot log says, and what a reader may conclude from it"
code:
  - kernel/test/test.c
  - kernel/test/test.h
audit: none
guarded-by: []
validated-by: [prose, gate-smp]
updated: 2026-10-10
---

## Purpose

`sub-substrate-gates` owns the gate's half of this story: which boot counts as a
pass, how a multi-boot run classifies a non-pass. This dossier owns the other
half -- what the kernel itself EMITS while the suite runs, in what order, and
therefore which readings of a captured serial log are sound.

Every gate in the fleet consumes that text, and so does every ad-hoc parser an
agent writes mid-investigation. It had no owning dossier, and the cost of the
gap is on the record: a parser written against a reasonable belief about the
format read 1749 of 1836 verdicts and reported a test that had FAILED as absent,
while 87 PASSING verdicts in the same green boot had been invisible to the same
pattern all along.

## Contract

What a reader of the boot log is entitled to assume:

- The number of tests is derivable from the source without booting: it is the
  number of registration rows before the table's sentinel.
- Every test that runs produces exactly one verdict -- `PASS`, or `FAIL: ` with
  a message -- AFTER the line that announces its name.
- A verdict belongs to the most recently announced name, not to the line it sits
  on.
- The suite prints a summary tally with its own verdict word, whether it passed
  or failed.
- Every kernel-printed line ends CRLF.

What a reader is NOT entitled to assume: that a name and its verdict share a
line; that a failing suite is cut short before its tally; that an absent
`[skip]` line means the probe set was present; that the first `  tests:` match
in the log carries numbers.

## Mechanism

**The registration table is the denominator.** `g_tests[]` is the single
registration site, declared in `test.h` and defined in `test.c`. The runner
walks it until it meets a row whose function pointer is NULL -- the table ends
in an explicit sentinel -- so the test count is the number of rows before that
sentinel. Derive it; do not carry it in prose. A census that counts
registration rows and compares them with the suite's own total has two
independent routes to one number, which is the only form in which a count is
evidence.

**But counting those rows out of the log needs the conjunction of two markers,
not either one.** Measured on a green boot whose registration count is known:
the `[test] ` marker alone appears 1837 times, because the runner prints a
NON-TEST summary line (the yield-waits report) behind the same marker. The
` ... ` separator alone also appears 1837 times, because a userspace probe line
happens to contain it. Lines carrying BOTH are exactly the registration count.
Two decoys, from unrelated sources, each inflating a census by exactly one --
and off-by-one is the error least likely to be noticed, because the number still
looks right.

**A verdict is a STATE in the log, not a line in it.** The runner prints the
indented `[test] ` marker, the name, and ` ... ` BEFORE calling the test
function. The verdict is printed only after that function returns. Everything
the test emits in between lands between a name and its verdict, and three
sources routinely do:

- `test_fail` records the failure on the current test and then calls
  `sched_dump_runnable` immediately, on the failure path only -- the instrument
  that cracked a scheduler-quiescence bug, and worth its cost. So a FAILING test
  prints its name, then an all-CPU runnable dump, then `FAIL: <msg>`: three
  lines or more, never one.
- The runner itself interposes a note before the verdict when a test leaves a
  counted condition behind, such as the proc-wait timeout report.
- `test_soft_warn` prints a `[SOFT-WARN]` line, with its own newline, from
  inside the test, and does not fail it.

So a reader must track the pending name from the `[test]` marker and resolve it
on the first following verdict.

**The tally is printed by the boot, not by the harness.** `boot_main` emits the
summary; the harness only counts. The prefix `  tests:` appears TWICE in a
passing boot -- first as a bare header before the run, with no numbers after it
-- so a reader that takes the first match gets the header. The tally itself
carries its verdict word: `<passed>/<total> PASS`, with a parenthesised
soft-warn count appended when any fired, or `<passed>/<total> FAIL`. A
production build compiles no suite and prints `  tests: DISABLED` with a reason:
the same prefix, no numbers, no verdict word.

## Data structures

`struct test_case` (in `test.h`) is one row: the name as it appears in the log,
the test function, and then the row's own VERDICT -- a failed flag and a failure
message. Those last two are OUTPUT slots written by the harness, not
configuration: the `false, NULL` that ends every registration row is their
initial value, which is why a new test is registered by extending the array and
needs no constructor and no options. The runner holds a pointer to the row it is
currently running, and that is how `test_fail`, called from arbitrary depth
inside a test, attributes a failure without being passed anything.

(This dossier's first draft called those two fields "dispatch fields the runner
uses to decide how to invoke it". That was wrong, and it was caught by reading
the header rather than inferring from the registration rows' shape -- a comment,
or a dossier, can be true about the wrong thing.)

## Concurrency

The suite runs on one thread, in table order, in the boot's own context -- there
is no parallel test execution to reason about. Concurrency enters only as
SUBJECT: tests that start threads, and the runnable dump that `test_fail` prints
across all CPUs. Two consequences for a reader. Output from a test's own threads
can interleave with the harness's lines, which is a further reason a verdict
cannot be assumed to sit on its name's line. And the kernel's own CRLF applies
to every line regardless of which CPU printed it.

## Invariants enforced

None of the section-28 invariants is enforced here; the harness is an observer,
not a mechanism. The property it does carry is weaker but load-bearing for every
gate: **one announced name yields exactly one verdict, and the pair is ordered**
-- name first, verdict after. A parser may rely on that order and on nothing
else about adjacency.

## Error paths

`test_fail` is the only failure channel: it marks the row, stores the message,
and dumps the runnable set. A test that returns without calling it has passed,
and that is the harness's single most consequential behaviour -- see Caveats.

A failing suite is NOT cut short. The runner completes the whole table, the
summary prints the full tally with `FAIL`, and only then is `extinction` raised
with the suite's name. So a red boot carries a usable tally, and the kernel's
own failure count can be cross-checked against a reader's parse of the
individual verdicts -- two routes, one number, on the red leg as much as the
green.

## Performance

The failure-path dump costs nothing on a passing run and so cannot detune
timing-sensitive tests. The suite's cost is otherwise the sum of its tests; it
runs inside the boot, so it is on the critical path of every gate boot, which is
why a gate that only needs a verdict reads the log rather than re-running.

## Prosecution

The adversarial questions this surface deserves, each of which has been answered
wrongly here at least once:

- Does the pattern match a name and a verdict on one line? Then it is wrong
  about failures AND about a measured 87 of 1836 passing verdicts.
- Is the pattern `$`-anchored? Then it matches nothing, silently, because every
  line is CRLF.
- Does the reader report a parsed total, and does that total equal the
  registration count? A short parse is a refusal, never a verdict.
- Does an unresolved pending name exist at end of log? That is a reading
  failure, not an absent test.
- Does the reader cross-check the kernel's own tally? It is free, independent,
  and present on red legs too.
- Does the gate assert zero `[skip]` lines? If not, a missing image file turns
  a real test into a green no-op.

## Seams

The log text is the seam, and it is consumed by `tools/test.sh` (the boot
verdict), by the multi-boot classifier, and by any investigation-time parser.
Only the boot banner and the `EXTINCTION:` prefix are binding tooling ABI; the
`[test]`, `[skip]`, `[SOFT-WARN]` and tally lines are prose, so a consumer
should key on their SHAPE -- including the verdict word -- rather than on a
prefix alone, and a change to any of them should be looked for in consumers
before it is made.

## Caveats

**A skip is a pass, and it is a live trap, not a documented convenience.**
Sixteen test files print an indented `[skip]` line and return when a file they
need is absent from the ramfs. A test that returns without failing is counted as
a pass, so a renamed or unstaged binary converts a real test into a green no-op
and the suite's own tally cannot tell you. The skip exists for a fresh checkout
without userspace; a gate image always carries the probe set. A gate that cares
therefore asserts ZERO `[skip]` lines against a baseline that had none -- a
property of the IMAGE, which must be stated as such. Tracked as an open defect.

**This dossier covers the harness, not the tests.** `test.c` and `test.h` are
the harness. The individual test files are a separate and much larger surface --
137 files, of which 26 are claimed by the dossiers of the subsystems they
exercise and the rest are unclaimed. That gap is pre-existing and tracked with
the standing dossier backlog; nothing here closes it, and this dossier must not
be read as covering it.

## Provenance

Authored 2026-10-07 from the source, after the private-owner red legs exposed
that no dossier described the format every gate parses. The specific readings
recorded here -- the eager name print, the failure-path dump between name and
verdict, the 87 split passing verdicts, the CRLF, the tally surviving a failing
suite -- were measured against two real captured boot logs, a green one and a
mutant one, not inferred from the source alone. The skip trap predates this
dossier and is carried from the open-defect record rather than discovered here.


## October 8 private service ledger fixture

`loom.service_slots` is registered as its own ordinary suite entry and runs the
same real-module fixture as tools/test-loom-service-slots.py. Its verdict checks
bounded identity/borrow state, not private-engine activation or caller locking.
The registration increases the source-derived denominator by one. The older
50-boot 1836-test record remains historical; subsequent records must derive the
count from their exact source and observe this entry's verdict separately.


The subsequent private-owner integration adds `loom.private_slot_owner`, testing
actual page allocation, charge/refusal rollback, close with a retained borrow and
retirement refund. The denominator increases by another one; derive it from the
qualified source. This entry does not enable public service registration.


The October8 pool-owner checkpoint registers `loom.service_pool_table` as its own
ordinary test. Its shared host/guest fixture checks joint slot/member reservation,
rollback and publication, stale identity rejection, busy reap preserving both
ledgers, exact payload return and close during copyout. `loom.private_slot_owner`
adds pool-allocation refusal and exact aggregate metadata charging. These fixtures
exclude caller authority/protocol integration; a passing row does not activate
private services. The next source-specific full matrix remains pending.


October9 adds `loom.private_buffers`: an ordinary native row checking actual
registered-buffer pins, creator/image authority, partial-resolution rollback,
ring-backing refusal, provisional/live-pool replacement refusal, settled drops
after unmap, foreign-payer handling and private retirement. It does not enable
private service dispatch or certify unimplemented protocol/worker lifetimes.
The new source-specific full Mac matrix is pending.


October 9: b010ace93 passed all 50 Mac HVF boots, 1840/1840 each, including
the corrected loom.private_buffers row, pool/slot/owner rows, both D7 legs and
boot banner. Run current-gate-20261009T001717Z has 100 hash-verified raw logs
and ten pinned inputs. Default SMP1/4/8 and UBSan SMP4/8 each passed ten boots.
The earlier 3dcff4cb9 fixture-address failure remains recorded as failed; it is
not retroactively qualified. Pi/A72/KVM, guest final allocator reclamation and
absent optional graphics probes remain unqualified. Private dispatch stays off.


October 9 adds loom.private_pool_pins as an ordinary native row. It exercises
canonical registered aliases, independent backing pins, provisional overlap,
rollback identity burning, stale/busy refusal, unmap/reap/refund and close before
publication through the internal owner front-end. The denominator is now 1841;
one source-and-diff recorded SMP1 boot passed. Full matrix qualification of the
new source is pending; public service dispatch remains disabled.


October 9 adds loom.private_fixed, checking both physical-alias exclusion
directions, provisional pools, duplicate/capacity refusal, table replacement,
unmap, double release and a final fixed borrower retaining the closed owner.
The new denominator is 1842; its source-and-diff recorded SMP1 boot passed.
Protocol/worker integration and the new source's full matrix remain pending.


## October 9: native target owner witness

`loom.private_targets` raises this source's registered suite to 1843. It checks
actual native registry capture through a navigation handle, provisional rollback,
identity burn/stale refusal, busy reap, close-before-publish, fd/poster closure,
and final borrower retention/registry destruction without a peer responder.
The existing slot-owner row adds target-allocation failure/unwind and its fifth
metadata page. Default SMP1 diagnostic private-targets-diagnostic-20261009T025959Z
passed 1843/1843 plus D7 overlap, twenty distinct cycles and banner. Full Mac
matrix remains pending for the new source; prior d3563f828 50/50 had 1842 rows.
Host controls use checked doubles; no protocol, Pi or guest allocator proof is
inferred. See ASYNC-SERVICE-STATUS for hashes, harness corrections and scope.


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


## October 9: scope/fid reservation module witness

`loom.service_nodes` raises the suite to 1844 and invokes the same actual metadata
modules/fixture as the host sanitizer runner. It tests shared quota, provisional
child parent pins, rollback, exact identities, borrow refusal, abort versus
publication and cleanup after close. Parent protocol states are explicitly
simulated; this is no CONNECT, authority, allocator or concurrency witness.
The table is not yet owned by a runtime Loom. Diagnostic nodes-diagnostic-
20261009T035230Z passed 1844/1844 and D7 overlap/twenty cycles/banner. Full Mac
matrix is pending for this source; previous target-source50/50 had1843 rows.


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

## October 9: node owner transaction fixture

The suite now contains 1845 entries. `loom.private_node_owner` creates an actual
private Loom and exercises creator/image and ABI validation, failed-copyout
rollback, stale continuation rejection, parent abort before child publication,
and close before root publication. Parent READY/ABORTING states are test
injection, not a service handshake. The fixture retains its caller reference
until cleanup, then waits for the actual private retirer before dropping the
creator. A diagnostic boot passed 1845/1845 plus D7 overlap and twenty cycles
at node-transactions-diagnostic-20261009T053938Z. This single boot does not
qualify the current source's SMP/UBSan matrix.

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

## October 9: request ledger and actual terminal CQ fixture

The suite has1847 entries. `loom.service_requests` exercises the real bounded
request ledger with synthetic scope transitions. `loom.private_requests` owns an
actual private ring, checks request-allocation fault unwind, commits a synthetic
request result, fills the actual CQ and proves AGAIN retains the terminal. It
then latches close, corrupts user-writable CQ mirrors, advances the consumer and
verifies that private geometry posts the prior result exactly once with a
cleared receipt. Real owner retirement returns the charge. This is not a native
service handshake or proof of runtime callback retirement. The diagnostic
request-owner-diagnostic-20261009T064808Z passed1847/1847 and D7; full matrix
qualification is pending for this newer source.


## October 9: charged private protocol preparation fixture

loom.private_protocol uses actual native target admission and preallocated client
storage, with a process-tree-linked creator, both32/128KiB classes, three
allocation refusal points, image budget refusal, no backlog/byte effects,
published-owner cleanup refusal and final Loom/image retention. It exercises
unpublished construction only, not CONNECT handshake or active retirement.
Native protocol-owner-diagnostic-20261009T074413Z passes1848/1848 and D7; initial
074015Z unlinked-creator fixture failure is retained with its repair. Request
source45d26893b separately passes full50/50 at1847/1847; newer protocol source
full matrix pending. See sub-kernel-loom and ASYNC-SERVICE-STATUS for self-review.

## October 9: private CONNECT native fixture

loom.private_connect uses real registered targets, creator snapshot, native
admission and preallocated 9P version/attach progress for normal and bulk
connections. It checks allocation/expired publication rollback, duplicate
destination refusal, cancellation ordering, handshake deadlines, Rlerror phase
diagnostics, close, full CQ retention, local storage refund with a server-held
endpoint, and exact scope reap. connect-diagnostic-20261009T170730Z passes
1849/1849 plus D7 overlap/twenty cycles/banner with stable source and inputs.
Full Mac matrix pending on this source. Checked host boundary doubles separately
pass11 controls/12 named mutations; they do not constitute SMP race stress or
an independent audit. The engine is internal, handshake-only, and requires its
caller to retain entry storage. Private ring dispatch/activation stays disabled.


## October 10: private executor and operation-storage evidence boundaries

The1853 suite's loom.private_executor is qualified on exact f719ee4a3 by
current-gate-20261009T211533Z:50/50 Mac default SMP1/4/8 and UBSan4/8, D7
overlap/twenty distinct cycles in every boot. Evidence checkpoint79dfd1267
retains100 raw logs and ten input hashes. Earlier counts remain historical.

New loom.private_operation_storage raises the suite to1854 and uses real normal
and bulk READY peers to check request physical-owner retention, exact one-page
charge/refund, real allocator/quota refusal, terminal-capacity exhaustion,
identity burn and close PARKED until the owner is physically destroyed.
operation-storage-native-20261009T215931Z passes1854/1854 plus D7 overlap/twenty
cycles/banner with eleven source/six input hashes. Implementation e7ff5d1c7 is
covered only by this bounded CPU1 native run and recorded host/ARM64 checks;
prior full Mac qualification does not extend to it. General RPC/FID/buffer
dispatch and public activation remain unimplemented. Self-review only.

October10 Astra: suite remains1854 registrations. private_operation_storage
now additionally verifies actual normal/bulk root FID mapping after attach and
root metadata borrow/unused-child retention across close until physical
operation cleanup. CPU1 native fid-integration/native-20261009T221811Z passes
1854/D7/20cycles with exact source/artifact evidence. It does not test submitted
WALK/LOPEN/READ/WRITE/CLUNK RPCs or confer prior full-Mac qualification.

October10 coherent async integration adds loom.private_frontend:1855 registered
tests. New normal/bulk cases invoke the actual creator frontend and real protocol
constructor/engine adoption, verify copied correlation through native handshake,
root mapping, malformed-SQE CQ refusal and zero-submit CQ wake, then retain the
existing charged storage/close exercise. CPU1 async-integration/native-
20261010T010700Z passes1855/D7overlap/20cycles with41source/6artifact hashes stable.
Native general-operation/provided/MORE traffic and full Mac matrix remain next;
no private ABI/clipboard activation or128MiB protection change. Single-agent review.


### October 10: native general operations and provided payloads

The suite registers1856 tests after adding loom.private_native_operations. It
exercises normal/bulk native CONNECT, WALK/open DAC, fixed READ/WRITE/CLUNK,
provided leases, empty/full-CQ backpressure, independent RETURN, MORE and EOF.
The controlled peer reads actual wire requests; explicit engine visits drive
progress. This is not general-operation worker or real filesystem qualification.
Raw native-operations/provided-20261010T0210Z passes1856/1856,D7overlap/20cycles,
with41source/6artifact hashes stable. The earlier pinned-image50/50 matrix remains
qualified only at98dd4b618 (1855tests). Failed fixture build and wrong CQ flag
expectation are preserved. Single-agent self-review; no activation or128MiB change.
