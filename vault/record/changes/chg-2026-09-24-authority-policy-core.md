---
id: chg-2026-09-24-authority-policy-core
type: chg
title: "Bounded authority core and mandate safety model"
date: 2026-09-24
arc: arc-user-authority
commits: ["*(pending)*"]
touched: [sub-corvus-authority]
established: [spec-mandate, inv-i35]
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-24
---
Add an isolated no_std authority policy core and initial transaction/revocation
composition model. Validate single-envelope attenuation, conjunctive provenance,
revocation cascade, exact revision checks and authentication/term bounds before
exposing mutation endpoints. Host tests 24/24; bare-target check passes (existing
outline-atomics toolchain warning); host Clippy -D warnings clean. TLC clean 154
states, seven named mutants detected. First model Boolean-assignment error was
repaired; failed and passing logs retained. Self-review only. No runtime,
canonical codec, persistent storage or full UA-stage completion claimed.
