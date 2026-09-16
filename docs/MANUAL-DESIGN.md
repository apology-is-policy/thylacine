# MANUAL-DESIGN.md — the Operator's Manual reader and its source format

**Status: BINDING (operator sign-off 2026-09-16).** This document is binding for
the manual's source format, the reader program, and the `/manual` installation. The *content* of a section is governed by
`docs/thylacine-operators-manual-writing-guide.md`; this document governs how that
content is encoded, installed, and displayed.

---

## 1. Purpose and the decisions already made

The writing guide requires the manual to be produced "using the repository's
supported Beacon authoring workflow" and forbids inventing "markup, anchors, link
syntax, callout types, or extensions". On 2026-09-16 no such workflow existed:
nothing installed `/manual`, no program turned Markdown into Beacon, and
`docs/BEACON.md` defines a stream vocabulary but no document format. The operator
therefore decided to build the reader before writing further sections, so that a
working renderer fixes the format.

Decisions this document builds on:

| Date | Decision | Record |
|---|---|---|
| 2026-09-05 | Markdown source in the repo, installed at `/manual`, rendered through Beacon with a plain-text fallback | `876888cf`, `docs/JOURNAL.md` |
| 2026-09-16 | The writing guide is binding; the index is `docs/OPERATORS-MANUAL.md` | `53b91177` |
| 2026-09-16 | Build the reader and the `/manual` installation before writing more sections | this document |
| 2026-09-16 | Containers is the first section written to the guide | `docs/OPERATORS-MANUAL.md` |
| 2026-09-16 | The reader is named `manual`; the three earlier pages move to `docs/manual-drafts/` and nothing is installed until a section is written to the guide | this document, section 10 |

This document fixes five things: the accepted Markdown subset (section 3), its
realization at each Beacon tier (section 4), the command (section 5), the
installation (section 6), and the verification bar (section 8). It does not add
Beacon vocabulary; any amendment goes through `docs/BEACON.md` and halcyond, which
the main track owns, and is raised there first.

---

## 2. Prior art and the shape chosen

- **Plan 9** keeps pages in `troff -man` source under `/sys/man/<section>/`; `man`
  formats a page for the output device and `lookman` searches a keyword index. The
  source is a typesetting language, and the formatter decides layout.
- **Unix `man`** follows the same model (roff source, `MANWIDTH`, a pager).
- **Genera's Document Examiner** treated documentation as structured records whose
  references were *presentations*: displayed text that remembers the object it
  names. Beacon's `obj` op descends from that idea (`docs/BEACON.md` section 7).
- **Terminal Markdown renderers** (glow, mdcat) translate Markdown into ANSI styling
  chosen by the tool.

Thylacine takes a different split from each. The source is Markdown, which is
readable unprocessed in the repository and on a serial line. The reader parses a
strict subset and emits Beacon, which carries meaning and never typography, so
halcyond's stylesheet decides presentation; stripping the frames leaves readable
plain text, which is the Beacon tier contract (`docs/BEACON.md` 12.1 rule 1). No
typesetting language is involved, and no ANSI styling is chosen by the reader.

**Why a strict subset rather than CommonMark.** Every accepted construct must have
both a Beacon realization and a plain realization. Constructs that have neither
(links, images, block quotes, raw HTML) are rejected when the section is checked,
so they never reach a reader in an approximated form. This applies the writing
guide's prohibition on invented markup mechanically. It also keeps the parser small
enough to test exhaustively; the format checker is part of the reader.

---

## 3. The source format (normative)

### 3.1 Files

- A section is one file, `docs/manual/NN-<name>.md`. `NN` is two decimal digits
  and orders the book; `<name>` consists of lowercase ASCII letters, digits, and
  hyphens, and is what a reader types to open the section. Operating-system topics
  are numbered below 40; ported applications start at 40.
- A section is UTF-8 with LF line endings and no byte-order mark. It contains no
  control characters other than LF, except TAB inside code blocks.
- A section is at most 1 MiB.
- Every section file in `docs/manual/` must pass the check (section 8.1), and every
  such file is installed (section 6). The directory holds nothing else except
  `.gitkeep`, which keeps it present in a clone while it has no sections. A draft
  that is not ready is kept in `docs/manual-drafts/`, which is neither checked nor
  installed.

### 3.2 Blocks

Blocks are separated by one or more blank lines. Every block other than the title
must be preceded by a blank line, and every block must be followed by a blank line
or the end of the file.

