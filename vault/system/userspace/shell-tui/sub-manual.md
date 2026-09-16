---
id: sub-manual
type: sub
title: "manual -- the Operator's Manual reader: a strict Markdown subset, checked, rendered as Beacon"
parent: moc-userspace-shell-tui
code:
  - usr/manual/src/lib.rs
  - usr/manual/src/format.rs
  - usr/manual/src/render.rs
  - usr/manual/src/wrap.rs
  - usr/manual/src/catalog.rs
  - usr/manual/src/bounds.rs
  - usr/manual/src/main.rs
  - usr/manual/Cargo.toml
  - tools/manual-check/src/main.rs
  - tools/manual-check/Cargo.toml
audit: light
guarded-by: []
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design: ["docs/MANUAL-DESIGN.md", "docs/thylacine-operators-manual-writing-guide.md", "docs/BEACON.md"]
created: 2026-09-16
updated: 2026-09-16
---
## Purpose

`manual` reads the Thylacine Operator's Manual. Sections are Markdown files
installed at `/manual` (`docs/manual/NN-<name>.md` in the tree); the reader lists
them, shows one by name or a draft by path, and checks files against the section
format. It is the "supported Beacon authoring workflow" the writing guide
requires: the format is exactly what this parser accepts, and every accepted form
has both a Beacon realization and a plain one. Binding design:
`docs/MANUAL-DESIGN.md` (operator sign-off 2026-09-16, `4d76ae87`).

The shape follows `[[sub-view]]`: a pure `no_std + alloc` library (no I/O,
host-tested) and a thin libthyla-rs binary behind the `backend` feature.

## Contract

- `manual` -- the contents: a title-page heading (`hdr level=1 class=title`,
  "Thylacine Operator's Manual"), then a `Name | Section` table in book order and
  a line naming `manual <name>`; with `/manual` absent or empty, "No sections are
  installed." and exit 0.
- `manual <name>` -- `catalog::lookup`: exact name, then number (`5` or `05`),
  then `NN-<name>`, then the only name the operand begins; case-insensitive; a step
  matching several sections is ambiguous (the diagnostic lists each as `NN-<name>`,
  itself an operand naming one section; exit 1).
- `manual <file>` -- an operand containing `/` or ending `.md` is a path (author
  preview).
- `manual --check <file>...` -- diagnostics `manual: <file>:<line>: <message>` on
  stderr, in line order, a problem at most once per line; exit 1 if any.
- A file that fails the check is never rendered: nothing is written until the whole
  file has passed. Exit 2 = usage error. Diagnostics are prefixed `manual: `, and a
  name or path they repeat has the `is_replaced` characters replaced (`sanitize`).
- `tools/manual-check <dir>` -- the same checker built for the build host; the
  bake runs it over `docs/manual` before the pool opens and installs exactly the
  sections it lists (misnamed or failing files fail the bake). Its diagnostics
  sanitize the names they repeat, so each stays one line.
- `--beacon=auto|always|never` resolves the tier exactly as the coreutils do
  (`beacon::effective_tier` over `BEACON` + `fd_devclass(1)`).

## Mechanism

**format.rs -- one parser, streamed to a consumer.** `format::read(file_name,
src, &mut dyn Events)` reads a section front to back and reports as it goes:
`problem(line, Problem)` in line order and at most once per problem per line
(the parser keeps only the current line's reported set), and the structure as
`open(Open)` / `close()` / `run(Run, text)` / `code_line(text)`. Nothing
outlives the block being read, so no document tree exists. `check` is the
consumer that counts and forwards problems; `render::render` is the consumer
that writes. Lines are read with a cursor (`Parser::line`/`after`, no line
index) and classified in isolation (`classify` -> `Kind`); block parsers
`heading`, `fence` (finds its closer before checking content, so the opener's
problems come first), `bad_fence`, `list`, `table`, `paragraph`.

A paragraph's or list item's lines are joined into one scratch buffer with `\n`
between source lines (a setext underline joins as an empty segment), reserved to
the block's byte span. `scan` treats `\n` as the joining space everywhere a form
looks at its neighbours (`is_separator`) and emits it as a space. The scan's
source line advances as it crosses each `\n` (`cross`): entering a line reports
that line's own structure (`enter`: indentation, setext, nesting, control
characters), and leaving it reports a hard line break (`leave`) unless the
crossing is inside a code span. So problems stay in line order even where the old
tree parser learned them out of order, and a line's number is never searched
for. A table's header cells are scanned before its delimiter row is checked, for
the same reason.

