# 152 — kaua-term: the per-tile terminal process + the seam record stream [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-kaua-term-doc-absorb`).
The crash-isolated per-tile terminal (KT-1): one process per session tile, spawned
by the per-user halcyond as the user, holding the pts, running the VT parser over
the app's output, and shipping halcyond a pre-digested RECORD stream instead of
bytes. Its content lives, code-verified and current (the dossier is dated the same
day as this absorption), in:

- the **whole kaua-term surface** — the `Producer` (vt bytes -> records with a
  shadow screen), the `wire` codec both directions, the process topology, the
  RECORD-stream ordering, the **per-record-CLASS bound** ("the security core": a
  4 KiB read can yield ~30K rows or 512 alt-screen toggles, so `feed_into`'s sink
  triggers on `cells_in` after *every* boundary, not per read), the **span serial**
  (the 17-byte wire cell carrying the OSC-1936 serial, so a dropped/oversize Beacon
  frame can never shift cells onto the wrong span — the anti-clickjack property),
  the **resize ordering** (`drain_pending` rows-only before `resized`'s full diff,
  so an equal-cell-count resize never diffs the new cells at the old pitch), the
  bounds (32 MiB heap, `MAX_TITLE`, the 200x1 ms master-write back-pressure), and
  the concurrency (the master-write futex mutex, lock-free reads, two benign
  relaxed atomics):

      vault/system/userspace/shell-tui/sub-kaua-term.md   (audit: hard)

- the **companions** — the VT parser (`sub-lib-vt`), the pts hold (`sub-ptyhold`),
  the record consumer (`sub-halcyond`), and the `--beacon` tier the hosted shell
  reads (`sub-utopia-interactive`'s `env_beacon_tier`):

      vault/system/userspace/shell-tui/sub-lib-vt.md
      vault/system/userspace/runtime/sub-ptyhold.md
      vault/system/userspace/shell-tui/sub-halcyond.md
      vault/system/userspace/shell-tui/sub-utopia-interactive.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold onto a current dossier.**
  `sub-kaua-term` (audit: hard, updated the same day as this absorption) carries
  every atom, including the two the audit rounds hardened: the per-record-CLASS
  bound (the alt-screen-toggle amplifier that made a 4 KiB read 512 screens before
  round-3 F1) and the span-serial anti-clickjack. The KT-1 audit round records
  (`adt-kt1-r1`/`r2`) are in the vault; the AUDIT-TRIGGERS row remains the code
  trigger. No fold owed.
