# Halcyon Instrument profile — exact implementation specification

Version 1.0 · 2026-09-14

## 1. Objective and meaning of EXACT

Implement the existing Instrument Panel interface natively inside Halcyon. Carbon Optics is the first-launch default; all 12 other mockup themes remain selectable. This is not permission to produce something merely inspired by it. Retain the stable vertical header stacks, the lambda prompt, the short champagne focus mark, neutral pane focus frame, hard corners, quiet near-black fields, and generous rich-text composition.

Separate three deliverables:

1. **Reference mode:** deterministic demo content and layout reproducing the browser sample. This is the visual and interaction oracle.
2. **Production mode:** identical chrome and composition with real Halcyon tiles, processes, selection, diagnostics and file state. Replace sample facts with real facts, not hardcoded mockup strings.
3. **Legacy mode:** existing Daylight/Nightjar behavior remains available as a rollback path, including its original metrics and type identity.

Exact means exact flat RGB values, geometry, ordering, text hierarchy, visible affordances, and stable-state behavior at a specified viewport and scale. Browser and native glyph coverage can differ; establish pinned font files and native raster goldens before claiming byte-for-byte text equality. Never call a fallback-font rendering pixel-perfect. Source CSS is the oracle where this document has abbreviated a rule; explicit amendments in §2 and the production exceptions in §11 override source bugs, not its visual design.

## 2. Precedence and explicit amendments

Current operator request > this Instrument-profile contract > pinned mockup source for unstated visual details. Attached Halcyon documents continue to govern security, transport, ownership, boundedness and nonvisual semantics. Later AS-BUILT amendments in those documents outrank earlier design sketches. Preserve the older files as history and add a dated migration amendment in the OS repository.

| Earlier contract | Instrument target | Scope of amendment |
|---|---|---|
| Daylight default, paper-light | Carbon Optics default, near-black | New profile only; preserve Daylight fallback for legacy mode |
| Fixed orange ember #E07840 | Theme signal; Carbon #C7B98B | `ember` is a compatibility field, not a mandate to recolor Carbon orange |
| Four-face 2px NNW bevel plus inner hairline | Flat 1px pane border; 7px divider track | New layout profile, not invalid `bevel=0` in old schema |
| 20px headers and status bar | 32px headers, 34px top rail, 25px bottom rail | Shared profile metrics and compositor carve |
| No pane-level focus, status-colored bounded content | Neutral focused pane frame + 2px short header gutter | Do not carry old green/red full-body outline into Instrument |
| Header command pills, never-truncated name | Index / ellipsized title / metadata / close | Keep executable commands accessible elsewhere; do not insert pills into reference headers |
| Plex Sans Text 450, italic lightweight headings | Sans 400/500/600, roman 500 headings | Profile type mapping, fixed faces outside theme palettes |
| Cornucopia for all mono roles | Plex Mono for reference terminal/code/technical chrome | Required for exact visual mode; preserve Cornucopia legacy and fallback coverage |
| Proportional prompt + ordinary output | Mono terminal-view style; proportional rich-document style | Explicit presentation mode, not a change to VT or pts semantics |
| Prompt turnstile ⊢ | Lambda λ U+03BB | Change the real prompt producer or semantic marker, never blind byte substitution |

These are visible consequences of asking to implement this exact mockup. They are not claims that the old documentation already allows the result. If repository-level change controls demand separate ratification, present this table before crossing that control; do not silently substitute old metrics and claim parity.

## 3. Source evidence and architecture baseline

