---
id: fnd-aux-corona-join-r2-f3
type: fnd
round: adt-aux-corona-join-r2
severity: P3
status: fixed
title: "check-lock-helpers exempted two shapes that write a deferring lock's word"
surface: [sub-kernel-poll, sub-kernel-proc]
threatens: [inv-i9]
fixed-by: chg-2026-10-29-aux-corona-join
regression: "check-lock-helpers --self-test plants a .value store and a re-init on both named locks, and .value read controls"
created: 2026-11-02
---
## Prosecution

Rule A exempted `spin_lock_init(&L)` and `L.value` anywhere, and both store to the word: a release that skips the helper's owed drain. Fixed in 009d20767: `.value` only as an `__atomic_load_n` read; one `spin_lock_init` only for a lock defined without an initializer.
