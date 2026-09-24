---
id: sub-lantern
type: sub
title: "lantern -- the Beacon deck presenter: a folder of manual sections shown one at a time"
parent: moc-userspace-shell-tui
code:
  - usr/lantern/src/lib.rs
  - usr/lantern/src/deck.rs
  - usr/lantern/src/nav.rs
  - usr/lantern/src/main.rs
  - usr/lantern/Cargo.toml
  - usr/lantern/deck/slides.toml
  - tools/interactive/lantern.exp
  - tools/interactive/ls-halcyon-lantern.exp
  - tools/interactive/ls-halcyon-lantern-haul.exp
audit: light
guarded-by: []
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design: ["docs/LANTERN-DESIGN.md", "docs/MANUAL-DESIGN.md", "docs/BEACON.md"]
created: 2026-09-22
updated: 2026-09-23
---
## Purpose

`lantern` shows a directory of Markdown slides one at a time, driven by keys:
a rich Beacon document under Halcyon, the same words plain on a serial console,
and every slide at once down a pipe. It exists so a talk can be given from
inside Thylacine with the system's own renderer doing the drawing.

**A slide is an Operator's Manual section** -- `[[sub-manual]]`'s format, its
checker and its Beacon realization, used unchanged. The tree therefore carries
ONE Markdown dialect, not a second one private to slides, and that was not a
compromise: the manual subset already accepts everything a textual slide needs
(a title, headings, lists, tables, code fences, emphasis). What lantern adds is
only what a deck needs beyond a document -- the ORDER, the key map, the clear
between slides, and the output cooking that clear implies.

**Nothing in `[[sub-halcyond]]` changed to make this work**, which is the
finding the arc turned on. The shape follows `[[sub-view]]` and `[[sub-manual]]`:
a pure `no_std + alloc` library (host-tested, zero syscalls) and a thin
libthyla-rs binary.

## Contract

- `lantern <deck-directory>` -- present. Requires a terminal on **both** fd 1
  and fd 0 (`show_mode`); otherwise the deck is catted.
- `lantern --check <deck-directory>` -- parse the manifest and check every
  slide, print diagnostics, render nothing. Exit 1 if any slide fails.
- `--beacon=auto|always|never` -- the tier flag every Beacon emitter carries;
  `resolve_tier` folds it with `$BEACON` and `SYS_FD_DEVCLASS(1)` through
  `beacon::effective_tier`, exactly as `manual` and the coreutils do.
- `--no-footer` -- drop the `title · N / M` line.
- Exit 2 = usage error; `-h`/`--help` prints `USAGE` to stdout and exits 0.
- A deck directory holds `slides.toml` (`deck::MANIFEST`) and the `.md` files
  it names. The manifest sets `slides` (required, ordered, non-empty) and
  optionally `title` -- **and nothing else**.

### The manifest carries content and order, never display authority

`deck::parse` refuses an unknown key (`Problem::UnknownKey`), so `scale`,
`theme` and `font` cannot appear in a deck file. That is a boundary, not a
tidiness rule: those belong to the compositor and reach it through its own
gated verbs, so **a deck someone mails you cannot reach for them**.

The parser IS the checker, in the manual format's spirit -- 13 `Problem`
variants over `libhalcyon::toml` (the theme loader's subset, reused rather than
a second parser written here), each naming the line to look at. A table header,
a duplicate, an empty name, a name carrying `/`, beginning `.` or `-`, or not
ending `.md` is refused rather than guessed at, because a deck should fail at
`lantern --check` and not mid-talk. Bounds: `SLIDES_MAX` 64 (stated here rather
than inherited silently from the TOML subset's identical cap, which is free to
move for unrelated reasons), `MANIFEST_MAX` 64 KiB, and each slide capped at
`manual::SECTION_MAX`.

## Mechanism

**The clear is a GRID operation, and that is the whole trick.** `lantern::CLEAR`
is `ESC[0m ESC[H ESC[2J`. In a Halcyon tile the pts host's vt digests it:
`vt::Screen::erase_display(2)` blanks every cell IN PLACE -- no scroll-off, so
the transcript does not accumulate the slides already shown -- and `Cell::blank`
sets `span: 0`, so the previous slide's Beacon span tags leave with its text.
It is not a mode change, so the tile stays in `ScreenMode::Normal`, the mode
that lays the document out richly.

**KNOWN GAP (2026-09-24, operator-found; fix ratified, TC-1a):** "the whole trick"
is not whole. It proves slide two carries no residue of slide one, and says nothing
about history from BEFORE the deck: in a tile with scrollback, halcyond
bottom-anchors the live tail (PL-4), so the slide sits at the bottom of the view
with earlier output above it -- and `clear` misbehaves the same way. Fresh tiles
hide it. The fix is in halcyond's view, not here: an explicit `ScreenErased` wire
record and a view pin (HALCYON 14.13 AMENDED 2026-09-24; LANTERN-DESIGN 3's
withdrawn conclusion).

