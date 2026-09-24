---
id: chg-2026-09-24-hin1-envelope
type: chg
title: "HIN1 envelope and canonical clipboard text foundation"
date: 2026-09-24
arc: arc-halcyon-interaction
commits: ["*(pending)*"]
touched: [sub-libhalcyon]
established: ["HIN1 envelope and operation/mode reservations with C mirror"]
closed: []
opened: []
mirrors-checked: ["Rust encoder and native Linux/aarch64 C Hello fixture"]
depth: skeletal
created: 2026-09-24
---
The 24-byte HIN1 envelope pins explicit little-endian fields, operation/mode
numbers and the agreed limits. Rust decoding checks the exact envelope extent;
canonical clipboard text rejects controls except TAB/LF without stripping
Unicode format characters. Three new Rust tests pass in the 274-test Pi host
run, and the native C constant/Hello-fixture check passes separately. This is
only the framing foundation. No operation-body decoder, service endpoint,
controller authority, focus admission or asynchronous client is enabled.
