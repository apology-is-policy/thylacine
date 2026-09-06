---
id: chg-2026-09-06-spawn-wait-doc-absorb
type: chg
title: "absorb docs/reference/60-sys-spawn-wait (SYS_SPAWN/SYS_WAIT_PID, superseded first-cut): clean redirect to exec + proc + elf"
date: 2026-09-06
arc: arc-vault
commits: ["e8065be5"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The P5 minimal orchestration primitive (SYS_SPAWN=21 combined rfork+exec,
SYS_WAIT_PID=22). A first-cut doc, comprehensively superseded; verified atom-by-
atom.

WHERE EACH ATOM LIVES (verified, not assumed):
- The spawn machinery + the blob-lifetime discipline (the exec_setup blob path =
  the boot path joey loads init through; exec_setup_from_spoor = every SYS_SPAWN_*;
  "the blob that belongs to the caller" L-6a) -> sub-kernel-exec (:33-35, :104,
  :209).
- wait_pid_for (the successor with pid/pgrp selectors + WNOHANG + PTY-1e
  report-not-reap + the reap-any-hazard filter) -> sub-kernel-proc (:37, :142-148).
- The 8-aligned ELF copy (cpio pads to 4, elf_load needs 8) -> sub-kernel-elf.
- The handlers + ABI -> sub-kernel-syscall-dispatch.

EVERY "DEFERRED" CAVEAT LANDED + is covered by a successor:
- real SYS_RFORK with COW (I-44) -> LINEAGE arc / sub-kernel-burrow COW.
- SYS_SPAWN_WITH_CAPS + the spawn-variant family -> sub-kernel-exec + sub-kernel-
  caps (docs 62/63/64/73 absorbed).
- argv via SYS_SPAWN_FULL_ARGV -> sub-kernel-exec.
- SYS_WAIT_PID PID-selector + non-blocking via wait_pid_for -> sub-kernel-proc.
- the per-byte status write + partial-fault hazard -> sub-kernel-uaccess.

Zero-fold. Redirect stub.
