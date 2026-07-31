---
id: haz-driver-panic-dos
type: haz
title: "A sole-owner driver Proc's panic is a whole-subsystem DoS"
applies-to: [global]
instances: [fnd-net3d-r1-f1]
created: 2026-07-31
updated: 2026-07-31
---
## The failure shape

A driver Proc that exclusively owns its hardware (I-5: the KObj_PCI/
IRQ/DMA handles are non-transferable, so the claimer IS the service)
turns ANY of its own panics/aborts into a whole-subsystem outage: netd
aborting kills the network for every Proc; tapestryd the display;
stratumd the filesystem. In Rust userspace the sharpest instances are
library calls that PANIC on contract violation rather than erroring —
smoltcp's typed `SocketSet::get::<T>` on a mismatched socket type, its
`get_query_result` on a freed DNS query slot, `cancel_query` on a free
slot — so a slot-reuse or double-poll bug that would be a local error
elsewhere is a remote DoS here.

## The tell

- A typed recovery (`get::<T>`, a downcast, an enum unwrap) whose
  discriminator lives in a table an untrusted client can churn
  (mint/free/re-mint) — the netd net-3d F1: a stranded pending
  resolved against a re-minted cross-proto slot.
- A single-completion API (result frees the slot; re-poll panics)
  whose handle is reachable from more than one code path.
- "The trusted client never sends that" — the server must be sound
  against ANY client its namespace admits (the latent-P1 trap).

## The countermeasure

- Dispatch EVERY typed access on a locally-checked discriminator
  (netd's `slot_proto` matching, enumerated per audit round), never a
  non-local invariant alone; add a generation stamp where a table
  index can be re-minted under a stranded reference.
- Confine single-completion handles to exactly one owner field, nulled
  in the same step that observes the result.
- Prosecute the panic surface as a DoS class in every driver audit
  (the netd rounds' standing "proto-dispatch completeness" item); a
  future non-netd driver dossier joins `instances` as its rounds land.
