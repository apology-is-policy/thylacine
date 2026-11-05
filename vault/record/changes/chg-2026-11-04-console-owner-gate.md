---
id: chg-2026-11-04-console-owner-gate
type: chg
title: "Only the session factory names a console owner"
date: 2026-11-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-kernel-syscall-dispatch]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-04
---
Merge audit r3's F2 closed the compositor's route to `CONSOLE_OWNER`; this closes the rest. Any `MAY_POST_SERVICE` holder could confer the bit, and a console owner's every thread reads `/dev/cons` into INTERACTIVE with no ration. The census: `usr/login/src/main.rs` is the one site in `usr/` that confers it, on the session shell, and joey gives `PROC_FLAG_SESSION_REGISTRY` only to login at all four of its spawn sites. So the gate is console-attached or that role. It removes the bit from netd, ptyfs, stratumd, warden, the per-user proxy and a session's own compositor. Corvus keeps it by being console-attached.
