---
id: sub-kernel-allowance
type: sub
parent: moc-kernel-security
title: "The hardware allowance — which device, not just whether"
code:
  - kernel/allowance.c
  - kernel/include/thylacine/allowance.h
audit: hard
guarded-by: []
validated-by: [prose, gate-smp]
locks: []
abis: []
design: ["docs/MENAGERIE.md section 4", "specs/allowance.tla"]
created: 2026-08-02
updated: 2026-08-02
---
## Purpose

`CAP_HW_CREATE` is coarse: it says a Proc may create hardware handles at
all, not *which* hardware. The allowance is the scoping layer underneath —
a per-Proc set of MMIO physical windows, IRQ INTIDs, a maximum DMA buffer
size, and PCI `(bus, dev, fn)` functions, conferred by the warden when it
binds a driver to a device node.

It is the I-25 scope idea applied to hardware: bounded authority handed
down, revocable, never widened.

## Contract

`p->allowance == NULL` means **BROAD** — bounded only by `CAP_HW_CREATE`
plus the kernel's own MMIO reservations. This is the warden and the trusted
servers, and is the as-built v1.0 path.

`p->allowance != NULL` means **NARROWED** — the four `SYS_*_CREATE` gates
(`MMIO`, `IRQ`, `DMA`, and `PCI_CLAIM`) admit only what the conferred set
names.

Three properties hold by construction:

- **immutable after confer** — nothing mutates the windows;
- **never widened** — a forked child inherits an equally narrow copy;
- **fully revoked** on device removal, driver crash, or unbind.

## Mechanism

**The central hazard is a create racing a revoke, and it is closed by making
the create two-step.**

`allowance_permits` is *CreateBegin*: a lock-free acquire read of the
immutable conferred set and the `revoked` flag. `allowance_handle_alloc` is
*CreateCommit*: it re-checks `revoked` **under the same lock
`proc_revoke_allowance` takes**, then installs the handle without dropping
it. So a `SYS_*_CREATE` in flight when a `DeviceRemoved` arrives either
commits entirely before the revoke or observes the flag and aborts. It
cannot slip a handle through a gate that is being closed. That is the spec's
`revoke_race` counterexample, closed.

Holding the allowance lock across `handle_alloc` is sound because
`handle_alloc` is spinlock-only and never sleeps. The order is
`al->lock -> handle-table lock`; nothing takes them the other way round, so
it is acyclic.

**Revocation is folded into termination.** `proc_revoke_allowance` is called
from `proc_group_terminate`, so the warden's kill of a removed driver *is*
revoke-then-terminate atomically. The flag closes the gate against in-flight
creates; the `#809`/`#811` cascade then drops the already-live handles at
reap. Two axes, two mechanisms, one call site.

**The window arithmetic is written to be un-widenable by corruption.** The
MMIO check rejects a zero size, rejects a `base + size` overflow, and — for
each conferred window — `continue`s past any window whose own `base + size`
would overflow. A corrupt entry therefore fails to match rather than
matching everything. Containment is `base >= wb && end <= wb + ws`: strict
subset, not overlap.

**Confer is set-once at spawn, but the install still needs the process-table
lock.** The conferred fields are written into a fresh allocation with no
concurrent reader — the driver has not entered EL0 — so the field writes are
race-free. The *install* is not: the confer runs in the child's spawn thunk,
after the child is proc-tree-linked and therefore already reachable by a
concurrent `proc_group_terminate -> proc_revoke_allowance`, which locks the
**old** allowance. A lockless swap-and-free raced that revoke's
`spin_lock(&old->lock)` — a use-after-free on the narrowed-parent-spawns-child
path, where `old` is the inherited clone. So the swap runs under
`g_proc_table_lock` (the lock the revoke runs under) and the `kfree` happens
after, when `old` is unreferenced.

**Sub-conferral is gated by re-using the permit check.**
`allowance_confer_within_parent` asks, per resource, whether the parent's own
allowance permits it — so a broad parent may confer any narrowed set, a
narrowed parent only a subset, and a revoked parent nothing at all (because
`allowance_permits` is false while revoked). Empty axes confer nothing and
are trivially within; `dma_max == 0` is special-cased because
`allowance_permits` rejects size 0 outright.