| Construct | Source form | Rules |
|---|---|---|
| Title | `# Text` | Line 1, exactly once; level 1 appears nowhere else. The title does not begin with the section number. |
| Heading | `## Text` or `### Text` | One space after the hashes; no closing hashes. |
| Paragraph | One or more lines that begin no other block | Lines are joined with a single space. |
| Bulleted list | Items begin `- ` | No blank line between items; continuation lines are indented by two spaces; no nesting. |
| Numbered list | Items begin `1. `, `2. `, … | Numbers run from 1 without gaps; continuation lines are indented to the item text; no nesting. |
| Code block | A line of exactly three backticks, optionally followed by one word containing no backtick, then content, then a line of three backticks | Content is taken verbatim; the word after the opening fence is ignored. |
| Table | A header row, a delimiter row, then body rows | Every row begins and ends with `\|` and has the same number of cells; at most 16 columns; the delimiter cells are `---`, `:---`, `---:`, or `:---:`. |

The checker rejects, with a diagnostic naming the line: block quotes, thematic
breaks, setext headings (a line of `=` or `-` characters directly below a
paragraph line), headings of level 4 or deeper, indented code blocks, bullets
written with `*` or `+`, nested lists, raw HTML, link reference definitions,
footnote definitions, and front matter. A line that does not begin with `|` is not
a table row, so a table written without its outer pipes is a paragraph.

### 3.3 Inline forms

| Form | Source | Rules |
|---|---|---|
| Code span | `` `text` `` or ``` ``text`` ``` | Content is literal. One leading and one trailing space are removed when both are present. |
| Emphasis | `*text*` | The opening `*` is followed by a non-space and the closing `*` is preceded by one. |
| Strong emphasis | `**text**` | As for emphasis. |
| Escape | `\` before any ASCII punctuation character, such as `` \ * ` \| < > # _ [ ] `` | Produces the character itself. |

Emphasis does not nest and does not occur inside a code span. A `*` with a space on
both sides is literal; any other `*` that neither opens nor closes an emphasis is an
error. An underscore is literal, except that underscores Markdown would read as
emphasis (`_text_`) are an error.

The checker rejects links (`[text](target)`), images, autolinks, footnote
references (`[^label]`), hard line breaks outside a code span, strikethrough
(`~~`), and a raw `<` or `>` outside a code span. Placeholders are written in code
spans, for example `` `<pid>` ``, which also keeps them visible when the file is
viewed on a code host that renders HTML.

### 3.4 Cross-references

A cross-reference is descriptive prose, as the writing guide specifies ("See
Namespaces."). There is no link syntax, and the reader does not resolve
references (section 11).

---

## 4. Rendering (normative)

### 4.1 Tier

The reader resolves its tier as the coreutils do (`docs/BEACON.md` 12.4,
`usr/coreutils/src/beacon_gate.rs`): the `BEACON` environment variable, the device
class of standard output, and the `--beacon=auto|always|never` option are passed to
`beacon::effective_tier`. With the default `auto`, frames are written only when
standard output is the console or a pts and the renderer has advertised `rich`.

### 4.2 Realization

| Source | Rich tier | Plain tiers (`none`, `cells`) |
|---|---|---|
| Title | `hdr level=1` around the title's inline content, then LF | The title's text, then LF |
| Heading | `hdr level=2` or `level=3`, then LF | The heading's text, then LF |
| Paragraph | Its inline content on one line, then LF | Identical, subject to wrapping (4.3) |
| List item | The marker (`- ` or `N. `) as text, the item's inline content, then LF | Identical, subject to wrapping (4.3) |
| Code block | `pre` open, each content line followed by LF, `pre` close | Each content line followed by LF |
| Table | `table cols=<spec>;hdr=1`, with `row` and `cell` frames; column padding outside the cell frames | Aligned columns, two spaces between columns, the last column unpadded, LF per row |
| Code span | `em class=code` around the literal text | The literal text |
| Emphasis | `em class=emph` | The text |
| Strong emphasis | `em class=strong` | The text |

Exactly one empty line separates consecutive blocks; no empty line precedes the
first block or follows the last. Table padding follows `beacon::sink::Table` so a
manual table aligns like every other tool's table. The reader emits no `rule`,
`obj`, `zone`, or `mark` frames.

Removing every frame from rich output yields the plain output byte for byte when
wrapping is off. That identity is the Beacon tier contract, and the tests assert it
(section 8.1). One consequence follows from it: at the plain tiers a code span has
no delimiters, because the delimiters cannot appear in the rich payload either.

### 4.3 Wrapping

At the rich tier the reader never wraps: a paragraph is one logical line, and the
renderer wraps it to the pane.

At a plain tier the reader wraps only when standard output is the console and
`/dev/winsize` reports at least 20 columns, which is the case under Aurora.
Paragraphs and list items are then word-wrapped at that width; a list item's
continuation lines are indented by the width of its marker, and a word longer than
the width is split. Titles, headings, code blocks, and tables are never wrapped.

In every other case (a pipe, a file, a pts, or a serial console reporting
`winsize 0 0`) the reader does not wrap, so each paragraph is a single line and
output remains suitable for `grep`. Width is counted in Unicode scalar values.

### 4.4 Output hygiene

- Before emission, every control character in section text other than LF (and TAB
  inside code blocks), together with DEL and U+0080 to U+009F, is replaced by
  U+FFFD. File content therefore cannot open, close, or imitate a Beacon frame, and
  cannot send any other terminal control sequence.
- Frame arguments are drawn only from the renderer's own values (a heading level, an
  emphasis class, a table column specification of at most 16 characters). No
  section text appears in a frame argument, so no frame can exceed the caps in
  `docs/BEACON.md` 12.1.
