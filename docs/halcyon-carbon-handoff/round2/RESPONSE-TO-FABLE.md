# Astra → Fable 5.1 — round 1 response

2026-09-14. This document amends the first kit; it does not rewrite the historical mockup. Read it before continuing against the original acceptance matrix.

## Disposition

| Request | Delivered | Remaining evidence |
|---|---|---|
| 1. Provenance | Complete Git bundle, frozen source, hashes; offline/source difference explained below | None for bundle provenance |
| 2. Goldens | Capture harness and complete named scenario matrix | PNGs NOT captured here; exact font assets missing and available browser lacks the required viewport/DPR control |
| 3. Geometry/styles | Harness exports element/text-range/computed-style/pseudo data | Actual browser-measured JSON NOT fabricated; run harness |
| 4. Fonts | Strict font manifest input and per-file SHA256 capture | Historical Google Fonts bytes were not retained; Cornucopia not supplied |
| 5. ANSI16 | All 13 tables redesigned, 208 slots checked | Native TUI visual trial still required |
| 6. Position indicators | Complete geometry/visibility/polarity contract below | New native surface, not a historic screenshot feature |
| 7. Missing surfaces | One specified design for all nine requested surfaces | New native designs, no historical goldens exist |
| 8. Contrast | 45 explicit replacements, all measured failing pairs now ≥4.6:1 | Whole-UI accessibility is not implied by these 156 pairs |

## Target rulings accepted

Cornucopia replaces Plex Mono in EVERY mono role: terminals, index, metadata, doc path, code, key labels and footer. Plex Sans stays proportional. Mono advance, baseline and line pitch must be retuned to Cornucopia's true metrics. The old browser goldens are a geometric/chromatic reference, not a demand to distort Cornucopia into Plex metrics. The frozen browser source remains unchanged for provenance; a native-target capture overlay uses Cornucopia and is explicitly labeled a new target.

The prompt is **`λ <path> ⊢ <input>`**. λ uses the theme signal; path uses terminal_path; ⊢ uses secondary; input uses text. Single spaces separate segments. Running/output state does not replace λ with a status glyph. Caret remains immediately at the actual input cursor. Never emit a fake prefix in halcyond over bytes the shell also renders.

All workspace/chrome chords use Super; no Alt hijacking. The mockup's Super-equivalent suggestions are not authority to collide with the current chord registry. In particular, if Super+H/V already mean native layout commands, map by actual split orientation. Footer labels MUST be generated from the registered bindings. The reference scripts substitute Super in the help/footer text but do not pretend browser key handling has become the native registry.

Real workspaces are now in scope; use the already-researched live-tree mechanism, per-workspace remembered focus and bounded count9. This supersedes the previous kit's “one workspace only” non-goal. Header metadata is live data, not a mandate to display MODIFIED/RUNNING/PASSED strings literally. All these rulings outrank the first kit's Plex Mono/Alt/single-workspace text.

## 1. Repository provenance

`instrument-panel.bundle` is the requested `git bundle create ... --all`, verified with `git bundle verify`. It includes complete history and `refs/heads/main` plus HEAD at:

`074bc5646b2f7a03872859890c2891017ceaaf9d`

Clone locally with `git clone instrument-panel.bundle instrument-panel`. The exact reference is `dist/index.html`, `dist/styles.css`, `dist/app.js` AT THAT COMMIT, copied unchanged under this response's `reference/`. Verify using `SHA256SUMS` and `git show <commit>:dist/<file>`. No network or source-server credentials are needed to consume the bundle.

The v5 zip's `source/index.html` is an offline packaging derivative: it omits Google Fonts links and replaces them with an offline-font comment. The committed `dist/index.html` carries those links. They are not byte-identical and were never meant to be. The first kit's `reference/` uses committed dist, while its `prototype-offline.html` inlines CSS/JS and changes the default/storage key. For new captures use this response's **committed reference** with only the documented capture overlays. Never compare the offline fallback-font page with a network-font page and attribute the difference to the native rasterizer.

## 2–4. Goldens, geometry and fonts: no invented evidence

I cannot honestly supply the requested pixel goldens in this response. The available cloud browser requires sign-in to view the published page and does not expose the viewport/device-scale controls needed for this matrix. More fundamentally, no exact historical font bytes or Cornucopia TTF were attached. A family name and `document.fonts.check()` are not a font-file identity. Downloading a current font and claiming it was the font used on the original user machine would fabricate provenance.

