---
id: dec-2026-10-21-fontconfig-layout-dejavu
type: dec
title: "fontconfig keeps its upstream layout, and /lib/font holds all of DejaVu"
date: 2026-10-21
status: standing
decided-by: user-vote
affects: [sub-substrate-build, sub-webkit]
created: 2026-10-21
---
## Fork

`dec-2026-10-14-b3-fontconfig-port` put the fonts in `/lib/font`. Where does
fontconfig's own configuration live, and which fonts go in `/lib/font`?

## Research

- fontconfig compiles its configuration paths in from the build's prefix:
  `FONTCONFIG_PATH`, `CONFIGDIR`, `FC_TEMPLATEDIR` and `FC_CACHEDIR`
  (`meson.build` 412-452). Upstream installs `/etc/fonts/fonts.conf`, with
  `conf.d` links into `/usr/share/fontconfig/conf.avail`, and the DTD in
  `/usr/share/xml/fontconfig`.
- The native pool already keeps ported programs' configuration in `/etc`
  (`gitconfig`, `hosts`, `resolv.conf`) and their data in `/usr/share` (git's
  templates, `LOCALEDIR`).
- The tree vendors DejaVu Sans Condensed (4 faces, from
  `dejavu-fonts-ttf-2.37`, sha256 `fa9ca4d1...`, Bitstream Vera and Arev
  public-domain terms) and IBM Plex Sans (7 faces, OFL). fontconfig's
  `60-latin.conf` prefers DejaVu Sans, DejaVu Serif and DejaVu Sans Mono for
  sans-serif, serif and monospace.
- Plan 9 keeps fonts in `/lib/font` and much of its configuration under
  `/lib`, such as `/lib/ndb`.

## Options

- **Upstream layout, plus DejaVu Sans, Serif and Sans Mono** from the same
  pinned tarball (about 5-6 MB more).
- **Upstream layout, only the fonts the tree vendors.** Serif and monospace
  requests fall back to a sans face.
- **Configuration under `/lib/fontconfig` too.** Nearer Plan 9's habit, away
  from where every ported program and document expects it.

## The call

fontconfig keeps its upstream layout: `/etc/fonts` with `conf.d`,
`/usr/share/fontconfig/conf.avail` and `/usr/share/xml/fontconfig`.
`/lib/font` holds the vendored DejaVu Sans Condensed and IBM Plex Sans, plus
DejaVu Sans, Serif and Sans Mono from the pinned 2.37 tarball (operator vote,
2026-10-21).

## Rationale

Ported programs and documentation expect fontconfig's configuration where
upstream puts it, and the pool's own precedent agrees. With the full DejaVu
family, each generic family matches a real face, which is what WebKit and
`fc-match` ask for.
