---
id: chg-2026-10-13-b2e1-audit-close
type: chg
title: "B-2e1's Fable round 1 close: the Wasm stub map corrected, the finalizeCreation fence's position checked, the binding generators' MustSucceed queued"
date: 2026-10-13
arc: arc-boosty
commits: ["a59710f64"]
touched:
  - sub-webkit
  - sub-substrate-build
established: [dec-2026-10-13-wasm-plan-isb-writers]
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-13
---
The close of B-2e1's first Fable round (0 P0, 0 P1, 0 P2, 3 P3; clean). F1
corrected the map behind [[dec-2026-10-13-wasm-plan-isb]]: the wasm-to-JS
stubs come from the validation plan and a group's wasm-to-wasm stubs from the
thread that completes it, so the instance's fence is the one every consumer
passes. [[dec-2026-10-13-wasm-plan-isb-writers]] supersedes it with the call
unchanged. F2 made [[sub-substrate-build]]'s ISB check test the position of
`finalizeCreation`'s fence as well as its presence. F3, the Wasm binding
generators asking the pool to succeed, is queued into B-2e3 ([[sub-webkit]]).