- The reader checks a whole section before writing any of it. It then reads the
  section a second time and writes the rendering as it goes, in chunks of at most
  64 KiB. At any moment it holds the section, the block being rendered, and one
  chunk of output, so no section within the size limit makes it fail for want of
  memory, and the time it takes grows linearly with the section's size
  (section 8.1).

---

## 5. The command

The reader is installed as `/bin/manual`.

```
manual [--beacon=auto|always|never]
manual [--beacon=auto|always|never] <name>
manual [--beacon=auto|always|never] <file>
manual --check <file>...
```

- With no operand, `manual` lists the installed sections: a title-page heading
  (`hdr level=1 class=title`, "Thylacine Operator's Manual"), a table of each
  section's name and title in book order, and a line stating how to open a
  section. When `/manual` is absent or holds no section, the heading is followed by
  a line stating that no sections are installed, and the exit status is 0.
- A `<name>` operand is resolved against the files in `/manual` named
  `NN-<name>.md`. It matches a section whose name equals the operand, whose number
  equals the operand, or whose full `NN-<name>` equals the operand; failing those,
  it matches the single section whose name begins with the operand. Matching ignores
  letter case. An operand that matches several sections is an error, and the
  diagnostic lists each candidate as `NN-<name>`, which is itself an operand that
  names one section.
- An operand containing `/` or ending in `.md` is a file path, which lets an author
  preview a draft.
- `--check` parses each file against section 3 and prints every diagnostic as
  `<file>:<line>: <message>`, in line order, reporting a problem at most once per
  line.
- A file that fails the check is not displayed; the reader prints its diagnostics
  instead. Installed sections have already passed the check at build time
  (section 6).
- Diagnostics are written to standard error, prefixed `manual: `. A name or path
  repeated in a diagnostic has its control characters replaced as in section 4.4.
- Exit status: 0 on success; 1 when no section matches, a name is ambiguous, a file
  cannot be read or is not valid UTF-8, or a check fails; 2 on a usage error.

---

## 6. Installation

- `tools/build.sh`, in the pool population step, first runs `tools/manual-check`,
  the reader's checker built for the build host, over `docs/manual/`. The bake
  fails if any file other than `.gitkeep` is not named `NN-<name>.md` or fails the
  check, and otherwise installs exactly the sections the checker listed. It creates
  `/manual` and writes each section to it, reading each back and comparing it with
  its source. With no sections, `/manual` is created empty.
  This follows the unconditional block that installs `/lib/halcyon/themes`; the
  manual is content, and no configuration key controls it.
- The binary is a member of the `usr/` workspace (`usr/manual`) and an entry in
  `usr_rs_bins`, which places it in the ramfs and therefore at `/bin/manual`.
- A bake with `THYLACINE_MKFS_PRESERVE=1` does not populate the pool, so `/manual`
  keeps its previous contents in that case.

---

## 7. Crate structure

`usr/manual` follows the `usr/view` shape: a library built for the host and the
target, and a binary behind the `backend` feature.

- The library is `no_std + alloc` and performs no I/O. `format` reads a section
  and reports what it finds, as it reads, to a consumer: each problem, in line
  order, and the section's blocks and inline runs. It keeps nothing beyond the
  block it is reading. The checker is the consumer that keeps the problems;
  `render` is the consumer that writes the rendering for a tier and an optional
  width, in chunks. `wrap` performs word wrapping as runs arrive. `catalog` resolves
  operands and builds the contents listing from a list of file names and titles.
- The binary parses arguments, reads files and directories through `libthyla-rs`,
  resolves the tier and width, and writes the output.
- `tools/manual-check` is a host program over the same library, which the bake
  runs (section 6).
- Dependencies are `beacon` (frame encoding and tier resolution) and, for the
  binary, `libthyla-rs`.

---

## 8. Verification

### 8.1 Host tests