**The alt screen is the one thing to avoid**, and the avoidance is structural
rather than a convention: `ScreenMode::AltScreen` makes a tile paint its raw
mono grid, discarding the rich rendering the facility exists for. lantern
depends on `[[sub-kaua]]` **without its default `backend` feature**, which is
what gates `kaua::term` -- the alt-screen owner. The pure `kaua::input::Parser`
and `kaua::event` are not feature-gated, so the VT input parser comes along
without the screen half. There is no code path to the alt screen to review.

**Output cooking.** `ut`'s raw-mode dance sets `-onlcr` for a full-screen child
on the argument that such a child owns every byte it emits. lantern is such a
child (it needs `-isig` so a keystroke is a keystroke) but it emits a DOCUMENT,
whose lines end in a bare LF -- so `lantern::cook` does the translation the
discipline stopped doing. It is stateless per byte, so a chunk boundary anywhere
gives the same result as one whole write (`manual::render` streams in pieces and
splits wherever it must). The cook is keyed on `is_terminal(1)`, NOT on the
posture: `echo x | lantern deck` has a terminal stdout with translation off, and
keying on the posture would staircase it.

Safe across Beacon frames because a frame never carries an LF -- the checker
rejects control characters in section text and the renderer sanitizes every
value it did not produce -- and `lf_never_appears_inside_a_frame` pins that by
walking the OSC state over every construct rather than trusting it.

**The caret.** `HIDE_CARET` on entry, `SHOW_CARET` on every exit path; see
`[[sub-halcyond]]`'s caret section for the seam the escape crosses and the test
that pins it.

**Navigation** (`nav.rs`, pure). Next: space / right / down / pagedown / enter /
n / j / l. Prev: left / up / pageup / backspace / p / k / h. First: home / g.
Last: end / G. `Goto` on 1-9. Redraw on Ctrl-L. Quit on `q`, Ctrl-C, Ctrl-D --
Ctrl-C spelled out because `-isig` means the byte reaches the program instead of
becoming an `interrupt` note. **Escape is deliberately NOT quit**: it is the
first byte of every arrow key, so resolving a lone one needs a timing holdoff,
and a deck that vanishes because a holdoff guessed wrong is worse than pressing
`q`. An unmapped key is `Ignore`, the default, so a presenter leaning on the
keyboard does not lose the talk.

`Action::target` neither wraps nor exits at the ends -- a deck's end is where a
talk pauses for questions -- and a `Goto` past the end is REFUSED rather than
clamped, because `7` in a six-slide deck is a mistype and landing on the last
slide would hide it.

**The footer is written after the content, not at a screen edge.** Beacon has no
op that places a line at the bottom and must not grow one: position is the
renderer's, and a program reaching for it is the failure the format exists to
prevent. `Em::Dim` is available because dim is a MEANING (de-emphasised), not a
position.

## Data structures

`Deck { title: Option<String>, slides: Vec<String> }` -- the parsed manifest;
`slides` is never empty. `Problem` / `Diagnostic` carry the refusal and its
line. `Action` is the key map's output. `Out` wraps stdout with the cook flag.
No shared or persistent state: a slide is re-read from the filesystem on every
paint, so an edit mid-rehearsal shows on the next keypress.

## Concurrency

None. One process, one thread, one blocking read on fd 0. No locks, no waiters,
no signals (`-isig` is set for the duration).

## Invariants enforced

No §28 invariant is on this line -- lantern is an ordinary unprivileged program
with no capability, no shared memory and no service. Its own two rules are the
manifest's authority boundary (above) and the alt-screen exclusion, and both are
enforced by construction rather than by a check: an unknown key is a parse
refusal, and the alt-screen code is not linked.

## Error paths

- Manifest absent / unreadable / not UTF-8 / over `MANIFEST_MAX` -> diagnostic,
  exit 1.
- Manifest malformed -> `deck::Diagnostic` naming the line, exit 1.
- A slide missing, over `SECTION_MAX`, or failing `manual::format::check` ->
  named diagnostics; `validate` checks the WHOLE deck before anything is shown,
  so a broken slide 6 is found before slide 1 is painted.
- A slide that becomes invalid while the deck is open shows its diagnostics
  **in place** rather than ending the presentation -- losing the deck on a stage
  is a far worse failure than a slide that reports what is wrong with it.
- A write error or a read error on fd 0 restores the caret and exits; a read
  error is fatal because this `Error` set has no `Interrupted` and spinning on a
  would-block with no poll in hand would be worse than stopping.

## Performance