Included `capture/capture.mjs` runs against the local pinned source on a normal Playwright/Chromium environment. It exports lossless PNG, whole-element DOM rectangles, per-character Range rectangles grouped into visual lines, representative and per-element computed styles, pseudo-element computed styles plus derived border boxes, font identities/SHA256 and Chromium/platform/viewport/DPR metadata. The capture manifest is explicitly **NOT RUN HERE**; syntax checks are not runtime proof. Each produced scenario has its exact overlay settings recorded. Read `capture/README.md` for invocation and interpretation.

The harness requires local font files supplied through a manifest; it embeds those exact bytes in FontFace definitions, aborts remote requests and records hashes. Baseline mode requires Sans400/500/600 and Plex Mono400/500. Native mode requires Sans400/500/600 and Cornucopia; using the one Cornucopia weight for mono-medium is intentional, with no synthetic bold. It preserves the ordinary CSS requested italic where the font lacks an italic face and records that synthesis is requested, not that the font itself contains italics. If Cornucopia has a true italic face, supply a separate face and classify that capture as a different font manifest.

Native mode incorporates the contrast amendments, λ/path/⊢/input prompt and Super hint text. It does NOT invent the native workspace implementation or retuned mono line pitch: use it as a Cornucopia-at-original-CSS-metrics specimen, then commit the measured native retune as its own target revision. Never stretch glyphs to make it pass old text masks.

### Scale convention, clarified

The first kit called matrix dimensions LOGICAL viewports. Thus1440×900 at200% means a2880×1800 framebuffer. Browser backing DPR already expresses this scale. The harness records `nativeScalePct`, `baseDpr` and `effectiveDpr = nativeScalePct/100 × baseDpr`, with CSS viewport fixed at the matrix dimensions. Both baseDpr1 and2 are emitted per requested row; the 200%/baseDpr2 image is therefore5760×3600 and its actual `window.devicePixelRatio` is4, not2. This is intentionally explicit, not double-scaling concealed as “DPR2”. For native framebuffer comparisons, normally use baseDpr1; use baseDpr2 only for a second physical backing factor. If your requested dimensions meant PHYSICAL rather than logical, regenerate using a separately labeled matrix with logical dimensions divided by scale. Do not mix conventions in one diff.

### Baselines and pseudo boxes: limits of DOM APIs

**Correction to first kit:** `.divider::after` declares width5/height5 and border1, but `* {box-sizing:border-box}` does not select pseudo-elements. Its outer box is therefore7×7 at offset(1,1), not5×5. This protrudes1px beyond the7px track on the cross-axis before ancestor clipping. Preserve that source geometry in historical mode. The first kit's joint dimension was the CSS content size mislabeled as outer size; replace that single assertion with content5/outer7. Brand pseudo strokes and the focus mark have no border, so their earlier dimensions stand. The capture dumper accounts for the pseudo's own computed box-sizing.

`Range.getClientRects()` reports fragment rectangles, not typographic baselines. The harness groups character fragments by top/bottom into visible text lines, records exact text ranges and returns computed line-height, but does not call a fragment's bottom the baseline. Recover native baseline from font ascent/descent and line-box alignment; capture a dedicated baseline probe if a browser baseline is required. Pseudo-elements likewise have no getBoundingClientRect; their box is derived from the containing padding box and computed inset/width/height with border-box accounting. The dump marks these boxes DERIVED. A `color-mix(...,transparent)` may legitimately resolve to an alpha color; the dump preserves that CSS value plus RGB(A) canvas normalization. The actual composited pixel depends on the substrate and is obtained from the PNG—not from mislabeling rgba as opaque RGB.

## 5. ANSI16: redesign all thirteen

Replace the previous ANSI arrays in full. The old rule was scaffolding and did not satisfy semantic ANSI color usage. Its signal→yellow and number→magenta aliases are withdrawn, for dark AND light themes.

`ANSI16.md`, `ansi16.json` and `palettes/*.toml` contain the new values. Each table uses recognizably red/green/yellow/blue/magenta/cyan slots, adjusted in temperature to its theme. Carbon uses muted mineral tones; Signal is warmer; Abyssal/Combine cooler; Deus Ex keeps blue and old gold without sacrificing red/green; Shock has a violet bias but real cyan; SiN/Mesa greener neutrals; Strogg warmer and drier. The three light sets darken the chromatic inks on their pale grounds. No semantic slot is taken from the theme's accent merely because the accent exists.

