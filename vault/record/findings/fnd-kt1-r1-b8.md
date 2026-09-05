---
id: fnd-kt1-r1-b8
type: fnd
title: "a zero-count master write (raw-mode back-pressure) kills ptyhost's input pump for good; the kaua-term tears the key sequence"
round: adt-kt1-r1
severity: P2
status: fixed
surface: [sub-mechanism-drivers]
threatens: []
fixed-by: chg-2026-09-05-kt1-audit-close
regression: "none automated (a raw-mode app that stops reading its master is unconstructed)"
created: 2026-09-05
---
## Prosecution

**File**: usr/ptyhost/src/main.rs:75-80; usr/kaua-term/src/main.rs:97-107; usr/ptyfs/src/server.rs:636-644,1690; kernel/dev9p.c:1846-1866
**Invariant**: I-20 (byte conservation on the master->slave path)
**Prosecution**:
1. ptyfs raw input: `if Ptys::ring_push(&mut p.m2s, &[b]) == 0 { break; }` -> `consumed` may be 0 (server.rs:636-644); `h_write` replies `build_rwrite(.., pushed as u32)` at once (1690) -- it never parks. dev9p returns the accepted count (dev9p.c:1846, "A zero-accepted write ..." 1866) -> `t_write` = 0.
2. ptyhost: `if w <= 0 { unsafe { t_thread_exit() } }` (main.rs:75-80) -- the first 4 KiB of type-ahead into a raw-mode app that is busy ends outer input for the rest of the session ("Master unusable" is false: the ring is merely full). Pre-existing, "behavior-identical" extraction (a90eea53); in scope; OWNED.
3. kaua-term: `write_all` breaks on `w <= 0` (main.rs:102-104) and drops the remainder of the key's bytes: a CSI prefix delivered, its final byte dropped -> the next key completes it wrongly.
**Suggested fix**: treat 0 as back-pressure (bounded nap-and-retry; the master has no `ready` file to poll), and keep a key's bytes atomic (drop the whole key, never a prefix).

## Disposition

Fixed in 062efe18: a zero-count master write is raw-mode back-pressure, not death -- both ptyhost's input pump and the kaua-term's `write_all` retry it up to 200 x 1 ms (parked in torpor, no spin) before dropping the remainder; ptyhost exits its pump only on a negative count. The lock is held across the nap, so a CPR reply can wait <= 200 ms and then itself retry <= 200 ms: a bounded 400 ms stall, never a deadlock.
