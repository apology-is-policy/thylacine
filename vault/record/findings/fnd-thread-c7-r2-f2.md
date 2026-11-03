---
id: fnd-thread-c7-r2-f2
type: fnd
round: adt-thread-c7-r2
severity: P3
status: fixed
title: "The pouch mask leg read the TLS shadow only"
surface: [sub-pouch-thread]
threatens: [inv-i19]
fixed-by: chg-2026-10-29-thread-model-c7
regression: "pouch-hello-threads mask-c11 and mask-queued; sabotages S17 (0065 without its start_c11 hunk) and S18 (the native arm with its kernel test unregistered) at phase f"
created: 2026-10-29
---
## Prosecution

pthread_sigmask answers a query from the shadow `__restore_sigs` writes whatever the kernel did, so the `mask` leg could not see the kernel arm or `start_c11`. Two legs: `mask-c11` through thrd_create, and `mask-queued` -- a SIGINT queued while main blocks it must reach main past a thread's birth.
