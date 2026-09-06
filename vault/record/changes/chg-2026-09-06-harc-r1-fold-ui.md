---
id: chg-2026-09-06-harc-r1-fold-ui
type: chg
title: "H-arc round-1 fold (UI + beacon-relay half): the halcyond SpanMap/AltScreen findings, kaua-term scroll_cap sizing, and the ptyhold/ptyhost render-tier relay"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - sub-halcyond
  - sub-kaua-term
  - sub-ptyhold
  - sub-mechanism-drivers
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---
The peer's H-arc audit round-1 close ([[chg-2026-09-06-harc-audit-close-r1]], main's
`839a966f`) carried a `no-dossier-change` deferring the vault prose to this track --
the KT-1 inheritance pattern. It touches five dossiers; this fold takes the four UI +
beacon-relay ones, several of which AMEND the h4d2 folds landed hours earlier this
run. The tapestryd half (six A-F GPU/compositor findings) is a substantial unit
deferred to a follow-up fold ([[sub-tapestryd]] stays flagged). Every symbol verified
in the landed code first.

## [[sub-halcyond]] -- amends the fresh h4d2 SpanMap + Normal-mode prose

- B-F4: the `SpanMap` allocates **lazily** -- 16-byte `SpanSlot`s, `SPAN_MAP_BYTES` =
  128 KiB, `0` bytes for a native tile and `SPAN_MAP_BYTES` once for a rich one, and
  it sits OUTSIDE the scrollback cost budget (not double-counted). The h4d2 prose had
  it as a flat "8192-entry ring"; corrected.
- B-F3: `local_obj`'s remap cache is a `BTreeMap`; named where the h4d2 prose said only
  "copies the obj".
- B-F5: a `Record::Mode(AltScreen)` (an app switching TO full-screen) leaves Normal
  mode on the spot, so a selection cannot outlive the screen it was made on. The h4d2
  prose had the Esc-ENTERS-Normal gate but not this exit.

## [[sub-kaua-term]] -- scroll_cap sizing (B-F1/F2)

`scroll_cap()`'s `per_row` is `cols * size_of::<Cell>()` -- the IN-MEMORY `Cell`, not
the wire cell -- so the cap bounds the actual heap the accumulated rows occupy. Made
the previously-generic `per_row` concrete.

## [[sub-ptyhold]] + [[sub-mechanism-drivers]] -- the render-tier relay (C-F1)

ptyhold gained the shared consctl writer both pts hosts use: `declare_beacon(tier)` +
`relayed_tier()` (a relay declares what its own sink renders). ptyhost calls
`declare_beacon(relayed_tier())` before its mint (C-F1), so the aurora console's tier
is relayed down to a nested session rather than lost at the hop; `pty-4` witnesses
`beacon cells inherited (pts host)`. Both dossiers use generated Provenance blocks, so
the fold-chg's `touched` is the backlink.

`updated:` -> 2026-09-06 on all four. The kaua-term dossier already carried several B-F
findings (the 32 MiB allocator, B-F6, B-F8), so B-F1/F2 slotted into its existing
per-record-CLASS bounds section rather than opening a new one.
