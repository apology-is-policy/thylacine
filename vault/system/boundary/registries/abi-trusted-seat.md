---
id: abi-trusted-seat
type: abi
kind: registry
stability: append-only
title: "Trusted seat: bounded episodes and peer-bound backing imports"
pinned-by:
  - "seat_message size 544 and data offset 32 in kernel seat.h and Rust endpoint.rs"
mirrors:
  - kernel/include/thylacine/seat.h
  - kernel/include/thylacine/syscall.h
  - kernel/include/thylacine/proc.h
  - kernel/syscall.c
  - usr/lictor/src/endpoint.rs
  - usr/lib/libthyla-rs/src/lib.rs
  - usr/lib/libt/include/thyla/syscall.h
  - kernel/include/thylacine/vivarium.h
created: 2026-09-18
updated: 2026-09-18
---
## Contract

Native 121 TRUSTED_SEAT takes operation and a user message pointer. The 544-byte
message contains u64 generation/sequence, u32 phase/length/code/value, then
512 data bytes. Operations 1..12 are STATUS, INPUT, ACK, FRAME, KEY, RESTORED,
FAIL, QUERY, VISIBLE, MASK, GRANT and CLIENT. Phases 0..4 are normal, quiescing,
exclusive, restoring and failed. Success is 0; refusal is -1. Generation and
frame sequence bind state transitions and visibility, never a caller's name.
The operation-specific authority and lifecycle live in [[sub-lictor]].

Native 122 SEAT_IMPORT takes an accepted connection handle and one-shot share
ID. Only the bound service with hardware creation authority may import, and the
share must belong to the connection's live peer incarnation. Only weave/GPU-BO
backing qualifies. The resulting DMA handle has read/write/map rights and pins
the original backing; it carries no transfer authority. Failure returns -1.

Spawn permission bits 6/7/8 are seat manager, service and normal client.
The manager is boot-console designated; only boot console or that manager can
stamp service/client roles. They are not inherited. The normal client role
confers no trusted-seat operations; it only identifies the normal broker peer.
