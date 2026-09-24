---
id: chg-2026-09-24-hin1-bodies
type: chg
title: "HIN1 typed bodies and bounded fragmented record assembly"
date: 2026-09-24
arc: arc-halcyon-interaction
commits: ["*(pending)*"]
touched: [sub-libhalcyon]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-24
---
Pins the ten HIN1 request/response body pairs and the existing-errno failure map
before service consumers. Borrowed Rust decoders and twenty frozen C/Rust vectors
make exact lengths, reserved fields and bounds reviewable across languages. A
fragment receiver allocates only after checking the declared extent and supplied
connection allowance, withholds partial operations and poisons on any error.
All 148 libhalcyon tests and the independent C fixture pass on Linux/aarch64
(`work/hi1a-pi-all.log`). Controller/focus admission, aggregate accounting, replay,
storage and asynchronous clients remain open; no live endpoint is exposed.
Single-agent self-review, as directed by the operator; no independent audit claim.
