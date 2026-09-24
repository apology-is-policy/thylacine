# User authority implementation status

Specification 44d158c3 approved for implementation by the operator on 2026-09-24.
Permanent Astra worktree, codex/astra; main baseline currently 5ed51ff5. Main is
closing B-1c before integrating queued Aux work; Yip 0116 requests a stable base.
Aux owns seal work; Yip 0117 transfers UA-P0 debug-taint repair to Astra after
Aux supplies his round-3-cleared prerequisite SHA.
No unrelated worktree or unfinished tip is imported without coordination.

## Work queue

- UA-P0: prior-debug taint admission at elevation, source-confirmed missing;
  Astra owns repair after Aux round-3-cleared seal base. No exploit repro claimed.
- UA-P1: non-bit administrative/scoped-use debug coverage; design obligation.
- UA-0: ratification commit, exact ABI/codec contracts, mandate model and mutants,
  guest durability evidence before persistent transactions.
- UA-1: bounded pure policy engine, provenance/coverage, canonical codec and
  read-only discovery. Isolated core implemented and checked; canonical codec checked, discovery pending; no mutation endpoint.
- UA-2..UA-7: typed scopes/admission, durable transactions/replay, client and
  trusted scene, standing grants/installer, scoped resources, full qualification.
- Multiuser fixture prerequisite: registry headroom + bind errno, already Astra
  owned in docs/ASTRA-2026-09-24-STATUS.md. Do not shrink acceptance fixtures.

## Evidence

First source tranche (2026-09-24), on ratification 4c889a13:

- `corvus-authority`: 24 host tests pass; bare-target check passes, with the
  existing outline-atomics unstable-feature warning; host Clippy -D warnings clean.
- `specs/check-mandate.py`: clean 154 states and seven mutants each report their
  intended invariant. First attempt had a model Boolean-assignment error; repaired
  before the successful run. Both attempts retained under work/ua-model.
- Policy tests: cross-domain/resource denial, no use-to-admin conversion, complete
  envelope selection, conjunctive support/revisions, self/cyclic delegation,
  auth/term attenuation, live activation, revocation closure and immutable IDs.
- Self-review only. No kernel, Corvus runtime, durability or end-to-end claim.

UA-0 and UA-1 remain open. The initial execution-member model is supplemented by mandate_commit for
separate commit admission/publication, group-dependent preview and account
suspend/resume; full group/account policy resolution remains a runtime obligation. The library still needs discovery and complete pre-auth transaction reservations.
Canonical codec and record-count/depth bounds now have host checks.
Tombstones count toward its interim 4096 physical-record cap (stricter than the
4096-live target); safe compaction must preserve high-water IDs and audit.

The mac lease was acquired after Main's release and returned after the checks.
Main resumes B-1c verification; source work continues without competing builds.

## Storage investigation

Source-confirmed: existing Corvus identity/key-wrap rename helpers ignore the
post-rename directory fsync result. New authority transactions must not inherit
that best-effort acknowledgement contract. Trace Stratum's actual guest commit
and block flush, then run crash/fault probes before claiming durable outcomes.
No storage defect repair or guest crash test claimed yet.

UA-0 record/verb reservations: see `docs/USER-AUTHORITY-ABI.md`. Aux confirms
no Corvus/LCUR collision via Yip0119. The codec will consume this ABI after its
reservation commit; no numeric kernel scope/admission allocation yet.

Second source tranche: canonical codec after ABI reservation 8ab92592. 35 host
checks pass, including a complete one-byte mutation sweep and maximum 896-byte
record; invalid decode never returns a partial record. Bare target + host Clippy
pass. C byte layouts compile for macOS and AArch64; 47 constants and all offsets
match. The existing mandate clean/model-mutant matrix was rerun successfully.
No guest durability test or kernel/Corvus integration is implied.

UA-P0 allocation audit: bits 11..17 are the caught-note field, despite a previous
Yip census calling them free. Aux was notified. Recompute at the cleared seal tip;
add an overlap assertion for the new taint bit. Required lock order is process
lifecycle before grant-table (seat paths already take that order), never its
reverse. No kernel taint code changed yet.

Third source tranche: mandate_commit clean 3768 states and seven new mutants;
original mandate clean 154 and seven mutants also pass. 36 host tests now include
4096 densely populated records, using 4,127,808 bytes of retained vector payload
and about 15 ms for the closure on this Mac (debug build; excludes allocator
bookkeeping and guest RSS). New test was rerun from usr/ --offline after an
initial --manifest-path invocation selected usr/target. Host Clippy clean.
These measurements do not qualify guest operation or revocation latency.

Fourth source tranche: pure issuance approval,47 host tests plus bare-target and
Clippy checks pass; both model matrices rechecked. PreparedIssue checks preview
eligibility without fake activation, owns immutable bytes and accepts only bound
trusted receipts. Admit requires the same live peer/source/policy and Admin proof,
with successful restoration; its result does not publish or mutate policy.
Runtime receipts, transaction quotas, kernel ABI and durable publication remain
unimplemented. Requested current prerequisite status again via Yip0116/0117;
shared kernel and other agents' worktrees remain untouched.

Provenance closure: canonical Mandate itself now owns the transaction ID rather
than a codec-only wrapper. Ledger insertion and both revocation states retain
it; zero IDs are rejected at policy insertion too. MDTM v1 bytes unchanged.
49 host tests, bare-target check and host Clippy pass; evidence
work/ua-policy/provenance.log. Dense fixture now retains4,193,344 payload bytes
(the provenance adds16bytes/record); closure13ms, host debug only.
