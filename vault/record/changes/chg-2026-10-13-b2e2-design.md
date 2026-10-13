---
id: chg-2026-10-13-b2e2-design
type: chg
title: "B-2e2's design: SYS_ICACHE_SYNC_SPACE at 128, the per-CPU space record, the barrier acknowledged by ticket"
date: 2026-10-13
arc: arc-boosty
commits: ["*(pending)*"]
touched:
  - sub-kernel-syscall-dispatch
  - sub-webkit
established: [dec-2026-10-13-icache-sync-space]
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-13
---
B-2e2's scripture, before its code. The operator's second vote of the day
chose a new syscall over a mode of `SYS_ICACHE_SYNC`, so
[[dec-2026-10-13-icache-sync-space]] supersedes
[[dec-2026-10-13-wasm-space-sync]] with its substance unchanged and adds what
design settled: the number 128 (`restart_syscall`'s collision check becomes
per-number), a per-CPU record of the installed space written with a `DSB` at
every switch, an acknowledgement by ticket (which also repairs
`gic_synchronize_cpu`, whose count of interrupts an earlier sender's late
handler could satisfy), no timeout, and no capability. The mechanism is
`JIT-ON-WX-DESIGN.md` B-2 item 4; [[sub-kernel-syscall-dispatch]] gains the
call with the code, and [[sub-webkit]] calls it at B-2e3.
