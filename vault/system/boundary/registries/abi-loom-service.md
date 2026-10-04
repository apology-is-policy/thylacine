---
id: abi-loom-service
type: abi
kind: struct
stability: frozen
title: "Loom private service record reservations"
pinned-by:
  - "kernel/include/thylacine/loom_service_abi.h: all sizes and field offsets"
  - "tools/check-loom-service-abi.py: actual C/Rust bytes against independent vectors"
mirrors:
  - "usr/lib/libt/include/thyla/loom_service.h: ten records and constants"
  - "usr/lib/libthyla-rs/src/loom/service_abi.rs: ten repr(C) records and constants"
created: 2026-10-04
updated: 2026-10-04
---
The approved encoding is docs/ASYNC-SERVICE-ABI.md and its provided-buffer
companion docs/ASYNC-SERVICE-BUFFERS.md (operator-selected C, scripture30695b43e).
Existing Loom syscalls and64/16-byte SQE/CQE stay fixed. Private setup4 and
pool setup8 are reserved; pool mode exposes32-byte per-CQ-slot receipts through
Params88's reserved geometry outputs. Register2..9 and CONNECT20 are reserved.
F_NOTIF2 is preserved; the payload receipt flag is4 and BUFFER_SELECT is16.

Kernel C, native C and Rust declare30 constants and10 records. New member24,
create1568, receipt32, return40 and pool-snapshot64 pin every field offset, size
and eight-byte alignment. Compiled independent vectors cover all records and
constants. ARM64 full-header guards check the legacy envelope, flag collisions
and private-mode refusal. Three intentional mirror mutations fail by name.

Reservations only: legacy valid masks and opcode count remain unchanged. No
private runtime wrapper or decoder is claimed. [[spec-loom-service]] models
scope lifecycle, not the new buffer leases; the pool model and actual-source
ownership qualification remain owed by [[sub-kernel-loom]].