| Source | Relevant authority and caveat |
|---|---|
| HALCYON.md §13.1–13.5 | Brain/display-list/CPU floor; older fontdue and in-process VT text is superseded |
| HALCYON.md §14.1–14.6 | Every terminal on its own pts; isolated kaua-term; ordered cells/control seam |
| HALCYON.md §14.11–14.13 | Ingest, mode boundary, session identity, proportional live rendering and theme export |
| HALCYON.md §13.6–13.7 | Tapestry carve/paint, chrome/menu/status surfaces, claim placement, layout restore |
| HALCYON-VISUAL §§2–8 | Old bevel, focus and typography conventions requiring the explicit changes above |
| HALCYON-COMPOSITION §§1–4 | Logical units, typography baselines and composition discipline |
| HALCYON-SCALE §§3–7,10 | Compositor-owned scale, common metrics, authority, fans, resource bounds |
| HALCYON-TYPE §§4,6–7 | Landed skrifa/zeno, outline smoothing, fractional pen, live subset mono |
| HALCYON-THEME §§3–6 + TEMPLATE.toml | Current complete schema; opacity and atomic validation; no font keys |
| halcyon-status TH-4/TH-6 rows | Actual fallback is next valid lower tier; default-ink and theme-fan regressions |
| HALCYON-WORKSPACES §§4–7 | Proposal only: live workspaces, pills, diagnostic marks are NOT established implementations |

Do not implement the old in-process VT design or reintroduce fontdue from an early paragraph. Do not implement multiple workspaces just because the top label reads WORKSPACE 01: the reference has one workspace. Do not promote the attached proposal to built status.

### Ownership

- `tapestryd`: authoritative pane topology, all structural rectangles, input ownership, divider grabs, display-level rail reservations, visibility/dormancy, scale and admission checks.
- `halcyond`: session-owned tile models, transcript/Beacon composition, visible header text and semantic hit regions, rich-document layout, theme choice UI, per-tile state, glyph shaping/raster cache, display lists.
- `libhalcyon`: shared immutable theme/profile/metrics types, pure geometry helpers, strict parsers, mapping/export code and host tests.
- `kaua-term`: isolated pts master + generic VT parse and input encoding. No CSS, font rasterization or competing layout authority.
- `cartoon`: already-laid drawing operations, clipped CPU execution; future GPU executor consumes the same resolved geometry and color.
- Hosted editors/tools: truthful filename/dirty/status facts and syntax classifications. Plain VT truecolor remains producer-owned.

Suggested native touchpoints, to verify in the actual tree: `usr/lib/libhalcyon/src/theme.rs`, `usr/tapestryd/src/{pane,server,chords}.rs`, `usr/halcyond/src/{session,chrome,chromeset,status,layout,tile,raster,outline,transcript,select,menu}.rs`, `usr/lib/libtapestry/src/lib.rs`, `usr/lib/cartoon`, `usr/halcyon`, `usr/utopia/shell`, `usr/lib/vt`, `usr/kaua-term` and the current nora palette/highlighter module. A file not present is a reason to locate the contemporary owner, not create a duplicate implementation.

## 4. Domain model: do not confuse a visual pane with a current Tapestry leaf

The mockup's `Pane` is a spatial slot containing an ordered list of tiles. Current Halcyon often uses a Tapestry leaf for one hosted terminal, with stacked/tabbed container machinery above it. Audit that difference first. A new Rust struct named Pane without integrating Tapestry visibility and input would create two incompatible desktops.

Recommended representation: reuse compositor-owned split containers for spatial divisions and an explicit **InstrumentStack** presentation of an existing stack container for each visual pane. Each stack child remains a real hosted leaf with its own lifecycle. The stack container owns the outer frame and the ordered header allocation; only the active child's body is visible. A one-child InstrumentStack still has its header and frame, unlike the legacy single-fullscreen suppression rule. Adapt existing `Stacked` if it can express exactly this model; otherwise add a distinct mode with versioned serialization and admission, leaving legacy modes intact.

Logical data contract (names illustrative, not existing APIs):

```rust
struct WorkspaceView { root: SpatialNode, focused: TileRef, revision: u64 }
enum SpatialNode {
    Split { id: SplitId, axis: Axis, ratio: Ratio, first: Box<Self>, second: Box<Self> },
    Stack { id: StackId, tiles: Vec<TileRef>, expanded: TileId },
}
struct TileViewState {
    id: TileId, title: String, metadata: String, dirty: bool, attention: bool,
    content_mode: ContentMode, scroll: ScrollAnchor, selection: SelectionState,
    // Real process/pts/transcript handles remain in their existing owner.
}
```

Use stable IDs independent of labels, tab indices, addresses or wall-clock timestamps. A tile ID is not a catalog key reused in multiple panes. The reference copies catalog content when splitting; production must allocate a distinct tile and process.

### Invariants

