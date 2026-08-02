# 115 — KObj_PCI, the virtio-PCI function claim [ABSORBED INTO THE VAULT]

Absorbed at the hardware-capability sweep
(`chg-2026-08-02-devices-hwcap-sweep`). Its content now lives, code-verified and
current, in:

    vault/system/kernel/devices/sub-kernel-hwcap.md
    vault/invariants/inv-i5.md

(claim-by-identity and how its register windows delegate exclusivity to the
address-range mechanism; the width-correct size probe; the bounded capability
walk and its region validation; the ordering that makes rollback total; and the
quiesce-before-release teardown.)

**Unusually, this one was current.** It was written at its own focused audit and
never drifted: the four findings are recorded with their dispositions, the
lock-discipline note matches the code, and the status list ends where the work
ended. It is the counter-example in its own batch — absorbed beside
`39-hw-handles.md`, whose body froze at P4 while its caveats stayed alive.

The difference between the two is instructive and not flattering to either: this
document is current because it is *young* and was written once, at an audit, by
someone holding the whole surface in mind. That is a good way to produce a
correct document and a poor way to keep one — nothing here would have caught a
later drift either.

**What the vault added on top:** the contrast with the other two hardware objects
(this one's exclusivity is claimed by identity and delegated; a register range
needs an explicit table; a DMA buffer needs none at all), and the observation that
every counter in these files is read only by tests.

Binding design (unchanged): `docs/VIRTIO-PCI-DESIGN.md`.

---

**If you are here to add something, add it to the dossier, not to this file.**
This stub replaces the whole document, so any edit here becomes a merge conflict
— which is the intended behaviour, and the only thing that keeps main-track
knowledge from being lost silently at the next merge.