**Drivers are leaves, and the gate that makes them leaves lives here.**
`rfork_internal` refuses outright if `allowance_is_narrowed(parent)`. The
reason is a lifetime argument: `proc_group_terminate` is thread-group-scoped,
so a child *Proc* would be reparented to init rather than torn down —
leaving a hardware-capable grandchild holding live MMIO/IRQ/DMA that the
warden never tracked, scattering the privilege decision off the warden's
single chokepoint. The gate keys on the **allowance, not the identity**:
unlike the I-32 resource caps, there is deliberately no `PRINCIPAL_SYSTEM`
exemption, because a SYSTEM-identity driver is still a sandboxed leaf.

## Data structures

`struct Allowance`: a lock, `mmio[]` windows with a count, `irq[]` INTIDs
with a count, `dma_max`, `pci[]` packed BDFs with a count, and an atomic
`revoked`. Each array is bounded by its own `ALLOWANCE_*_MAX`.

Clone clamps every count against its maximum on copy — the same defensive
shape as the `supp_gid_count` clamp — so a corrupt source count cannot leave
a garbage tail in the child.

## Concurrency

`al->lock` is a near-leaf: `allowance_handle_alloc` nests only the
handle-table lock beneath it, and nothing nests `g_proc_table_lock` beneath
it. The live order is `g_proc_table_lock -> al->lock -> handle-table lock`.

Every `p->allowance` read is an **acquire** load, pairing with the release
publish in confer and clone. Uniformity here was itself an audit finding
(F3/F4): `proc_revoke_allowance` was the odd one out, sound in practice
because it runs under `g_proc_table_lock`, but the asymmetry was recorded as
a trap and closed.

A child forked after its parent's revocation is **born revoked** — the flag
is part of the copied state.

## Invariants enforced

**I-34** (driver authority bound). Its four legs map to code:
`HandlesWithinAllowance` is the two gates; `AllowanceWithinConferred` is the
immutability of the arrays; `RevokedFullyCleared` is revoke plus the
terminate cascade. The fourth, `ConferredWithinNode`, is deliberately *not*
here — the kernel copies whatever the warden confers, and computing
node ∩ manifest is the warden's policy.

## Error paths

Every rejection is a boolean false or `-1`; nothing extincts. A NULL Proc is
false (fail-closed). Confer rolls back cleanly: `allowance_clone_into`
leaves the child NULL on its own allocation failure, so `proc_free`'s
`allowance_free` is a no-op there, and a *later* spawn failure frees the
just-cloned allowance through the same path.

## Performance

Linear scans of small fixed arrays, on device-create paths only. The gate
read is lock-free.

## Prosecution

- The gate must remain complete across **all four** create sites. A new
  hardware-create syscall that skips `allowance_permits` /
  `allowance_handle_alloc` is an I-34 hole.
- CreateCommit's re-check must stay under `al->lock`, and that lock must
  stay the one `proc_revoke_allowance` takes. Splitting them reopens
  `revoke_race`.
- Nothing may mutate a conferred array after confer.
- The leaf gate must keep keying on the allowance, not on identity — a
  SYSTEM-identity driver is still a leaf.
- Anything held across `handle_alloc` must stay non-sleeping.
- The install must stay under `g_proc_table_lock`; the lockless swap was a
  real UAF.

## Seams

The cumulative DMA-pool budget (a driver may repeatedly create buffers each
within `dma_max`) composes with the I-32 resource work rather than living
here. The forked-child scope teardown is a documented v1.x refinement,
currently moot because a narrowed parent cannot fork at all.

## Caveats

- `dma_max` bounds a **single** buffer, not the total. See the seam above.
- The PCI axis is the fourth and newest; it gates `SYS_PCI_CLAIM` on the
  *resolved* BDF, which is why resolution must walk the same boot-immutable
  table the claim does — a mutable table would reopen check-A-claim-B.
- `allowance_free` reads `p->allowance` non-atomically. Sound — it runs at
  `proc_free`, past any possible concurrent reader — but it is the one
  reader that is not an acquire load.

## Provenance

[[chg-2026-08-02-authority-sweep]].
