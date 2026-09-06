---
id: chg-2026-09-06-sys-spawn-with-fds-doc-absorb
type: chg
title: "absorb docs/reference/62-sys-spawn-with-fds: zero-fold, multi-redirect"
date: 2026-09-06
arc: arc-vault
commits: ["57d6b4d2"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---

# docs/reference/62-sys-spawn-with-fds.md -> ABSORBED

Absorbed the 168-line SYS_SPAWN_WITH_FDS reference doc into a redirect stub. The
positional fd-inheritance (fd_list[i]->child fd i, KOBJ_SPOOR-only, transfer-not-
bump refcount) -> sub-pouch-process; the kernel handler + install-before-exec_setup
-> sub-kernel-exec / sub-kernel-syscall-dispatch; the /stub-driver orchestrator is
test scaffold (the 61-stratumd-stub arc). Zero fold -- coverage confirmed during
the 61-stratumd-stub absorption.

104 -> 105 absorbed of 157. lint 0-fail.
