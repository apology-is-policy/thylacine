# 95 — Identity model (A-1a) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-identity-doc-absorb`).
The kernel half of the identity model (A-1a): principal-id / groups on the Proc,
inheritance across rfork/spawn, the `CAP_SET_IDENTITY`-gated identity-at-spawn, and
the `srv_peer_info` exposure — I-22 (no identity carries ambient super-authority).
Every atom is carried by the fresh owning dossiers:

- the **identity fields, inheritance, and `proc_apply_identity`** — `principal_id`
  / `primary_gid` / `supp_gids` on the Proc, inherited across rfork/spawn,
  `kproc = PRINCIPAL_SYSTEM`, and `proc_apply_identity` (the single audited
  identity-mutation site, which **extincts** on an attempt to stamp
  `PRINCIPAL_SYSTEM` or the INVALID sentinel — so the TCB identity is unforgeable):

      vault/system/kernel/execution/sub-kernel-proc.md

- **`CAP_SET_IDENTITY` and I-22** — capabilities are the *only* growth path, so no
  identity confers authority; the identity-at-spawn gate is this capability:

      vault/system/kernel/security/sub-kernel-caps.md

- **why the identity-at-spawn is race-free** — `proc_apply_identity` runs in the
  *child*, before it enters EL0, so the check needs no lock; I-22's enforcement
  site:

      vault/system/kernel/security/sub-kernel-perm.md

- the **`srv_peer_info` / `SYS_SRV_PEER` identity exposure** — the kernel-stamped
  peer identity a `/srv` server reads (the `SO_PEERCRED` origin):

      vault/system/kernel/ninep/sub-kernel-ninep-attach.md

- the **spawn ABI identity block** (`struct sys_spawn_args`) and the
  `srv_peer_info` record layout:

      vault/system/kernel/entry/sub-kernel-syscall-abi.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **A-1b landed.** This file says "A-1b (corvus identity DB + `RESOLVE_*` + CRVS v2)
  is the userspace authority half and is not yet landed" — corvus is built (see the
  corvus dossiers, absorbed earlier in this sweep); the userspace authority half
  exists.
- **The content is distributed** across the dossiers above — notably the I-22
  enforcement, which is `proc_apply_identity`'s extinct-on-stamp guard plus the
  no-`PRINCIPAL_SYSTEM`-branch of `perm_check`, split across `sub-kernel-proc`,
  `sub-kernel-caps`, and `sub-kernel-perm`.
