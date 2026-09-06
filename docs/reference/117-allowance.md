# 117 — allowance: the per-Proc hardware allowance (I-34) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-allowance-doc-absorb`).
The one new kernel mechanism the Menagerie driver framework needs: the per-Proc
`struct Allowance` that scopes the coarse `CAP_HW_CREATE` to a bounded resource
set (MMIO PA windows / IRQ INTIDs / a per-buffer DMA ceiling / PCI `(bus,dev,fn)`
functions), conferred by the warden at driver spawn, never widened, fully revoked
on removal. Introduces **invariant I-34** (ARCHITECTURE.md §28); audit-bearing.
Its content lives, code-verified and current, in:

- the **whole I-34 mechanism** — the NULL-pointer broad/narrowed hinge, the
  two-step create gate (`allowance_permits` CreateBegin / `allowance_handle_alloc`
  CreateCommit) and the revoke-vs-create SMP race it closes, `proc_confer_allowance`
  (set-once at spawn), `proc_revoke_allowance` (the #160 fold into termination),
  `allowance_clone_into` (the born-revoked / equally-narrow rfork inherit),
  `allowance_confer_within_parent` (the I-2 hardware-axis narrowing gate), the
  drivers-are-leaves `rfork_internal` refusal, the un-widenable-by-corruption
  window arithmetic, the state machine, and the four legs of I-34:

      vault/system/kernel/security/sub-kernel-allowance.md   (guarded-by inv-i34)

- the **hardware-handle constructors the gate wraps** — `KObj_MMIO` / `KObj_IRQ`
  / `KObj_DMA` create, plus the **fourth door** `SYS_PCI_CLAIM` (`kobj_pci_claim`
  + `kobj_pci_resolve_bdf`, the resolve-the-same-boot-immutable-table-the-claim-does
  discipline that keeps the gated bdf == the claimed bdf):

      vault/system/kernel/devices/sub-kernel-hwcap.md

- the **Proc-lifecycle wiring** — the leaf gate in `rfork_internal`, the rfork
  inherit + reap-release sites, and the #160 revoke-folded-into-terminate:

      vault/system/kernel/execution/sub-kernel-proc.md
      vault/system/kernel/execution/sub-kernel-death.md

- the **confer-at-spawn path** — the warden's `node INTERSECT manifest` grant
  policy and the `SYS_SPAWN_FULL_ARGV` `t_allowance_desc` extension it rides
  (`Command::allowance`), gated by `allowance_confer_within_parent` in the parent
  and conferred in the child thunk before EL0:

      vault/system/userspace/runtime/sub-libdriver-grant.md
      vault/system/kernel/execution/sub-kernel-exec.md

- the **formal model** — `specs/allowance.tla` (clean + 4 buggy cfgs:
  `revoke_race` / `revoke_leak` / `confer_widen` / `self_widen`), landed
  TLC-green before the impl (spec-first re-enabled for this surface).

**What this file got WRONG or MISSED by the time it was absorbed:**

- **The set-once caveat is pre-audit-F1, and wrong on a soundness-critical point.**
  This doc's "Known caveats" set-once bullet says the confer `kfree(old)` is
  "still lock-free, sound only because the caller guarantees the conferred-upon
  Proc has not yet entered EL0, so nothing reads `p->allowance` concurrently."
  That is the pre-fix understanding. The F1 fix (`kernel/allowance.c:66-77`,
  `proc_allowance_install_locked`) corrected it: the child IS reachable by a
  concurrent `proc_group_terminate` → `proc_revoke_allowance` on the
  **inherited-clone** `old` allowance (independent of EL0 entry — the child is
  proc-tree-linked before the confer runs), so the swap raced the revoke's
  `spin_lock(&old->lock)` — a real UAF on the narrowed-parent-spawns-child path.
  The **install/swap must run under `g_proc_table_lock`** (the lock the revoke
  runs under); the `kfree(old)` is safe only *after*, when `old` is unreferenced.
  `sub-kernel-allowance` documents the corrected mechanism; this doc's F4
  visibility note (the RELEASE publish pairs with the gate reads' ACQUIRE) it has
  right, but it lacks F1.
- **The "warden is dormant / lands at step 5c" Status is a build-arc snapshot.**
  The warden's grant policy is built — `sub-libdriver-grant` owns it.
- **The content is distributed** — the mechanism to `sub-kernel-allowance`, the
  hardware-handle constructors + the PCI fourth door to `sub-kernel-hwcap`, the
  leaf gate + #160 fold to `sub-kernel-proc` / `sub-kernel-death`, the confer ABI
  + warden grant to `sub-libdriver-grant` / `sub-kernel-exec`. The ABI byte
  layout of the 216-byte `t_allowance_desc` (a spawn-args extension) belongs to
  the spawn surface, not the allowance mechanism.
