---
id: abi-pty-interaction
type: abi
title: "Terminal ownership observation records and operations"
kind: contract
stability: append-only
pinned-by: ["tools/test-pty-interaction-abi.py"]
mirrors:
  - kernel/include/thylacine/syscall.h
  - usr/lib/libt/include/thyla/syscall.h
  - usr/lib/libthyla-rs/src/pty_interaction.rs
created: 2026-09-24
updated: 2026-09-24
---
## Layout / semantics

**Reserved, not dispatched.** The approved contract is
`docs/HALCYON-INTERACTION-PTY-ABI.md`. SYS_PTY_REGISTER remains 93. Its existing
server operations 0..2 are unchanged; interaction BIND/UNBIND/WATCH/STATE/ACK/CHECK
reserve 16..21 respectively. Version is 1. Binding IDs lie in 1..INT64_MAX,
so successful syscall results cannot alias a negative errno.

The native little-endian state is 80 bytes, aligned to eight: version/flags
(u32 at 0/4), binding ID/pts ID/foreground epoch/acknowledged epoch/revision
(u64 at 8/16/24/32/40), controlling sid/foreground pgid (u32 at 48/52),
subject stripes/binder stripes (u64 at 56/64), binder PID/reserved
(u32 at 72/76). Only LIVE=1 and ACKNOWLEDGED=2 flag bits are reserved.
ACK/CHECK input is 24 bytes, aligned to eight: version/size (u32 at 0/4),
expected epoch/subject stripes (u64 at 8/16). Reserved fields must be zero.

C declarations assert every offset, both sizes and both alignments; the Rust
repr(C) declarations do likewise. `tools/test-pty-interaction-abi.py` compiles
both real C headers to AArch64 ELF objects, extracts their relocation-free
constant section, and compares it and the standalone Rust record to the same
literal 200-byte oracle. This checks constants and compiler layout, not any
kernel role, readiness or lifecycle behavior.

## Change protocol

Update all three mirrors and the frozen fixture in one commit, and record
`abi-pty-interaction` in the change note's mirrors-checked edge. The existing
syscall number space and native ceiling do not move for suboperations.
No live admission path may consume this reservation before the kernel roles,
epochs, lifecycle retirement and bounded watcher ownership are implemented.
