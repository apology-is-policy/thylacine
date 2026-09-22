# Acceptance tests — evidence required for an exact native port

## 1. Test environment and proof levels

Pin OS/native source commit, mockup source hash, host browser version, framebuffer dimensions, display scale, font hashes/loaded faces, raster settings, locale, time and pointer position. Disable OS color/night-shift transformations in reference capture. Capture raw framebuffer/sRGB output, not a photograph. Browser CSS viewport and OS LOGICAL viewport must agree: a2880×1800 framebuffer at 200% is a1440×900 logical viewport.

Freeze clock at 09:41, caret visible, statusREADY, menu closed, default ratios, no hover and scroll offsets0 for baseline. Wait for all fonts; verify actual requested faces loaded rather than trusting family declarations. Capture animations at fixed timestamps or suppress motion identically on both sides. Use fixture mode for static content; real process timing is not a screenshot oracle.

Three statuses must never be conflated: (a) bundle validation, (b) native host-unit tests, (c) guest integration and pixel parity. This kit completes(a) only. The published reference in this browser session required sign-in; no fresh live geometry or screenshot baseline was obtained. The local pinned source is authoritative and included. Screenshot fixtures below are tasks for the implementing agent, not existing proof.

## 2. Geometry goldens

| Viewport logical | Scale | Required checks |
|---|---|---|
| 1440×900 |100,125,150,175,200 | Primary3-pane fixture; rails, frame, tracks, all headers |
| 1280×720 |100,200 | Dense layout; visible Rust scroll; header budget |
| 1920×1080 |100,200 | Width-capped text and viewport-based padding |
| 820×900 and 821×900 |100 | Exact responsive threshold and labels |
| 390×844 |100 | Scrollable840px workspace, reachable popup/help controls |
| 840×600 |100 | Wide branch vs workspace lower bounds |

For each logical viewport, first derive physical framebuffer from scale. Assert shared boundaries exactly at the defined snap function; interiors partition without holes/overlap. Check top 34, bottom 25, outer3, divider7, rule2, frame1 and headers32 at 100%; scale through one shared helper. Every painted header edge must match its hit rectangle and the published geometry record. Body Configure dimensions and pts winsize must derive from the body, not outer pane.

Primary fixture assertions:3 panes,10 ordered headers,3 expanded bodies, focused p1/renderer, neutral focus frame, one2×20 focus gutter. Initial ratios.515/.49. Verify both orientations by first-child rectangles, not enum names. Open the FIRST, MIDDLE and LAST tile of each stack; verify separator accounting when expanded is the final article. Closed-body rectangles are absent from hit-testing and screenshot paint.

### Numerical pixel verdict

- Stable opaque interior patches: EXACT RGB8, zero mismatched pixels outside glyphs/effects. Use at least3×3 patches known to be wholly inside each region.
- Structural edges: exact snapped boundary and thickness; zero unaccounted1px seams. If Chromium quantizes a reference boundary differently, record its actual subpixel box and one consistent native snap rule—do not accept arbitrary per-element drift.
- Text layout: exact line count, word wrapping, token ordering and baseline grid. Compare glyph bounding boxes against pinned-font reference. No cumulative advance drift; no replacement glyphs. A real-font variant must be labeled separately, not accepted into the exact baseline.
- Raster coverage: pixel-identical native CPU goldens on the same raster pipeline. Cross-browser/native comparisons may allow per-channel coverage differences INSIDE a tight glyph-edge mask only; record max error, mean error and differing-pixel count. Masking may not hide shifted glyphs, changed font weight, a whole line or backgrounds.
- Transient effects: separate timestamped captures; verify blur/shadow footprint and opacity rather than folding them into an all-image tolerance.

Never accept a generic screenshot similarity percentage such as99%: a missing active gutter or wrong one-row header can disappear in the other99% of black pixels. Assert those tiny features directly. A missing expected object is a failed setup, not a valid zero-difference result. The supplied theme audit records precisely this failure mode when testing a bevel on a borderless single-pane screen.

## 3. State and pointer tests

