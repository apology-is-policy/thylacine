# 34 — devramfs: cpio-loaded in-memory filesystem [ABSORBED INTO THE VAULT]

Absorbed at the content-Devs sweep
(`chg-2026-08-02-devices-content-sweep`). Its content now lives, code-verified
and current, in:

    vault/system/kernel/devices/sub-kernel-content.md
    vault/invariants/inv-i28.md
    vault/invariants/inv-i32.md
    vault/invariants/inv-i33.md

Note on the absorbed text: it was the most stale document in the area. It stated
the file-table cap as 32 (raised three times since, to 256), the implementation
as roughly 270 lines (662), and its own test count as both 10 and 15 in two
different sections (24 are registered). It knew nothing of the synthetic
mount-point directories, the fused walk-plus-metadata path, the metadata handler,
directory enumeration, the reuse-the-caller's-clone walk contract, or that this
is the one filesystem in the area that enforces permissions.

---

**If you are here to add something, add it to the dossier, not to this file.**
This stub replaces the whole document, so any edit here becomes a merge conflict
— which is the intended behaviour, and the only thing that keeps main-track
knowledge from being lost silently at the next merge.