1. Every nonempty visible stack has exactly one expanded tile and one header per tile.
2. Stack order does not change on selection. Earlier headers sit above the open header/body; later headers sit below that body. The open header is always above its own body.
3. There is one globally focused tile; its stack receives the focused pane treatment. Every other pane retains its own expanded tile without an accent gutter.
4. Spatial splits form an acyclic tree; children partition the parent minus one divider track. No leaf interiors overlap; menus/dialogs are the explicit overlay exception.
5. Hidden bodies cannot receive pointer input, contribute damage, or tick visible animations. Their real processes are not killed or restarted by a collapse.
6. Geometry is computed once and consumed by paint, hit-test, input and winsize. No painter subtracts a second header or legacy bevel.
7. Focus, expanded state, dirty state, last exit, running state, hover and keyboard focus are independent facts.
8. Every retained tile keeps its scroll anchor, selection and editor/process state across focus, theme, resize and header changes.

### Dormancy and the header exception

Existing dormancy stamps hidden leaves zero-rect and suppresses FRAME/CONFIGURE. A collapsed tile still needs a visible header whose geometry is owned by its containing stack. Do not ask a zero-rect hidden leaf's ordinary chrome surface to magically paint that header. Add stack-level header geometry and renderer-owned display-only header surfaces, or one strip-list surface per stack. Publish `(stack_id,tile_id,header_rect,body_rect?,expanded,focus,revision)` in one coherent snapshot. Protocol shape is NEW work: version it, bound counts and check the current ABI before choosing an encoding.

Header clicks route through the session/chrome action channel under the seat's authority. The header surface must not become a hosted client that can receive Direct, impersonate a tile, or escape the renderer gate. Existing chrome being non-focusable is a security property to retain, not an inconvenience to remove.

## 5. Exact spatial geometry

All values below are logical CSS-equivalent pixels at scale 100%. Origin is display top-left. Rectangles are half-open. Border widths are included in specified box sizes (`border-box`). Corner radius is zero for all workspace elements.

| Element | Logical value | Exact interpretation |
|---|---:|---|
| Top rail | 34 high | Includes 1 bottom structural border |
| Bottom rail | 25 high | Includes 1 top structural border |
| Workspace | H−59 high | Between the two rails |
| Workspace padding | 3 each edge | Independent of internal divider width |
| Divider track | 7 | Occupies real layout space, not overlay |
| Divider visible rule | 2 | Offset 2 from track leading edge |
| Divider joint | 5×5 | Leading corner offset (1,1), 1px structure border, desktop fill |
| Pane frame | 1 | Ordinary pane_border, focused focus_neutral |
| Tile collapsed box | 32 high | Includes its bottom separator if not last |
| Tile header | 32 high | Index 32, flexible title, intrinsic metadata, action 28, 3 gaps of 7 |
| Focus mark | 2×20 | Header x=0, y=6..26, only focused expanded tile |
| Action glyph area | 28 wide | Inner visual height 24; × size 15 |
| Index divider | 1 | Right border of 32px index column |

Display `(0,0,W,H)` yields workspace content root `(3,37,W−6,H−65)` at 100%. Rails remain visible regardless of pane count; one-pane Instrument mode must not become a legacy borderless fullscreen layout. Explicit application fullscreen/zoom remains a separate command, not an accidental optimization.

### Split arithmetic and orientation

Mockup `vertical` means a vertical dividing line: first child LEFT, second RIGHT. Mockup `horizontal` means a horizontal dividing line: first ABOVE, second BELOW. Halcyon's `splith` in the attached welcome layout means left/right placement; never infer a translation from English labels. Write an adapter test that inspects rectangles for each native enum/verb.

Initial root ratio is 0.515. Its second child is a horizontal split with ratio 0.49. CSS gives both children percentage flex bases summing to 100% and a nonshrinking 7px divider; proportional flex shrink produces ideal extents `first = r*(E−7)`, `second = (1−r)*(E−7)`. Preserve this final geometry, not a naive `r*E` plus another divider. Initial visual stack order and contents are in `fixtures.json`.

