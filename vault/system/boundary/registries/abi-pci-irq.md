---
id: abi-pci-irq
type: abi
kind: registry
stability: append-only
title: "PCI interrupt endpoint tickets"
pinned-by:
  - "pci_irq_event is 32 bytes and pci_irq_info is 48 bytes in kernel/C/Rust"
mirrors:
  - kernel/include/thylacine/pci_irq.h
  - kernel/include/thylacine/syscall.h
  - kernel/syscall.c
  - usr/lib/libt/include/thyla/syscall.h
  - usr/lib/libthyla-rs/src/lib.rs
  - "kernel/include/thylacine/vivarium.h: native ceiling 120"
created: 2026-09-17
updated: 2026-09-17
---
## Calls

115 CREATE(pci, mode, ordinal); 116 ARM(endpoint); 117 WAIT(endpoint, timeout_ns,
event_out); 118 COMPLETE(endpoint, generation, sequence); 119 DISABLE(endpoint);
120 INFO(endpoint, info_out). Create returns a handle, WAIT returns 0 timeout or
1 event, other successful operations return 0. Errors use existing negative
errno. INTx mode is 1 and ordinal 0. MSI-X mode 2 is reserved by the approved
design but currently returns ENODEV: it is not an automatic mode substitution.

The 32-byte event contains u64 generation/sequence, u32 count/reason and u64
retry_after_ns. Reasons 1/2/3 are delivery/retry/cooldown. WAIT blocks through
retry delays, so the returned relative retry_after_ns is currently zero. Tickets
are replayed until completion and invalidated by terminal revoke.

The 48-byte info contains u64 generation/deliveries/retries/cooldowns, then u32
mode/state/table_index/reserved. INTx table_index is UINT32_MAX; reserved is zero.
States 0..4 are disarmed, armed, delivered, revoked, fault. No writable controller
routing registers or user-selected INTIDs are exposed.

## Authority and verification

Create requires CAP_HW_CREATE, a writable owned PCI handle and its BDF allowance.
Publication rechecks allowance revocation. All endpoint calls pin the object;
WAIT requires SIGNAL, INFO READ, and mutators WRITE. Kernel/C/Rust builds and the
shared-ticket guest test pass. Full concurrency/controller verification remains.
