# 53 — SYS_READ / SYS_WRITE — byte I/O over fds (P5-fd-rw) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-sys-rw-doc-absorb`). The
two SVC handlers that exchange bytes through a `KOBJ_SPOOR` fd — validate the
user-VA buffer and the handle's rights, then route through the Spoor's `dev->read`
/ `dev->write`. A P5-fd-rw-era doc, superseded; its content is carried by:

- the **dispatcher, the rights gates, the user-VA validation, and the staging** —
  `sys_read_handler` / `sys_write_handler`, the `RIGHT_READ` / `RIGHT_WRITE` gates,
  and the **two-tier bounce staging** (a stack scratch buffer for the metadata-storm
  path, a per-process-budgeted heap tier up to the 128 KiB `SYS_RW_MAX` for bulk
  transfers — the CF-3 A widening from the original 4 KiB); this file's whole
  subject is that dossier's literal title:

      vault/system/kernel/entry/sub-kernel-syscall-dispatch.md

- the **single-byte user-VA primitives** — `uaccess_store_u8` (the store side this
  chunk added) and `uaccess_load_u8`, with fault-fixup recovery:

      vault/system/kernel/entry/sub-kernel-uaccess.md

- the **handle rights** the gates read (`RIGHT_READ` = 0 … `RIGHT_WRITE`, monotonic
  reduction on transfer):

      vault/system/boundary/registries/abi-handle-rights.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **`SYS_RW_MAX` is 128 KiB now, not 4 KiB.** This doc's ABI is P5-fd-rw, before
  CF-3 A added the heap staging tier; the per-call cap grew from the 4 KiB stack
  scratch to 128 KiB, and the two-tier staging that makes both the metadata-storm
  path free and the bulk path possible is `sub-kernel-syscall-dispatch`'s.
- **The content is distributed** — the dispatcher/staging half to
  `sub-kernel-syscall-dispatch`, the byte primitives to `sub-kernel-uaccess` (which
  also flags the stale in-source "only `uaccess_load_u8` is provided" header comment
  this doc's era left behind).
