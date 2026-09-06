# 151 — libhalcyon: the Halcyon environment library [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-07-libhalcyon-doc-absorb`).
`usr/lib/libhalcyon` — the native (`no_std`, libthyla-rs-family) library carrying
the Halcyon environment's shared code: `theme` (the Daylight token source),
`layout` (the `halcyon-layout v1` save format), `skeleton` (the pure restore
planner), `tag` (a tag as a command line) — plus the `usr/halcyon` tool that drives
them. Its content lives, code-verified and current, in:

- **the crate** — `theme` as the single token source (`DAYLIGHT` = the visual
  scripture verbatim, the four-bevel-one-derivation, `hairline(t) == header`,
  `daylight_palette()` agreeing with the Sheet), the `halcyon-layout v1`
  serializer + the bounded no-panic parser + `from_render_text`, the `skeleton`
  restore planner (the nest-vs-flatten MODEL of the compositor's split rule, refs
  not ids, focus replay), the `prune_env` dissolve, and `tag`'s `argv_of`/
  `resolve_prog`/anchor rules:

      vault/system/userspace/runtime/sub-libhalcyon.md

- **the `halcyon` tool** (`usr/halcyon`, **now folded here**) — `name_is_valid`
  (traversal closed by construction), the save as the aurora durability discipline
  (write-tmp / content-fsync / atomic-rename / metadata-fsync), and the restore
  executor (the `Session(principal)` peer whose acts are judged as the user's; the
  `skeleton::plan`-driven build with a live-dump id-binding that **verifies each
  split's predicted nest/flatten and aborts rather than misplace**; the one-shot
  `TAPESTRY_CLAIM` seeding; the H-4d-1 session-compositor anchor placement):

      vault/system/userspace/runtime/sub-libhalcyon.md   ("The halcyon tool" section)

- **the compositor authority the tool is a client of** — the `Session(principal)`
  actor, the owner-gated one-shot placement claim, `PFK_OWNER`, and the H-4b audit
  F1 build-then-fill window (harmless under single-session; the fix lands with
  multi-seat) — prosecuted where the authority lives:

      vault/system/userspace/services/sub-tapestryd.md   (audit: hard)

- **the token auto-consume + the save discipline it mirrors**:

      vault/system/userspace/runtime/sub-libtapestry.md   (TAPESTRY_CLAIM auto-consume)
      vault/system/userspace/shell-tui/sub-aurora.md       (the config::save durability pattern)

**What this file got WRONG or MISSED by the time it was absorbed:**

- **One genuine gap, now folded — the `halcyon` tool was UNOWNED.** sub-libhalcyon
  covered the pure crate but *explicitly* scoped itself to "the pure, authority-free
  planner + save format", so the authority-bearing tool (`usr/halcyon/{lib,main}.rs`)
  had no home — its `name_is_valid` traversal defense, the save durability
  discipline, and the restore executor's abort-not-misplace divergence check were
  homeless. Now the crate's driver, folded into sub-libhalcyon (the authority gate
  stays sub-tapestryd's); `usr/lib/libhalcyon/src/tag.rs` (also unclaimed) was added
  to its code list in the same pass (`chg-2026-09-07-libhalcyon-doc-absorb`).
- **Everything else was covered** — the theme scripture, the layout format, the
  skeleton model, `prune_env`, the H-4b F1 caveat (sub-tapestryd), the token
  auto-consume (sub-libtapestry). The `TAG_BAR_H`-vs-`header_h` one-name discipline,
  the Frutiger-Aero-second-theme seam, and the width/indexed-color seams are as-built
  notes the dossier holds. Zero code change.