Searches ahead resume instead of repeating (`Look`, per scan): a code span or
emphasis closer search that fails records where it started, since a later search
from a later opener examines a subset of the same runs (every search starts right
after a maximal run, so escape pairing reads the same); `[`'s `]` search reuses
its last result; the underscore-emphasis closer is found once per scan.

Three rules added 2026-09-16 (operator decisions; `b4f5c822` scripture):
`check_chars` rejects the nine bidirectional embedding/override/isolate controls
on every line, code included (`Problem::BidiControl`); `scan` treats `&` as a
special byte and flags a character reference (`is_character_reference`: a
letter-led alphanumeric name, `#` digits, or `#x` hex digits, then `;` -- any
name, deliberately wider than the HTML5 table, so no decoded form escapes;
`\&` and code spans are literal); `row` measures every cell (a `Count` scan, in
both modes) and reports `CellTooWide` past `TABLE_CELL_MAX` = 256 displayed
characters, the unit the padding is counted in.

**render.rs -- the rendering consumer.** `Renderer` implements `Events`, writing
payload always and frames only at the rich tier through `Chunks` (64 KiB), so
stripping frames yields the plain output (BEACON.md 12.1 rule 1). Tables mirror
`beacon::sink::Table` byte for byte; the widths it pads to arrive with the events,
because a consumer that `measures_tables` makes the parser scan a table's cells
for their widths before opening it (`Open::Table { widths }`, `Open::Cell {
width }`). The renderer clamps each column width to `TABLE_CELL_MAX`, so padding
stays linear for text that never passed the check (a checked section is already
within it). `render_contents` drives the same renderer.

**wrap.rs.** `Wrap` fills greedily as runs arrive (`begin` / `feed` / `end`):
list items hang under their marker; a code span does not break at its own spaces;
a word longer than the line is split. A pending word is placed the moment its
placement is decided, so it never holds more than a line's width. Plain tier
only, and only on a console reporting at least 20 columns; pipes, pts and the
serial console get one line per paragraph. The whole-paragraph algorithm it
replaced is kept in its tests as a differential reference.

**Hygiene.** One predicate, `is_replaced` (lib.rs: `is_control` -- C0, DEL,
U+0080-U+009F -- or `is_bidi_control` -- U+202A-202E, U+2066-2069), decides what
becomes U+FFFD at every site that writes text the reader did not produce:
`sanitize` (echoed names, paths), `Renderer::put_text` (payload, code lines with
TAB kept), and `Wrap::feed`. It runs independently of the checker, which rejects
such text first. No section text ever becomes a frame argument (only the heading
level, the em class, and a table column spec of at most 16 characters), so no
frame can exceed the wire caps.

## Data structures