| Test | Setup/action | Required outcome |
|---|---|---|
| S01 | Open p1 build | p1 focus, build expanded, notes/renderer above, refs below |
| S02 | Click open header again | Body remains open; no0-open state |
| S03 | Focus p2 body | p2 frame/gutter activate; p1 content luminance unchanged |
| S04 | Dirty inactive renderer | Metadata amber, no accent gutter |
| S05 | Dirty+attention | Metadata error overrides dirty; frame still structural |
| S06 | Hover collapsed header | hover fill, primary title, visible× |
| S07 | Hover expanded header | Source's expanded background wins; no full hover fill |
| S08 | Close middle active renderer | Successor build opens; renumber indices |
| S09 | Close last active tile in multi-tile pane | Previous tile opens |
| S10 | Attempt close sole tile | Refused, unchanged process and topology |
| S11 | Native dirty close | Confirmation; cancel preserves everything |
| S12 | Native Reset | No silent process loss; confirmation or geometry-only path |
| S13 | Open each of 10 fixture tiles | Content exactly matches frozen catalog; no swapped IDs |
| S14 | Scroll code, focus away/back | Native preserves scroll anchor, selection, content |

Capture pointer ownership on press/release: opening a tile, closing× and click-away dismissal cannot deliver the same event to underlying terminal. × and header-open are distinct actions even at the shared boundary. Right-click and wheel in a document retain existing Halcyon semantics unless a specified overlay owns the event.

## 4. Split and keyboard tests

Drag each root/nested divider to.22,.50,.78; beyond ends clamps. Doubleclick restores.50. Pointer release outside ends capture. Escape ends at current ratio, not its starting ratio; record the intentionally inherited semantics. Pointercancel and pane retirement release capture without invalid references. A modal opening during drag ends the grab safely.

Keyboard focus on a divider: all four arrow keys match source±.025 rule; tab order does not strand focus after a rerender. Alt+arrow computes nearest center in the requested half-plane with±5 cutoff and depth-first tie-break; provide a diagonal case where edge-overlap navigation would choose differently. Alt+J/K wraps first/last; Alt+H/V creates the correct orientation and focuses a distinct native process. Consumed chords never appear in pts input. Input still works in every pane after repeated splits.

Native minimum constraints: a split that cannot fit both recursive minima is refused with no topology change. With Nheaders, test body exactly54 and one pixel smaller; no usize underflow or offscreen actionable header. Test maximum tree depth/leaf count from current system limits and one beyond. Fuzz split ratios including0,1,NaN/infinity in any floating ingest path; invalid external payload must be refused before state mutation.

Production process checks: new shell PID differs; cwd inheritance follows existing policy; repeated activation never spawns a replacement; hidden child output is drained/bounded; busy hidden processes cannot starve visible input. Resize produces correct cols/rows and SIGWINCH; no freeze at the relay's backpressure edge.

## 5. Themes, ANSI and parser gates

For each of 13 stock files run the REAL `halcyon theme lint <path>` on host/guest as supported. Require all current keys supplied, no inheritance. Assert all 35 exact companion colors match resolved-tokens.json. Parse→wire→parse roundtrip both old and new structures with exhaustive field guards (a size check alone can miss fields fitting padding). Validate name safety including Cc/Cf, bidi marks, zero-width control formats, escape/CR/NUL, long UTF-8 names and invalid IDs. Preserve names with ordinary legitimate Unicode under the documented rule.

Mutation suite with a positive twin for each: missing key, extra key/table, duplicate key, malformed color, alpha, wrong ANSI length, duplicate forbidden ANSI slots, malformed array, invalid integer, bounds−1/+1, excessive file size, unsupported schema/profile, invalid ID, mismatched bundle halves, malformed user file with valid system fallback. Every refusal names an actionable cause; no panic, partial apply, silent clamp or truncated parse. Verify the positive twin loads or the negative test proves nothing.

Runtime selection: cycle all 13, preserve focus/expanded/order/ratio/scroll/selection/PIDs. Toggle dark→light→dark; no stale glyph smoothing, old caret/background or Daylight patches. Test color-only change where dimensions are identical: a new frame is still painted by BOTH session and compositor. Verify late-spawned tile inherits the selected theme. Existing nora repaint uses the supported notification channel; uncooperative explicit-RGB app pixels are not replaced.

Use the deliberate Carbon terminal/UI default mismatch to assert default-color classification compares the terminal pair. Draw default text, SGR reset, dim, bold, object, explicit background, ANSI16 and truecolor; no opaque unintended rectangles. Seed an unrelated explicit color equal to an old syntax value and ensure theme change does not reinterpret it as syntax.

Contrast: check Carbon 9 syntax categories against code_bg each≥4.5:1. Inspect dim chrome and all other themes from contrast-report.json; warnings are not hidden by claiming full accessibility. ANSI slots are derived and require actual TUI testing, not only array-length validation.

