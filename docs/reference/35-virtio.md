# 35 — VirtIO core: the MMIO transport and the split virtqueue [ABSORBED INTO THE VAULT]

Absorbed at the hardware-discovery sweep
(`chg-2026-08-02-devices-discovery-sweep`). Its content now lives, code-verified
and current, in:

    vault/system/kernel/devices/sub-kernel-discovery.md
    vault/invariants/inv-i15.md
    vault/invariants/inv-i5.md
    vault/invariants/inv-i34.md

Note on the absorbed text: it had frozen at P4-F and described its own closing
audit in the future tense. That audit has since run; its findings (the
death-time device reset and its entropy-source carve-out, the power-of-two queue
size rejection, the status readback) were load-bearing in the code and absent
here. Its test table listed seven of the eleven registered tests — the four
missing are exactly the four the audit added.

---

**If you are here to add something, add it to the dossier, not to this file.**
This stub replaces the whole document, so any edit here becomes a merge conflict
— which is the intended behaviour, and the only thing that keeps main-track
knowledge from being lost silently at the next merge.
