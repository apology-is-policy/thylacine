# The native-target goldens (revision 2) -- the record of the run

**What.** `round2/capture/capture.mjs` run in **native** mode on the frozen
reference (`reference/`, = the bundle's `dist/` at `074bc564`) on
2026-09-14: 98 of 98 scenarios, 0 page errors,
status `complete`. Chromium `153.0.8010.12` (Playwright 1.63.0,
`chromium-headless-shell v1243`), node v24.1.0, darwin
25.4.0 arm64. Overlays: Cornucopia at existing CSS sizes, contrast amendments, prompt turnstile, Super labels, capture freeze.
Fonts injected as exact bytes (`fonts.json`; SHA256 per file in every
`metadata/*.json`): IBM Plex Sans 400/500/600 v3.005 from
`third_party/ibm-plex/ttf/` and Cornucopia Regular v34.6.1 (the operator's
family) in every mono role. Historical mode (Plex Mono) was NOT run: Plex
Mono is on no machine of ours.

**Where the pixels are.** NOT in git. The run lives at
`build/instrument-goldens/native-r2/` on the capturing machine (1.0 GB:
28 MB of PNG, 996 MB of `geometry-styles.json` -- 10 MB per scenario,
the full computed style of ~325 elements plus per-character fragment
rectangles). This directory holds the identity of the run instead:
`manifest.json` (the SHA256 of every PNG), every scenario's
`metadata.json` (its settings, DPR, font hashes, PNG hash), `fonts.json`,
and two reference images for the eye: `carbon-1440x900-dpr1.png` and
`carbon-1440x900-dpr2.png` (the baseline scenario at both backing
factors; their hashes are in the manifest).

**Regenerate** (from the repo root; Playwright and its Chromium in a
scratch dir, never in the tree):

```
cp -R docs/halcyon-carbon-handoff/round2 $SCRATCH/round2   # so node finds $SCRATCH/node_modules
cd $SCRATCH && npm i playwright@1.63.0 && PLAYWRIGHT_BROWSERS_PATH=$SCRATCH/browsers npx playwright install chromium
cd $SCRATCH/round2/capture && PLAYWRIGHT_BROWSERS_PATH=$SCRATCH/browsers FONT_MANIFEST=<fonts.json with absolute paths> TARGET=native OUT=$SCRATCH/out node capture.mjs
```

A second run of `theme-carbon-1440x900-baseDpr1` on this machine
reproduced the manifest's hash exactly (determinism measured for one
scenario, one machine; cross-machine determinism is NOT claimed).

**Collisions, all explained.** 87 distinct hashes for 98 scenarios:
- 6 by the scale convention: `s100-baseDpr2` == `s200-baseDpr1` at 1440x900,
  1280x720 and 1920x1080 (effective DPR 2 both ways) -- the identity that
  validates "200 % is a backing factor";
- 3 by the baseline: `theme-carbon-1440x900` and `state-carbon-open-renderer`
  ARE the matrix baseline (renderer is the fixture's expanded tile);
- 2 by CSS no-op states: `hover-expanded` (`.tile.expanded .tile-header`
  outranks `.tile-header:hover`, so hovering an expanded header changes
  nothing) and `dirty-inactive` (a collapsed dirty tile already carries
  its metadata; only `attention` adds pixels).

**Status.** These are the CANDIDATE oracle of HALCYON-INSTRUMENT section
11, not accepted goldens: one image was reviewed by eye (fonts real, the
prompt `λ path ⊢`, Super labels, the divider at x 737.906 / y 442.719 as
the flex arithmetic predicts). The review of all 98 is owed with the diff
tool at I-9; until then a changed hash is a question, not a failure.