At 1440×900, scale 100, ideal root extent is 1434×835; first pane width 734.905, divider x=737.905..744.905; right extent 692.095; its top pane height 405.72, horizontal track y=442.72..449.72; bottom height 422.28. These are mathematical reference positions, not a claim of measured Chromium raster edges. Native snapped intervals must partition the same boundaries. Capture the browser to settle subpixel-layout-unit differences before promoting a screenshot golden.

Keep layout coordinates in at least 1/256 logical-pixel precision or equivalent rational arithmetic; snap shared absolute boundaries with round-half-up after display scaling. Derive adjacent widths by subtraction of snapped boundaries, never round both child widths independently. Flat rules occupy integer physical pixels, at least 1. At 125%, a 2px rule becomes 3 physical pixels. Glyph positions keep the existing fractional pen; do not quantize the pen cumulatively to quarter pixels.

### Header/body allocation

Inside a pane frame, let available height be A, tile count N, open index k. Collapsed tile boxes have height 32. Expanded article height is `A−32*(N−1)`. Its header is 32 and its bottom border is 1 unless it is the last tile; therefore body height is `A−32*N−b`, b=1 for a nonlast expanded article, otherwise 0. The other separators are already inside collapsed 32px boxes: do NOT subtract all N−1 again. CSS's collapsed 32px header may be clipped at its article border; this is part of reference border-box accounting.

Allocate headers before/after the expanded article in stable order. The final tile has no bottom separator. Bodies and text clip to their allocated rectangles. No header floats, reorders, overlaps content, or leaves a hidden body capable of hit-testing.

