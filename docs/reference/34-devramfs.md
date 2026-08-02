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

**What was NOT absorbed, and is therefore owed** (found at the ledger
reconciliation, `chg-2026-08-02-absorption-reconciliation`): this document was
named for `devramfs` but also documented **the cpio parser** — `kernel/cpio.c`
and `kernel/include/thylacine/cpio.h`, 213 lines — including the newc header's
field-offset table, the iterator API, and the trailer that stops iteration. The
dossier covers the filesystem that *consumes* the archive, not the parser that
*reads* it, and no other note covers it either. Until that sweep lands the
parser's only description is this file's own history. Tracked as task #32.

---

**If you are here to add something, add it to the dossier, not to this file.**
This stub replaces the whole document, so any edit here becomes a merge conflict
— which is the intended behaviour, and the only thing that keeps main-track
knowledge from being lost silently at the next merge.
