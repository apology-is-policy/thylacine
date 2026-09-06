---
id: chg-2026-09-06-joey-docs-absorb
type: chg
title: "absorb docs/reference/{29-joey, 59-joey-from-ramfs}: two joey-era docs (P3-F embedded blob; P5 initrd load) both superseded by sub-kernel-joey + sub-stratum-boot -- zero-fold, two redirect stubs"
date: 2026-09-06
arc: arc-vault
commits: []
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
Two joey docs at different eras, both superseded by the entry-cluster dossiers
this run's earlier `sub-kernel-joey` authoring closed. Zero-fold: `sub-kernel-joey`
was written from a full read of the current `kernel/joey.c`, so it already
carries every current kernel-side atom of both docs (`joey_run`, the boot
namespace, the `#85` exec-window transient, the trust-root stamps, wait-by-pid,
the boot-fatal failure paths); the userspace supervisor is `sub-stratum-boot`.
Verified coverage atom-by-atom before stubbing rather than trusting a bare
"covered."

**29-joey** (P3-F milestone) is almost entirely stale: the 9-instruction
hand-encoded blob + `build_init_elf` synthetic ELF, the 8 KiB `g_joey_elf_blob`
BSS array (grew to 640 KiB before `#85` retired it for the heap transient),
"prints hello and exits" (joey is now the long-running init rforked `CAP_ALL`),
the ~200 us cost, and the `#157` hang (closed at P4-Fix157). Stubbed
-> sub-kernel-joey + sub-stratum-boot.

**59-joey-from-ramfs** (P5) is partly current: its `#85`-transient content
matches sub-kernel-joey exactly. Stale bits noted in the stub: the `joey.c`
"banner + exit 0" body (now the supervisor), every DEFERRED Status row (all
landed -> sub-stratum-boot), the 416 test count, and -- the one worth flagging
-- **caveat 3 (`JOEY_BLOB_MAX = 32 KiB`) contradicts the doc's own `#85` body**,
a leftover the `#85` edit did not scrub. No `JOEY_BLOB_MAX` exists now; the bound
is `EXEC_FILE_MAX`.

No dossier content changed (both were comprehensive), so no node `touched` and no
`updated:` bump -- purely the two redirect stubs. view-absorption: 66 -> 68
absorbed, 89 live.