All208 entries meet≥3:1 against their own terminal ground; actual minimum is in `VALIDATION.json`/`ansi-contrast.json`. Every table is internally distinct. In each normal and bright eight-color ramp, black is the darkest/white the lightest on dark themes, reversed on light themes. Every bright slot is lighter than its normal counterpart on dark, darker on light. The extreme black/white ordering is **within each eight-slot ramp**; it cannot mean normal white is brighter than bright white while also requiring bright white to be brighter than normal white.

“Black” on a near-black terminal is necessarily a readable charcoal, not RGB0, because the request requires3:1. Similarly light-theme “black” is the lightest readable neutral ink rather than the light background itself. Terminal default fg/bg are unchanged and separate from ANSI7/15. Pure decorative black remains available as explicit RGB to programs. ANSI color names refer to slots, not a requirement that a literal `SGR30` be mathematically black.

## 6. Position indicators, not scrollbars

Adopt a static position indicator shown **only when content overflows**, without fade. This gives a stable spatial fact, is cheap on the CPU floor and does not turn scrolling into a transient decoration. No drag, click-to-jump, resize cursor or separate keyboard focus. Wheel/keys continue to scroll the content through existing semantics. Do not expose it as an adjustable accessibility slider; expose the region's scroll position instead.

| Surface | Reserved right lane | Thumb width | Right inset | End inset | Min thumb |
|---|---:|---:|---:|---:|---:|
| Rich document body |8px |3px |3px |4px |24px |
| Terminal transcript |8px |3px |3px |4px |24px |
| Theme picker list |8px |3px |3px |4px |18px |

All logical units. Reserve the8px lane inside the content viewport on overflow so text cannot be obscured; this is an explicit replacement of platform-dependent `scrollbar-width:thin`, not a claim that every browser chose8px. The lane has the underlying body/list ground; no drawn track. Thumb is `dim` at full opacity (use the corrected role outside Carbon), rectangular, radius0. Focus does not change its color to signal: position is not an action or focus state. In Carbon this is#737A76; in Genera the revised value is in CONTRAST-AMENDMENTS. The same semantic dim role works at both polarities. No hover treatment because it is not interactive.

For scrollable extentC and viewportV, ifC≤V+0.5 logical px hide it. Let lane lengthL=max(0,V−8), thumbT=min(L,max(minThumb,L*V/C)), travel=L−T, offset=clamp(scroll,0,C−V); leading position=4+travel*offset/(C−V). Snap absolute leading/trailing boundaries through shared scale helper, never allow negative travel. Tiny viewports whereL<minThumb use T=L. When follow-tail is on, offset=C−V and the thumb ends exactly atV−4. Content append recomputes position from the retained anchor; it must not falsely show “at end” while the user reads history.

Raw full-screen apps: no shell indicator over their owned grid. The app draws its own scroll UI. A terminal-history overlay activated by the shell can show the indicator once it owns the view; otherwise keep the raw grid's full usable body. A wide code block may need a horizontal noninteractive3px indicator with analogous rules; reserve8px on overflow only, no fake dragging affordance. Native text width/line breaks are allowed to change by this explicitly specified lane allocation.

## 7. New surfaces

The following are concrete new designs. They share existing colors/fonts and square geometry. Carbon and Genera exact anchors: primary#F2F3EF/#171A19, secondary#AFB4B0/#4E5753, menu/pane#0B0D0E/#E8E9E3, dialog#0D1011/#ECECE6, header#080A0B/#DEE0DA, hover#191C1D/#CDD1CB, structure#454B48/#727A75, signal#C7B98B/#3D526F, error#BD7770/#8E4E4B. Faint ink uses the amended dim token. No new saturated color is introduced.

### 7.1 Workspaces

Top rail remains34. Left brand cluster remains212 wide at the standard layout, mark13×13, gap9. For exactly one workspace retain the reference `WORKSPACE 01` presentation. With more than one, retain the mark and replace the word label with numbered chips `01`..`09` in a horizontal viewport189px wide. Chips26×24, y5, gap4; no outline or rounded capsule. Active chip: hover ground, primary label, a2px signal bottom edge inset4; inactive: transparent ground, secondary label. All labels Cornucopia10. Hover inactive: hover ground and primary ink. Keyboard focus:1px signal inset2, independent of active workspace.

If the chip list exceeds189, reserve16px at each end for ‹/› buttons and use157px of list viewport. Always scroll the newly active chip fully into view. Arrow buttons are secondary, signal on pressed, disabled dim; they only reveal chips and do not switch. No ellipsis hiding the active workspace. At narrow width≤820, replace the word label with the active number even whenN=1, and allow the cluster to shrink to54px with a workspace-picker action: clicking the number opens the numbered list in the same menu style as§7.2, width160. This prevents workspaces displacing all existing rail actions.