`format::Open` {Title, Heading(2|3), Paragraph, Bullets, Numbered, Item(n), Code,
Table{align, widths}, Row, Cell{width}}; `format::Run` {Text, Code, Emph, Strong}
(flat: emphasis never nests); `format::Problem` (one variant per rejection, the
message in its `Display`); `catalog::Entry {number, name, file}`;
`render::Listed {name, title}`. `SECTION_MAX` = 1 MiB; `HEAP_BYTES` = 16 MiB (the
binary's `ThylaAllocN`); `TABLE_COLUMNS_MAX` = 16; `TABLE_CELL_MAX` = 256. The binary reads into a buffer sized from `fstat`
(`read_capped`). The test-only `format::tree` rebuilds the old block tree from
events and asserts they are well formed, in line order, and identical whether or
not tables are measured.

## Concurrency

None. Single-threaded, short-lived, no shared state.

## Invariants enforced

- A rendered section passed the format check (the binary renders only after
  `check` returned 0 for the whole file; `manual.exp` leg (f) shows a failing file
  prints none of its rendering).
- Memory: the section (plus the holes its buffer leaves when `fstat` gives no
  length), at most a few copies of its largest block, and one output chunk
  (`bounds::the_heap_bounds_hold`, under the guest's `linked_list_allocator`, with
  each diagnostic formatted into a String as libthyla-rs does: worst 6648 KiB of
  an 8 MiB working-set bound at 1 MiB, the 16 MiB heap above it).
- Time linear in the section (`bounds::the_time_bounds_hold`).
- Strip identity: `wire::strip(render(Rich)) == render(None)` without wrapping,
  for every fixture and every installed section (host test).
- Section text cannot inject a frame or a control sequence, or reorder its display
  (`is_replaced` at every write site; host tests `section_text_cannot_forge_frames`
  and `bidirectional_controls_are_replaced_without_the_check`).
- Output linear in the section: padding per cell <= `TABLE_CELL_MAX`, enforced by
  the check and clamped by the renderer (`table_padding_is_bounded_without_the_check`;
  `the_time_bounds_hold` asserts output as well as time).

## Error paths

Read failures print `manual: <path>: <libthyla-rs error>`; a file over 1 MiB is
"larger than 1 MiB"; non-UTF-8 is "not valid UTF-8"; `/manual` absent reads as
no sections, any other directory error is reported with exit 1; a failed stdout
write latches (`io::OutSink`) and exits 1 with "write error".

## Performance

Measured on thyla-pi (A72, release) through `bounds`: every expensive shape
scales 4.0x from 64 to 256 KiB in time and in output. Heap high-water marks at
1 MiB (the section itself is 1024 KiB of it): 1088 KiB for block-per-line shapes,
2112 KiB for one 1 MiB block, 3136 KiB for one emphasis or code span across it,
6208 KiB with wrapping on a console wider than the block. A read with no length
hint adds the grown buffer's holes: 2040 KiB for block-per-line shapes, 6648 KiB
worst (2026-09-16, round 2 F4). Table output at the cell limit: 255 KiB of empty
rows under sixteen 256-character header cells writes 116,182 KiB across the rich
and the 80-column plain rendering together (63 KiB writes 27,635 KiB; the ratio is
the constant header's). Before the 2026-09-16 rewrite the
same shapes measured up to 158 MiB against the 16 MiB heap (an allocation
failure, which exits 1 silently), and a paragraph of one-character lines took
5.8 s at 256 KiB on the M2. The contents listing reads every installed section to
take its title.

## Prosecution

Format-parsing class: the parser is exposed to any file a user names (author
preview). Prosecute: (1) frame or escape injection through any inline or block
path, including table cells, emphasis bodies and code blocks; (2) panics on
adversarial input (byte arithmetic in `scan`, `emphasis`, `Cells`,
`find_code_close`, `find_emphasis_close`, the backslash skips, `char_before`);
(3) the `Look` resumption arguments -- a cached "nothing from here" that is wrong
for a later start changes what is accepted; (4) the line-order argument -- a
problem reported for an earlier line breaks per-line dedup (debug-asserted,
enforced by `tree`); (5) the rewrite's equivalence with fe79e6c8 on accepted and
rejected input (the known deliberate changes are in the commit); (6) memory: a
shape that makes the parser or renderer hold more than a bounded number of
copies of one block; (7) output: any loop that writes in proportion to something
other than input bytes (the padding was one -- review round 2 F1, found by a
shape the 37 enumerated ones lacked); (8) the character-reference detector: a
form a code host decodes that it misses, and a false positive on ordinary prose;
(9) the bidirectional set at every check and write site.

## Seams

- `obj` references in sections: no source form yet (MANUAL-DESIGN 11).
- Beacon list/link ops: not requested; lists render as text.
- Wrapping on a pts: v1 does not query a pts's geometry.
- Keyword search and a pager.

## Caveats

- Plain tiers drop inline delimiters (a code span shows no backticks) -- forced
  by the strip identity.
- halcyond does not wrap table cells, so a wide table overflows a pane; keep cells
  short (MANUAL-DESIGN 8.3 review item).
- A console renderer reads through the 8 KiB drop-oldest console drain; a large
  section written at once may lose bytes on that path (MANUAL-DESIGN 8.4,
  measurement owed).
- Within one line, a hard line break is reported after that line's inline
  problems (it is found when the scan leaves the line).
- halcyond's tile path remembers 32 table specs, so a section with more tables
  than that may lose table structure in a tile.
- The character-reference rule over-approximates: `&foo;` or `AT&T;`, which a code
  host leaves literal, is rejected too; the author writes `\&`.
- The implicit direction marks (U+061C, U+200E, U+200F) are allowed and printed.
- The cell limit bounds table output linearly but with a large constant: sixteen
  256-character header cells over 18-byte rows of empty cells write about 227
  bytes per source byte per rendering (measured 116,182 KiB for the rich plus the
  plain rendering of 255 KiB).

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
