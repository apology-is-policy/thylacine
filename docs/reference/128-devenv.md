# 128 — devenv: the per-Proc environment device (`/env`) [ABSORBED INTO THE VAULT]

Absorbed at the content-Devs sweep
(`chg-2026-08-02-devices-content-sweep`). Its content now lives, code-verified
and current, in:

    vault/system/kernel/devices/sub-kernel-content.md
    vault/invariants/inv-i1.md
    vault/invariants/inv-i32.md
    vault/locks/lock-env.md

Note on the absorbed text: current on its subject, including the minted
per-environment device number and the executable-image cache collision it exists
to prevent — a recent document about a recent mechanism.

It carries one inherited error. It states that the metadata handler makes seeking
to end-of-file work on an environment value. It does not: seekability is a
separate explicit flag that this Dev does not set, and the refusal precedes the
size lookup. The claim is copied from the code comment, so the two agree without
being independent.

---

**If you are here to add something, add it to the dossier, not to this file.**
This stub replaces the whole document, so any edit here becomes a merge conflict
— which is the intended behaviour, and the only thing that keeps main-track
knowledge from being lost silently at the next merge.