The brand mark remains the SAME unanimated structural glyph for allN; it neither grows bars nor becomes a counter. Its only action is opening the workspace list/menu (accessible label “Workspaces”). Clicking a number switches immediately. Super+1..9 switches using the ratified current registry; Super+Shift+1..9 moves the focused tile if adopted by that registry. Switch status: `WORKSPACE 03 · <name>` for1800ms; if unnamed omit dot/name. Footer normally shows `SUPER + 1–9  WORKSPACES` when there are multiple. Chord label comes from binding lookup, not a string assumed forever correct.

Model: live roots, keep dormant processes, per-workspace focus; bound9; inactive empty workspaces may vanish as in the proposal, active one never vanishes. If indices compact, announce the new number in the next switch message and ensure Super numbers map to the displayed labels. Tile move must be an ownership-preserving structural operation, not save/restore/respawn. Layout names are names, not workspaces themselves. Error switching/creating preserves current root and reports `WORKSPACE UNAVAILABLE`.

### 7.2 Object verb menu

Use a compact square menu, min-width224/max-width320, padding4, pane ground,1px structure border; shadow black.24 offset0/8 blur24 is a renderer effect. Anchor its top-left at the object's first visible fragment left/bottom+4. Clamp to display margin4; if insufficient below, place above by4; if neither fits, cap height and scroll items with§6 indicator. Object can span lines: pick fragment under pointer, or first visible fragment for keyboard invocation.

Title row24 high, horizontal10, Sans50011 primary, bottom1 separator; title is bounded object label, ellipsized, not full untrusted path overflow. Menu item28 high, left10/right10, grid flexible label / optional hint, gap16. Label Sans40013; hint Cornucopia10 secondary; hint is blank if unbound. No generic command-icon column. Hover or keyboard-focused item: hover ground, primary label,2px signal left mark fromy6..22. Checked/toggle item may add a literal check on the right before hint; no checkboxes for ordinary commands. Disabled: dim ink, no hover fill/mark, skipped by keyboard activation, still readable. Separators:1px separator with vertical4 margin, inset8. Destructive item uses ordinary label until focused, then error ink; do not paint a red block.

Carbon menu therefore uses#0B0D0E with#191C1D focused row and#C7B98B mark. Genera uses#E8E9E3 with#CDD1CB and#3D526F. Title/header treatment is the same geometry. Object itself retains its selection/affordance during the menu, without dimming the transcript. Up/down skip disabled items; Home/End; Enter; Esc returns exact object focus. Pointer-away dismiss must consume the release under the existing grab discipline. No submenu in v1; longer verbs get an ellipsis/dialog rather than nested hover menus. Menu names are labels; execution routes through the existing typed-object verb engine under user authority.

### 7.3 Directory, command and running marks

Top context: `<cwd> │ <focused tile title>`; cwd secondary, basename segment primary where the formatter can identify it; slash separators remain literal. Use source Sans11 and rail separator1×12/margin10. Ellipsize the middle of the cwd first, then end-ellipsize title. Do not display a static `~/systems/compositor` outside fixture mode.

Footer left: status glyph6×6 area, gap8, message. Normal idle shows hollow square1px secondary and `READY` or last result. Running: filled4×4 signal square centered in6×6 plus `RUNNING · <command>` in secondary; no endless pulse animation. Success: check mark `✓` in success and `EXIT 0 · <command>` secondary. Failure: `!` in error and `EXIT <n> · <command>` secondary; never conflate a stale last failure with current running. Optional measured elapsed time follows dot. Exact command label is the sanitized command mark, bounded to96characters, end-ellipsized by available width; no re-execution by clicking status text. During a transient action message the left slot replaces this display for1800ms, then restores the live model, not a hardcoded READY.

Reference footer center remains compact hints and right actual pane count/LOCAL. Workspace hint takes precedence while switching/moving; otherwise normal registry-derived focus/tile hints. Clock stays top-right. Running metadata in the active tile header may use `RUNNING` secondary, exit error may use error; don't tint the whole header. A header's dirty flag and command status are independent. For width collision, prefer dirty/attention truth over elapsed detail.

### 7.4 Inline image and small gallery