The sample enforces only ratio bounds 0.22..0.78, not usable pane minima. Production must additionally refuse a new split that cannot fit a 260px-wide pane and a 54px body after its header budget (new safety rule, from the earlier handoff's design intent, not implemented by the browser). For a stack, minimum outer height is `2 + 32*N + b + 54`. A split's minimum is recursive: along its axis sum child minima plus 7; perpendicular take max. Intersect that constraint with ratio bounds; if impossible, report refusal without changing topology. An already-loaded layout on a smaller display must retain data and offer pan/scroll, not delete tiles.

## 6. Pixel rendering and state priority

Use the 35 resolved roles in `resolved-tokens.json` / `ui-palettes/*.toml`; see conversion guide for legacy mappings. All opaque fields are exact sRGB RGB8. Do not derive Carbon's black levels from a single background or use opacity to dim inactive content. Inactive rich text and media remain fully legible and unchanged.

Paint order: rail/workspace grounds; split tracks; pane frame + interior; tile article grounds and separators; header background overlay; index rule, labels, metadata, action; active gutter; clipped body content; selection/caret; permitted transient overlays. A pane focus inset overlay is bounded within the frame. New geometry damages both old and new bounds.

| Element/state | Color source / treatment |
|---|---|
| Desktop/track | desktop |
| Pane interior | pane |
| Collapsed tile | header |
| Expanded article | open |
| Ordinary pane border | pane_border |
| Focused pane border | focus_neutral, plus 1px inner text-at-3% overlay |
| Tile separator/index divider | separator |
| Expanded header | text at 1.5% over open |
| Collapsed header hover | hover; labels text |
| Expanded header hover | expanded rule wins background in source; labels text |
| Header title | secondary collapsed, text expanded/hover |
| Index | dim, or amber only for focused+expanded |
| Metadata | dim; dirty overrides to amber; attention overrides dirty to error |
| Focus mark | amber, only focused+expanded |
| × | hidden collapsed idle; visible expanded or hovered; dim, then error on × hover |
| Divider | structure; hover amber_muted; drag amber |
| Code block | code_bg, code_body, left rule amber_muted |
| Terminal | terminal_bg, terminal_text |

Source order matters: the expanded header's later background declaration overrides the earlier hover background. Do not turn all open headers into hover-color bars. A collapsed dirty tile may have champagne metadata but never the focus gutter. A failed command changes diagnostic/metadata ink, not the whole header ground.

`color-mix(in srgb, text 1.5%, transparent)` is source-over with alpha .015 on the existing expanded ground. At Carbon open #121516 this resolves approximately to #151819 under round-nearest RGB8. Precompute only when the substrate is known; do not paint transparent pixels as black. Selection is amber at 15% over actual content plus a 1px bottom amber_muted rule. A focus inset is text at 3% over the pixels it overlays, not a single globally valid hex.

### Source effects that are not theme tokens

The source still has literal effect colors: divider drag glow `rgba(213,154,66,.25)` blur 10; split flash fill the same RGB at .04; status pulse glow `rgba(112,161,124,.25)` blur 8. These remain amber/green even on Carbon and light themes. Theme-menu shadow is black .32, offset 0/20, blur 55; help shadow black .35, offset 0/24 blur 80; backdrop RGB(3,4,4) at .72 plus blur 3. The swatch has a white .12 inset border. Record these in the profile effects, not the opaque theme TOMLs. Exact mode preserves them. A later cleanup can make them semantic, but that is a deliberate visible change.

Opaque theme-file requirements do not prohibit renderer-owned transient compositing. If the existing CPU executor cannot express blur, implement bounded renderer-generated alpha masks for these small effects and, for the modal backdrop, a downsampled bounded blur of the permitted scene. Do not require Vulkan to see the UI. Absence of these effects is an explicitly incomplete parity milestone, not a reason to disable a security bound.

## 7. Typography and document composition

### Fixed font assets

The supplied TYPE document records that native Plex kerning is zero because the old path does not read GPOS. A browser can apply kerning and standard shaping. Exact line wrapping requires matching the pinned browser's advances, including tracking, kerning and enabled ligatures—not only nominal font size. Add the needed bounded shaping/GPOS support in the existing layout owner, or produce measured proof that the chosen reference run does not use those features. Do not turn kerning off in the reference merely to hide native drift. Preserve baseline/phase precision; the executor still must not measure text.

The browser requests Plex Sans 400/500/600 and Plex Mono 400/500. Its CSS also requests italics without loading dedicated italic files; an exact browser run may synthesize them. Pin actual font files, version, hashes, coverage, synthesis and shaping settings in a font manifest before final capture. No font binaries were attached to this kit. Never infer identical metrics merely from a family name.

For Instrument v1, faces belong to a fixed profile asset registry, NOT a theme key or arbitrary file path from a theme. Keep legacy Plex Text/Bold/Italic and Cornucopia available in legacy mode. Supporting Plex Mono in this profile is an explicit identity amendment; retaining Cornucopia instead is a valid alternative design but fails literal type parity and must not be silently called exact.

Use the landed skrifa/zeno pipeline. Keep smoothing 0 for dark; the compatibility light themes use 12 as the Halcyon convention, which may differ from the browser's host rasterizer. Keep native quarter-pixel horizontal phases, integer baselines, fractional advance accumulation at 1/256px precision, and no default hinting. Do not simulate weight 500 with a bold face or apply text-smoothing CSS as a native rendering specification.

| Role | Face | Size / line-height | Weight and tracking |
|---|---|---|---|
| Rail | Sans | 11 / normal | 400; .08em; uppercase |
| Brand | Sans | 11 / normal | 600; inherits .08em |
| Rail button | Sans | 10 / normal | 400; .08em |
| Clock | Mono | 11 / 1 | 500 |
| Footer | Mono | 10 / 1 | 500; .08em uppercase |
| Header title | Sans | 13 / normal | 500; .01em |
| Index | Mono | 10 / 1 | 500 |
| Metadata | Mono | 10 / 1 | 400; .04em |
| Doc path | Mono | 10 / 1 | 500; .09em; uppercase |
| Body | Sans | 15 / 1.62 =24.3 | 400 |
| H1 | Sans | clamp(23, .024*viewportW, 34) /1.12 | 500 roman; −.025em |
| H2 | Sans | 17 /1.3 =22.1 | 500 roman |
| Inline code | Mono | .86*body =12.9 | 400; inherits body line box |
| Block code | Mono | 12 /1.65 =19.8 | 400; keywords500 |
| Terminal | Mono | 12 /1.6 =19.2 | 400 |

Body padding: top `clamp(18,.022*displayLogicalWidth,34)`, left/right `clamp(20,.03*displayLogicalWidth,48)`, bottom 50. IMPORTANT: CSS vw is viewport width, not pane width. Resizing only the left pane does not change its font sizes or padding. At 1440 wide: top 31.68, side43.2, H1=34. Use the viewport's logical width after display scaling.

Doc path margin-bottom 28. H1 margin0/0/14; H2 margin-top 28, bottom 10. Paragraphs inherit the browser UA block margins (1em above/below at 15px) and lists likewise; normalize these explicitly in native code. Implement CSS vertical margin collapsing between adjacent in-flow blocks, including H2 and paragraphs: use the maximum positive adjacent margin, not their sum. Doc path and first H1 are block siblings; trailing margins and child layout must follow the pinned DOM. Body width caps on H1, p, ul and pre are720, NOT a centered 720px whole document. H2 is not width-capped. List padding-left 20; list item top/bottom 5, padding-left 5. Pre margin18 top/bottom, padding 15 top/bottom and 17 sides, 2px left border included; horizontal overflow scrolls. Long code stays preformatted; no hard wrap. Terminal lines use pre-wrap; preserve spaces and wrap only as the source's CSS text layout specifies, without interpreting terminal lines as HTML.

Rich prose/objects still come from semantic content. Use these styles for the Instrument rich-document presentation, while retaining the existing content model. Terminal-view style may paint normal shell output mono without removing Beacon meaning, object hit regions or typed selection. Raw alt-screen continues to honor exact cells, cursor addressing, wide glyphs and app-owned backgrounds. Do not apply a 19.2px prose line box to a grid whose cell metrics require a different pitch without changing winsize coherently.

The lambda is painted at the prompt's semantic location in amber; cwd uses terminal_path; typed command uses text. The sample cursor is a 7×14 block, left margin2, baseline offset−2, amber. Reference animation: 1100ms with `steps(2,start)` and opacity0 at 55%; preserve the source keyframes for screenshot timing. In production the active caret position/visibility comes from the grid/selection state. A paused or unfocused tile must not show a second fake cursor.

## 8. Rust highlighting

The renderer document includes a genuine syntax-colored Rust listing, not an image of code. The browser sample is a read-only illustrative fragment, not a complete compiling crate: `SplitNode`, `Axis` and `Rect::split_x/split_y` are not defined there. Do not use its compile status as a UI acceptance condition. Copy/select must return the original code characters, with no inserted markup, ANSI bytes, line numbers or altered spaces.

| Classification | Carbon hex | Treatment |
|---|---|---|
| keyword/control flow | #C7B98B | Medium500 |
| type/trait/enum path variant | #8EA4B8 | Regular |
| function/method | #91AA98 | Regular |
| string | #B99A7B | Regular |
| numeric literal | #A693AD | Regular |
| attribute/macro | #B58B70 | Regular |
| lifetime | #9D8FA5 | Italic |
| comment | #77807C | Italic |
| punctuation/operators | #A6ACA8 | Regular |
| unclassified identifier | #C5CAC6 | code_body |

All nine Carbon syntax colors exceed4.5:1 against #090B0C (see generated measurements). Do not brighten or saturate them on hover. Syntax color conveys code meaning, not focus, dirty state or success/failure. `MIN_RATIO`/`MAX_RATIO` identifiers in the pinned snippet are unclassified base ink even though their values are numbers. A real highlighter can classify constants semantically, but use the exact reference spans for the reference fixture.

Production syntax classification belongs to a real tokenizer/editor, not regex coloring in the compositor. Preserve category IDs until palette resolution when possible, so theme switching can recolor without reparsing source. A VT producer that emits already-resolved truecolor needs a cooperative repaint on theme change; changing the compositor's palette cannot identify which old RGB values were syntax versus user-authored colors. Do not search-and-replace RGB values across the grid.

## 9. Rails, picker, help and responsive behavior

Top rail horizontal padding 10 left/8 right. Brand reserved width 212, internal gap 9; mark13×13 with muted-signal1px border and two signal1px strokes. Context is single-line, clipped; strong fragment weight 500. Vertical rail separator1×12, horizontal margin10. Right actions have auto left margin and gap 2; each action height 26, horizontal padding 9, transparent 1px left/right borders; hover fills hover and borders structure; pressed ink amber. Icon-only min-width 28. Clock padding left 10/right 5. Preserve reference labels/order: Split H, Split V, theme, Reset, ?, clock. In production Reset opens the safety flow below rather than deleting active work.

Footer left: 6×6 success square, margin-right 8, status message. Center: `ALT + ARROWS / FOCUS · ALT + J/K / TILES` with gap 8 and dim/secondary alternation. Right: actual pane count, separator, LOCAL. Footer padding 10 each side. Transient status messages are uppercase, lifetime1800ms, last message resets the timer; normal uses inherited secondary, active amber, error error. READY returns when timer expires. Do not render success if the system cannot establish readiness.

### Theme picker

Anchored to theme control, top=rail bottom+5, right-aligned; width 286; padding 5; max-height viewportH−72, vertical scrolling. Frame1 structure, pane background. Heading27 high, inner horizontal8, Mono5009/1 tracking.11em, separator bottom. Group labels21 high, header ground, Mono5008/1 tracking.13em. Each theme row52 high; padding 6/8; grid columns42, flexible text,16; gaps10; separators1 except last. Title Sans50012 line1.1; subtitle Mono4009 line1, dim; gap 3. Check mark appears only for selected theme. Mini preview38×28, padding 4, gap 2, three columns with first flex1.45 and others1, first leading signal border 2; miniature uses its OWN theme colors, not the selected theme's colors.

Order: DARK FIELD = Signal Amber, Carbon Optics, Abyssal Sonar, Oxidized Archive; TERMINAL STUDIES = Combine Relay, Deus Ex Access, System Shock Node, SiN Network, Black Mesa Lab, Strogg Process; LIGHT FIELD = Genera Ivory, Mineral Sage, Warm Logic. Keep13 visible in the count. Production may generate count from the registry, but initially registry matches these 13.

Opening selects keyboard focus on current item, does not apply another theme. ArrowUp/Down wraps, Home/End jumps; Enter/Space commits chosen item, closes and restores focus to toggle. Pointer selection also restores toggle focus. Escape and outside click dismiss without changing theme. Do not let click-away leak a release into a different tile. Old inherited child environments do not magically change on selection; live switching requires the migration protocol.

### Help dialog

Centered modal, width min540 or viewportW−32, panel dialog_bg with 1px focus_neutral. Header min-height 78 padding 17/18/15/22, bottom structure1. Eyebrow Mono50010, .12em amber; title Sans50023 margin-top 7. Close29×29 with structure1, text20. Key list padding 12/22; rows grid190/remaining with gap 18, vertical padding 10, bottom separator1. Keys min-width 26, padding 4/6,1px focus_neutral,kbd_bg,Mono50010. Description text13 secondary. Footer explanatory paragraph padding 15/22/20, line1.55. Modal backdrop uses the effect above. It must trap keyboard focus, close with Esc and explicit×, and restore the invoking control; underlying terminal input must not receive modal keystrokes.

### Narrow-screen branch

At logical viewport width≤820: hide brand text, context, button labels and footer center; brand widthauto; rail gap 8; theme menu shifts right−44; workspace scrolls and root split min-width 840; footer font 9. This is a horizontally scrollable desktop, NOT a redesign into stacked phone cards. At width 821 the wide layout returns. Bound popup rectangles to the real display in production; do not make a offscreen menu item unreachable. Record that clipping correction as a small safety delta if it differs from the browser.

## 10. Input/state transitions

| Gesture | Reference result | Native contract |
|---|---|---|
| Click collapsed header | Focus its pane, expand it, collapse previous | Atomic active/focus change, same order |
| Click already expanded header | Focus pane, remains open | Never collapse final open body |
| Click body | Focus its pane | Preserve selection/scroll; do not rebuild model |
| Alt+J / K | Next/previous tile, wrap | Same order and wrap |
| Alt+arrow | Nearest pane center in requested half-plane | Match source distance rule; deterministic tie order |
| Split V / Alt+V | Left/right split of focused pane, ratio.5; new pane focused | New distinct process/tile in production |
| Split H / Alt+H | Top/bottom split, ratio.5; new pane focused | Orientation adapter must be tested |
| Drag divider | Ratio clamp.22..78 | Pointer capture; real child winsize updates |
| Divider double click | Ratio.5 | Same, constrained if minima require |
| Focused divider arrow | Ratio±.025 | Source accepts any of four arrows, even perpendicular |
| Close tile× | Remove; next same-index successor else preceding | Confirm dirty/process-close as needed |
| Close final tile | Refuse `FINAL TILE IS PROTECTED` | Preserve stack and process |
| Reset | Restore fixture layout and p1 focus | Reference only; production confirmation required |
| Escape during drag | Ends capture, retains current ratio | Preserve exact legacy gesture, label honestly |

Directional focus computes centers of visible visual panes, filters requested `dx`/`dy` beyond±5, and chooses smallest Euclidean distance. It does not prefer shared-edge overlap. Tie-break follows depth-first first-child traversal. Treat these numbers as logical units. If a better navigation algorithm is desired later, keep it out of this parity migration.

Drag ratio is pointer distance from the split origin divided by the FULL split extent, including the 7px track. It is not pointer position divided by available child extent. Thus it can jump slightly on pointerdown/move; preserve in reference mode. Pointerup/cancel end capture; releasing outside remains safe. Source Escape is stop, not rollback; reference help says cancel but the code does not restore a snapshot. Optional true rollback must be separately labeled and tested, not silently substituted. Coalesce drag rendering to frame cadence; never drop the final pointerup position.

Input priority: trusted system chord > active modal > theme menu > divider capture > workspace chords > focused tile transcript mode/application. If legacy Super chords conflict, retain them as aliases where safe; Alt bindings belong to the Instrument profile. Do not feed consumed Alt+H/V/J/K or navigation bytes to the pts. Never move Ctrl-C/SAK handling into this UI routing layer. Accessibility keyboard-focus outline is1px amber inset2, separate from logical pane focus. Repair the reference's loss of DOM focus on rerender in native code: stable focus identity is required.

## 11. Production behavior and explicit non-parity bugs

The mockup is a UI simulator. It has no terminal editing, persistence of layout, executable header commands, real dirty tracking, real processes, multiworkspace switching or drag-to-reorder. Do not promise these as existing browser features. Native Halcyon already provides some and must retain them without altering reference chrome.

- **Split:** spawn a new user-owned shell in the new pane, preferably inheriting the source cwd through the sanctioned session spawn path. Never clone a pts descriptor or duplicate PID. Keep sample-copy behavior only in fixture mode.
- **Close:** send a graceful request through the existing lifecycle. Dirty editor or active job requires confirmation/explicit force action; modal shares the help visual language. Final tile protection remains. Successful removal chooses successor at removed index else previous.
- **Reset:** in production reset geometry only while preserving all tiles, or ask before restoring a saved layout that respawns content. Never invoke the mockup's destructive object replacement on a user's running session.
- **Scroll:** keep per-tile anchors. The sample recreates DOM bodies and loses scroll on many actions; native must not reproduce that data/usability defect.
- **Limits:** enforce current compositor limits and the new recursive usable minima. A refusal is visible and leaves state unchanged. Do not copy unbounded recursive split creation or Date.now IDs.
- **Dirty/error metadata:** source sample is hardcoded. Real state must be authenticated to the owning tile, length-bounded, sanitized and never evaluated as a command.
- **Accessibility:** × is a nested span in a header button in the browser. Native must expose distinct open/close semantic actions and keyboard paths without ambiguous targets, keeping identical visible geometry.
- **Theme default:** source defaults to Signal Amber. This request explicitly changes default to Carbon. Existing saved choices should survive migration; fresh Instrument profiles use Carbon.

These exceptions are enumerated to prevent accidental redesign while refusing to turn a browser demo's shortcuts into OS data loss.

## 12. Completion definition

Do not declare completion when a TOML loads or when one screenshot looks plausible. Completion requires all 13 palettes, the exact profile geometry, correct scaled carve/paint/input agreement, native real terminals and rich documents, highlighted Rust, state preservation, strict failures, guest authority tests, CPU-floor rendering, and the screenshot/state matrix in `ACCEPTANCE-TESTS.md`. Deliver changed source, reproducible tests, captures, numerical diffs, known limitations and rollback instructions. Native screenshot identity remains unproven until that agent runs those gates.
