---
id: fnd-kt1-r1-c7
type: fnd
title: "`aurora-push` is spawned as the user with inherit-all caps and parses a user-controlled file while holding CAP_SET_IDENTITY"
round: adt-kt1-r1
severity: P2
status: fixed
surface: [sub-stratum-session]
threatens: [inv-i22]
fixed-by: chg-2026-09-05-kt1-audit-close
regression: "mask read in source; caps-probe covers the mechanism on the tile path"
created: 2026-09-05
---
## Prosecution

**File**: usr/login/src/main.rs:1244-1252
**Invariant**: I-22.
**Prosecution**: `push_cmd.identity(pid, gid, &supp).stdin(..)...; push_cmd.spawn()` -- no `.caps()`, so `aurora-push` inherits LOGIN_CAPS including CAP_SET_IDENTITY (process.rs:231; syscall.c:8230) and then reads `$HOME/lib/aurora` (user-writable). A parser fault in a process holding SET_IDENTITY is an identity-escalation primitive. Not in the KT-1.5d commits (stewardship: enqueue, not walk past). **Fix**: `.caps(0)` (it needs no cap) -- and it is the same one-line class as F1.

## Disposition

Fixed in 062efe18: login spawns aurora-push with `.caps(0)`. No gate constructs a hostile config file; the mask is read in the login source and the caps-probe witnesses the identical mechanism on the tile path.
