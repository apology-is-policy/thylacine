# 106 — Kernel CSPRNG (`random.c` + `chacha20.c`) [ABSORBED INTO THE VAULT]

Absorbed at the content-Devs sweep
(`chg-2026-08-02-devices-content-sweep`). Its content now lives, code-verified
and current, in:

    vault/system/kernel/devices/sub-kernel-content.md
    vault/invariants/inv-i16.md
    vault/locks/lock-random.md
    vault/locks/lock-rng-dev.md

Note on the absorbed text: it was the best-maintained document in the area —
current on the two-bound device poll, the retry asymmetry, the all-zero
rejection, and the reasoning behind each, down to the audit findings that
produced them. It was also the one whose subject had a live debugging episode,
which is this area's rule for where documents stay true.

The one thing it did not record: the keystream buffer's *first* fill is generated
from an unkeyed cipher state and is therefore a compile-time constant, and what
keeps it from being served is boot ordering rather than the fail-closed gate. The
document states that ordering — for the seeding property, which is a different
property with the same guard.

---

**If you are here to add something, add it to the dossier, not to this file.**
This stub replaces the whole document, so any edit here becomes a merge conflict
— which is the intended behaviour, and the only thing that keeps main-track
knowledge from being lost silently at the next merge.
