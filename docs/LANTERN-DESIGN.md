# LANTERN — the Beacon deck presenter

`lantern` shows a directory of Markdown slides one at a time as Beacon: a rich
document under Halcyon, the same words without frames on a serial console, and a
plain concatenation down a pipe.

The name is the magic lantern, the slide projector's ancestor; "lantern slide"
is the original term for the thing this shows. **Ratified by the operator
2026-09-22** over the thematic runner-up `specimen` (a museum specimen; a
microscope slide), on the grounds that a presenter's name should read as a
projector at a glance — and the project is already comfortable off the marsupial
theme, as Halcyon and Stratum are.

## 1. What was decided, and by whom

The design is the operator's, ratified in conversation 2026-09-22:

> I would not formalize or integrate the deck type. A slide is just some beacon
> text and halcyon knows how to render that. [...] the pager can just target a
> folder with a slides.toml that specifies the order of the md files within that
> folder. The pager will just clear and rerender using keys.

Everything below either implements that or records a fact measured while
implementing it. Two things the earlier sketch called for turned out to be
unnecessary, and one thing it dismissed turned out to be real; each is marked.

## 2. A slide is an Operator's Manual section

There is **no slide format**. A slide is a file in the manual's Markdown subset
(`MANUAL-DESIGN.md` §3), parsed by `manual`'s own parser, checked by its own
checker, and realized by its own renderer. `lantern` depends on the `manual`
library and adds not one construct.

This was not a shortcut; it is the point. The manual's subset already requires
`# Title` on line 1 (`Problem::NoTitle`) and already accepts headings, bulleted
and numbered lists, tables, code fences and inline emphasis — which is
everything a textual slide is. **The pre-implementation plan to extend Markdown
with "Beacon features Markdown doesn't have", and to lift `format`/`render`/
`wrap` into a new shared crate, was both unnecessary:** the library is already
the shared crate (lib + bin, `no_std`, `beacon` its only dependency), and the
dialect already covers the use. Two Markdown dialects in one tree is how a
format rots; there is one.

The one deviation: a slide is checked as an **anonymous** section
(`format::check(None, …)`). The `TitleNumber` rule — a title must not begin with
its section number — exists for the manual's `NN-name.md` book ordering, and a
deck's order comes from its manifest. So `01-open.md` may title itself however
its author likes.

## 3. The clear, and why the rich rendering survives it

A presenter must put one slide on the screen and then replace it. `lantern`
writes `ESC[0m ESC[H ESC[2J` before each slide (`lantern::CLEAR`).

**In a Halcyon tile that is a grid operation and nothing more**, and three
measured properties make it exactly right:

1. `vt::Screen::erase_display(2)` blanks every cell **in place** — it does not
   scroll the old content anywhere, so the tile's scrollback does **not**
   accumulate the slides already shown.
2. `Cell::blank` sets `span: 0` ("Blanks (erase / scroll fill) carry 0"), so the
   previous slide's Beacon span tags are cleared along with its text. A new slide
   cannot inherit the old one's emphasis.
3. It is not a mode change, so the tile stays in `ScreenMode::Normal` — and
   Normal is the mode in which `Tile::render` lays the document out
   **proportionally and richly**, scrollback blocks and live tail alike.

So the interactive-rich mode the operator asked for **required no halcyond change
at all.** It was already there; what was missing was a program that used it.

**AMENDED 2026-09-24 — that conclusion is withdrawn.** The three properties are
true, and they establish exactly one thing: slide two carries no residue of slide
one. They say nothing about history from BEFORE the deck. In a tile with
scrollback, the proportional composition bottom-anchors the live tail (HALCYON
14.13, PL-4), so the slide landed at the BOTTOM of the view with the shell's
earlier output filling the space above it. That was the operator's first
Lantern-over-Haul run: "it doesn't clear the console before it starts". Every
capture in this arc was taken on a fresh tile, which has no history, so none of
them could show it; the measurement answered the question it was given, and the
question was too narrow.

The fix is in halcyond's VIEW, not in the presenter. A whole-screen erase now
reaches the tile as an explicit record, and the view pins the live tail to its
top until output next scrolls (HALCYON 14.13, AMENDED 2026-09-24).
`lantern::CLEAR` is unchanged, and the alt screen stays wrong for the reason
§3.1 gives. So the rich mode did require a halcyond change: one, in the view, and
nothing in the presenter.

