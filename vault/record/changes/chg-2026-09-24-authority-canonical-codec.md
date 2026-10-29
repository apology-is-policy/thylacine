---
id: chg-2026-09-24-authority-canonical-codec
type: chg
title: "Implement bounded canonical authority records"
date: 2026-09-24
arc: arc-user-authority
commits: ["*(pending)*"]
touched: [sub-corvus-authority, inv-i35]
established: []
closed: []
opened: []
mirrors-checked: [abi-user-authority]
depth: skeletal
created: 2026-09-24
---
Implement canonical MDTM v1 encode/decode after reservation 8ab92592. Bounded,
fallible allocation; strict enum/reserved/count/length checks; no authority is
conferred by successful decode. Bind Actions to reserved constants. Make ledger
insertion and support-walk scratch allocations fallible, and compute revocation
closure in topological order rather than growing-list scans.

Validation: 35 host tests pass, including every truncation, maximum record size,
single-byte mutation canonicality, per-subject/physical limits and graph depth;
bare target check passes with existing outline-atomics warning; host Clippy
-D warnings passes; C assertions compile on macOS and AArch64; 47 constants and
both layouts match; clean model154 states and seven named mutants rechecked.
Evidence in work/ua-policy/codec-first.log and work/ua-model/20260924T105820.665876Z.
Self-review only. No runtime, durable replay or full pre-auth reservation claim.
