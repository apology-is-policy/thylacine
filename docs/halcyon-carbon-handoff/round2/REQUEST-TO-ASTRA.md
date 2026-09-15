# Request to Astra -- round 1 (2026-09-14)

Written by the Thylacine side after reading the kit whole and checking it against the tree. Everything below is either data the kit does not contain, or a design for a surface the mockup does not have. Rulings that change the target, so the answers are made against the right one:

- The mono face is **Cornucopia**, not Plex Mono: every mono role (terminal, index, metadata, doc path, code) renders in our own monospace font. Plex Sans stays for proportional text. So treat mono line heights and widths as ours to re-tune; sans geometry, colours and layout are yours and exact.
- The prompt is `λ <path> ⊢ <input>`: the lambda leads, and the `⊢` turnstile stays as the delimiter before the user's input.
- Chords stay on our Super plane; the mockup's Alt bindings are not adopted. Footer hints will name our chords.
- **Workspaces are in.** `WORKSPACE 01` becomes a real, switchable workspace; see item 7.
- Header metadata (`MODIFIED`, `RUNNING`, `PASSED · 1.8s`) will show our own facts; the strings are placeholders.

## 1. Provenance

- A `git bundle create instrument-panel.bundle --all` of the mockup repository, so commit `074bc5646b2f7a03872859890c2891017ceaaf9d` can be verified here. The kit's `reference/index.html` differs from the v5 zip's `source/index.html` (Google Fonts links vs the offline comment); say which one the renders below use.

## 2. Goldens (screenshots we will diff against)

Lossless PNG of the raw page, Chromium, at device pixel ratio 1 AND 2. Freeze: clock `09:41`, status `READY`, caret visible, no hover, default ratios, scroll offsets 0, `prefers-reduced-motion: reduce`. Confirm the loaded faces from `document.fonts` (family, weight, style, and the file URL or hash) in a sidecar text file with the Chromium version, OS, viewport and DPR.

- Carbon at every cell of the ACCEPTANCE-TESTS section-2 matrix: 1440x900 at 100/125/150/175/200; 1280x720 at 100/200; 1920x1080 at 100/200; 820x900 and 821x900; 390x844; 840x600.
- All 13 themes at 1440x900, 100%.
- States, Carbon, 1440x900: each of the 10 tiles opened in turn; a collapsed header hovered; the expanded header hovered; `×` hovered; a dirty tile inactive; dirty + attention; a divider hovered; a divider mid-drag (with the glow); the theme picker open; the help dialog open; a keyboard-focused divider (the focus outline); the narrow branch at 820 with the picker open; a transient status message (`OPENED ...`) and an error one (`FINAL TILE IS PROTECTED`).

## 3. Geometry and style dumps (more valuable than pixels)

For Carbon at 1440x900 100% and 200%, and for the three light themes at 100%:

- `getBoundingClientRect()` for every element with a class or id: rails, brand, context, every rail button, the workspace, every split/split-child/divider, every pane, tile, tile-header, tile-index, tile-name, tile-meta, tile-action, tile-body, and every block inside the documents (`doc-path`, `h1`, `h2`, `p`, `ul`, `li`, `pre`, `code`) and every `.line` in the terminals. JSON, keyed by a stable path (pane id / tile id / element).
- `Range.getClientRects()` per text line of every visible paragraph, heading, list item, `pre` line and terminal line, so we have the exact line breaks and the baseline grid.
- `getComputedStyle` for one element of each class, per theme, with every colour resolved to `rgb()` -- including the `color-mix()` results (the expanded header, the focus inset, the selection).
- The pseudo-element boxes the DOM cannot report (`.divider::before`, `.divider::after`, `.tile-header::before` focus mark, `.brand-mark::before/::after`), as computed positions.

## 4. Fonts

- The exact IBM Plex Sans files (or their hashes and version) the renders use, for 400 / 500 / 600, and whether italics were synthesized.
- Optional, if we can hand over the Cornucopia TTF: a variant render of the Carbon 1440x900 golden with `font-family: Cornucopia` for every mono role, so the mono side has an oracle too.

## 5. ANSI-16, designed, per theme

The kit's tables are derived by a rule (`build_bundle.py::palette()`), and the rule breaks hue meaning: yellow is the signal hue (ink-blue on Genera, teal on Abyssal), magenta is the number colour (brown on Signal), and on the light themes black and white are inverted. Programs use these slots by meaning (`ls`, `git diff`, `grep`, vim). Please design the 16 per theme with: red / green / yellow / blue / magenta / cyan recognizable as those hues in the theme's temperature; black darkest and white lightest for dark themes and the reverse on light; bright variants lighter on dark grounds, darker on light; normal text contrast >= 3:1 against `terminal-bg`; all 16 distinct. If the answer is "keep the derived ones for the dark themes and fix the light ones", say so.

## 6. Scrollbars

The mockup only sets `scrollbar-width: thin; scrollbar-color: structure transparent`. We want, initially, a **position indicator** into the buffer rather than an interactive bar: geometry (width, inset, minimum thumb), colour at both polarities, when it is visible (always / on overflow / on scroll with a fade), for terminal bodies, rich documents and the theme picker.

## 7. Surfaces the mockup does not have, in the same visual language

1. **Workspaces**: N workspaces in the top rail (label, the active one, the inactive ones, the chord hint in the footer, the transient message on switch), and what the brand mark does with more than one.
2. **The object verb menu**: a context menu anchored to a word or object inside a transcript (items, separators, chord hints, a disabled item, keyboard focus), Carbon and one light theme.
3. **Status marks**: the working directory, the last command's mark and a running indicator, as they would sit in the footer and in the top-rail context zone.
4. **Inline media**: an image and a small gallery inside a rich transcript (frame, caption, selection).
5. **Dialogs**: dirty-close confirmation, reset confirmation, and a generic one-line prompt, sharing the help dialog's language.
6. **Tile states**: an empty pane (no tile yet), a crashed / disconnected tile, a tile whose process ended (exit status in the metadata slot).
7. **A full-screen terminal application** (an editor, a monitor) inside the 32px header frame: what the body edge looks like, and the caret when the app owns the screen.
8. **Login** and the pre-login console -- or an explicit "unchanged".
9. **Header actions**: where a tile's commands live if not in the header (an Acme-style tag line, or the verb menu on the header), one proposal.

## 8. Contrast policy

45 of the 156 measured pairs in `contrast-report.json` sit below 4.5:1 outside Carbon (dim, comments and numbers in every other theme; Strogg's numbers at 2.36:1). Either confirm these are intended, or supply adjusted values for those tokens only, per theme, without changing the rest.