**AMENDED 2026-09-25 — property 1 no longer holds, by the operator's vote.** A
clear now moves the erased screen into the tile's history before blanking it
(HALCYON 14.13: a clear keeps the screen it erases, and no escape deletes the history), so the scrollback
DOES accumulate the slides already shown, in the order they were shown. What
property 1 was really protecting survives: the pin puts each slide at the top of
a clean view, so no slide is ever seen beside its predecessor. Scrolling up after
a talk shows the talk.

### 3.1 Never the alternate screen

`ScreenMode::AltScreen` makes `Tile::render` paint the raw mono grid and return
early (`usr/halcyond/src/tile.rs`). Everything rich is discarded. A presenter
that entered the alt-screen would therefore destroy the one property it exists
to deliver.

This is structural in the dependency list, not merely a convention: `lantern`
takes `kaua` **without** its default `backend` feature, which is what gates
`kaua::term` — the alt-screen owner. It uses `kaua::input::Parser` and
`kaua::event`, which are pure and ungated, so the tree keeps one VT input parser
and `lantern` never names the screen half.

(A correction worth recording, since it was stated wrongly earlier in the arc: a
pager cannot render Beacon "because halcyond latches `raw_vt_intent` and demotes
to raw VT" is **false**. `raw_vt_intent` is set at three sites in
`transcript.rs` and read by nothing but tests — an inert reserved latch. The
real demotion is `ScreenMode::AltScreen`, above.)

## 4. The three postures

| stdout | stdin | posture |
|---|---|---|
| console or pts, tier rich | a terminal | rich frames, cleared per slide, keys |
| console or pts, tier none | a terminal | plain text, cleared per slide, keys |
| a pipe or a file | anything | every slide once, no clear, no keys |

Both halves are asked of the kernel separately (`SYS_FD_DEVCLASS` on fd 1 and fd
0) and neither is inferred from the other, because `lantern deck | tee log` has a
terminal on stdin and a pipe on stdout — and clearing a pipe would write escape
sequences into a file.

The tier itself is resolved by `beacon::effective_tier`, the same two-condition
gate `manual` and the coreutils use. `lantern` adds no new tier logic.

## 5. The manifest

`slides.toml`, in the deck directory:

```toml
title = "Beacon slides"
slides = ["01-title.md", "02-how.md", "03-keys.md"]
```

Parsed with `libhalcyon::toml` — the theme loader's existing `no_std` TOML
subset, reused rather than a second parser written here. Its own bounds (512
entries, 64 array elements, 64 array lines) are the first gate a hostile file
meets; `SLIDES_MAX` is stated independently at 64 so `lantern`'s bound is not an
unstated inheritance from a constant free to move for unrelated reasons.

Strict on purpose, in the manual format's spirit — the parser is the checker. An
unknown key, a `[table]` header, a duplicate key, a slide named twice, or a
slide name that could be a path (`/`), a traversal (`..`), a hidden file (`.`),
an option (`-`) or not Markdown is **refused with its line**, never guessed at or
ignored.

**A manifest carries content and order, never display authority.** `scale`,
`theme` and `font` are refused like any unknown key. Those belong to the
compositor and reach it only through its own gated verbs, so a deck file someone
mails you cannot reach for them.

### 5.1 The shipped demo deck

`usr/lantern/deck/` is a tracked three-slide deck, installed at **`/deck`** so
`lantern /deck` shows something on a fresh boot. It is installed into the
**pool**, beside `/manual` and `/test.png` — *not* the ramfs, whose root is not
the running system's `/` (the pool is, after the pivot), so a deck baked into the
ramfs would simply never be found. Its slides are checked with the manual's own
checker before the pool opens and a failure is fatal, so a demo deck that stopped
being a valid section set cannot ship.

The check runs against a temporary directory holding only the `.md` files,
because `manual-check` reads a whole directory and requires every entry to be
named `NN-<name>.md` — which `slides.toml` is not. That rule is the manual
*book's* ordering; a deck's order comes from its manifest, which is exactly the
distinction §2 draws.

## 6. Keys

