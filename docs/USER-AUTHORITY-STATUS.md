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
  read-only discovery. Isolated core implemented and checked; codec/discovery pending; no mutation endpoint.
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

UA-0 and UA-1 remain open. The initial model lacks full group/account transitions
and separate commit admission/publication. The library still needs canonical
codec, discovery, quota/depth maximum tests and pre-auth fallible reservations.
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