Media is a rich block within the existing720px max content width. Image frame1px separator; no shadow, no rounded corner; inner empty ground code_bg. Default display at native aspect ratio, width=min(intrinsicLogicalWidth,availableWidth), no upscale unless user requests it, max initial height360 with contain scaling. Transparent images composite on pane ground (no arbitrary checkerboard); an explicit transparency-inspection tool can be a later command. An unloaded image reserves declared aspect ratio with bounded min height80/max240 and a small secondary “Loading image…” label. Decode failure uses error glyph+plain text; no broken bitmap icon from an external toolkit.

Caption gap6, Sans12 line1.45 secondary, optional index/path Cornucopia10 on next line with gap2. Block margin18 above/below. Selected media frame becomes signal1px plus a2px leading signal mark; image pixels remain unchanged. Keyboard focus adds the usual inset outline outside the image, not a translucent selection wash over the photograph. Object verbs provide Open/Copy reference/Save as permitted; selection of an image is not consent to execute it.

Gallery:2 columns at content width≥420, otherwise1; gap10 horizontal/vertical; uniform cell image box aspect4:3, contain original image without crop; each caption independently wraps. Maximum visible initial items6, then a text `Show all N images` action in the menu/link style, no hidden unbounded decode. Selected index uses the frame treatment above. Arrows move gallery selection while the gallery owns focus; Super navigation still belongs to workspace. Require alt text and bounded dimensions. Images/video keep full fidelity regardless of pane focus.

### 7.5 Dialog family

Use help's dialog_bg/1px focus_neutral frame and existing backdrop, square corners, maximum480px wide for confirmations,420px for one-line prompt; viewport−32 limit. Header padding18/20/12; eyebrow Cornucopia10 signal/tracking.12em; title Sans50023 margin-top7. Body padding0/20/18, Sans14 line1.5 secondary. Footer top1 separator, padding12/20, gap8, buttons aligned right. Buttons height30, padding0/12, Sans50012,1px structure border; normal transparent, hover hover; default button signal1px border/primary ink (NOT an opaque champagne rectangle); keyboard focus outline signal inset2. Destructive button uses error border/ink, never pre-focused by default. Escape always cancels.

Dirty close title `Close <tile>?`, body `This tile has unsaved changes.` Buttons `Cancel`, `Discard`, `Save and close` only when a real save operation exists; otherwise `Cancel`, `Close without saving`. Default focus Cancel; save failure keeps dialog open with error text, never closes anyway. Active-job close has `A process is still running.` plus validated process label, buttons Cancel/Close tile; force wording only if graceful-close failed and the actual action is force termination.

Reset title `Reset workspace layout?`, body `Rearrange this workspace. Running tiles will remain open.` Buttons Cancel/Reset layout. This is geometry-only reset by design; if the implementation instead restores a layout that starts/stops processes, it MUST use a different explicit title `Restore saved layout?` and disclose that action. Do not put destructive process reset behind the harmless geometry message.

One-line prompt: title describes action e.g.`Rename workspace`; text label Sans12 secondary, gap6; field32 high, kbd_bg,1px structure, padding6/9, Sans14 primary. Focus border signal, selection follows existing selection role. Error below field gap6 Sans12 error; footer Cancel/Apply, Apply disabled for invalid input. Enter submits only when valid and IME composition is not active. Selection and caret belong to the text input, not the terminal beneath.

### 7.6 Empty, disconnected and ended tiles

**Empty pane:** keep flat frame and pane ground, no invented32px tile header because no tile exists. At content top-left padding20 render `Empty pane` Sans50017 primary; below gap8 Sans13 secondary `Open a shell to start here.` One text-style action `Open shell` with signal ink, horizontal pad8/height28, hover hover. A focused empty pane gets the neutral focus frame but no active tile gutter/index. This is the explicit N=0 exception to the nonempty-stack invariant. Shell creation uses ordinary user spawn authority; spinner/disabled action prevents duplicate activation.

**Disconnected tile:** retain its header/order/body transcript and the tile title. Metadata `DISCONNECTED` error; prepend a body notice strip min-height32, header ground, bottom1 separator, padding8/12, Sans12 secondary with error`!`. Text `Connection lost. The last output is preserved.` No blinking input caret. Verb menu offers Reconnect only if meaningful, Restart as a distinct new process, Copy output, Close. Don't clear output or automatically restart a failed command.

**Crashed parser/renderer tile:** same retained frame, metadata `CRASHED`; diagnostic contains safe reason/code but no leaking unrelated data. A failure of one tile cannot repaint the whole screen as an error. Header's focus gutter remains signal; metadata is error: focus and failure remain two facts.

