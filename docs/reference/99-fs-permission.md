# 99 — File permission + ownership surface (A-2) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-fs-permission-absorb`).
The kernel's per-file ownership + rwx-enforcement surface — the metadata plumbing
(A-2a: `t_stat` owner/group, `SYS_WSTAT`, `dev9p`/`devramfs` `stat_native`) and the
Linux-VFS enforcement layer (A-2d: `perm.c` + the resolution/handler chokepoints).
Its content is carried, more currently and across more surfaces, by:

- the **enforcement helpers** — `perm_check` (owner-first), the
  `perm_want_for_omode`/`rights_for_omode` matched pair (the OEXEC execute-to-read
  leak close), `perm_wstat_check` (the three-authority chmod/chown/chgrp policy),
  `proc_in_group`, the `want == 0` fail-closed default, and the I-22 enforcement
  (no `PRINCIPAL_SYSTEM` bypass):

      vault/system/kernel/security/sub-kernel-perm.md

- the **`struct t_stat` record** — byte layout, the `uid`/`gid` A-2a fields, and
  the seven mirrors (now **88 bytes**, not this file's 80 — see below):

      vault/system/boundary/registries/abi-t-stat.md

- the **`SYS_WSTAT` / `SYS_FSTAT` numbers** and the record's growth history:

      vault/system/kernel/entry/sub-kernel-syscall-abi.md

- the **`SYS_WSTAT` handler** (`sys_wstat_for_proc`) — the kind-gate-not-rights-gate
  metadata authority (#47), the `T_WSTAT_SIZE` content/metadata split, the
  #81-class truncate-via-`O_PATH` close, and where the `perm_wstat_check` /
  rename / unlink / walk-create identity gates are *placed*:

      vault/system/kernel/entry/sub-kernel-syscall-dispatch.md

- the **`dev9p` read/write native** — `dev9p_stat_native` / `dev9p_wstat_native`,
  the `T_WSTAT_* == P9_SETATTR_*` asserts, the `Rgetattr` valid-mask fail-close,
  and `perm_enforced` (now **true** on dev9p — A-3b landed):

      vault/system/kernel/ninep/sub-kernel-ninep-dev9p.md

- the **`devramfs` metadata stamp** — every entry `PRINCIPAL_SYSTEM`/`GID_SYSTEM`,
  `perm_enforced = true`, and why enforcing the boot FS does not brick boot:

      vault/system/kernel/devices/sub-kernel-content.md

- the **walk-open access gate** — the per-component X-search and the
  R/W-per-`omode` check on the target (this file attributed it to the handler; it
  lives in the resolver) **and the #81 `O_PATH` read-bypass close** (the
  `CWALKONLY` reject on `read`/`write`/`readdir` — it once leaked the 0400
  `/system.key`):

      vault/system/kernel/namespace/sub-kernel-stalk.md

- the **`CWALKONLY` Spoor flag** it turns on:

      vault/system/kernel/namespace/sub-kernel-spoor.md

**What this file got WRONG or MISSED by the time it was absorbed** — it is an
A-2a-era snapshot with a stale intro and a body superseded on several axes:

- **Its header contradicts its own body.** The intro (para 3) says "the kernel
  rwx **enforcement** layer (A-2d) is **not yet built**"; its own Status section
  says "**A-2d … LANDED (devramfs-live)**". The intro was written at A-2a and never
  updated when A-2d landed into the same file. A-2d is built.
- **`t_stat` is 88 bytes, not 80.** This file's table stops at `gid`@76 and says
  80; #100 later appended `devno`@80 (+pad), and the durable authority (`abi-t-stat`)
  documents 88 with seven mirrors, not the three named here.
- **`dev9p.perm_enforced` is `true` now.** This file says `false` / "deferred to
  A-3" — but A-3b landed and flipped the one flag, so dev9p rwx **is** enforced,
  and `perm_wstat_check` is no longer "dormant in production."
- **No `T_WSTAT_SIZE`.** The handler has since grown an `ftruncate` axis with its
  own #81-class truncate-via-`O_PATH` close (folded into `sub-kernel-syscall-
  dispatch` at absorption, having been undocumented anywhere before); this file's
  `SYS_WSTAT` covers only MODE/UID/GID.
