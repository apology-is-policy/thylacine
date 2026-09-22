---
id: fnd-shed-r3-f1
type: fnd
title: "spoor_clone copies COPEN: any Proc opens /dev, walks consdrain off it, closes the leaf, and disarms the renderer's output drain"
round: adt-shed-r3
severity: P2
status: fixed
surface: [sub-kernel-spoor, sub-kernel-devdev, sub-kernel-cons]
threatens: [inv-i27]
fixed-by: chg-2026-09-21-mount-shed
regression: "devdev.drain_walk_off_opened_dev_no_disarm (the unprivileged route), devdev.drain_opath_clone_no_disarm + spoor.clone_copies_state (aux c1b25cdf)"
created: 2026-09-21
---
## Prosecution

**File**: `kernel/spoor.c` `spoor_clone` (`nc->flag = c->flag & ~CWALKONLY`), `kernel/devdev.c` `devdev_close`
**Invariant**: I-27 (the renderer's console path)
**Prosecution**:
1. An unprivileged Proc opens `/dev` (kind 0: no console/renderer gate) -> COPEN.
2. It walks `consdrain` off the opened handle (devdev_walk is ungated); the clone inherits COPEN.
3. It closes the leaf: `devdev_close` sees {CONSDRAIN, COPEN} and calls `cons_drain_close()` -- the renderer's drain is disarmed.
**Suggested fix**: strip COPEN in spoor_clone (aux's H9).

## Disposition

Fixed by cherry-picking aux c1b25cdf (`-x`, as 1d00cea7): COPEN now means "open() succeeded on THIS Spoor". The route the round named has its own test. The miss: main's self-audit never read aux's closed H-list, which already had it.