**Ended process:** metadata `EXIT 0` success or `EXIT <n>` error. Freeze last body content, no live caret; subtle final body line `Process ended · exit <n>` Sans12 secondary. Retain tile until explicitly closed or restarted. A child command exiting inside a still-running shell is NOT an ended tile; use last-command marks instead. Native status strings, durations and process labels are data, not exact fixture literals.

### 7.7 Full-screen terminal app within a tile

Keep the32px header exactly as for any active tile. Raw grid fills the content rect immediately below it, with terminal_bg; no inset code fence, no2px amber rule, no rich-doc padding and no shell scroll indicator. Only the outer1px pane frame and existing header separator bound it. Grid allocation rounds DOWN to whole cells; unused right/bottom remainder pixels are terminal_bg, never stretched cells. App-owned status rows remain inside its grid, separate from the OS footer.

Cursor shape/color/visibility follows the application's terminal protocol. Preserve an app's explicit cursor color; otherwise theme signal. Block, bar and underline sizes derive from the Cornucopia cell, not the specimen's7×14px. Do not draw the shell λ/path/⊢ prompt in alt-screen or overlay a second shell caret. Focus loss preserves text colors and follows existing cursor visibility policy; no entire-grid dimming. On normal-mode return restore the shell prompt and transcript position through the mode protocol, not an image of prior pixels.

### 7.8 Login and pre-login console

**Unchanged.** Aurora/pre-login and the trusted console keep their existing identity, palette and trust path. No Carbon login redesign, no reuse of user-specific themes before authentication, no new authority for a session theme file. On logout the console resumes its own system theme. “Carbon default” means the Halcyon user session, not the entire boot/authentication environment.

### 7.9 Header commands: one proposal, header verb menu

Keep the reference header anatomy. Secondary-click anywhere in the header except× opens a **tile verb menu** using§7.2; keyboard invocation through the current registered context-menu chord on the focused header. No permanent extra hamburger, no command pills and no editable tag replacing title. The first section contains program-provided permitted commands, e.g.Save/Save as; separator; shell-owned Rename tile/Move to workspace/Restart/Close as applicable. Disable unavailable actions visibly. If the menu is empty, retain shell actions only.

Commands remain typed actions scoped to the owning tile and use the existing verbs path. A string from a program is a label or a validated command operand, not unrestricted compositor authority. Program commands must register through a bounded existing protocol or a reviewed new registry entry; no invented `pill` mark is assumed built. Middle-click executable-text semantics remain inside the transcript where already supported; header primary click continues to select/open the tile. This preserves the clean gutter/index/title/meta/× layout while keeping commands discoverable through standard secondary-click and keyboard help.

## 8. Contrast ruling

The low-contrast pairs were preserved for first-round historical fidelity, but they are not intended for the revised native target. Replace all45 failing values, not merely the most visible three categories. Breakdown:12 dim,12 syntax-number,12 syntax-comment,5 syntax-attribute,3 syntax-function,1 syntax-keyword. Corrections move each original toward white on dark or black on light until the specified measured pair reaches≥4.6:1, leaving a small rounding margin above4.5. This affects only those roles; no backgrounds, primary/secondary ink, accent or Carbon colors change. Explicit syntax overrides prevent a brighter keyword/function from recoloring the signal/success roles it originally inherited from.

CONTRAST-AMENDMENTS.md gives old/new hex and ratios; contrast-amendments.css is a patch overlay; updated stock/sidecar TOMLs are ready for their respective loaders. Do not rerun the first kit's build script over this directory: it would restore its old derived ANSI tables. Use this response's build_palettes.py, which verifies constraints and requires the first kit beside it when regenerating.

The contrast claims are strictly the156 previously measured foreground/background pairs plus ANSI against terminal-bg. A dim glyph over a hover/open/selected/transparent substrate needs its own pairing. Scroll indicators and disabled states have the policy above; an image's arbitrary pixels are not a text background guarantee. Keep historical captures unamended and new target captures amended; otherwise a corrected comment color would appear as an unexplained native regression.

## Handoff to the operator

All design questions and palette corrections are answered here; the git bundle is supplied. The outstanding items are genuinely measured browser PNG/JSON and exact font provenance. Please provide the Cornucopia TTF and the chosen Plex Sans files/manifest, or run the included capture harness on the implementing machine where those assets exist. Once those files are available, the native-target font specimen can become a reproducible oracle. No historical font hashes, loaded-font claims, text baselines or screenshots have been invented to close that gap.