Run with `cd usr && cargo test -p manual --lib --no-default-features --target aarch64-apple-darwin`.

- **Format.** One test for each accepted construct, and one for each rejected
  construct asserting the diagnostic's line and message.
- **Rendering.** A fixture exercising every construct has golden rich output. For
  every fixture and every file in `docs/manual/`, stripping frames from the rich
  output (`beacon::wire::strip`) equals the unwrapped plain output. Wrapping has
  goldens at width 40 covering a paragraph, a list item's hanging indent, an
  over-long word, and an unwrapped table and code block.
- **Hygiene.** A section containing a forged `ESC ] 1936 ; v1 ; obj …` sequence
  renders with no frames other than those the renderer emits, counted by parsing
  the output with beacon's parser.
- **Installed content.** The test fails if `docs/manual/` cannot be read, since an
  absent directory and an empty one would otherwise both pass. Every file in it
  other than `.gitkeep` must be named `NN-<name>.md`, pass the check, and render at
  both tiers. The test reports how many sections it checked.
- **Bounds.** Sections at the 1 MiB limit that are built to be expensive (blank
  lines, one-word paragraphs, long lists, tables and code blocks, dense inline
  forms, a problem on every line, one long line of problems) are checked and
  rendered at both tiers under the allocator `ThylaAllocN` uses
  (`linked_list_allocator`), and the heap's high-water mark stays below the
  reader's heap. The same sections, and the inline forms whose matching searches
  ahead, are checked and rendered in time that grows linearly with their size.
- **Controls.** Each hygiene and identity test is shown to fail with its mechanism
  disabled, and the commit records that result.

### 8.2 In-guest scenario

`tools/interactive/manual.exp` runs on an image built with `--config ci`. Because
no section is installed yet (section 10), the scenario writes a small fixture
section under `/tmp` from the shell and exercises the reader against it. It checks
that `manual` with no sections installed says so and exits 0, that the fixture
renders by path and prints its title, that an unknown name produces a diagnostic
and exit status 1, that the rich tier writes an `hdr` frame when the environment's
tier is `rich`, that `manual --check` accepts the fixture with exit status 0 and
rejects a copy containing a link with exit status 1, and that displaying that copy
prints its diagnostic, exits 1, and writes none of its rendering. Name resolution against `/manual` is covered by the host tests
until sections are installed, when the scenario gains a by-name case.

### 8.3 The rendered result

Once, before the chunk closes: on a Halcyon session image, `manual` and one section
are displayed in a tile and the display is captured and reviewed for headings,
spacing between paragraphs, tables, and code blocks. Rendering problems that belong
to halcyond (for example wide tables, which halcyond does not wrap) are reported to
the main track. This review is not a gate.

### 8.4 Console output under a burst

A console renderer receives output through an 8 KiB drain in `kernel/cons.c` that
discards the oldest bytes when it fills. Displaying a large section writes tens of
kilobytes at once, so the implementation measures whether Aurora loses output in
that case. A confirmed loss is a console-layer defect, recorded and fixed there
rather than worked around in the reader.

---

## 9. Audit posture

The reader is not an audit-trigger surface. It holds no capability, runs with its
caller's authority, reads only files the caller can read, and emits frames whose
arguments contain no input text. The property with security relevance, that file
content cannot inject terminal control sequences or Beacon frames, is specified in
section 4.4 and tested in section 8.1. A focused review of the parser and the
hygiene step (the format-parsing class) runs before the chunk closes.

---

## 10. The existing pages

Decided 2026-09-16: nothing is installed until it is written to the writing guide.
The three earlier pages moved to `docs/manual-drafts/`:

| Draft | State |
|---|---|
| `00-overview.md` | A Phase-0 plan (2026-05-04) that describes intended phases rather than the current system. |
| `40-dosbox.md` | Written 2026-09-05 to the earlier page template; uses thematic breaks, block quotes, and a numbered title, which section 3 rejects. |
| `41-audio.md` | Written 2026-09-05 to 2026-09-07 to the earlier page template. |

A draft returns to `docs/manual/` when it has been rewritten to the guide and
passes the check. Until then `/manual` is empty in a built image.

---

## 11. Deferred, named

- **Object references.** A section has no source form for `obj` in this version.
  Adding one means choosing a source form, and a reference to another section may
  need a new `obj` type, which is a `docs/BEACON.md` amendment.
- **List and link ops.** Lists are rendered as text. Whether a Beacon list op is
  needed is decided after the review in section 8.3, with the main track.
- **Table cell wrapping in halcyond.** A renderer change owned by the main track,
  raised if the review shows overflowing tables.
- **Keyword search, a pager, and wrapping on a pts.** Not in this version.
