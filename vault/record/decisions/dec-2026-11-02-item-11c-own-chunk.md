---
id: dec-2026-11-02-item-11c-own-chunk
type: dec
title: "Item 11c is its own chunk with its own audit, not part of C4"
date: 2026-11-02
status: standing
decided-by: user-vote
affects: [sub-kernel-notes, sub-kernel-rendez]
created: 2026-11-02
---
## Fork

After C4 a blocked Pouch worker takes `pthread_kill` when its call ends, since native and Pouch waits ride a caught note out. Options: fold the opt-in into C4; a separate chunk.

## Decision

Vote 16 of 2026-11-02: a separate chunk with its own audit. C4's native leg proves a worker at EL0.
