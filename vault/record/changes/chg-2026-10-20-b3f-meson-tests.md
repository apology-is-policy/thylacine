---
id: chg-2026-10-20-b3f-meson-tests
type: chg
title: "FreeType and HarfBuzz through meson, their upstream tests on the device; a test's meson depends are built and staged (B-3f)"
date: 2026-10-20
arc: arc-boosty
commits: ["826135e83", "11277232a"]
touched:
  - sub-substrate-libtests
  - sub-substrate-build
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-20
---
B-3f of Boosty ([[dec-2026-10-14-b3-test-bar]]): FreeType 2.15.0 and
HarfBuzz 14.6.0 are built static through a pinned meson, FreeType twice
around HarfBuzz, and their registered tests run on the device by muster,
which gained per-test `env` and meson's TAP verdict
([[sub-substrate-libtests]], [[sub-substrate-build]]). The first device run
(2026-10-15, `826135e83`) passed HarfBuzz 76 of 76 and FreeType 4 of 5:
issue-1441's two fonts come from a target only that test depends on, which
`meson test` builds before it runs and the cross build never did, and the
translator passed the two missing names through as outputs. `11277232a`
builds each meson tree's `meson-test-prereq` before the list is read and
refuses a test whose depends are not built or not staged. On the CI image of
`11277232a` FreeType passes 5 of 5 and HarfBuzz 76 of 76; with three inputs
broken the device fails exactly the three tests a native `meson test` of the
same pins fails, with the same messages, and is green again restored. The
first bar, hb-shape over `test/shape`, could not hold: hb-shape is built only
with GLib, whose suites the vote skips, so the bar became the compiled lists.
issue-1063's font grants no licence, so an image baked with the library tests
is never published.