Not on any latency budget. One slide is read and rendered per keypress; a slide
is bounded by `SECTION_MAX` and a deck by `SLIDES_MAX` = 64. Memory is one
slide at a time -- `validate` holds one, not the deck.

## Prosecution

Low-value target, but the two places worth attacking:

- **The manifest parser** (format-fuzz class): a hostile `slides.toml` reaches
  `libhalcyon::toml` first (512 entries / 64 array elements / 64 array lines),
  then lantern's own per-slide rules. Prosecute name handling -- a slide name
  reaching outside the deck directory is the interesting escape, and it is
  refused on three separate grounds (`/`, leading `.`, leading `-`) rather than
  one. Every diagnostic passes user bytes through `manual::sanitize` before
  printing.
- **The tier gate**: lantern must not emit frames into a pipe or a file. It
  resolves the tier through `beacon::effective_tier` and computes nothing
  itself; the check is that no other emission path exists.

## Seams

- `usr/lantern/deck/` is a TRACKED demo deck installed to `/deck` **in the
  POOL** by `populate_stratum_pool` -- NOT the ramfs. The running `/` is the
  pool after the pivot, so a guest data file staged in the ramfs is never found.
- **A deck on another machine** rides `[[sub-haul]]` with no change on either
  side: `haul -t TOKEN HOST!PORT /tmp/remote /bin/ut`, then
  `lantern /tmp/remote/deck` inside that sub-shell. The sub-shell is required,
  not a convenience. A mount lands only in haul's own namespace (I-1), so
  lantern must be haul's descendant; and `haul ... lantern DECK` would run
  lantern in the terminal's cooked mode (canonical input, echo, `isig`),
  because `is_raw_command` reads argv[0], sees `haul`, and the pts path gives
  an ordinary job `CHILD_MODE` (by reading). The nested ut seats itself as the
  foreground group and gives lantern the dance. lantern re-opens the slide on
  every paint, so an edit on the host is on the screen at the next key:
  `tools/interactive/ls-halcyon-lantern-haul.exp` serves a deck it writes from
  a host npxf-server, and asserts the paint, the server's `opened` log lines,
  paging, and a host edit shown after Ctrl-L (2026-09-23, both profiles).
- `console::is_raw_command` in `[[sub-utopia-eval]]` gained `lantern`: the
  basename set is hardcoded, so membership is required for the INPUT half (raw
  mode) and is this chunk's only edit to an audit-trigger surface.
- `docs/LANTERN-DESIGN.md` §10.1 records the `vw`-bound document type scale as
  an available option that was **offered and declined** -- the display scale
  verb (Super+=, 100-200%) already makes a deck presentable, and the
  alternatives would move the type scale of every document including the
  terminal and the manual. Ratified by the operator 2026-09-22, with the name
  `lantern` and the left-aligned H1 title.

## Caveats

- **Judge the look on an Instrument image.** Under the legacy (Daylight)
  profile -- the one `--config ci` pins, so the gates' default -- every blank
  line between a slide's blocks lays out as a one-row raw island: the
  zero-height paragraph break for a blank raw line is Instrument-only in
  `[[sub-halcyond]]`'s layout (the `Role::Empty` arm keyed on `fractional`),
  and legacy keeps raw islands byte for byte. Under Instrument, the product
  default, a slide is a clean document. Build the gate image with
  `THYLACINE_HALCYON_PROFILE=instrument` to see what a presenter sees.
- Presenting requires a terminal on fd 0 AND fd 1, judged independently:
  `lantern deck | tee log` has a terminal on stdin and a pipe on stdout, and
  clearing a pipe would write escapes into a file.
- A deck is not a format and has no version. It is a directory; the manifest is
  its table of contents.
- The gate tokens `tools/interactive/lantern.exp` matches are pinned by
  `slide_tokens_render_contiguously` against the SHIPPED slides, because a token
  the renderer splits across a line break turns a real pass into a TIMEOUT --
  which reads as a hang rather than a regression, the most expensive way for a
  gate to fail.

## Provenance

- 2026-09-22 `e70f34ac` -- the crate, the demo deck, `is_raw_command`, the design doc.
- 2026-09-22 `658f1306` -- the deck relocated from the ramfs to the pool.
- 2026-09-22 `58bedd35` -- the E2E gate and the gate-token pin.
- 2026-09-22 `6743b2fe` -- the rich-tier gate in a real tile.
- 2026-09-22 `05c7c0f9` -- the three operator decisions recorded as ratified.
- 2026-09-23 -- the Haul composition gate (`ls-halcyon-lantern-haul.exp`) and
  the profile caveat.
- 2026-09-22 `1319b4ba` -- the caret escapes (its claim that they did not work
  was wrong; corrected in `ae30a6b3`).