## 6. Text and content tests

Assert H1 size depends on display width and not pane width; at 1440 viewport it remains34 when dragging the root. Assert document padding 31.68/43.2 before physical snapping. Wide viewport caps top 34, sides48, H134; narrow caps lower18/20/23. Code line-height 19.8, terminal19.2, prose24.3 are separate values, not rounded once globally. Mixed inline code aligns to the proportional baseline without growing the prose line box.

Measure font advances/bearings for `AV`, `ffi`, `λ`, `→`, `×`, Czech `Příliš žluťoučký kůň`, accented capitals and math/box-drawing samples. Test code punctuation at all supported scales and prevent per-glyph rounding drift. Verify the attached known Cornucopia clipped-diacritic defect is not unknowingly reintroduced through fallback sizing. Establish expected glyph coverage for the actual bundled face set; missing faces fail the typography gate.

Rust copy comparison: select the whole listing and compare exact text from fixture, including apostrophes, `<`, `>`, `&`, macro!, spacing and newlines. Plain identifiers remain code_body in reference mode. No color escape sequences in clipboard. Content remains selectable text; images are not accepted as a substitute. Highlighting malformed/incomplete code must not panic or invoke arbitrary parsing actions from the compositor.

Real shell: type/edit long prompt, multibyte text, resize at wrap boundary, scroll history, select/yank and return to input. Beacon objects remain clickable and semantic in terminal view. `pre` preserves columns; normal rich content joins soft wraps; alternate screen fills only the body and restores the normal transcript/caret on exit. Do not lose state after repeated nora entry/exit or a theme change inside alt-screen.

## 7. Popup and accessibility tests

Picker opens on current theme; arrows wrap, Home/End move, Enter/Space applies, Escape dismisses without mutation, outside click does not select. Popup fits/scrolls at short viewport; all 13 options reachable and correctly checked. The new atomic theme transaction may have latency; show pending state without pretending the theme is already committed. Failed selection retains previous check mark and colors.

Help is modal with focus trap, Escape/× closure, restored invoking focus; no key leaks to terminal. Scale while picker/help open: remeasure anchors, font cache and capture; do not strand an old-generation popup. Test820/821 threshold and menu reachable at 390 width. Expose name/role/state for pane, expanded header, close action and splitter, including value/min/max; tab focus is visible even on an unfocused pane.

Reduced motion: freeze blinking to a visible caret and stop transient animations in production. The browser's blanket `.01ms` infinite animation override is not a safe native blink policy. Static captures retain the same final colors/geometry. This is a listed accessibility correction, not permission to remove the gutter or alter the palette.

## 8. Authority, lifecycle and resource tests

Only renderer or declared HOSTING session can change visual/scale/rail geometry. Unauthorized same-user client, other principal, undeclared connection, idle former seat and retired connection are negative controls; E_PERM precedes mutation. Verify same principal alone is not sufficient. Positive control performs identical payload under legitimate authority.

Crash one kaua-term: only its tile fails; other panes/menus/render/input survive. Crash/retire chrome: compositor-owned capture dismissal and fallback paint work. Logout retires user surfaces and theme authority; system console resumes with its system theme, not last user's Carbon. Trusted SAK behavior stays unchanged. No arbitrary font/file/image path from a theme crosses a privileged boundary.

Maintain bounded atlas pages and layout caches as display scales; measure worst-case large headings, four phases, all 13 switch sequence, long transcripts and popup lists. No unbounded frame allocation, repeated font parse, hidden-tile render loop or static5ms busy poll. Compare CPU-floor latency with pre-migration baseline; record actual frame timings and input response, not an invented guaranteed60fps target. Verify no redraw on idempotent theme set, but redraw on color-only changes.

Required regression lanes: existing theme/scale host suites, no_std native release build, Daylight and Nightjar guest gates, Halcyon transcript tests, composed/pane/session scenarios, Aurora and trusted-console unchanged paths. Use current project command names after discovery; paths cited in old docs are examples, not guaranteed current binaries.

## 9. Release report

Deliver a table listing each requirement/test ID, pass/fail/skip, evidence path, commit and limitation. Include final all 13-theme screenshots and primary Carbon pixel diff, actual font manifest, native lint outputs, state-machine tests, security negative controls and rollback steps. Do not say “pixel-perfect” while any font substitution, missing effect, stale-app palette, geometry mismatch or native gate remains unresolved. If an effect or production exception is intentionally accepted, list it plainly with operator approval.
