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
updated: 2026-09-21
---
## Contract

Native 121 TRUSTED_SEAT takes operation and a user message pointer. The 544-byte
message contains u64 generation/sequence, u32 phase/length/code/value, then
512 data bytes. Operations 1..12 are STATUS, INPUT, ACK, FRAME, KEY, RESTORED,
FAIL, QUERY, VISIBLE, MASK, GRANT and CLIENT. Phases 0..4 are normal, quiescing,
exclusive, restoring and failed. Success is 0; refusal is -1. Generation and
frame sequence bind state transitions and visibility, never a caller's name.

The attention gesture is scanned in the kernel from INPUT key reports, in evdev
codes: either Control (29, 97), either Alt (56, 100), and Delete (111) or F10
(68). The second final key exists because Delete is absent from compact and
laptop keyboards. `seat.h` names the six codes; nothing above the kernel
decides what attention is.

RESTORED closes two phases. From restoring it commits the episode's held grant.
From failed it commits nothing, because the failure already cancelled the grant
and cleared its identity; it only returns the seat to normal once the service
has put ordinary output back and every key is up. A failure closes the console
episode only when the seat opened it (phase exclusive): a serial episode runs
while the seat is normal and is never closed by a seat failure.
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