| Key | Action |
|---|---|
| space, right, down, page-down, enter, `n`, `j`, `l` | next |
| left, up, page-up, backspace, `p`, `k`, `h` | previous |
| home / `g`, end / `G` | first / last |
| `1`..`9` | that slide |
| Ctrl-L | repaint |
| `q`, Ctrl-C, Ctrl-D | leave |

Three choices are deliberate and each protects a talk in progress:

- **Advancing off the end stays on the last slide.** It does not wrap and does
  not quit: the end of a deck is where a talk pauses for questions, and blanking
  the screen there would be the worst possible moment.
- **Escape is not a quit.** It is the first byte of every arrow and function
  key, so resolving a lone one needs a timing holdoff
  (`kaua::input::Parser::pending_escape`). A deck that vanishes because a
  holdoff guessed wrong is a worse failure than pressing `q`.
- **`Goto` past the end is refused, not clamped.** `7` in a six-slide deck is a
  mistype; landing on the last slide would hide it.

Every other key is ignored, which is the default rather than the exception: a
presenter leaning on the keyboard must not lose the talk.

## 7. The raw-mode dance, and the line endings it costs

`lantern` is a member of `console::is_raw_command` (`nora | ptyhost | prowl |
quarry | lantern`). It has to be: on the pts/job-control path an ordinary
foreground child is given `CHILD_MODE` (`+icanon +echo`) — canonical lines and
kernel echo, unusable for a pager — and on the console path it is given
piped-then-dropped stdin, which EOFs at birth.

Membership is for the **input** half only. `lantern` is not a full-screen TUI and
never enters the alt-screen (§3.1), and the screen backstop `exec_external_raw`
re-emits on exit is harmless to it: every escape in `RESTORE_SCREEN` is
idempotent, and leaving an alt-screen never entered is inert.

The cost is `RAW_MODE`'s `-onlcr`: the kernel stops translating output line
endings, on the argument that a full-screen child owns every byte it emits.
`lantern` emits a *document*, whose lines end in a bare LF, so it does the
translation the discipline stopped doing (`lantern::cook`). That is the honest
reading of owning your own bytes.

`cook` is stateless per byte, so a chunk boundary anywhere gives the same result
as one whole write — which matters because `manual::render` streams in
`CHUNK`-sized pieces and splits wherever it must. It is safe across Beacon
frames because a frame never carries an LF: the checker rejects control
characters in section text and the renderer sanitizes every value it did not
produce. That claim is **pinned by a test** that renders every construct at the
rich tier and walks the OSC state, rather than trusted.

## 8. Validation happens before the talk, not during it

The whole deck is read and checked at startup, one slide at a time, before
anything is shown. A deck fails at the start or not at all — never on the slide
the talk has reached. `lantern --check <dir>` is the same pass without
presenting, for use the day before.

Memory is one slide, whatever the deck's size: each slide is dropped after
validation and re-read when it is shown, so the working set is `manual`'s own
bound (one section and its largest block) rather than 64 × 1 MiB. Re-reading has
a second benefit — a slide edited while the deck is open shows its new text the
next time it comes round, which is what rehearsing wants.

A slide that becomes invalid *while* the deck is open shows its diagnostics in
place instead of ending the presentation. Losing the deck on a stage is a far
worse failure than a slide that reports what is wrong with it.

## 9. Position is the renderer's, not lantern's

