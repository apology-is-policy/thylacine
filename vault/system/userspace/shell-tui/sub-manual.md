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
  - usr/manual/src/main.rs
  - usr/manual/Cargo.toml
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
  matching several sections is ambiguous (diagnostic lists them, exit 1).
- `manual <file>` -- an operand containing `/` or ending `.md` is a path (author
  preview).
- `manual --check <file>...` -- diagnostics `manual: <file>:<line>: <message>` on
  stderr; exit 1 if any.
- A file that fails the check is never rendered; its diagnostics are printed and
  the exit status is 1. Exit 2 = usage error. Diagnostics are prefixed `manual: `.
- `--beacon=auto|always|never` resolves the tier exactly as the coreutils do
  (`beacon::effective_tier` over `BEACON` + `fd_devclass(1)`).

## Mechanism

**format.rs -- parser and checker in one pass.** Lines are classified in
isolation (`classify` -> `Kind`), then block parsers consume them: `heading`,
`fence`, `bad_fence` (skips a rejected fence's body so it is not reported as
Markdown), `list`, `table` (`row_cells` splits on unescaped `|`; `\|` is a
literal pipe), `paragraph`. Every block must be separated by a blank line; the
block that ends at a non-blank line reports it once. Inline content is joined
across source lines with single spaces and scanned by `scan`, which keeps a
line map so each diagnostic names the line its character came from. Emphasis
bodies are re-scanned with `in_emphasis` set, which turns a code span or another
asterisk into a nesting diagnostic. `parse` returns a `Document` only when there
are no diagnostics; they are de-duplicated per line and sorted by line.
`check_section` adds the file-name check (a title must not begin with its
section number).

**render.rs -- one writer, two tiers.** `Writer` appends payload always and
frames only at the rich tier, so stripping frames yields the plain output (the
BEACON.md 12.1 rule 1 identity). Blocks are separated by exactly one empty line.
Tables mirror `beacon::sink::Table` byte for byte (two-space gutters, last column
unpadded, padding outside cell frames) -- asserted by
`the_table_matches_the_beacon_sink_table`. The implementation writes table
frames itself because `sink::Cell` holds one run, and a manual cell can mix
text, code and emphasis.

**wrap.rs.** Plain tier only, and only when stdout is the console and
`/dev/winsize` reports at least 20 columns (Aurora). Greedy fill; list items
hang under their marker; a code span does not break at its own spaces; a word
longer than the line is split. Pipes, pts and the serial console
(`winsize 0 0`) get one line per paragraph.

**Hygiene.** `sanitize` replaces C0 controls (TAB kept only in code blocks), DEL
and U+0080-U+009F with U+FFFD at emission, independently of the checker, which
rejects such text first. No section text ever becomes a frame argument (only the
heading level, the em class, and a table column spec of at most 16 characters),
so no frame can exceed the wire caps.

## Data structures

`format::Inline` {Text, Code, Emph, Strong} (flat: emphasis never nests);
`format::Block` {Title, Heading(2|3), Paragraph, Bullets, Numbered, Code(lines),
Table{align, header, rows}}; `format::Diagnostic {line, message}`;
`catalog::Entry {number, name, file}`; `render::Listed {name, title}`.
`SECTION_MAX` = 1 MiB (the binary reads with `slurp_capped`); the binary heap is
16 MiB (`ThylaAllocN`).

## Concurrency

None. Single-threaded, short-lived, no shared state.

## Invariants enforced

- A rendered section passed the format check (the binary renders only an `Ok`
  document).
- Strip identity: `wire::strip(render(Rich)) == render(None)` without wrapping,
  for every fixture and every installed section (host test).
- Section text cannot inject a frame or a control sequence (`sanitize`; host test
  `section_text_cannot_forge_frames`).

## Error paths

Read failures print `manual: <path>: <libthyla-rs error>`; a file over 1 MiB is
"larger than 1 MiB"; non-UTF-8 is "not valid UTF-8"; `/manual` absent reads as
no sections, any other directory error is reported with exit 1; a failed stdout
write latches (`io::OutSink`) and exits 1 with "write error".

## Performance

Linear in the section size; output rendered into memory and written in 64 KiB
chunks. The contents listing reads every installed section to take its title.

## Prosecution

Format-parsing class: the parser is exposed to any file a user names (author
preview). Prosecute: (1) frame or escape injection through any inline or block
path, including table cells, emphasis bodies and code blocks; (2) panics on
adversarial input (index arithmetic in `scan`, `emphasis`, `row_cells`,
`find_run`, the backslash skips); (3) unbounded work (quadratic scans on long
lines of asterisks or brackets -- the closing searches are linear per opener, so
a pathological line is O(n^2) within a 1 MiB cap); (4) a checker/renderer
disagreement that lets a rejected construct render.

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
- halcyond's tile path remembers 32 table specs, so a section with more tables
  than that may lose table structure in a tile.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
