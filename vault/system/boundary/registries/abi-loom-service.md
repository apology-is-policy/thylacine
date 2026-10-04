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
  - "usr/lib/libt/include/thyla/loom_service.h: five records and constants"
  - "usr/lib/libthyla-rs/src/loom/service_abi.rs: five repr(C) records and constants"
created: 2026-10-04
updated: 2026-10-04
---
The approved encoding is docs/ASYNC-SERVICE-ABI.md. The service extension uses
existing Loom syscalls and SQE/CQE sizes; setup bit2, register operations2..6
and SQE20 are reserved. The kernel and two userspace mirrors declare the same
22 constants and five records. Every size and field offset is asserted; the
repeatable compiled byte gate independently checks values, signed fields and
64-bit incarnation representation. ARM64 C layout compilation is included.

These are reservations only. Legacy valid masks and opcode count are unchanged,
so declarations alone cannot enable private service admission. No C/Pouch/Go
runtime wrapper is claimed. [[spec-loom-service]] models the lifecycle, not the
binary encoding. [[sub-kernel-loom]] owns implementation and activation.

## Approved companion encoding, mirrors pending

Operator-selected option C is specified in docs/ASYNC-SERVICE-BUFFERS.md:
setup8; register7/8/9; POOL kind4; BUFFER_SELECT16; CQ receipt flag4, preserving
F_NOTIF2. A32-byte companion receipt retains full pool/lease identities without
changing CQE16 or user_data. Five new records and pool-mode setup geometry are
reserved in scripture; the compiled mirrors still contain only the original
22 constants/five records until the following ABI checkpoint. No activation.