The footer (`3 / 12`, and the deck's title) is written **after** the slide's
content, not at the bottom of the screen. Beacon has no op that places a line at
a screen edge and must not grow one: position is the renderer's business, and a
program reaching for it is precisely the failure the format exists to avoid.
`em class=dim` is available because de-emphasis is a *meaning*.

## 10. Type size — measured, and the one thing still owed

The operator asked whether to "bind the font raster size to the tile size and
cap it at the bottom and top". The tempting answer — that a slide's size comes
from its heading semantics, so nothing is owed — is **wrong**, and measuring it
is the only reason that is known:

- Legacy profile: `hdr_px = [17.5, 14.5, 12.5]` against `body_px` 11.5. H1 is
  **1.5× body**. Nowhere near projector-sized.
- Instrument profile: `hdr_px[0] = px(vw(2.4, 23.0, 34.0))` — a viewport
  percentage with a floor **and** a ceiling. **That is exactly the mechanism the
  operator described, and it already exists** (`halcyond/src/layout.rs`:
  `let vw = |pct, lo, hi| (disp_logical * pct / 100.0).clamp(lo, hi)`).
- But `vw` is keyed to `disp_logical`, the **display** width, not the tile; and
  `body_px` under Instrument is a fixed 15, so slide **body** text does not grow
  with anything.
- The display `scale` verb (100–200 %, step 25, chord Super+=) multiplies
  everything on top, and is already built, gated and live.

### 10.1 Measured: the existing scale verb already closes it

Before proposing any change to halcyond's type, the cheap possibility was
measured: **run the deck and press Super+= to the ceiling.** At 200 % the body
renders near 30 px and the H1 near 48 px, the slide fills the screen, and it
reads from across a room (`build/lantern-rich-slide2-200.png`, leg 4 of
`ls-halcyon-lantern.exp`).

**So there is nothing owed for the operator to present.** The path is: open a
tile, `Super+F` to zoom, `Super+=` to taste, `lantern <deck>`. Zero code.

That demotes the `vw`-bound document type scale from a blocker to an optional
refinement — it would make a deck presentable *without* the display-wide scale,
which is nicer but is not required.

**RATIFIED 2026-09-22: it is NOT built.** The operator was offered three
options — leave it, make the document type scale `vw`-bound, or add a third
"presentation" profile beside legacy/instrument — and chose to leave it, because
the scale verb already does the job and the alternatives would change the type
scale of every document including the terminal and the manual, i.e. the look
they tuned. Recorded here as an available option should a real rehearsal want
it; **not an open item.**

## 11. Settled by looking (2026-09-22), and what is left

These were visual questions, so they were settled by booting the Instrument
session at 1280×800 and capturing, not by reading more code
(`tools/interactive/ls-halcyon-lantern.exp`; captures at
`build/lantern-rich-*.png`).

1. **A short slide sits at the TOP.** It does not sink to the bottom like a
   terminal's live tail — the document flows from the top of the tile, which is
   what a slide wants. Settled, no change.
2. **The clear is exact.** Slide two carried no residue of slide one: no stale
   text, no inherited emphasis, no accumulated scrollback. The `span: 0` and
   no-scroll-off properties of §3 hold in practice.
3. **The title renders as a left-aligned H1**, not centred, because
   `manual::render` emits `Op::Hdr level=1` rather than `class=title`.
   **RATIFIED 2026-09-22: left-aligned stays.** `HdrClass::Title` would centre it
   with its own top margin — the title-card look — and was offered with the split
   that would have kept the Operator's Manual's section titles unchanged. The
   operator chose the current rendering, so this is a settled decision and not an
   open question.
4. **The caret is hidden while presenting** — `ESC[?25l` on entry, `ESC[?25h` on
   every exit path. A blinking bar under the footer is a desk affordance with
   nothing to mark on a projected slide.

   The observation in this run's captures was the *opposite* — a caret below the
   footer — and it was correct for this build, which did not yet emit the escape.
   That observation was then carried forward past the rebuild and reported at
   `1319b4ba` as a measured defect of the escape-bearing build, which it was not:
   the later captures show no caret, and a frame from the same run seconds
   earlier shows the two ut prompts' carets, so the caret machinery was demonstrably
   alive. Because the caret **blinks**, no single frame settles this in either
   direction, so the durable answer is a host test rather than a capture:
   `halcyond::tile::tests::dectcem_travels_the_whole_seam_to_the_caret_predicate`
   drives the escape through vt → the kaua-term Producer → a wire
   encode/parse round-trip → the Grid → `Tile::paints_caret`, with both sabotage
   legs verified (a forced-visible wire byte and a forced-visible grid store each
   fail it).

5. **Type size at the default scale is too small for a room** — the body renders
   at the Instrument 15 px and the H1 near 24 px, and the slide fills only the
   top third. The fix is **bigger type, not vertical centring**, because
   centring is a layout op and §9 forbids reaching for one.

   **But that is already solved by the display scale** (§10.1): at 200 % the
   deck reads from across a room with no code at all. Nothing is owed for the
   operator to present.

## 12. What this facility does not do

- It does not page a single file. `manual <file>` already renders any conforming
  Markdown file as Beacon; that capability existed before this arc.
- It does not scroll within a slide. A slide taller than the tile is an
  authoring problem, and a scroll position would be state the renderer already
  owns.
- It does not transition, animate, or lay out. Those are either the renderer's
  or nobody's.
