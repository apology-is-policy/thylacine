# 73 — SYS_SPAWN_WITH_PERMS: spawn + atomic SPAWN_PERM_* stamp [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-spawn-perms-absorb`).
The fifth spawn variant (`SYS_SPAWN_WITH_PERMS` = 31): `SYS_SPAWN_FULL` plus a
`perm_flags` word of `SPAWN_PERM_*` bits the kernel stamps on the child atomically
inside the spawn thunk, before `exec_setup`. Its content lives, code-verified and
current, in:

- the **grant-gate security mechanism** — the two-site split
  (`spawn_perm_grant_check` at the entry before any user-VA read;
  `apply_spawn_perms` in the child thunk before `exec_setup`, closing the SMP
  "mark-after-spawn" race), the per-bit rules (`CONSOLE_TRUSTED` console-attach-
  only + never-delegable per I-27; `MAY_POST_SERVICE` / `CONSOLE_OWNER`
  holder-delegable — the A-5b one-hop delegation), the I-2 non-propagation (the
  perm bits are spawn-time `perm_flags`, not `cap_mask`, so the fork-grantable
  ceiling is untouched), and the tail-`extinction` backstop — **folded here at
  this absorption** (the mechanism was not owned as a mechanism before):

      vault/system/kernel/entry/sub-kernel-syscall-dispatch.md   (audit: hard)

- the **ABI** — the syscall number, the `SPAWN_PERM_*` constants + `SPAWN_PERM_ALL`,
  and the three-copy pinning (`<thylacine/syscall.h>` / `<thyla/syscall.h>` /
  `libthyla_rs`):

      vault/system/kernel/entry/sub-kernel-syscall-abi.md

- what the **`MAY_POST_SERVICE` bit unlocks** — `SYS_POST_SERVICE` registering a
  `/srv/<name>` 9P server:

      vault/system/kernel/srv/sub-kernel-devsrv.md

- the **production callers** naming the bits they consume — joey -> corvus
  (`MAY_POST_SERVICE` + `CONSOLE_TRUSTED`), joey -> `/sbin/login`, `/sbin/login`
  -> `ut` (`CONSOLE_OWNER`) + the per-user proxy, and the driver/service posters:

      vault/system/stratum/sub-stratum-boot.md
      vault/system/stratum/sub-stratum-session.md
      vault/system/userspace/shell-tui/sub-halcyond.md
      vault/system/userspace/services/sub-ptyfs.md
      vault/system/userspace/services/sub-netd-nic.md
      vault/system/userspace/services/sub-viv.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **The grant gate was covered as a *shape*, not as a *security mechanism* —
  folded at absorption.** `sub-kernel-syscall-dispatch` already carried the
  one-hop delegation shape, but only for the sibling I-32 `MAY_RAISE_PAGE_BUDGET`
  raise authority. The load-bearing security atoms of *this* family — the I-27
  reason `CONSOLE_TRUSTED` is never delegable (a service-poster must not confer
  the console-trust used for hostowner elevation), and the SMP race the
  atomic-stamp-in-thunk closes (a child scheduled onto another CPU could reach
  `SYS_POST_SERVICE` before a mark-after-spawn lands) — lived only here. Now
  folded into the dispatch dossier's Mechanism, with the I-27 line and a
  Prosecution bullet extended to name it.
- **The doc's five-variant table and the SMP-race narrative are current** — the
  code (`kernel/syscall.c:8428` `spawn_perm_grant_check`, `:8470`
  `apply_spawn_perms`) matches. Nothing in the doc was refuted; the gap was
  ownership, not correctness.
